#pragma once
// High-throughput LSL acquisition for the new display path.
//
// HfStreamSource owns one lsl::stream_inlet pulled on a dedicated worker thread
// with pull_chunk_multiplexed into a preallocated flat buffer, then:
//   - ONE memcpy into InterleavedRing   (raw, for zoomed-in / FFT reads)
//   - fold into MinMaxSummary           (decimated envelope, normal view)
// Both structures are lock-free SPSC; the render thread reads them directly.
//
// The producer is the ONLY writer to the ring and the summary (DESIGN invariant 1).
// For a regular-rate stream we do NOT store per-sample timestamps: x is derived
// from absolute sample index + srate using a single (index <-> LSL clock) anchor
// (DESIGN invariant 5).
//
// Discovery is unchanged from lsl_source.hpp (kept here so the new path has no
// dependency on the superseded per-sample StreamSource).

#include <lsl_cpp.h>

#include "magic_ring_buffer.hpp"
#include "minmax_summary.hpp"
#include "filter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Per-channel sensor position from the stream's XDF/LSL metadata
// (channels/channel/location X,Y,Z). `valid` is false when the source publishes no
// location for that channel — the modality-agnostic basis for any spatial view.
struct ChanLoc { float x = 0.0f, y = 0.0f, z = 0.0f; bool valid = false; };

class HfStreamSource {
public:
    // history_seconds is only an initial view hint; buffers are sized for the
    // viewer's maximum window (kMaxHistory) so the history slider can range
    // freely without reallocating.
    HfStreamSource(const lsl::stream_info& info, double /*history_seconds*/)
        : info_(info),
          name_(info.name()),
          type_(info.type()),
          uid_(info.uid()),
          sourceId_(info.source_id()),
          channels_(info.channel_count()),
          srate_(info.nominal_srate()),
          dt_(srate_ > 0.0 ? 1.0 / srate_ : 0.0)
    {
        // Irregular-rate float streams (e.g. a mouse) are resampled onto a fixed
        // grid by sample-and-hold at ingestion (see runIrregular), so the rest of the
        // pipeline treats them as a regular stream. Present that grid rate as srate/dt;
        // the browser still shows the true (irregular) nominal from the stream info.
        if (srate_ <= 0.0) {
            irregular_ = true;
            srate_ = kIrregularRate;
            dt_    = 1.0 / kIrregularRate;
        }
        const double rate = effectiveRate();

        // Raw ring: hold kMaxHistory seconds of interleaved samples.
        const std::size_t hist = (std::size_t)(rate * kMaxHistory);
        ring_.init(channels_, std::max<std::size_t>(hist, 4096));
        // Parallel filtered ring (same shape/indices) holding the display-filter chain
        // output, so the FFT / spectrogram / zoomed line view can read filtered samples
        // at full rate without re-running the filter per window.
        ringHp_.init(channels_, std::max<std::size_t>(hist, 4096));

        // One min/max level sized for the most zoomed-out window: pick B so the
        // full window is ~kTargetBins bins (≈ 1 bin/pixel at max zoom-out).
        bin_ = std::max<int>(1, (int)(rate * kMaxHistory / kTargetBins));
        const std::size_t windowBins = (std::size_t)(rate * kMaxHistory / bin_) + 1;
        summary_.init(channels_, bin_, windowBins + 256);  // +guard +slack
        summaryHp_.init(channels_, bin_, windowBins + 256); // high-pass filtered

        hp_.init(channels_, 0.999f);   // running high-pass; R set per chunk
        setHighpassHz(0.5);            // default 0.5 Hz display high-pass
        notch_.init(channels_);        // optional mains notch + low-pass (off by default)
        lp_.init(channels_);

        // Default channel labels; refined from desc() XML once connected.
        labels_.resize(channels_);
        for (int c = 0; c < channels_; ++c)
            labels_[c] = "ch " + std::to_string(c);
        units_.assign(channels_, "");
        types_.assign(channels_, "");
        locs_.assign(channels_, ChanLoc{});
        labelsDefault_ = labels_;   // immutable snapshots used until metadata is parsed
        unitsDefault_  = units_;
        typesDefault_  = types_;
        locsDefault_   = locs_;

        chanGain_.assign(channels_, 1.0f);   // per-channel display gain (render-only)
        chanAmp_.assign(channels_, 0.0f);    // measured µV amplitude (render-only)

        chunkSamples_ = std::max<std::size_t>(1024, (std::size_t)(rate * 0.5));
    }

