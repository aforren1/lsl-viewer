#pragma once
// Drift-free time mapping for regular streams. No LSL dependency, so tests can feed it
// synthetic timestamps.
//
// A regular stream places sample i at the "old-frame" time x = t0 + i * dt, exactly linear in
// the index; gaps, replay breaks and the spectrogram's gap lookup are keyed in it. The display
// time adds two terms:
//
//   display(x) = x + G(x) + D(x)
//
// G is the step function of recorded dropouts (each adds its length to every later time). D is
// a continuous piecewise-linear drift term, D(x) = d_k + s_k (x - x_k) on [x_k, x_{k+1}), that
// takes up the device's rate error and the drift of the clock offset, which a fixed dt cannot.
// Knots are appended only at the newest sample, with d set by continuity, so a sample's display
// time is final once it is published: paused views, folded ERP epochs and the scroll edge never
// see the map of past data change. The cost is that an estimate error stays in the samples
// already published; the servo removes it only from later ones.
//
// TimeMap is the map itself (the owner guards it with a lock shared by writer and readers).
// DriftServo estimates D from one residual per pull and is producer-only state.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

struct DriftKnot { double x, d, s; };   // old-frame s, correction s, slope (fraction: 50 ppm = 5e-5)

namespace driftmap {
// First guesses, to be tuned on a real amplifier and on the synthetic test.
inline constexpr double kKnotSpacing = 5.0;     // L: s of data between knots
inline constexpr double kFitHalfLife = 60.0;    // H: s of data, the fit's forgetting half-life
inline constexpr double kServoTau    = 20.0;    // s: time constant that pulls D onto the fitted line
inline constexpr double kGate        = 0.020;   // s: residuals this far from the prediction are skipped
inline constexpr double kResetSpan   = 2.0;     // s of data with mostly skipped points before a refit
inline constexpr unsigned kResetMinPoints = 8;  // and at least this many points: one outlier at 1 Hz is not a step
inline constexpr double kMaxSlope    = 1e-3;    // 1000 ppm: a broken fit cannot run the axis away
inline constexpr double kMinCover    = 10.0;    // s of data the fit needs before it steers
inline constexpr double kRetention   = 120.0;   // s: as the gap list, past any visible window
inline constexpr std::size_t kKnotReserve = 64; // > kRetention / kKnotSpacing + slack: no growth
}

class TimeMap {
public:
    using Gap = std::pair<double, double>;   // (old-frame time of the first sample after it, seconds)

    TimeMap() { knots_.reserve(driftmap::kKnotReserve); }

    const std::vector<Gap>&       gaps()    const { return gaps_; }
    const std::vector<DriftKnot>& knots()   const { return knots_; }
    double                        gapBase() const { return gapBase_; }

    // G(x): folded base plus every listed gap at or before x.
    double gapOffset(double x) const {
        double off = gapBase_;
        for (const Gap& g : gaps_) { if (g.first > x) break; off += g.second; }
        return off;
    }
    // D(x). Before the first knot, its segment extends backward (only times older than the
    // retention cutoff land there once knots are pruned).
    double drift(double x) const { return value(segment(x), x); }
    // Slope of the open-ended last segment: the rate correction now applied.
    double headSlope() const { return knots_.empty() ? 0.0 : knots_.back().s; }

    double realTime(double x) const { return compose(x, gapOffset(x), segment(x)); }

    // In place, for a monotonically increasing array: one pass over knots and gaps together.
    void apply(double* x, int n) const {
        if (n <= 0 || (gaps_.empty() && gapBase_ == 0.0 && knots_.empty())) return;
        constexpr double inf = std::numeric_limits<double>::infinity();
        const std::size_t ng = gaps_.size(), nk = knots_.size();
        double off = gapBase_;
        std::size_t gi = 0, ki = 0;
        const DriftKnot* k = nk ? &knots_[0] : &kZero;
        // Start in the segment of x[0] directly: walking from knot 0 is the same result.
        if (nk > 1) {
            ki = segmentIndex(x[0]);
            k  = &knots_[ki];
        }
        double nextK = ki + 1 < nk ? knots_[ki + 1].x : inf;
        double nextG = ng ? gaps_[0].first : inf;
        for (int i = 0; i < n; ++i) {
            const double xi = x[i];
            while (nextG <= xi) { off += gaps_[gi].second; nextG = ++gi < ng ? gaps_[gi].first : inf; }
            while (nextK <= xi) { k = &knots_[++ki]; nextK = ki + 1 < nk ? knots_[ki + 1].x : inf; }
            x[i] = compose(xi, off, *k);
        }
    }

