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
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

        // One min/max level sized for the most zoomed-out window: pick B so the
        // full window is ~kTargetBins bins (≈ 1 bin/pixel at max zoom-out).
        bin_ = std::max<int>(1, (int)(rate * kMaxHistory / kTargetBins));
        const std::size_t windowBins = (std::size_t)(rate * kMaxHistory / bin_) + 1;
        summary_.init(channels_, bin_, windowBins + 256);  // +guard +slack
        summaryHp_.init(channels_, bin_, windowBins + 256); // high-pass filtered

        hp_.init(channels_, 0.999f);   // running high-pass; R set per chunk
        setHighpassHz(0.5);            // default 0.5 Hz display high-pass

        // Default channel labels; refined from desc() XML once connected.
        labels_.resize(channels_);
        for (int c = 0; c < channels_; ++c)
            labels_[c] = "ch " + std::to_string(c);
        units_.assign(channels_, "");

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

    // --- live state (lock-free reads from the render thread) -----------------
    InterleavedRing&     ring()       { return ring_; }
    MinMaxSummary&       summary()    { return summary_; }    // raw envelope
    MinMaxSummary&       summaryHp()  { return summaryHp_; }  // high-pass filtered
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
        double off = 0.0;
        for (auto& g : gaps_) if (g.first <= oldT) off += g.second;
        return oldT + off;
    }
    // In-place old-frame -> real time for a monotonically increasing array.
    void applyGaps(float* x, int n) const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        if (gaps_.empty()) return;
        double off = 0.0; std::size_t gi = 0;
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
    float hpR() const { return hpR_.load(std::memory_order_relaxed); }  // for on-the-fly filtering

    // Snapshot of channel labels / units (written once on connect, under a lock).
    std::vector<std::string> labels() {
        std::lock_guard<std::mutex> lk(mtx_);
        return labels_;
    }
    std::vector<std::string> units() {
        std::lock_guard<std::mutex> lk(mtx_);
        return units_;
    }
    std::string error() {
        std::lock_guard<std::mutex> lk(mtx_);
        return error_;
    }

private:
    double effectiveRate() const { return srate_ > 0.0 ? srate_ : 100.0; }

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
            double lastCorr = lsl::local_clock();

            std::vector<float>  buf(chunkSamples_ * (std::size_t)channels_);
            std::vector<double> ts(chunkSamples_);
            std::vector<float>  hpBuf(chunkSamples_ * (std::size_t)channels_);

            while (!st.stop_requested()) {
                const std::size_t got = inlet.pull_chunk_multiplexed(
                    buf.data(), ts.data(), buf.size(), ts.size(), 0.1);
                if (got == 0) continue;
                const std::size_t n = got / (std::size_t)channels_;
                if (n == 0) continue;
                lastData_.store(lsl::local_clock(), std::memory_order_relaxed);

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
                            std::lock_guard<std::mutex> lk(gapMtx_);
                            gaps_.push_back({oldTime, g});
                        }
                    }
                    lastTs_ = ts[n - 1];
                    lastTsValid_ = true;
                }

                // Publish: head advances now, with the gap (if any) already recorded.
                ring_.write(buf.data(), n);        // one memcpy
                summary_.append(buf.data(), n);    // fold, cache-hot
                hp_.setR(hpR_.load(std::memory_order_relaxed));   // running high-pass
                hp_.process(buf.data(), hpBuf.data(), n);
                summaryHp_.append(hpBuf.data(), n);

                const double now = lsl::local_clock();
                if (now - lastCorr > 5.0) {        // refresh for cross-stream align
                    // A transient timeout here (stream briefly down) must NOT kill the
                    // worker — recover=true on the inlet reconnects the pull on its own.
                    try { offset = inlet.time_correction(1.0); }
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
            std::vector<float> out, hpOut;
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
                    hp_.setR(hpR_.load(std::memory_order_relaxed));
                    hpOut.resize(batch * (std::size_t)channels_);
                    hp_.process(out.data(), hpOut.data(), batch);
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
        lsl::xml_element ch = full.desc().child("channels").child("channel");
        std::vector<std::string> plabels, punits;
        for (; !ch.empty(); ch = ch.next_sibling("channel")) {
            const char* label = ch.child_value("label");
            const char* unit  = ch.child_value("unit");
            plabels.emplace_back(label ? label : "");
            punits.emplace_back(unit ? unit : "");
        }
        std::lock_guard<std::mutex> lk(mtx_);
        for (int c = 0; c < channels_ && c < (int)plabels.size(); ++c) {
            if (!plabels[c].empty()) labels_[c] = plabels[c];
            if (c < (int)punits.size() && !punits[c].empty()) units_[c] = punits[c];
        }
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

    InterleavedRing  ring_;
    MinMaxSummary    summary_;     // raw
    MinMaxSummary    summaryHp_;   // high-pass filtered

    // Producer-only DC blocker; cutoff R is read atomically (UI may change).
    DcBlocker           hp_;
    std::atomic<float>  hpR_{0.999f};

    // Render-thread-only display state (one plot per source).
    std::vector<float>  chanGain_, chanAmp_;

    std::atomic<bool>   anchored_{false};
    std::atomic<double> t0_{0.0};
    std::atomic<double> lastData_{0.0};   // local_clock of the last chunk received

    // Dropout tracking. gaps_ is producer-appended / render-read under gapMtx_;
    // lastTs_/lastTsValid_ are producer-only.
    mutable std::mutex                          gapMtx_;
    std::vector<std::pair<double, double>>      gaps_;   // (old-frame time, seconds)
    double                                      lastTs_ = 0.0;
    bool                                        lastTsValid_ = false;

    std::jthread        worker_;
    std::atomic<bool>   finished_{false};   // worker has exited -> safe to reap (instant join)

    std::mutex               mtx_;     // guards labels_, units_, error_
    std::vector<std::string> labels_, units_;
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