    ~HfStreamSource() { stop(); }   // joins worker before the buffers it uses go away

    void start() {
        if (worker_.joinable()) return;
        worker_ = std::jthread([this](std::stop_token st) { run(st); });
    }
    void stop() {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
    }
    // Signal the worker to stop without joining, so many sources can shut down
    // concurrently (their pull-timeout waits then overlap rather than summing).
    void requestStop() { worker_.request_stop(); }
    // True once the worker thread has fully exited — then ~HfStreamSource (join) is
    // instant, so the owner can reap closed sources without an UI-thread stall.
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    // --- identity / shape (immutable after construction) ---------------------
    const std::string& name()     const { return name_; }
    const std::string& type()     const { return type_; }
    const std::string& uid()      const { return uid_; }
    const std::string& sourceId() const { return sourceId_; }  // globally-unique, reconnect-stable
    int    channels() const { return channels_; }
    double srate()    const { return srate_; }
    double dt()       const { return dt_; }
    int    binSamples() const { return bin_; }
    bool   irregular() const { return irregular_; }  // resampled sample-and-hold

    // Running high-pass cutoff (Hz). Updated from the UI; producer reads it per
    // chunk. Only affects samples folded after the change (fine for a live view).
    void setHighpassHz(double fc) {
        hpR_.store(DcBlocker::cutoffToR(fc, srate_), std::memory_order_relaxed);
    }
    // Optional notch (mains 50/60 Hz) and low-pass stages of the display filter chain
    // (high-pass -> notch -> low-pass), feeding the filtered envelope. The producer
    // reads these atomics per chunk and reconfigures the biquads when they change.
    void setNotch(bool on)        { notchOn_.store(on, std::memory_order_relaxed); }
    void setNotchHz(double f0)    { notchHz_.store((float)f0, std::memory_order_relaxed); }
    void setLowpass(bool on)      { lpOn_.store(on, std::memory_order_relaxed); }
    void setLowpassHz(double fc)  { lpHz_.store((float)fc, std::memory_order_relaxed); }
    // Re-reference montage applied as the FIRST stage of the conditioned (filtered)
    // chain: 0 = none, 1 = common-average (CAR), 2 = single reference channel.
    void setReference(int mode)   { refMode_.store(mode, std::memory_order_relaxed); }
    void setRefChannel(int c)     { refChan_.store(c, std::memory_order_relaxed); }

    // --- live state (lock-free reads from the render thread) -----------------
    InterleavedRing&     ring()       { return ring_; }       // raw full-rate
    InterleavedRing&     ringHp()     { return ringHp_; }     // filter-chain full-rate (FFT/spectro/zoom)
    MinMaxSummary&       summary()    { return summary_; }    // raw envelope
    MinMaxSummary&       summaryHp()  { return summaryHp_; }  // filtered envelope
    std::vector<float>&  chanGain()   { return chanGain_; }   // per-channel display gain
    std::vector<float>&  chanAmp()    { return chanAmp_; }    // measured µV amplitude
    bool          anchored() const { return anchored_.load(std::memory_order_acquire); }
    double        time0()    const { return t0_.load(std::memory_order_acquire); }
    // Seconds since the last chunk arrived (for a "temporarily missing" indicator).
    double staleSeconds() const {
        const double t = lastData_.load(std::memory_order_relaxed);
        return t > 0.0 ? (lsl::local_clock() - t) : 0.0;
    }
    std::uint64_t head()     const { return ring_.head(); }
    // Wall-clock time (s) of the newest sample, accounting for dropouts.
    double newestTime() const { return realTime(time0() + (double)head() * dt_); }