    // Inverse of realTime. A time inside a dropout maps to the first sample after it:
    // subtracting only the earlier gaps would land up to one gap length past it, in data
    // recorded later, so a marker a microsecond before the resumption would be matched to
    // samples taken seconds after it.
    double oldFrameTime(double y) const {
        constexpr double inf = std::numeric_limits<double>::infinity();
        const std::size_t ng = gaps_.size(), nk = knots_.size();
        double off = gapBase_;
        std::size_t gi = 0, ki = 0;
        for (;;) {
            const double xg = gi < ng ? gaps_[gi].first : inf;
            const double xk = ki + 1 < nk ? knots_[ki + 1].x : inf;
            if (xk <= xg) {
                if (xk == inf) break;
                // D is continuous, so the knot's own d is the display time there.
                if (y < xk + off + knots_[ki + 1].d) break;
                ++ki;
                continue;
            }
            const double yNear = compose(xg, off, nk ? knots_[ki] : kZero);
            if (y < yNear) break;
            if (y < yNear + gaps_[gi].second) return xg;
            off += gaps_[gi++].second;
        }
        if (nk == 0) return y - off;
        const DriftKnot& k = knots_[ki];
        return k.x + (y - off - k.x - k.d) / (1.0 + k.s);
    }

    // Each listed dropout as a display-time interval [start, end). `out` is reused.
    void bands(std::vector<Gap>& out) const {
        out.clear();
        double off = gapBase_;
        for (const Gap& g : gaps_) {
            const double a = compose(g.first, off, segment(g.first));
            out.push_back({a, a + g.second});
            off += g.second;
        }
    }
    // Seconds of every recorded dropout, the folded ones included.
    double missingSeconds() const {
        double off = gapBase_;
        for (const Gap& g : gaps_) off += g.second;
        return off;
    }

    // Record a dropout of g seconds before the sample at old-frame time x (the newest so far).
    // Gaps older than the retention fold into a constant base, in list order, so G of a later
    // time is summed in the same order before and after the fold (bit-identical).
    void addGap(double x, double g) {
        gaps_.push_back({x, g});
        const double cutoff = x - driftmap::kRetention;
        std::size_t k = 0;
        while (k < gaps_.size() && gaps_[k].first < cutoff) gapBase_ += gaps_[k++].second;
        if (k > 0) gaps_.erase(gaps_.begin(), gaps_.begin() + (std::ptrdiff_t)k);
    }

    // Start a segment of slope s at the newest sample's old-frame time. d comes from the
    // segment before, so every time at or before xHead keeps its value bit for bit.
    void addKnot(double xHead, double s) {
        const double d = knots_.empty() ? 0.0 : value(knots_.back(), xHead);
        knots_.push_back({xHead, d, s});
        // Drop a knot only when the one after it is also past the cutoff: the oldest kept
        // segment must still cover the cutoff.
        const double cutoff = xHead - driftmap::kRetention;
        std::size_t k = 0;
        while (k + 1 < knots_.size() && knots_[k + 1].x < cutoff) ++k;
        if (k > 0) knots_.erase(knots_.begin(), knots_.begin() + (std::ptrdiff_t)k);
    }

    // The one formula for a display time, so every path gives the same bits for the same x.
    static double value(const DriftKnot& k, double x) { return k.d + k.s * (x - k.x); }
    static double compose(double x, double off, const DriftKnot& k) { return x + off + value(k, x); }

private:
    static constexpr DriftKnot kZero{0.0, 0.0, 0.0};

    std::size_t segmentIndex(double x) const {   // last knot with knot.x <= x, or 0
        const auto it = std::upper_bound(knots_.begin(), knots_.end(), x,
                                         [](double v, const DriftKnot& k) { return v < k.x; });
        return it == knots_.begin() ? 0 : (std::size_t)(it - knots_.begin()) - 1;
    }
    const DriftKnot& segment(double x) const { return knots_.empty() ? kZero : knots_[segmentIndex(x)]; }

    std::vector<Gap>       gaps_;
    double                 gapBase_ = 0.0;   // folded offset of pruned gaps
    std::vector<DriftKnot> knots_;           // sorted by x
};

// Estimates D from one residual per pull and decides each new knot's slope. A type-2 clock
// servo: an exponentially weighted line through the residuals gives the rate error, and a
// steering term pulls the published correction onto the line, so a constant rate error leaves
// no steady-state offset. Producer-only: no lock.
class DriftServo {
public:
    // The first knot sits at the anchor with d = 0; the fit starts there too.
    void start(double xAnchor) {
        x0_ = xAnchor; lastKnotX_ = xAnchor;
        reset(xAnchor);
        mismatch_ = false; skippedTotal_ = 0; resets_ = 0;
    }

