#pragma once
// Records LSL streams to an XDF file (LabRecorder-compatible). One worker thread per
// stream pulls RAW sample timestamps (sender clock, post-processing left off) and
// writes Samples chunks; every few seconds it writes a ClockOffset chunk
// (time_correction vs local_clock) so an importer (pyxdf) can map all streams onto a
// common clock and dejitter — i.e. the timestamp synchronization lives in the file,
// not in altered timestamps. A shared, mutex-guarded xdf::Writer interleaves chunks.

#include "xdf_writer.hpp"
#include <lsl_cpp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

class Recorder {
public:
    ~Recorder() { stop(); }

    bool active() const { return active_.load(std::memory_order_acquire); }
    int  streams() const { return nstreams_; }
    const std::string& path() const { return path_; }
    double seconds() const { return active() ? lsl::local_clock() - t0_ : elapsed_; }
    std::uint64_t bytes() const { return writer_ ? writer_->bytes() : 0; }
    std::string error() const { std::lock_guard<std::mutex> lk(emtx_); return error_; }

    // Begin recording every stream in `infos` to `path`. Opens its OWN inlets, so
    // recording is independent of what's displayed. Returns false (and sets error)
    // if the file can't be opened or nothing is selected.
    bool start(const std::string& path, const std::vector<lsl::stream_info>& infos) {
        if (active() || infos.empty()) return false;
        { std::lock_guard<std::mutex> lk(emtx_); error_.clear(); }
        try { writer_ = std::make_unique<xdf::Writer>(path); }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(emtx_); error_ = e.what(); return false;
        }
        path_ = path; nstreams_ = (int)infos.size();
        t0_ = lsl::local_clock();
        active_.store(true, std::memory_order_release);
        xdf::streamid_t id = 1;
        for (const auto& info : infos) {
            workers_.emplace_back([this, info, id](std::stop_token st) { record(st, info, id); });
            ++id;
        }
        return true;
    }

    void stop() {
        if (!active()) return;
        elapsed_ = lsl::local_clock() - t0_;
        for (auto& w : workers_) w.request_stop();
        workers_.clear();                 // jthread joins each worker
        writer_.reset();                  // flush + close the file
        active_.store(false, std::memory_order_release);
    }

private:
    template <class T>
    void pumpLoop(std::stop_token st, lsl::stream_inlet& inlet, xdf::streamid_t id, int nchan,
                  double& first, double& last, std::uint64_t& count) {
        std::vector<T>      buf;
        std::vector<double> ts;
        double lastClock = 0.0, lastBoundary = lsl::local_clock();
        while (!st.stop_requested()) {
            try { inlet.pull_chunk_multiplexed(buf, &ts, 0.5); }
            catch (const std::exception&) { continue; }  // recover=true reconnects the pull
            const double now = lsl::local_clock();
            if (now - lastClock > 5.0) {                 // periodic clock sync
                try { writer_->clock_offset(id, now, inlet.time_correction()); } catch (...) {}
                lastClock = now;
                if (id == 1 && now - lastBoundary > 10.0) { writer_->boundary(); lastBoundary = now; }
            }
            if (ts.empty()) continue;
            if (first == 0.0) first = ts.front();
            last   = ts.back();
            count += ts.size();
            writer_->data_chunk(id, ts, buf.data(),
                                (std::uint32_t)ts.size(), (std::uint32_t)nchan);
        }
    }

    void record(std::stop_token st, lsl::stream_info info, xdf::streamid_t id) {
        try {
            lsl::stream_inlet inlet(info, /*max_buflen*/ 360, /*max_chunklen*/ 0, /*recover*/ true);
            inlet.open_stream();
            lsl::stream_info full = inlet.info(5.0);     // full XML (with desc) after connect
            writer_->stream_header(id, full.as_xml());
            const int nchan = full.channel_count();
            try { writer_->clock_offset(id, lsl::local_clock(), inlet.time_correction(2.0)); } catch (...) {}

            double first = 0.0, last = 0.0; std::uint64_t count = 0;
            switch (full.channel_format()) {
            case lsl::cf_float32:  pumpLoop<float>      (st, inlet, id, nchan, first, last, count); break;
            case lsl::cf_double64: pumpLoop<double>     (st, inlet, id, nchan, first, last, count); break;
            case lsl::cf_int32:    pumpLoop<std::int32_t>(st, inlet, id, nchan, first, last, count); break;
            case lsl::cf_int16:    pumpLoop<std::int16_t>(st, inlet, id, nchan, first, last, count); break;
            case lsl::cf_int8:     pumpLoop<char>        (st, inlet, id, nchan, first, last, count); break;
            case lsl::cf_int64:    pumpLoop<std::int64_t>(st, inlet, id, nchan, first, last, count); break;
            case lsl::cf_string:   pumpLoop<std::string>(st, inlet, id, nchan, first, last, count); break;
            default: break;       // cf_undefined: header only
            }

            std::ostringstream f;        // footer: bounds for the importer
            f << "<?xml version=\"1.0\"?><info><first_timestamp>" << first
              << "</first_timestamp><last_timestamp>" << last
              << "</last_timestamp><sample_count>" << count << "</sample_count></info>";
            writer_->stream_footer(id, f.str());
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(emtx_); error_ = e.what();
        }
    }

    std::unique_ptr<xdf::Writer> writer_;
    std::vector<std::jthread>    workers_;
    std::atomic<bool>            active_{false};
    double                       t0_ = 0.0, elapsed_ = 0.0;
    std::string                  path_;
    int                          nstreams_ = 0;
    mutable std::mutex           emtx_;
    std::string                  error_;
};