    // --- dropout-aware time mapping ------------------------------------------
    // Map an index-derived ("old-frame") time to real time by adding the duration
    // of every recorded dropout that occurred at/before it. Keeps the time axis
    // honest so a stream that was down shows a gap rather than collapsing.
    double realTime(double oldT) const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        double off = gapBase_;   // gaps pruned from the list contribute a constant base
        for (auto& g : gaps_) if (g.first <= oldT) off += g.second;
        return oldT + off;
    }
    // In-place old-frame -> real time for a monotonically increasing array.
    void applyGaps(float* x, int n) const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        double off = gapBase_; std::size_t gi = 0;
        if (gaps_.empty() && off == 0.0) return;
        for (int i = 0; i < n; ++i) {
            while (gi < gaps_.size() && gaps_[gi].first <= (double)x[i]) { off += gaps_[gi].second; ++gi; }
            x[i] = (float)((double)x[i] + off);
        }
    }
    // Snapshot of recorded dropouts as (old-frame time, seconds).
    std::vector<std::pair<double, double>> gaps() const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        return gaps_;
    }
    // Consistent (folded-base offset, recent gaps) for display positioning — taken
    // under one lock so a concurrent prune can't double-count a just-folded gap.
    std::pair<double, std::vector<std::pair<double, double>>> gapSnapshot() const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        return {gapBase_, gaps_};
    }
    float hpR() const { return hpR_.load(std::memory_order_relaxed); }  // for on-the-fly filtering

    // --- stream health (lock-free reads) -------------------------------------
    double        measuredRate() const { return measuredRate_.load(std::memory_order_relaxed); }   // EMA samples/s
    double        clockOffset()  const { return clockOffset_.load(std::memory_order_relaxed); }     // remote->local (s)
    std::uint64_t dropouts()     const { return dropouts_.load(std::memory_order_relaxed); }        // recorded gaps

    // Snapshot of channel labels / units (written once on connect, under a lock).
    // Channel labels/units are write-once: ctor defaults, finalized once by
    // parseLabels just after connect. Once finalized they're immutable, so we hand
    // out a lock-free const-ref — was a vector<string> copy under a mutex on EVERY
    // call (per stream/spectrogram/ERP/Spectrum window, every frame). Before the
    // metadata lands we return the immutable defaults snapshot.
    const std::vector<std::string>& labels() const {
        return metaReady_.load(std::memory_order_acquire) ? labels_ : labelsDefault_;
    }
    const std::vector<std::string>& units() const {
        return metaReady_.load(std::memory_order_acquire) ? units_ : unitsDefault_;
    }
    // Per-channel modality type ("EEG", "EOG", "fNIRS", ...) and sensor position, parsed
    // from the stream metadata (write-once; lock-free after metaReady_, like labels()).
    const std::vector<std::string>& types() const {
        return metaReady_.load(std::memory_order_acquire) ? types_ : typesDefault_;
    }
    const std::vector<ChanLoc>& locs() const {
        return metaReady_.load(std::memory_order_acquire) ? locs_ : locsDefault_;
    }
    std::string error() {
        std::lock_guard<std::mutex> lk(mtx_);
        return error_;
    }