    // r = (stamp + clock offset) - (x + G(x)) for the newest sample of a pull at old-frame x:
    // the drift built up so far, plus jitter. Only the newest sample is used, because liblsl
    // back-fills the earlier samples of a chunk from the nominal rate.
    // Returns true when a knot is due at x; `sNew` is then the new segment's slope, and the
    // caller appends it with TimeMap::addKnot(x, sNew). `map` is read only.
    bool update(double x, double r, const TimeMap& map, double& sNew) {
        if (!(x >= lastX_)) return false;   // not newer: nothing to learn (and keeps decay sane)
        // Gate only a fit that can steer. Before that, the slope from a few points is too noisy
        // to predict with, and a replay at another rate would gate out every point and never
        // let the fit cover enough data to say so.
        const bool skip = covered() && std::fabs(r - at(x)) > driftmap::kGate;
        if (skip) { ++winSkip_; ++skippedTotal_; } else { ++winKeep_; add(x, r); }
        // The window runs until it spans kResetSpan and holds kResetMinPoints points, so a slow
        // stream judges a refit on as many points as a fast one, over a longer time.
        if (x - winStart_ >= driftmap::kResetSpan && winSkip_ + winKeep_ >= driftmap::kResetMinPoints) {
            // A clock offset step, or a replay whose stamps follow another rate: refit from here.
            if (winSkip_ > winKeep_) { reset(x); add(x, r); ++resets_; }
            winStart_ = x; winSkip_ = winKeep_ = 0;
        }
        if (x - lastKnotX_ < driftmap::kKnotSpacing) return false;
        sNew = steer(x, map.drift(x), map.headSlope());
        rebase(x);
        lastKnotX_ = x;
        return true;
    }

    // Length to record for a dropout found before the sample at old-frame x, whose stamp
    // mapped to the local clock is y, with gBefore = G just before the gap. The raw length (the
    // stamp difference across the gap) carries the jitter of two single stamps, and all of it
    // shows in the display time of every later sample until the servo removes it. Once the fit
    // can steer, take the length that puts this sample on the fit's line instead: r stays
    // continuous, and only the stamp after the gap adds jitter. Raw before that, and if the
    // fit's answer is not a forward gap.
    double gapLength(double x, double y, double gBefore, double raw) const {
        if (!covered()) return raw;
        const double g = y - (x + gBefore) - at(x);
        return g > 0.0 ? g : raw;
    }

    bool          mismatch()     const { return mismatch_; }       // fitted rate beyond the clamp
    unsigned long skippedTotal() const { return skippedTotal_; }   // points outside the gate
    unsigned long resets()       const { return resets_; }         // refits after a step
    bool          covered()      const { return lastX_ - fitStart_ >= driftmap::kMinCover; }
    double        fitSlope()     const { return slope(); }

private:
    // Slope of the next segment: the fitted rate plus a pull toward the fitted line.
    double steer(double xHead, double dHead, double sPrev) {
        if (!covered()) return sPrev;   // startup and reacquisition: hold the current slope
        const double sh = slope();
        mismatch_ = std::fabs(sh) > driftmap::kMaxSlope;
        // A rate this far off is not a crystal: a replay with stamps scaled by its speed.
        // Following it would bend the axis by the speed; hold the slope and let the panel say so.
        if (mismatch_) return sPrev;
        const double e = at(xHead) - dHead;
        return std::clamp(sh + e / driftmap::kServoTau, -driftmap::kMaxSlope, driftmap::kMaxSlope);
    }

    void add(double x, double r) {
        const double u = x - x0_;
        if (cnt_ > 0) {
            const double f = std::exp2(-(x - lastX_) / driftmap::kFitHalfLife);
            w_ *= f; sx_ *= f; sr_ *= f; sxx_ *= f; sxr_ *= f;
        }
        w_ += 1.0; sx_ += u; sr_ += r; sxx_ += u * u; sxr_ += u * r;
        lastX_ = x;
        ++cnt_;
    }
    double slope() const {
        if (cnt_ < 2) return 0.0;
        const double mx = sx_ / w_, mr = sr_ / w_;
        const double var = sxx_ / w_ - mx * mx;
        return var > 1e-12 ? (sxr_ / w_ - mx * mr) / var : 0.0;
    }
    double at(double x) const {
        if (cnt_ == 0) return 0.0;
        return sr_ / w_ + slope() * ((x - x0_) - sx_ / w_);
    }
    // Keep x relative to the newest knot, so the squared terms stay small.
    void rebase(double xNew) {
        const double dx = xNew - x0_;
        sxx_ += -2.0 * dx * sx_ + dx * dx * w_;
        sxr_ -= dx * sr_;
        sx_  -= dx * w_;
        x0_ = xNew;
    }
    void reset(double x) {
        w_ = sx_ = sr_ = sxx_ = sxr_ = 0.0;
        cnt_ = 0;
        lastX_ = x; fitStart_ = x; winStart_ = x;
        winSkip_ = winKeep_ = 0;
    }

    double        x0_ = 0.0, lastKnotX_ = 0.0, lastX_ = 0.0, fitStart_ = 0.0, winStart_ = 0.0;
    double        w_ = 0.0, sx_ = 0.0, sr_ = 0.0, sxx_ = 0.0, sxr_ = 0.0;   // weighted sums
    unsigned long cnt_ = 0, winSkip_ = 0, winKeep_ = 0, skippedTotal_ = 0, resets_ = 0;
    bool          mismatch_ = false;
};
