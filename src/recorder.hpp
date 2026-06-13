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
#include "thread_compat.hpp"   // jthread / stop_token (with an Apple-libc++ polyfill)

class Recorder {
public:
    ~Recorder() { stop(); if (closer_.joinable()) closer_.join(); }   // flush before exit

    bool active() const { return active_.load(std::memory_order_acquire); }
    int  streams() const { return nstreams_; }
    const std::string& path() const { return path_; }
    double seconds() const { return active() ? lsl::local_clock() - t0_ : elapsed_; }
    std::uint64_t bytes() const { return writer_ ? writer_->bytes() : lastBytes_; }  // final size survives the deferred close
    std::string error() const { std::lock_guard<std::mutex> lk(emtx_); return error_; }

    // Begin recording every stream in `infos` to `path`. Opens its OWN inlets, so
    // recording is independent of what's displayed. Returns false (and sets error)
    // if the file can't be opened or nothing is selected.
    bool start(const std::string& path, const std::vector<lsl::stream_info>& infos) {
        if (active() || infos.empty()) return false;
        // Finish any prior deferred close before reopening — else opening a new
        // (truncating) Writer on the same path would clobber a file the old workers
        // are still flushing. Practically instant unless a stop happened <0.5 s ago.
        if (closer_.joinable()) closer_.join();
        { std::lock_guard<std::mutex> lk(emtx_); error_.clear(); }
        try { writer_ = std::make_shared<xdf::Writer>(path); }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(emtx_); error_ = e.what(); return false;
        }
        path_ = path; nstreams_ = (int)infos.size();
        t0_ = lsl::local_clock();
        active_.store(true, std::memory_order_release);
        xdf::streamid_t id = 1;
        for (const auto& info : infos) {
            auto wr = writer_;   // shared ref: keeps the file alive for a deferred close
            workers_.emplace_back([this, info, id, wr](stop_token st) { record(st, info, id, wr); });
            ++id;
        }
        return true;
    }

    void stop() {
        if (!active_.exchange(false)) return;   // mark inactive immediately
        elapsed_ = lsl::local_clock() - t0_;
        lastBytes_ = writer_ ? writer_->bytes() : 0;   // snapshot before the writer is moved away
        for (auto& w : workers_) w.request_stop();
        // Hand the workers + writer to a detached-ish closer so the UI thread doesn't
        // block on each worker's join (a worker can be parked ~0.5 s in pull_chunk's
        // timeout). Same deferred-reap idea as stream disconnect. The workers hold their
        // own shared_ptr to the writer, so the file stays valid until they're all done,
        // then the writer drops here and flushes + closes. A subsequent stop() / the
        // dtor joins this thread, so the file is never truncated.
        if (closer_.joinable()) closer_.join();   // prior close (practically already done)
        closer_ = std::thread([w = std::move(workers_), wr = std::move(writer_)]() mutable {
            w.clear();      // join each worker off the UI thread
            wr.reset();     // last ref -> flush footer + close the file
        });
        workers_.clear();   // already moved-from; ensure empty for the next start()
    }

private:
    template <class T>
    void pumpLoop(stop_token st, lsl::stream_inlet& inlet, xdf::Writer& w, xdf::streamid_t id,
                  int nchan, double& first, double& last, std::uint64_t& count) {
        std::vector<T>      buf;
        std::vector<double> ts;
        double lastClock = 0.0, lastBoundary = lsl::local_clock();
        while (!st.stop_requested()) {
            try { inlet.pull_chunk_multiplexed(buf, &ts, 0.5); }
            catch (const std::exception&) { continue; }  // recover=true reconnects the pull
            const double now = lsl::local_clock();
            if (now - lastClock > 5.0) {                 // periodic clock sync
                try { w.clock_offset(id, now, inlet.time_correction()); } catch (...) {}
                lastClock = now;
                if (id == 1 && now - lastBoundary > 10.0) { w.boundary(); lastBoundary = now; }
            }
            if (ts.empty()) continue;
            if (first == 0.0) first = ts.front();
            last   = ts.back();
            count += ts.size();
            w.data_chunk(id, ts, buf.data(), (std::uint32_t)ts.size(), (std::uint32_t)nchan);
        }
    }

    void record(stop_token st, lsl::stream_info info, xdf::streamid_t id,
                std::shared_ptr<xdf::Writer> wr) {
        xdf::Writer& w = *wr;            // own ref; outlives a deferred close
        try {
            lsl::stream_inlet inlet(info, /*max_buflen*/ 360, /*max_chunklen*/ 0, /*recover*/ true);
            inlet.open_stream();
            lsl::stream_info full = inlet.info(5.0);     // full XML (with desc) after connect
            w.stream_header(id, full.as_xml());
            const int nchan = full.channel_count();
            try { w.clock_offset(id, lsl::local_clock(), inlet.time_correction(2.0)); } catch (...) {}

            double first = 0.0, last = 0.0; std::uint64_t count = 0;
            switch (full.channel_format()) {
            case lsl::cf_float32:  pumpLoop<float>      (st, inlet, w, id, nchan, first, last, count); break;
            case lsl::cf_double64: pumpLoop<double>     (st, inlet, w, id, nchan, first, last, count); break;
            case lsl::cf_int32:    pumpLoop<std::int32_t>(st, inlet, w, id, nchan, first, last, count); break;
            case lsl::cf_int16:    pumpLoop<std::int16_t>(st, inlet, w, id, nchan, first, last, count); break;
            case lsl::cf_int8:     pumpLoop<char>        (st, inlet, w, id, nchan, first, last, count); break;
            case lsl::cf_int64:    pumpLoop<std::int64_t>(st, inlet, w, id, nchan, first, last, count); break;
            case lsl::cf_string:   pumpLoop<std::string>(st, inlet, w, id, nchan, first, last, count); break;
            default: break;       // cf_undefined: header only
            }

            std::ostringstream f;        // footer: bounds for the importer
            f << "<?xml version=\"1.0\"?><info><first_timestamp>" << first
              << "</first_timestamp><last_timestamp>" << last
              << "</last_timestamp><sample_count>" << count << "</sample_count></info>";
            w.stream_footer(id, f.str());
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(emtx_); error_ = e.what();
        }
    }

    std::shared_ptr<xdf::Writer> writer_;   // shared with workers so a deferred close is safe
    std::vector<jthread>    workers_;
    std::thread                  closer_;   // deferred join + flush (keeps stop() off the UI thread)
    std::atomic<bool>            active_{false};
    double                       t0_ = 0.0, elapsed_ = 0.0;
    std::uint64_t                lastBytes_ = 0;   // file size at the last stop() (writer_ is moved away)
    std::string                  path_;
    int                          nstreams_ = 0;
    mutable std::mutex           emtx_;
    std::string                  error_;
};