private:
    double effectiveRate() const { return srate_ > 0.0 ? srate_ : 100.0; }

    // Re-reference montage, the first stage of the conditioned chain: subtract a common
    // reference from every channel. Returns `in` unchanged when off, else fills `scratch`
    // and returns it. Producer-only (reads the mode/channel atomics).
    const float* reference(const float* in, float* scratch, std::size_t n) {
        const int mode = refMode_.load(std::memory_order_relaxed);
        const int C = channels_;
        if (mode == 0 || C <= 1) return in;
        if (mode == 2) {                                   // single reference channel
            int rc = refChan_.load(std::memory_order_relaxed);
            if (rc < 0 || rc >= C) rc = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const float* x = in + i * (std::size_t)C;
                float* o = scratch + i * (std::size_t)C;
                const float r = x[rc];
                for (int c = 0; c < C; ++c) o[c] = x[c] - r;
            }
        } else {                                           // common-average reference
            for (std::size_t i = 0; i < n; ++i) {
                const float* x = in + i * (std::size_t)C;
                float* o = scratch + i * (std::size_t)C;
                float sum = 0.0f; for (int c = 0; c < C; ++c) sum += x[c];
                const float m = sum / (float)C;
                for (int c = 0; c < C; ++c) o[c] = x[c] - m;
            }
        }
        return scratch;
    }

    // Reconfigure the notch/low-pass biquads from the UI atomics (only on change) and
    // apply the enabled stages in place to the already-high-passed buffer. Producer-only.
    void applyPostFilters(float* x, std::size_t n) {
        const bool  non = notchOn_.load(std::memory_order_relaxed);
        const float nf  = notchHz_.load(std::memory_order_relaxed);
        if (non != notchApplied_ || nf != notchHzApplied_) {
            notch_.enabled = non;
            if (non) notch_.setNotch(nf, srate_, 30.0);   // narrow (Q=30): only the mains line
            notchApplied_ = non; notchHzApplied_ = nf;
        }
        const bool  lon = lpOn_.load(std::memory_order_relaxed);
        const float lf  = lpHz_.load(std::memory_order_relaxed);
        if (lon != lpApplied_ || lf != lpHzApplied_) {
            lp_.enabled = lon;
            if (lon) lp_.setLowpass(lf, srate_, 0.70710678);  // Butterworth
            lpApplied_ = lon; lpHzApplied_ = lf;
        }
        if (notch_.enabled) notch_.process(x, n);
        if (lp_.enabled)    lp_.process(x, n);
    }

    void run(std::stop_token st) {
        // Set on ANY exit (return/exception) so the owner can reap us off the UI
        // thread: it stops + drops the window now, then joins once finished() is true
        // (instant), instead of blocking ~0.5 s in join() while the worker winds down.
        struct Done { std::atomic<bool>& f; ~Done() { f.store(true, std::memory_order_release); } }
            _done{finished_};
        if (irregular_) { runIrregular(st); return; }
        try {
            lsl::stream_inlet inlet(info_, /*max_buflen*/ 360, /*max_chunklen*/ 0,
                                    /*recover*/ true);
            inlet.open_stream();

            // Full info (with desc()) only becomes available after the inlet
            // connects; the resolve result often lacks the channel descriptions.
            try { parseLabels(inlet.info(2.0)); } catch (...) { /* keep defaults */ }

            double offset = inlet.time_correction(2.0);   // remote -> local clock
            clockOffset_.store(offset, std::memory_order_relaxed);
            double lastCorr = lsl::local_clock();

            std::vector<float>  buf(chunkSamples_ * (std::size_t)channels_);
            std::vector<double> ts(chunkSamples_);
            std::vector<float>  hpBuf(chunkSamples_ * (std::size_t)channels_);
            std::vector<float>  refBuf(chunkSamples_ * (std::size_t)channels_);  // re-reference scratch

            while (!st.stop_requested()) {
                const std::size_t got = inlet.pull_chunk_multiplexed(
                    buf.data(), ts.data(), buf.size(), ts.size(), 0.1);
                if (got == 0) continue;
                const std::size_t n = got / (std::size_t)channels_;
                if (n == 0) continue;
                const double tnow = lsl::local_clock();
                lastData_.store(tnow, std::memory_order_relaxed);
                // Measured incoming rate (EMA samples/s) — the actual delivery rate, which
                // a quick glance against nominal exposes a misconfigured or struggling source.
                if (lastChunkT_ > 0.0) {
                    const double dtc = tnow - lastChunkT_;
                    if (dtc > 1e-6) {
                        const double inst = (double)n / dtc;
                        const double cur  = measuredRate_.load(std::memory_order_relaxed);
                        measuredRate_.store(cur <= 0.0 ? inst : cur + 0.1 * (inst - cur),
                                            std::memory_order_relaxed);
                    }
                }
                lastChunkT_ = tnow;

                const std::uint64_t headBefore = ring_.head();

                // Anchor once: map the newest sample's LSL timestamp to its
                // absolute index, giving t0 = wall-clock time of sample 0.
                if (!anchored_.load(std::memory_order_relaxed) && dt_ > 0.0) {
                    const std::uint64_t absLast = headBefore + n - 1;
                    const double t = (ts[n - 1] + offset) - (double)absLast * dt_;
                    t0_.store(t, std::memory_order_release);
                    anchored_.store(true, std::memory_order_release);
                }

                // Dropout detection: compare the sender timestamp of the first new
                // sample to the one predicted by continuous sampling. A forward jump
                // means the stream was down; record the gap (at the sample's
                // index-derived "old-frame" time) so display time stays honest.
                // Done BEFORE publishing the samples (ring_.write advances head), so the
                // render never sees the resumed data without its gap — otherwise the live
                // red would briefly mask the just-arrived samples for a frame.
                if (dt_ > 0.0 && anchored_.load(std::memory_order_relaxed)) {
                    if (lastTsValid_) {
                        const double g = ts[0] - (lastTs_ + dt_);
                        if (g > 0.25) {   // s; above jitter, below real dropouts
                            const double oldTime = t0_.load(std::memory_order_acquire)
                                                 + (double)headBefore * dt_;
                            dropouts_.fetch_add(1, std::memory_order_relaxed);
                            std::lock_guard<std::mutex> lk(gapMtx_);
                            gaps_.push_back({oldTime, g});
                            // Fold gaps far older than any visible window into a constant
                            // base (a gap offsets ALL later times), so the per-frame walk in
                            // realTime/applyGaps stays bounded over an hours-long session.
                            const double cutoff = oldTime - 120.0;   // > max history + ring reach
                            std::size_t k = 0;
                            while (k < gaps_.size() && gaps_[k].first < cutoff) gapBase_ += gaps_[k++].second;
                            if (k > 0) gaps_.erase(gaps_.begin(), gaps_.begin() + k);
                        }
                    }
                    lastTs_ = ts[n - 1];
                    lastTsValid_ = true;
                }

                // Publish: head advances now, with the gap (if any) already recorded.
                ring_.write(buf.data(), n);        // one memcpy
                summary_.append(buf.data(), n);    // fold, cache-hot
                // Conditioned chain: re-reference -> high-pass -> notch -> low-pass.
                const float* condIn = reference(buf.data(), refBuf.data(), n);
                hp_.setR(hpR_.load(std::memory_order_relaxed));   // running high-pass
                hp_.process(condIn, hpBuf.data(), n);
                applyPostFilters(hpBuf.data(), n);                // optional notch + low-pass
                ringHp_.write(hpBuf.data(), n);                   // full-rate filtered (FFT/spectro/zoom)
                summaryHp_.append(hpBuf.data(), n);

                const double now = lsl::local_clock();
                if (now - lastCorr > 5.0) {        // refresh for cross-stream align
                    // A transient timeout here (stream briefly down) must NOT kill the
                    // worker — recover=true on the inlet reconnects the pull on its own.
                    try { offset = inlet.time_correction(1.0); clockOffset_.store(offset, std::memory_order_relaxed); }
                    catch (const std::exception&) { /* keep previous offset */ }
                    lastCorr = now;
                }
            }
            inlet.close_stream();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mtx_);
            error_ = e.what();
        }
    }

    // Irregular float stream: resample onto the fixed grid (dt_ = 1/kIrregularRate)
    // by sample-and-hold. We pull one sample at a time and, every grid step, write the
    // most recent value — so an event-driven stream (mouse) becomes a regular trace
    // that holds its last value between updates. All downstream code is unchanged.
    void runIrregular(std::stop_token st) {
        try {
            lsl::stream_inlet inlet(info_, /*max_buflen*/ 360, /*max_chunklen*/ 0,
                                    /*recover*/ true);
            inlet.open_stream();
            try { parseLabels(inlet.info(2.0)); } catch (...) { /* keep defaults */ }
            double offset = 0.0;
            try { offset = inlet.time_correction(2.0); } catch (...) {}
            double lastCorr = lsl::local_clock();

            std::vector<float> cur(channels_, 0.0f);     // most recent (held) value
            std::vector<float> in(channels_);            // one pulled sample
            std::vector<float> out, hpOut, refOut;
            std::uint64_t      written = 0;              // grid samples emitted
            double             t0local = 0.0;            // local time of grid sample 0
            bool               started = false;

            while (!st.stop_requested()) {
                double sts = 0.0;
                try { sts = inlet.pull_sample(in, dt_); }   // wait ~one grid step
                catch (const std::exception&) { continue; } // recover reconnects
                const double now = lsl::local_clock();
                if (now - lastCorr > 5.0) {
                    try { offset = inlet.time_correction(1.0); } catch (...) {}
                    lastCorr = now;
                }
                if (sts != 0.0) {                            // new value arrived
                    for (int c = 0; c < channels_; ++c) cur[c] = in[c];
                    lastData_.store(now, std::memory_order_relaxed);
                    if (!started) {
                        t0local = sts + offset;              // anchor the grid here
                        t0_.store(t0local, std::memory_order_release);
                        anchored_.store(true, std::memory_order_release);
                        started = true;
                        written = 0;
                    }
                }
                if (!started) continue;

                // Advance the grid up to 'now', emitting the held value.
                const std::uint64_t target =
                    (std::uint64_t)std::max(0.0, (now - t0local) * (double)kIrregularRate);
                while (written < target && !st.stop_requested()) {
                    const std::size_t batch =
                        (std::size_t)std::min<std::uint64_t>(target - written, 512);
                    out.resize(batch * (std::size_t)channels_);
                    for (std::size_t i = 0; i < batch; ++i)
                        for (int c = 0; c < channels_; ++c)
                            out[i * (std::size_t)channels_ + c] = cur[c];
                    ring_.write(out.data(), batch);
                    summary_.append(out.data(), batch);
                    refOut.resize(batch * (std::size_t)channels_);
                    const float* condIn = reference(out.data(), refOut.data(), batch);
                    hp_.setR(hpR_.load(std::memory_order_relaxed));
                    hpOut.resize(batch * (std::size_t)channels_);
                    hp_.process(condIn, hpOut.data(), batch);
                    applyPostFilters(hpOut.data(), batch);        // optional notch + low-pass
                    ringHp_.write(hpOut.data(), batch);           // full-rate filtered
                    summaryHp_.append(hpOut.data(), batch);
                    written += batch;
                }
            }
            inlet.close_stream();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mtx_);
            error_ = e.what();
        }
    }

    void parseLabels(lsl::stream_info full) {  // desc() is non-const in liblsl
        if (metaReady_.load(std::memory_order_acquire)) return;  // finalize once (reconnect re-calls)
        lsl::xml_element ch = full.desc().child("channels").child("channel");
        std::vector<std::string> plabels, punits, ptypes;
        std::vector<ChanLoc>     plocs;
        for (; !ch.empty(); ch = ch.next_sibling("channel")) {
            const char* label = ch.child_value("label");
            const char* unit  = ch.child_value("unit");
            const char* type  = ch.child_value("type");
            plabels.emplace_back(label ? label : "");
            punits.emplace_back(unit ? unit : "");
            ptypes.emplace_back(type ? type : "");
            // Sensor position (channels/channel/location X,Y,Z) — present for spatially
            // located modalities (EEG/MEG/fNIRS); absent for the rest.
            ChanLoc loc;
            lsl::xml_element le = ch.child("location");
            if (!le.empty()) {
                const char* X = le.child_value("X");
                const char* Y = le.child_value("Y");
                const char* Z = le.child_value("Z");
                if (X[0] || Y[0] || Z[0]) {
                    loc.x = (float)std::atof(X); loc.y = (float)std::atof(Y); loc.z = (float)std::atof(Z);
                    loc.valid = true;
                }
            }
            plocs.push_back(loc);
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (int c = 0; c < channels_ && c < (int)plabels.size(); ++c) {
                if (!plabels[c].empty()) labels_[c] = plabels[c];
                if (c < (int)punits.size() && !punits[c].empty()) units_[c] = punits[c];
                if (c < (int)ptypes.size()) types_[c] = ptypes[c];
                if (c < (int)plocs.size())  locs_[c]  = plocs[c];
            }
        }
        metaReady_.store(true, std::memory_order_release);  // labels_/units_ now immutable
    }

    static constexpr double kMaxHistory   = 60.0;   // seconds buffers are sized for
    static constexpr double kTargetBins   = 2000.0; // ~bins across the max window
    static constexpr double kIrregularRate = 100.0; // sample-and-hold grid for irregular streams

    lsl::stream_info info_;
    std::string      name_, type_, uid_, sourceId_;
    int              channels_ = 0;
    double           srate_ = 0.0, dt_ = 0.0;
    bool             irregular_ = false;   // resampled (sample-and-hold) from an irregular stream
    int              bin_ = 32;
    std::size_t      chunkSamples_ = 1024;

    InterleavedRing  ring_;        // raw full-rate
    InterleavedRing  ringHp_;      // filter-chain full-rate (parallel to ring_)
    MinMaxSummary    summary_;     // raw envelope
    MinMaxSummary    summaryHp_;   // filtered envelope

    // Producer-only display filter chain: DC blocker (high-pass) -> notch -> low-pass.
    // Cutoffs/enables are read atomically (UI may change); biquad state is producer-only.
    DcBlocker           hp_;
    std::atomic<float>  hpR_{0.999f};
    Biquad              notch_, lp_;
    std::atomic<bool>   notchOn_{false}, lpOn_{false};
    std::atomic<float>  notchHz_{60.0f}, lpHz_{40.0f};
    std::atomic<int>    refMode_{0}, refChan_{0};   // 0 none / 1 CAR / 2 single-channel
    // last-applied params, so the producer only recomputes coeffs on change
    bool                notchApplied_ = false, lpApplied_ = false;
    float               notchHzApplied_ = -1.0f, lpHzApplied_ = -1.0f;

    // Render-thread-only display state (one plot per source).
    std::vector<float>  chanGain_, chanAmp_;

    std::atomic<bool>   anchored_{false};
    std::atomic<double> t0_{0.0};
    std::atomic<double> lastData_{0.0};   // local_clock of the last chunk received

    // Stream health (producer-written, render-read).
    std::atomic<double>        measuredRate_{0.0};   // EMA of incoming samples/s
    std::atomic<double>        clockOffset_{0.0};    // last remote->local time_correction (s)
    std::atomic<std::uint64_t> dropouts_{0};         // count of recorded gaps
    double                     lastChunkT_ = 0.0;    // producer-only: local time of previous chunk

    // Dropout tracking. gaps_ is producer-appended / render-read under gapMtx_;
    // lastTs_/lastTsValid_ are producer-only.
    mutable std::mutex                          gapMtx_;
    std::vector<std::pair<double, double>>      gaps_;   // (old-frame time, seconds), recent only
    double                                      gapBase_ = 0.0;   // folded offset of pruned gaps
    double                                      lastTs_ = 0.0;
    bool                                        lastTsValid_ = false;

    std::jthread        worker_;
    std::atomic<bool>   finished_{false};   // worker has exited -> safe to reap (instant join)

    std::mutex               mtx_;     // guards labels_/units_ until metaReady_, and error_
    std::vector<std::string> labels_, units_, types_;
    std::vector<ChanLoc>     locs_;                          // per-channel sensor positions (valid when published)
    std::vector<std::string> labelsDefault_, unitsDefault_, typesDefault_;  // immutable pre-parse snapshots
    std::vector<ChanLoc>     locsDefault_;
    std::atomic<bool>        metaReady_{false};   // labels_/units_ finalized -> lock-free ref
    std::string              error_;
};

