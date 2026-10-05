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
#include "profiler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include "thread_compat.hpp"   // jthread / stop_token (with an Apple-libc++ polyfill)
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
        carIncludeAll();                                        // CAR over all channels until types known

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
        worker_ = jthread([this](stop_token st) { run(st); });
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
    const lsl::stream_info& info() const { return info_; }      // for reclassify (rebuild as a MarkerSource)
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
    // High-pass is one STAGE of the conditioned chain (re-reference -> high-pass -> notch
    // -> low-pass) and can be bypassed independently, so you can run e.g. a notch alone.
    void setHighpass(bool on) { hpOn_.store(on, std::memory_order_relaxed); }
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
    // While paused (frozen) these return the snapshot so the view survives the live ring
    // overwriting the paused span; the producer keeps writing the live ring_ underneath.
    InterleavedRing&     ring()       { return frozen() ? snapRing_     : ring_; }     // raw full-rate
    InterleavedRing&     ringHp()     { return frozen() ? snapRingHp_   : ringHp_; }   // filter-chain (FFT/spectro/zoom)
    MinMaxSummary&       summary()    { return frozen() ? snapSummary_  : summary_; }  // raw envelope
    MinMaxSummary&       summaryHp()  { return frozen() ? snapSummaryHp_: summaryHp_; }// filtered envelope

    // Pause snapshot control (render thread). freeze() copies the rings + summaries; snapHead()
    // is the frozen head to anchor reads on (use it for the paused window's end index).
    bool          frozen()  const { return frozen_.load(std::memory_order_acquire); }
    std::uint64_t snapHead() const { return snapHead_; }
    void freeze() {
        snapRing_.snapshotFrom(ring_);       snapRingHp_.snapshotFrom(ringHp_);
        snapSummary_.snapshotFrom(summary_); snapSummaryHp_.snapshotFrom(summaryHp_);
        snapHead_ = snapRing_.head();        // the head captured into the snapshot
        frozen_.store(true, std::memory_order_release);
    }
    void unfreeze() { frozen_.store(false, std::memory_order_release); }
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
    // Inverse of realTime: map a real (wall-clock) time back to the index-derived
    // "old-frame" time, subtracting every dropout that precedes it. Needed to turn a
    // marker's real timestamp into a sample index (idx = (oldFrameTime(T) - t0) / dt).
    // A T landing inside a dropout interval maps to the resumption boundary.
    double oldFrameTime(double realT) const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        double off = gapBase_;
        for (auto& g : gaps_) {
            // Real-time position of this gap's far edge = g.first + off + g.second.
            if (realT >= g.first + off + g.second) { off += g.second; continue; }
            // Inside the dropout no sample exists, so clamp to the first one after it.
            // Subtracting only the earlier gaps would land up to one gap length past it, in
            // data recorded later: a marker a microsecond before the resumption would be
            // matched to samples taken seconds after it.
            if (realT > g.first + off) return g.first;
            break;
        }
        return realT - off;
    }
    // In-place old-frame -> real time for a monotonically increasing array.
    void applyGaps(double* x, int n) const {
        std::lock_guard<std::mutex> lk(gapMtx_);
        double off = gapBase_; std::size_t gi = 0;
        if (gaps_.empty() && off == 0.0) return;
        for (int i = 0; i < n; ++i) {
            while (gi < gaps_.size() && gaps_[gi].first <= x[i]) { off += gaps_[gi].second; ++gi; }
            x[i] += off;
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

    // --- replay breaks ---------------------------------------------------------
    // A replay's seek or loop wrap joins two parts of a recording that were not adjacent,
    // and its stamps go on without a gap (see XdfPlayer::breaks). Without a mark the plot
    // would show the join as continuous data; and a dropout check against the previous
    // sample would paint the time between the two parts red, although nothing is missing
    // from the recording. The worker places each break at the first sample whose stamp
    // reaches `stamp` and checks for missing data against the break stamp instead.
    struct BreakMark { double t; char label[24]; };   // t: display time (see breakTimes)
    void markBreak(double stamp, const char* label) {   // render thread
        if (irregular_) return;   // resampled on the local clock: no stamps to place it by
        std::lock_guard<std::mutex> lk(brkMtx_);
        if (brkPending_.size() >= 64) brkPending_.erase(brkPending_.begin());   // no data for a long time
        PendingBreak b{stamp, {}};
        std::snprintf(b.label, sizeof(b.label), "%s", label);
        brkPending_.push_back(b);
        brkFlag_.store(true, std::memory_order_release);
    }
    // A recorded dropout or a placed replay break between absolute samples i0 and i1 (the
    // window [i0, i1)). The ring is indexed without the gaps, so a run of samples read across
    // one joins data that was not adjacent in time: an ERP epoch cut there would fold
    // misaligned data into the average. Both lists keep the old-frame time of the first
    // sample after the discontinuity, t0 + idx * dt, so the window's interior is (i0, i1).
    bool discontinuityIn(std::uint64_t i0, std::uint64_t i1) const {
        const double t0 = t0_.load(std::memory_order_acquire);
        const double a  = t0 + ((double)i0 + 0.5) * dt_, b = t0 + ((double)i1 - 0.5) * dt_;
        std::lock_guard<std::mutex> lk(gapMtx_);
        for (const auto& g : gaps_)   if (g.first > a && g.first < b) return true;
        for (const auto& m : breaks_) if (m.t > a && m.t < b)         return true;
        return false;
    }
    // A break was posted and no data at or after it has arrived yet.
    bool breakPending() const { return brkFlag_.load(std::memory_order_acquire); }
    // Placed breaks in display time, oldest first. `out` is reused (no allocation once grown).
    void breakTimes(std::vector<BreakMark>& out) const {
        out.clear();
        std::lock_guard<std::mutex> lk(gapMtx_);
        double off = gapBase_;
        std::size_t gi = 0;
        // Strictly earlier gaps only: a seek into a part with no data records its dropout at
        // the same index, and the mark belongs at the start of that red band, not its end.
        for (const BreakMark& b : breaks_) {
            while (gi < gaps_.size() && gaps_[gi].first < b.t) off += gaps_[gi++].second;
            out.push_back(b);
            out.back().t = b.t + off;
        }
    }

    // --- stream health (lock-free reads) -------------------------------------
    // Samples per second of the stream's own timestamps, over the last ~5 s (see the worker).
    double        measuredRate() const { return shownRate_.load(std::memory_order_relaxed); }
    // Timestamp seconds delivered per wall second over the same window: about 1 live, the
    // speed during a replay that keeps the recorded spacing.
    double        deliverySpeed() const { return deliverySpeed_.load(std::memory_order_relaxed); }
    double        chunkSpan()    const { return chunkSpan_.load(std::memory_order_relaxed); }      // data s per delivery
    double        recentRate()   const { return recentRate_.load(std::memory_order_relaxed); }     // samples/s, last ~0.3 s
    // Wall seconds between deliveries (bursts): a high percentile of the recent ones, so a
    // stall does not count as an interval.
    double        arrivalGap()   const { return arrivalGap_.load(std::memory_order_relaxed); }

    // The data's trajectory: the newest data time (display time) as a function of the local
    // clock, from the delivery times, linearly interpolated between them and flat outside.
    // Averaged over [tA, tB]: `value`, and `slope`, the average's rate of change as the window
    // moves (data seconds per wall second). A plot edge that follows this a little in the past
    // moves exactly as the data arrived, speed changes included, with no speed estimate to wait
    // for; the averaging smooths delivery jitter and turns a change of speed into a short ramp.
    // Past the newest delivery, N goes on at the slope of the last `ahead` seconds for up to
    // `ahead` seconds, then stays flat. A delivery that comes late (a busy machine held the
    // transport for 100 ms) then fits the line the edge was already following. Flat at once,
    // the slope fell as the window ran past the last delivery and jumped back when the late
    // block arrived, and the edge lurched. False before the first delivery.
    bool trajectory(double tA, double tB, double& value, double& slope, double ahead = 0.0) const {
        std::lock_guard<std::mutex> lk(markMtx_);
        if (markCount_ == 0 || !(tB > tA)) return false;
        const std::size_t cap = marks_.size(), first = (markHead_ + cap - markCount_) % cap;
        auto at = [&](std::size_t i) -> const Mark& { return marks_[(first + i) % cap]; };
        const Mark& last = at(markCount_ - 1);
        double lastSlope = 0.0;
        if (ahead > 0.0 && markCount_ > 1) {
            std::size_t lo = 0, hi = markCount_ - 1;   // the newest knot at least `ahead` s older
            if (at(0).wall <= last.wall - ahead)
                while (hi - lo > 1) { const std::size_t mid = (lo + hi) / 2; (at(mid).wall <= last.wall - ahead ? lo : hi) = mid; }
            const Mark& ref = at(lo);
            if (last.wall > ref.wall) lastSlope = std::max(0.0, (last.newest - ref.newest) / (last.wall - ref.wall));
        }
        auto valueAt = [&](double t) {             // N(t)
            if (t <= at(0).wall) return at(0).newest;
            if (t >= last.wall) return last.newest + lastSlope * std::min(t - last.wall, ahead);
            std::size_t lo = 0, hi = markCount_ - 1;   // at(lo).wall <= t < at(hi).wall
            while (hi - lo > 1) { const std::size_t mid = (lo + hi) / 2; (at(mid).wall <= t ? lo : hi) = mid; }
            const Mark& a = at(lo); const Mark& b = at(hi);
            return a.newest + (b.newest - a.newest) * (t - a.wall) / (b.wall - a.wall);
        };
        // Integral of the piecewise-linear N over [tA, tB]: trapezoids between the knots inside.
        double area = 0.0, t = tA, v = valueAt(tA);
        for (std::size_t i = 0; i < markCount_; ++i) {
            const Mark& m = at(i);
            if (m.wall <= tA) continue;
            if (m.wall >= tB) break;
            area += 0.5 * (v + m.newest) * (m.wall - t);
            t = m.wall; v = m.newest;
        }
        const double kink = last.wall + ahead;   // where the extension turns flat
        if (lastSlope > 0.0 && kink > t && kink < tB) {
            const double vk = valueAt(kink);
            area += 0.5 * (v + vk) * (kink - t);
            t = kink; v = vk;
        }
        const double vB = valueAt(tB);
        area += 0.5 * (v + vB) * (tB - t);
        value = area / (tB - tA);
        slope = (vB - valueAt(tA)) / (tB - tA);
        return true;
    }
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
        } else {                                           // common-average reference (over EEG channels)
            const float inv = 1.0f / (float)std::max<std::size_t>(1, carIdx_.size());
            for (std::size_t i = 0; i < n; ++i) {
                const float* x = in + i * (std::size_t)C;
                float* o = scratch + i * (std::size_t)C;
                float sum = 0.0f;
                for (int k : carIdx_) sum += x[k];                          // average the good channels
                const float m = sum * inv;
                for (int c = 0; c < C; ++c) o[c] = x[c] - m;                // subtract from every channel
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
            notch_.setEnabled(non);                       // self-resets z-state on re-enable
            if (non) notch_.setNotch(nf, srate_, 30.0);   // narrow (Q=30): only the mains line
            notchApplied_ = non; notchHzApplied_ = nf;
        }
        const bool  lon = lpOn_.load(std::memory_order_relaxed);
        const float lf  = lpHz_.load(std::memory_order_relaxed);
        if (lon != lpApplied_ || lf != lpHzApplied_) {
            lp_.setEnabled(lon);                          // self-resets z-state on re-enable
            if (lon) lp_.setLowpass(lf, srate_, 0.70710678);  // Butterworth
            lpApplied_ = lon; lpHzApplied_ = lf;
        }
        if (notch_.enabled) notch_.process(x, n);
        if (lp_.enabled)    lp_.process(x, n);
    }

    // Run the conditioned chain (re-reference -> high-pass-or-bypass -> notch -> low-pass)
    // on one raw chunk and publish it. `refScratch`/`hpScratch` are caller-owned buffers
    // of at least n*channels_ floats. Writes the derived structures (ringHp_/summaryHp_/
    // summary_) FIRST, then publishes the raw ring's head LAST so ring_.head() — the
    // canonical head every reader bounds by, including filtered reads of ringHp_ — never
    // runs ahead of the filtered ring/summaries for the same chunk. Producer-only.
    void publishChunk(const float* raw, std::size_t n, float* refScratch, float* hpScratch) {
        LSL_ZONE("publish");
        const float* condIn = reference(raw, refScratch, n);   // re-reference (first stage)
        const bool hpOn = hpOn_.load(std::memory_order_relaxed);
        if (hpOn && !hpApplied_) hp_.reset();                  // re-enabled: re-prime clean
        hpApplied_ = hpOn;
        if (hpOn) { hp_.setR(hpR_.load(std::memory_order_relaxed)); hp_.process(condIn, hpScratch, n); }
        else      { std::copy(condIn, condIn + n * (std::size_t)channels_, hpScratch); }  // bypass HP
        applyPostFilters(hpScratch, n);                        // optional notch + low-pass
        ringHp_.write(hpScratch, n);  summaryHp_.append(hpScratch, n);
        summary_.append(raw, n);      ring_.write(raw, n);     // publish canonical head LAST
    }

    void run(stop_token st) {
        // Set on ANY exit (return/exception) so the owner can reap us off the UI
        // thread: it stops + drops the window now, then joins once finished() is true
        // (instant), instead of blocking ~0.5 s in join() while the worker winds down.
        struct Done { std::atomic<bool>& f; ~Done() { f.store(true, std::memory_order_release); } }
            _done{finished_};
        if (irregular_) { runIrregular(st); return; }
        try {
            lsl::stream_inlet inlet(info_, /*max_buflen*/ 360, /*max_chunklen*/ 0,
                                    /*recover*/ true);
            // open_stream()'s default timeout is infinite and it is only cancelable between
            // blocking calls, so a stream that vanished between resolve and connect would make
            // this worker unjoinable (hanging shutdown). Poll with a finite timeout instead.
            while (!st.stop_requested()) {
                try { inlet.open_stream(1.0); break; }
                catch (const lsl::timeout_error&) { /* keep retrying, but stay cancelable */ }
            }
            if (st.stop_requested()) { inlet.close_stream(); return; }

            // Full info (with desc()) only becomes available after the inlet
            // connects; the resolve result often lacks the channel descriptions.
            try { parseLabels(inlet.info(2.0)); } catch (...) { /* keep defaults */ }

            // A transient timeout on the initial correction must NOT kill the worker (the
            // periodic refresh below recovers it); wrap it like that refresh does.
            double offset = 0.0;
            try { offset = inlet.time_correction(2.0); } catch (const std::exception&) { /* 0 until refresh */ }
            clockOffset_.store(offset, std::memory_order_relaxed);
            double lastCorr = lsl::local_clock();

            std::vector<float>  buf(chunkSamples_ * (std::size_t)channels_);
            std::vector<double> ts(chunkSamples_);
            std::vector<float>  hpBuf(chunkSamples_ * (std::size_t)channels_);
            std::vector<float>  refBuf(chunkSamples_ * (std::size_t)channels_);  // re-reference scratch

            while (!st.stop_requested()) {
                // Block only for the first sample, then collect for a few ms. With a timeout,
                // pull_chunk waits until the buffer is full or the timeout has run out (see
                // liblsl's stream_inlet_impl::pull_chunk_multiplexed), so the 0.1 s timeout this
                // used delivered every stream in 100 ms batches: added latency, a scroll that
                // moved in steps, and rate and block-size estimates that measured the batching
                // instead of the sender. The short window bounds the pull rate for a sender that
                // pushes one sample at a time.
                constexpr double kCollect = 0.004;   // s
                const double first = inlet.pull_sample(buf.data(), channels_, 0.1);
                if (first == 0.0) continue;
                ts[0] = first;
                const std::size_t got = (std::size_t)channels_ + inlet.pull_chunk_multiplexed(
                    buf.data() + channels_, ts.data() + 1, buf.size() - (std::size_t)channels_,
                    ts.size() - 1, kCollect);
                const std::size_t n = got / (std::size_t)channels_;
                if (n == 0) continue;
                const double tnow = lsl::local_clock();
                lastData_.store(tnow, std::memory_order_relaxed);
                // Data seconds per delivery. A sender block larger than one pull arrives as
                // back-to-back pulls, so pulls a few ms apart count as one burst. The plot edge
                // trails the newest data by about this much; less, and it overtakes the data
                // between blocks and paints that wait as a dropout. Rises at once, decays over
                // ~10 s, so one large block holds the margin through the next few.
                if (tnow - lastChunkT_ > 0.005) burst_ = 0.0;
                burst_ += (double)n * dt_;
                {
                    const double prev = chunkSpan_.load(std::memory_order_relaxed);
                    const double decayed = lastChunkT_ > 0.0 ? prev * std::exp(-(tnow - lastChunkT_) / 10.0) : 0.0;
                    chunkSpan_.store(std::max(burst_, decayed), std::memory_order_relaxed);
                }
                // Samples per second over at least the last kRateWindow s: from this pull back to
                // the newest earlier pull that is that old. The plot edge glides at this rate, so it
                // must notice a slowdown (a replay going from 5x to 1x) within a fraction of a
                // second; a per-pull moving average took about half a second to get halfway.
                // A sender of large blocks spans whole blocks, so it reads steady, not 0 or a burst.
                arrivedTotal_ += n;
                arrivals_[arrivalHead_] = {tnow, arrivedTotal_};
                arrivalHead_ = (arrivalHead_ + 1) % arrivals_.size();
                arrivalCount_ = std::min(arrivalCount_ + 1, arrivals_.size());
                {
                    const Arrival* from = nullptr;
                    for (std::size_t k = 2; k <= arrivalCount_; ++k) {   // k = 1 is this pull
                        const Arrival& a = arrivals_[(arrivalHead_ + arrivals_.size() - k) % arrivals_.size()];
                        from = &a;
                        if (tnow - a.t >= kRateWindow) break;
                    }
                    if (from && tnow > from->t)
                        recentRate_.store((double)(arrivedTotal_ - from->total) / (tnow - from->t),
                                          std::memory_order_relaxed);
                }
                // The rate shown to the user (Info panel): samples per second of the stream's own
                // timestamps over the last ~kShownRateWindow s, refreshed 4 times a second. Its own
                // clock, so a replay shows its recorded rate at any speed, while missing samples
                // or a device off its declared rate still show. A long window, so 1 kHz reads as
                // a steady 1000.0 instead of the per-delivery jitter. The delivery speed (wall
                // clock against timestamps) is kept beside it for a replay's "5.00x".
                if (tnow - lastSnapT_ >= 0.25) {
                    lastSnapT_ = tnow;
                    rateSnaps_[rateSnapHead_] = {tnow, ts[n - 1], arrivedTotal_};
                    rateSnapHead_ = (rateSnapHead_ + 1) % rateSnaps_.size();
                    rateSnapCount_ = std::min(rateSnapCount_ + 1, rateSnaps_.size());
                    const RateSnap* old = nullptr;
                    for (std::size_t k = 2; k <= rateSnapCount_; ++k) {   // k = 1 is this snapshot
                        old = &rateSnaps_[(rateSnapHead_ + rateSnaps_.size() - k) % rateSnaps_.size()];
                        if (tnow - old->wall >= kShownRateWindow) break;
                    }
                    if (old && arrivedTotal_ > old->total) {
                        const double samples = (double)(arrivedTotal_ - old->total);
                        const double spanTs = ts[n - 1] - old->lastTs, spanWall = tnow - old->wall;
                        if (spanTs > 0.0) shownRate_.store(samples / spanTs, std::memory_order_relaxed);
                        if (spanTs > 0.0 && spanWall > 0.0)
                            deliverySpeed_.store(spanTs / spanWall, std::memory_order_relaxed);
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
                    brkHere_.clear();
                    if (brkFlag_.load(std::memory_order_acquire)) placeBreaks(ts.data(), n, headBefore);
                    std::size_t bk = 0;
                    // After a break, the samples before it are from another part of the
                    // recording: data is missing only if the first sample comes later than
                    // the break stamp, the start of the new part.
                    // Before the first sample there is nothing to compare: an inlet that
                    // subscribes late misses what was sent before, and that is not missing data.
                    const bool brk0 = !brkHere_.empty() && brkHere_[0].first == 0;
                    const double expect = brk0 ? brkHere_[bk++].second : lastTs_ + dt_;
                    if (lastTsValid_) {
                        const double g = ts[0] - expect;
                        if (g > 0.25) recordGap(headBefore, g);   // s; above jitter, below real dropouts
                    }
                    // Within the pull as well: a pull that spans a dropout (the viewer fell
                    // behind by more than the gap, or the sender pushed both sides at once) has
                    // the jump between two of its samples, and checking only its first sample
                    // would join the two sides as if no time had passed. One pass in sample
                    // order keeps gaps_ sorted; at a replay break the new part is measured from
                    // the break stamp, not from the sample before it.
                    for (std::size_t i = 1; i < n; ++i) {
                        double g;
                        if (bk < brkHere_.size() && brkHere_[bk].first == i) g = ts[i] - brkHere_[bk++].second;
                        else                                                  g = ts[i] - (ts[i - 1] + dt_);
                        if (g > 0.25) recordGap(headBefore + i, g);
                    }
                    lastTs_ = ts[n - 1];
                    lastTsValid_ = true;
                }

                // Conditioned chain + publish (derived structures first, raw head last).
                publishChunk(buf.data(), n, refBuf.data(), hpBuf.data());

                // Record the delivery for trajectory(). Pulls a few ms apart are one delivery (a
                // block split across pulls): update its knot rather than add a near-vertical one.
                if (dt_ > 0.0 && anchored_.load(std::memory_order_relaxed)) {
                    const double nw = newestTime();
                    std::lock_guard<std::mutex> lk(markMtx_);
                    const std::size_t cap = marks_.size();
                    if (markCount_ > 0 && tnow - lastPullT_ <= 0.005) {
                        marks_[(markHead_ + cap - 1) % cap] = {tnow, nw};
                        if (gapCount_ == 0) arrivalGap_.store(burst_, std::memory_order_relaxed);   // first delivery, still arriving
                    } else {
                        if (markCount_ > 0) {
                            // The 80th percentile of the last gaps, not a running max: a stall is not
                            // a delivery interval, and counting one stretched the edge's delay to
                            // several seconds, freezing the scroll until it caught up.
                            burstGaps_[gapHead_] = tnow - lastPullT_;
                            gapHead_ = (gapHead_ + 1) % burstGaps_.size();
                            gapCount_ = std::min(gapCount_ + 1, burstGaps_.size());
                            std::array<double, 16> g{};
                            std::copy_n(burstGaps_.begin(), gapCount_, g.begin());
                            // (count - 1): with few gaps, a plain 80% index would pick the largest.
                            const std::size_t k = ((gapCount_ - 1) * 8) / 10;
                            std::nth_element(g.begin(), g.begin() + (std::ptrdiff_t)k, g.begin() + (std::ptrdiff_t)gapCount_);
                            arrivalGap_.store(g[k], std::memory_order_relaxed);
                        }
                        // Before any gap is known, guess one from the first delivery: at 1x a block
                        // of data arrives about as often as it lasts.
                        if (markCount_ == 0) arrivalGap_.store(burst_, std::memory_order_relaxed);
                        marks_[markHead_] = {tnow, nw};
                        markHead_ = (markHead_ + 1) % cap;
                        markCount_ = std::min(markCount_ + 1, cap);
                    }
                    lastPullT_ = tnow;
                }

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
    void runIrregular(stop_token st) {
        try {
            lsl::stream_inlet inlet(info_, /*max_buflen*/ 360, /*max_chunklen*/ 0,
                                    /*recover*/ true);
            // Finite-timeout poll so a vanished stream can't hang shutdown (see run()).
            while (!st.stop_requested()) {
                try { inlet.open_stream(1.0); break; }
                catch (const lsl::timeout_error&) { /* keep retrying, stay cancelable */ }
            }
            if (st.stop_requested()) { inlet.close_stream(); return; }
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
                    refOut.resize(batch * (std::size_t)channels_);
                    hpOut.resize(batch * (std::size_t)channels_);
                    publishChunk(out.data(), batch, refOut.data(), hpOut.data());
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

        // CAR good-channel list (producer-only): average only EEG channels, so EOG/EMG/etc.
        // don't pollute the common average. Type match is trimmed + case-insensitive (LSL
        // sources vary: "EEG", "eeg", " EEG "...). Unknown/blank type counts as EEG (so a
        // stream with no per-channel types behaves as before = CAR over all). If nothing
        // qualifies, fall back to all channels.
        auto isEeg = [](std::string t) {
            const std::size_t a = t.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return true;            // blank = unknown = include
            const std::size_t b = t.find_last_not_of(" \t\r\n");
            t = t.substr(a, b - a + 1);
            for (char& ch : t) ch = (char)std::toupper((unsigned char)ch);
            return t == "EEG";
        };
        carIdx_.clear();
        for (int c = 0; c < channels_; ++c) if (isEeg(types_[c])) carIdx_.push_back(c);
        if (carIdx_.empty()) carIncludeAll();
    }

    // Record a dropout of `g` seconds before absolute sample `idx`. Producer-only.
    void recordGap(std::uint64_t idx, double g) {
        const double oldTime = t0_.load(std::memory_order_acquire) + (double)idx * dt_;
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
        // breakTimes counts only the gaps still listed; a break before a folded gap would
        // be placed without it.
        std::erase_if(breaks_, [&](const BreakMark& b) { return b.t < cutoff; });
    }

    // Moves the pending breaks this chunk reaches into breaks_ (old-frame time) and lists
    // them in brkHere_ as (sample index in the chunk, break stamp). Breaks that land on
    // the same sample (two seeks with no data between) collapse into the last one: only
    // it says where the data now comes from. Producer-only.
    void placeBreaks(const double* ts, std::size_t n, std::uint64_t headBefore) {
        std::lock_guard<std::mutex> lk(brkMtx_);
        std::size_t used = 0, i = 0;
        for (; used < brkPending_.size(); ++used) {
            const PendingBreak& p = brkPending_[used];
            while (i < n && ts[i] < p.stamp) ++i;
            if (i == n) break;                                   // a later chunk reaches it
            const double oldT = t0_.load(std::memory_order_acquire) + (double)(headBefore + i) * dt_;
            std::lock_guard<std::mutex> gl(gapMtx_);
            if (!brkHere_.empty() && brkHere_.back().first == i) {
                brkHere_.back().second = p.stamp;
                std::snprintf(breaks_.back().label, sizeof(breaks_.back().label), "%s", p.label);
                continue;
            }
            brkHere_.push_back({i, p.stamp});
            BreakMark b{oldT, {}};
            std::snprintf(b.label, sizeof(b.label), "%s", p.label);
            breaks_.push_back(b);
            std::erase_if(breaks_, [&](const BreakMark& m) { return m.t < oldT - 120.0; });
        }
        brkPending_.erase(brkPending_.begin(), brkPending_.begin() + (std::ptrdiff_t)used);
        brkFlag_.store(!brkPending_.empty(), std::memory_order_release);
    }

    // CAR over every channel (the default before types are known, and the fallback when
    // no channel qualifies as EEG).
    void carIncludeAll() { carIdx_.resize(channels_); std::iota(carIdx_.begin(), carIdx_.end(), 0); }

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

    // Pause snapshot: a frozen copy of the rings + summaries, captured on pause. While frozen
    // the read accessors below hand out these copies, so the paused plots stay put even after
    // the producer (which keeps running) overwrites that span of the live ring.
    InterleavedRing   snapRing_, snapRingHp_;
    MinMaxSummary     snapSummary_, snapSummaryHp_;
    std::atomic<bool> frozen_{false};
    std::uint64_t     snapHead_ = 0;

    // Producer-only display filter chain: DC blocker (high-pass) -> notch -> low-pass.
    // Cutoffs/enables are read atomically (UI may change); biquad state is producer-only.
    DcBlocker           hp_;
    std::atomic<float>  hpR_{0.999f};
    std::atomic<bool>   hpOn_{true};   // high-pass stage enabled (else bypassed)
    Biquad              notch_, lp_;
    std::atomic<bool>   notchOn_{false}, lpOn_{false};
    std::atomic<float>  notchHz_{60.0f}, lpHz_{40.0f};
    std::atomic<int>    refMode_{0}, refChan_{0};   // 0 none / 1 CAR / 2 single-channel
    // CAR good-channel index list: which channels join the common average. Producer-only
    // (built in parseLabels, read in reference() — both on the worker thread). Excludes
    // non-EEG channels (EOG/EMG/ECG/triggers) so they don't pollute the average. Stored as
    // an index list so the inner sum has no per-sample branch.
    std::vector<int>    carIdx_;
    // last-applied params, so the producer only recomputes coeffs on change
    bool                notchApplied_ = false, lpApplied_ = false, hpApplied_ = true;
    float               notchHzApplied_ = -1.0f, lpHzApplied_ = -1.0f;

    // Render-thread-only display state (one plot per source).
    std::vector<float>  chanGain_, chanAmp_;

    std::atomic<bool>   anchored_{false};
    std::atomic<double> t0_{0.0};
    std::atomic<double> lastData_{0.0};   // local_clock of the last chunk received

    // Stream health (producer-written, render-read).
    // For measuredRate(): a snapshot every 0.25 s (producer-only), a fixed ring of ~8 s.
    static constexpr double    kShownRateWindow = 5.0;   // s
    struct RateSnap { double wall = 0.0, lastTs = 0.0; std::uint64_t total = 0; };
    std::array<RateSnap, 32>   rateSnaps_{};
    std::size_t                rateSnapHead_ = 0, rateSnapCount_ = 0;
    double                     lastSnapT_ = 0.0;
    std::atomic<double>        shownRate_{0.0};
    std::atomic<double>        deliverySpeed_{0.0};
    std::atomic<double>        clockOffset_{0.0};    // last remote->local time_correction (s)
    std::atomic<std::uint64_t> dropouts_{0};         // count of recorded gaps
    double                     lastChunkT_ = 0.0;    // producer-only: local time of previous chunk
    double                     burst_ = 0.0;         // producer-only: data seconds in the current burst
    std::atomic<double>        chunkSpan_{0.0};      // decaying max of data seconds per burst
    // Recent pulls for recentRate_ (producer-only), a fixed ring: no allocation per pull.
    static constexpr double    kRateWindow = 0.3;    // s
    struct Arrival { double t = 0.0; std::uint64_t total = 0; };
    std::array<Arrival, 64>    arrivals_{};
    std::size_t                arrivalHead_ = 0, arrivalCount_ = 0;
    std::uint64_t              arrivedTotal_ = 0;
    std::atomic<double>        recentRate_{0.0};     // samples/s over the last >= kRateWindow s
    // Deliveries for trajectory(): (local clock, newest data time after it). A fixed ring that
    // covers a few seconds at the highest pull rate the 4 ms collection window allows.
    struct Mark { double wall = 0.0, newest = 0.0; };
    std::array<Mark, 1024>     marks_{};
    std::size_t                markHead_ = 0, markCount_ = 0;
    mutable std::mutex         markMtx_;
    double                     lastPullT_ = 0.0;    // producer-only: local clock at the last pull
    std::array<double, 16>     burstGaps_{};         // producer-only: recent wall gaps between bursts
    std::size_t                gapHead_ = 0, gapCount_ = 0;
    std::atomic<double>        arrivalGap_{0.0};     // their 80th percentile

    // Dropout tracking. gaps_ is producer-appended / render-read under gapMtx_;
    // lastTs_/lastTsValid_ are producer-only.
    mutable std::mutex                          gapMtx_;
    std::vector<std::pair<double, double>>      gaps_;   // (old-frame time, seconds), recent only
    double                                      gapBase_ = 0.0;   // folded offset of pruned gaps
    double                                      lastTs_ = 0.0;
    bool                                        lastTsValid_ = false;
    std::vector<BreakMark>                      breaks_;   // placed replay breaks (old-frame time), under gapMtx_

    // Replay breaks posted by the render thread, not yet reached by the data.
    struct PendingBreak { double stamp; char label[24]; };
    std::mutex                                  brkMtx_;
    std::vector<PendingBreak>                   brkPending_;
    std::atomic<bool>                           brkFlag_{false};   // brkPending_ is not empty
    std::vector<std::pair<std::size_t, double>> brkHere_;          // producer-only scratch, see placeBreaks

    jthread        worker_;
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

    void start() { worker_ = jthread([this](stop_token st) { run(st); }); }
    void requestStop() { if (worker_.joinable()) worker_.request_stop(); }
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    const std::string& name()     const { return name_; }
    const std::string& type()     const { return type_; }
    const std::string& uid()      const { return uid_; }
    const std::string& sourceId() const { return sourceId_; }
    const lsl::stream_info& info() const { return info_; }      // for reclassify (rebuild as an HfStreamSource)
    int                channels() const { return channels_; }

    std::size_t count() const { std::lock_guard<std::mutex> lk(mtx_); return total_ - cleared_; }
    double firstTime() const { std::lock_guard<std::mutex> lk(mtx_); return firstT_; }  // 1st event since the last clear (relative-time zero)

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

    // The last up to `n` events, cached (rebuilt only when a new event arrives). For the
    // standalone marker-event log, which wants a fixed line count regardless of rate, unlike
    // cachedEvents()'s 64 s window. Main-thread only.
    const std::vector<Event>& tailCached(std::size_t n) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (total_ != tailSeen_ || tailN_ != n) {
            tailSeen_ = total_; tailN_ = n;
            const std::size_t start = events_.size() > n ? events_.size() - n : 0;
            tail_.assign(events_.begin() + start, events_.end());
        }
        return tail_;
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
    // Drop every buffered event. Display only: the recorder pulls its own inlets, so a
    // clear never touches what is being written to disk. INVALIDATES every reference
    // returned by cachedEvents() / tailCached() and every pointer into their elements,
    // so callers must not clear while a frame still holds one (main.cpp defers the log's
    // Clear buttons to after the window's End()).
    //
    // total_ stays monotonic: it is the source of Event::seq, which the ERP epoch
    // accumulator uses to skip events it already consumed, so rewinding it would replay
    // old triggers.
    //
    // `rebaseTime` restarts the relative-time origin (firstT_) on the next event. Only
    // valid when EVERY marker stream is cleared together: the log's relative-time origin
    // is shared (the earliest firstTime() across streams), so re-basing one stream alone
    // would shift the displayed times of the logs that kept their events.
    void clear(bool rebaseTime = false) {
        std::lock_guard<std::mutex> lk(mtx_);
        events_.clear();
        cleared_ = total_;
        if (rebaseTime) firstT_ = 0.0;
        cached_.clear();  cachedSeen_ = total_;   // accurate for the (now empty) events_
        tail_.clear();    tailSeen_   = total_;
    }

    std::string        error()  const { std::lock_guard<std::mutex> lk(mtx_); return error_; }
    double staleSeconds() const {
        const double t = lastData_.load(std::memory_order_relaxed);
        return t == 0.0 ? 0.0 : (lsl::local_clock() - t);
    }

private:
    void run(stop_token st) {
        struct Done { std::atomic<bool>& f; ~Done() { f.store(true, std::memory_order_release); } }
            _done{finished_};   // reaped off the UI thread once set (see HfStreamSource::run)
        try {
            lsl::stream_inlet inlet(info_, /*max_buflen*/ 360, /*max_chunklen*/ 0,
                                    /*recover*/ true);
            // Finite-timeout poll so a vanished marker stream can't hang shutdown (see
            // HfStreamSource::run()).
            while (!st.stop_requested()) {
                try { inlet.open_stream(1.0); break; }
                catch (const lsl::timeout_error&) { /* keep retrying, stay cancelable */ }
            }
            if (st.stop_requested()) { inlet.close_stream(); return; }
            // A transient timeout on the initial correction must not kill the worker.
            double offset = 0.0;
            try { offset = inlet.time_correction(2.0); } catch (const std::exception&) { /* 0 until refresh */ }
            double lastCorr = lsl::local_clock();
            std::vector<std::string> sample;

            while (!st.stop_requested()) {
                double ts = 0.0;
                try { ts = inlet.pull_sample(sample, 0.2); }
                catch (const std::exception&) { continue; }   // recover reconnects the pull
                const double now = lsl::local_clock();
                if (now - lastCorr > 5.0) {                    // refresh clock offset
                    try { offset = inlet.time_correction(1.0); } catch (...) {}
                    lastCorr = now;
                }
                if (ts == 0.0 || sample.empty()) continue;
                lastData_.store(now, std::memory_order_relaxed);

                // Join multi-channel markers with ", " — deliberately NOT " | ", which is the ERP
                // label-filter's OR separator (erpLabelMatches); using it here would split a joined
                // event's text and make multi-channel marker labels unfilterable.
                std::string text = sample[0];
                for (std::size_t c = 1; c < sample.size(); ++c)
                    if (!sample[c].empty()) text += ", " + sample[c];

                std::lock_guard<std::mutex> lk(mtx_);
                if (firstT_ == 0.0) firstT_ = ts + offset;   // stable zero for relative-time display (also after a clear)
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
    std::vector<Event>  tail_;           // render-thread snapshot (see tailCached)
    std::size_t         tailSeen_ = (std::size_t)-1, tailN_ = 0;  // total_/n when tail_ was built
    std::size_t         total_ = 0;     // lifetime count (events_ is pruned); monotonic -> Event::seq
    std::size_t         cleared_ = 0;   // total_ at the last clear(); count() reports events since then
    double              firstT_ = 0.0;  // display time of the first event since a rebasing clear (relative-time zero)
    std::string         error_;
    std::atomic<double> lastData_{0.0};
    jthread        worker_;
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