// ---------------------------------------------------------------------------
// Marker / event stream source. Irregular string streams (LSL "Markers"): a worker
// pulls one sample at a time and appends a timestamped event (timestamp mapped to
// the local clock via time_correction, so events line up with the data plots). The
// render thread reads a snapshot to overlay vertical event lines on the time series.
// ---------------------------------------------------------------------------
class MarkerSource {
public:
    struct Event { double t; std::string text; std::uint64_t seq; };  // seq = stable ordinal

    explicit MarkerSource(const lsl::stream_info& info)
        : info_(info), name_(info.name()), type_(info.type()),
          uid_(info.uid()), sourceId_(info.source_id()),
          channels_(info.channel_count()) {}
    ~MarkerSource() { requestStop(); }

    void start() { worker_ = std::jthread([this](std::stop_token st) { run(st); }); }
    void requestStop() { if (worker_.joinable()) worker_.request_stop(); }
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    const std::string& name()     const { return name_; }
    const std::string& type()     const { return type_; }
    const std::string& uid()      const { return uid_; }
    const std::string& sourceId() const { return sourceId_; }
    int                channels() const { return channels_; }

    std::size_t count() const { std::lock_guard<std::mutex> lk(mtx_); return total_; }

    // Events with display-time >= tmin (events_ is time-ordered). Bounds the per-frame
    // copy for long recordings — callers pass the oldest visible time.
    std::vector<Event> eventsSince(double tmin) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::lower_bound(events_.begin(), events_.end(), tmin,
                                   [](const Event& e, double t) { return e.t < t; });
        return std::vector<Event>(it, events_.end());
    }

    // A cached, ~64 s-trimmed view rebuilt ONLY when a new event has arrived (events_
    // grows monotonically); otherwise a lock + size check. The render thread reads it
    // by const-ref every frame instead of copying every event (+ its std::string) per
    // marker stream and per ERP. Main-thread only (cached_ isn't touched by worker).
    const std::vector<Event>& cachedEvents() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (total_ != cachedSeen_) {   // total_ is monotonic (events_ is pruned, so size isn't)
            cachedSeen_ = total_;
            const double tmin = (events_.empty() ? 0.0 : events_.back().t) - 64.0;
            auto it = std::lower_bound(events_.begin(), events_.end(), tmin,
                                       [](const Event& e, double t) { return e.t < t; });
            cached_.assign(it, events_.end());
        }
        return cached_;
    }

    // Recent rate (events/s over the last `window` s) — more useful than a raw count
    // for a long session; 0 once events stop arriving.
    double rate(double window = 5.0) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const double cutoff = lsl::local_clock() - window;
        std::size_t cnt = 0;
        for (auto it = events_.rbegin(); it != events_.rend() && it->t >= cutoff; ++it) ++cnt;
        return (double)cnt / window;
    }
    std::string        error()  const { std::lock_guard<std::mutex> lk(mtx_); return error_; }
    double staleSeconds() const {
        const double t = lastData_.load(std::memory_order_relaxed);
        return t == 0.0 ? 0.0 : (lsl::local_clock() - t);
    }

private:
    void run(std::stop_token st) {
        struct Done { std::atomic<bool>& f; ~Done() { f.store(true, std::memory_order_release); } }
            _done{finished_};   // reaped off the UI thread once set (see HfStreamSource::run)
        try {
            lsl::stream_inlet inlet(info_, /*max_buflen*/ 360, /*max_chunklen*/ 0,
                                    /*recover*/ true);
            inlet.open_stream();
            double offset   = inlet.time_correction(2.0);   // remote -> local clock
            double lastCorr = lsl::local_clock();
            std::vector<std::string> sample;

            while (!st.stop_requested()) {
                double ts = 0.0;
                try { ts = inlet.pull_sample(sample, 0.2); }
                catch (const std::exception&) { continue; }   // recover reconnects the pull
                const double now = lsl::local_clock();
                if (now - lastCorr > 5.0) {                    // refresh clock offset
                    try { offset = inlet.time_correction(); } catch (...) {}
                    lastCorr = now;
                }
                if (ts == 0.0 || sample.empty()) continue;
                lastData_.store(now, std::memory_order_relaxed);

                std::string text = sample[0];                 // join multi-channel markers
                for (std::size_t c = 1; c < sample.size(); ++c)
                    if (!sample[c].empty()) text += " | " + sample[c];

                std::lock_guard<std::mutex> lk(mtx_);
                events_.push_back({ts + offset, std::move(text), total_});  // seq = ordinal
                ++total_;
                if (events_.size() > kMaxEvents)              // keep memory bounded
                    events_.erase(events_.begin(),
                                  events_.begin() + (events_.size() - kMaxEvents));
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mtx_); error_ = e.what();
        }
    }

    static constexpr std::size_t kMaxEvents = 20000;

    lsl::stream_info info_;
    std::string      name_, type_, uid_, sourceId_;
    int              channels_ = 0;

    mutable std::mutex  mtx_;            // guards events_, total_, error_
    std::vector<Event>  events_;
    std::vector<Event>  cached_;         // render-thread snapshot (see cachedEvents)
    std::size_t         cachedSeen_ = (std::size_t)-1;  // total_ when cached_ was built
    std::size_t         total_ = 0;     // lifetime count (events_ is pruned)
    std::string         error_;
    std::atomic<double> lastData_{0.0};
    std::jthread        worker_;
    std::atomic<bool>   finished_{false};   // worker exited -> safe to reap (instant join)
};

// ---------------------------------------------------------------------------
// Live stream discovery via lsl::continuous_resolver: a background resolver
// (started in the ctor) keeps a current set of visible streams, dropping any not
// seen for `forget_after` seconds. snapshot() just reads that set — no explicit
// refresh, so streams appear and disappear on their own.
// ---------------------------------------------------------------------------
class Discovery {
public:
    Discovery() : resolver_(5.0) {}   // forget streams unseen for 5 s
    std::vector<lsl::stream_info> snapshot() { return resolver_.results(); }

private:
    lsl::continuous_resolver resolver_;
};
