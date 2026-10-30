// Headless tests for src/drift_map.hpp, with synthetic timestamps (no LSL, no GPU).
//
//   drift_map_test           the drift-map tests; exits non-zero on failure
//   drift_map_test --bench   microbenchmark of the per-frame time mapping, old against new
//
// The simulation drives TimeMap and DriftServo in the same order as HfStreamSource::run():
// anchor, dropout check, one residual per pull, knot, then publish.

#include "drift_map.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);   \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
        }                                                                   \
    } while (0)

struct SimConfig {
    double ppm        = 100.0;     // device rate error: + runs fast
    double hours      = 4.0;
    double srate      = 1000.0;
    int    chunk      = 20;        // samples per pull (20 ms at 1 kHz)
    double jitter     = 0.001;     // s, Gaussian, on each chunk's newest stamp
    double dropoutAt  = 3600.0;    // s after start
    double dropoutLen = 2.0;       // s
    double stepAt     = 7200.0;    // s after start: time_correction steps
    double step       = 0.030;     // s
    double outlierAt  = -1.0;      // s after start: one chunk stamp off by `outlier` (< 0: none)
    double outlier    = 0.050;     // s
    double settle     = 60.0;      // s after start before the error bound applies
    double recover    = 0.0;       // s after the offset step excluded from the bound (0 = none)
    double recoverDrop = 0.0;      // s after the dropout excluded from the bound (0 = none)
    bool   fitGap     = true;      // dropout length from the fit (as the viewer), else the raw stamps
    bool   recheck    = true;      // re-evaluate the kept display times after every knot
    unsigned seed     = 12345;
};

struct SimResult {
    double maxErr = 0.0;           // outside the excluded windows, after `settle`
    double maxErrDrop = 0.0;       // inside the window after the dropout
    double maxErrAll = 0.0;        // after `settle`, events included
    double firstConverged = -1.0;  // s: from here on the error stays < 1 ms (outside event windows)
    double recoverDropout = 0.0;   // s after the dropout until the error stays < 1 ms
    double recoverStep = 0.0;      // s after the step until the error stays < 1 ms
    double finalPpm = 0.0;         // applied slope at the end
    std::size_t knots = 0, maxKnots = 0, knotCount = 0;
    unsigned long skipped = 0;
    unsigned long resets = 0;      // refits after a step
    std::size_t   gaps = 0;        // dropouts recorded
    double gapLen = 0.0;           // the recorded dropout length
    double gapErr = 0.0;           // its error against the true jump of the sample times
    unsigned long reevals = 0;     // stored display times re-evaluated after knots
    unsigned long mismatches = 0;  // of those, not bit-identical
    double inverseErr = 0.0;       // max |oldFrameTime(realTime(x)) - x|
    double realTimeDiff = 0.0;     // max |realTime(x) - apply(x)|, should be 0
};

SimResult simulate(const SimConfig& c) {
    SimResult res;
    const double dt = 1.0 / c.srate;
    const double dtTrue = dt / (1.0 + c.ppm * 1e-6);
    const double T0 = 86400.0 * 3.0;   // a local_clock() a few days after boot
    const std::uint64_t totalDevice = (std::uint64_t)(c.hours * 3600.0 * c.srate);
    const std::uint64_t dropK0 = (std::uint64_t)(c.dropoutAt * c.srate) / c.chunk * c.chunk;
    const std::uint64_t dropK1 = dropK0 + (std::uint64_t)(c.dropoutLen * c.srate);
    const std::uint64_t stepK  = (std::uint64_t)(c.stepAt * c.srate) / c.chunk * c.chunk;
    const std::uint64_t outK   = c.outlierAt < 0.0 ? ~0ull : (std::uint64_t)(c.outlierAt * c.srate) / c.chunk * c.chunk;

    std::mt19937_64 rng(c.seed);
    // Not std::normal_distribution: its algorithm is up to the library, so libstdc++, libc++
    // and MSVC draw different jitter from the same seed and the seeded checks would pass or
    // fail by platform. The engine itself is fully specified.
    auto jit = [&rng, sd = c.jitter]() {
        const double u1 = (double)((rng() >> 11) + 1) * 0x1p-53;   // (0, 1]
        const double u2 = (double)(rng() >> 11) * 0x1p-53;
        return sd * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    };

    TimeMap    map;
    DriftServo servo;
    double t0 = 0.0, lastTs = 0.0, lastTrue = 0.0;
    bool anchored = false;
    std::uint64_t head = 0;

    // Every published sample of the last 100 s (within the 120 s retention): old-frame time and
    // the display time it was published with. A ring of two monotone runs.
    const std::size_t keep = (std::size_t)(100.0 * c.srate);
    std::vector<double> keptX(keep), keptY(keep), scratch(keep);
    std::size_t keptHead = 0, keptCount = 0;

    std::vector<double> ts(c.chunk), xs(c.chunk), disp(c.chunk);
    std::vector<double> trueT(c.chunk);
    std::vector<std::uint64_t> devK(c.chunk);

    // Error tracking: the last time (s after start) each region saw |err| >= 1 ms.
    double lastBadPre = -1.0, lastBadDrop = -1.0, lastBadStep = -1.0;
    const double tDrop = (double)dropK1 / c.srate, tStep = (double)stepK / c.srate;

    auto recheck = [&]() {
        // Re-evaluate every kept sample through the batch path and compare bits.
        const std::size_t first = (keptHead + keep - keptCount) % keep;
        const std::size_t runA = std::min(keptCount, keep - first);
        std::memcpy(scratch.data(), keptX.data() + first, runA * sizeof(double));
        std::memcpy(scratch.data() + runA, keptX.data(), (keptCount - runA) * sizeof(double));
        map.apply(scratch.data(), (int)runA);
        map.apply(scratch.data() + runA, (int)(keptCount - runA));
        for (std::size_t i = 0; i < keptCount; ++i) {
            const double y = keptY[(first + i) % keep];
            if (std::memcmp(&scratch[i], &y, sizeof(double)) != 0) ++res.mismatches;
        }
        res.reevals += (unsigned long)keptCount;
        // The scalar path and the inverse, on a subsample.
        for (std::size_t i = 0; i < keptCount; i += 997) {
            const std::size_t j = (first + i) % keep;
            const double rt = map.realTime(keptX[j]);
            res.realTimeDiff = std::max(res.realTimeDiff, std::fabs(rt - keptY[j]));
            res.inverseErr = std::max(res.inverseErr, std::fabs(map.oldFrameTime(rt) - keptX[j]));
        }
    };

    std::uint64_t k = 0;
    while (k < totalDevice) {
        if (k >= dropK0 && k < dropK1) { k = dropK1; continue; }   // the device's samples are lost
        const int n = c.chunk;
        const double offset = k >= stepK ? c.step : 0.0;           // time_correction() of this pull
        for (int j = 0; j < n; ++j) {
            devK[j] = k + (std::uint64_t)j;
            trueT[j] = T0 + (double)devK[j] * dtTrue;              // sender clock
        }
        // liblsl stamps the newest sample and back-fills the rest from the nominal rate.
        const double newest = trueT[n - 1] + jit() + (k == outK ? c.outlier : 0.0);
        for (int j = 0; j < n; ++j) ts[j] = newest - (double)(n - 1 - j) * dt;
        k += (std::uint64_t)n;

        const std::uint64_t headBefore = head;
        if (!anchored) {
            const std::uint64_t absLast = headBefore + (std::uint64_t)n - 1;
            t0 = (ts[n - 1] + offset) - (double)absLast * dt;
            anchored = true;
            const double xa = t0 + (double)absLast * dt;
            map.addKnot(xa, 0.0);
            servo.start(xa);
        } else {
            const double g = ts[0] - (lastTs + dt);
            if (g > 0.25) {
                const double xg = t0 + (double)headBefore * dt;
                res.gapLen = c.fitGap ? servo.gapLength(xg, ts[0] + offset, map.gapOffset(xg), g) : g;
                map.addGap(xg, res.gapLen);
                res.gapErr = res.gapLen - (trueT[0] - lastTrue - dt);
                ++res.gaps;
            }
        }
        lastTs = ts[n - 1];
        lastTrue = trueT[n - 1];

        const double xh = t0 + (double)(headBefore + (std::uint64_t)n - 1) * dt;
        const double r = (ts[n - 1] + offset) - (xh + map.gapOffset(xh));
        double sNew = 0.0;
        bool knot = servo.update(xh, r, map, sNew);
        if (knot) {
            map.addKnot(xh, sNew);
            res.maxKnots = std::max(res.maxKnots, map.knots().size());
            ++res.knotCount;
        }

        // Publish: display times are final from here on.
        for (int j = 0; j < n; ++j) disp[j] = xs[j] = t0 + (double)(headBefore + (std::uint64_t)j) * dt;
        map.apply(disp.data(), n);
        for (int j = 0; j < n; ++j) {
            const double truth = trueT[j] + offset;
            const double err = std::fabs(disp[j] - truth);
            const double tSim = (double)devK[j] / c.srate;
            if (tSim >= c.settle) res.maxErrAll = std::max(res.maxErrAll, err);
            const bool inStep = tSim >= tStep && tSim < tStep + c.recover;
            const bool inDrop = tSim >= tDrop && tSim < tDrop + c.recoverDrop;
            if (tSim >= c.settle && !inStep && !inDrop) res.maxErr = std::max(res.maxErr, err);
            if (inDrop) res.maxErrDrop = std::max(res.maxErrDrop, err);
            if (err >= 1e-3) {
                if (tSim < tDrop)      lastBadPre = tSim;
                else if (tSim < tStep) lastBadDrop = tSim;
                else                   lastBadStep = tSim;
            }
            keptX[keptHead] = xs[j];
            keptY[keptHead] = disp[j];
            keptHead = (keptHead + 1) % keep;
            keptCount = std::min(keptCount + 1, keep);
        }
        head += (std::uint64_t)n;
        if (knot && c.recheck) recheck();
    }
    res.firstConverged  = lastBadPre;
    res.recoverDropout  = lastBadDrop < 0.0 ? 0.0 : lastBadDrop - tDrop;
    res.recoverStep     = lastBadStep < 0.0 ? 0.0 : lastBadStep - tStep;
    res.finalPpm = map.headSlope() * 1e6;
    res.knots = map.knots().size();
    res.skipped = servo.skippedTotal();
    res.resets = servo.resets();
    return res;
}

// The fit removes the jitter of the stamp before a dropout, but the length still takes the
// jitter of the one stamp after it, and the samples published right after the gap keep that
// error until the servo removes it. With 1 ms Gaussian jitter that is above 1 ms in about a
// third of dropouts and takes up to about a minute to go. So inside kDropWindow the check is
// that the error comes from the length alone, does not grow past it, and decays; the 1 ms
// bound applies outside.
constexpr double kDropWindow = 90.0;   // s

void checkDropout(const SimResult& r, const SimConfig& c, const char* tag) {
    CHECK(std::fabs(r.gapErr) < 4.0 * c.jitter, "%sdropout length error %+.3f ms", tag, r.gapErr * 1e3);
    CHECK(r.maxErrDrop < std::fabs(r.gapErr) + 0.25e-3, "%smax error %.3f ms after the dropout, length error %+.3f ms",
          tag, r.maxErrDrop * 1e3, r.gapErr * 1e3);
    CHECK(r.recoverDropout < kDropWindow, "%sdropout recovery %.1f s", tag, r.recoverDropout);
}

void testSynthetic(double ppm) {
    std::printf("synthetic: %+.0f ppm, 4 h at 1 kHz, 1 ms jitter, 20 ms chunks, 2 s dropout at 1 h, "
                "30 ms offset step at 2 h\n", ppm);
    SimConfig c;
    c.ppm = ppm;
    c.recover = 120.0;
    c.recoverDrop = kDropWindow;
    const auto t = std::chrono::steady_clock::now();
    const SimResult r = simulate(c);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    std::printf("  max |display - true| after %.0f s, outside %.0f s after the step and %.0f s after the "
                "dropout: %.3f ms\n", c.settle, c.recover, c.recoverDrop, r.maxErr * 1e3);
    std::printf("  max |display - true| in the %.0f s after the dropout: %.3f ms (length error %+.3f ms)\n",
                c.recoverDrop, r.maxErrDrop * 1e3, r.gapErr * 1e3);
    std::printf("  max |display - true| after %.0f s, all samples:               %.3f ms\n",
                c.settle, r.maxErrAll * 1e3);
    std::printf("  error < 1 ms from %.1f s on (startup); after the dropout %.1f s; after the step %.1f s\n",
                r.firstConverged + 1.0 / c.srate, r.recoverDropout, r.recoverStep);
    std::printf("  applied slope at end %+.3f ppm (device %+.0f ppm, expect %+.3f)\n",
                r.finalPpm, ppm, -ppm / (1.0 + ppm * 1e-6));
    std::printf("  knots: %zu appended, %zu kept at end, %zu at most; %lu points gated out, %lu refits\n",
                r.knotCount, r.knots, r.maxKnots, r.skipped, r.resets);
    std::printf("  re-evaluated %lu published display times after knots: %lu not bit-identical\n",
                r.reevals, r.mismatches);
    std::printf("  realTime vs apply max diff %.3g s; inverse max error %.3g s; %.2f s to run\n",
                r.realTimeDiff, r.inverseErr, secs);
    CHECK(r.maxErr < 1e-3, "max error %.3f ms", r.maxErr * 1e3);
    checkDropout(r, c, "");
    CHECK(r.firstConverged < c.settle, "converged only at %.1f s", r.firstConverged);
    CHECK(r.mismatches == 0, "%lu re-evaluations changed", r.mismatches);
    CHECK(r.reevals > 0, "nothing re-evaluated");
    CHECK(r.realTimeDiff == 0.0, "realTime differs from apply by %.3g", r.realTimeDiff);
    CHECK(r.inverseErr < 1e-9, "inverse error %.3g", r.inverseErr);
    CHECK(r.maxKnots <= driftmap::kKnotReserve, "knot list grew to %zu", r.maxKnots);
    CHECK(std::fabs(r.finalPpm + ppm) < 5.0, "final slope %.3f ppm", r.finalPpm);
    CHECK(r.gaps == 1 && r.resets == 1, "%zu dropouts, %lu refits", r.gaps, r.resets);

    // The same over 8 seeds, the dropout length from the fit (as the viewer) and, for
    // reference, from the raw stamps.
    std::printf("  seed  max err  startup  dropout recovery (fit / raw)  gap error (fit / raw)  step\n");
    for (unsigned i = 0; i < 8; ++i) {
        SimConfig cs = c;
        cs.seed = 1000u + i;
        cs.recheck = false;
        const SimResult f = simulate(cs);
        cs.fitGap = false;
        const SimResult w = simulate(cs);
        std::printf("  %4u  %5.3f ms  %5.1f s  %7.1f s / %5.1f s            %+6.3f / %+6.3f ms   %5.1f s\n",
                    cs.seed, f.maxErr * 1e3, f.firstConverged + 1.0 / cs.srate, f.recoverDropout,
                    w.recoverDropout, f.gapErr * 1e3, w.gapErr * 1e3,
                    f.recoverStep);
        char tag[32];
        std::snprintf(tag, sizeof tag, "seed %u: ", cs.seed);
        CHECK(f.maxErr < 1e-3, "%smax error %.3f ms", tag, f.maxErr * 1e3);
        checkDropout(f, cs, tag);
        CHECK(f.firstConverged < c.settle, "seed %u: converged only at %.1f s", cs.seed, f.firstConverged);
    }
}

// A 1 Hz regular stream: one point per pull and per second. One 50 ms outlier must not refit
// (a 2 s window holds 2 points, so one outlier would be a majority); the offset step must.
void testLowRate() {
    std::printf("low rate: 1 Hz, +100 ppm, 1 ms jitter, 2 h, 50 ms outlier at 30 min, 2 s dropout at 1 h, "
                "30 ms step at 90 min\n");
    SimConfig c;
    c.srate = 1.0; c.chunk = 1; c.hours = 2.0;
    c.outlierAt = 1800.0; c.dropoutAt = 3600.0; c.stepAt = 5400.0;
    c.settle = 120.0; c.recover = 300.0;
    const SimResult r = simulate(c);
    std::printf("  max |display - true| after %.0f s, outside %.0f s after the step: %.3f ms\n",
                c.settle, c.recover, r.maxErr * 1e3);
    std::printf("  startup %.0f s, dropout recovery %.0f s, step recovery %.0f s; %lu gated out, %lu refits, "
                "%lu not bit-identical\n", r.firstConverged + 1.0, r.recoverDropout, r.recoverStep,
                r.skipped, r.resets, r.mismatches);
    CHECK(r.resets == 1, "%lu refits (expect 1: the step, not the outlier)", r.resets);
    CHECK(r.maxErr < 1e-3, "max error %.3f ms", r.maxErr * 1e3);
    CHECK(r.mismatches == 0, "%lu re-evaluations changed", r.mismatches);
}

// Correction off (the LSL_DRIFT_CORR=0 path keeps no knots): the old mapping, and the drift it
// leaves, as the reference for the A/B.
void testUncorrected() {
    std::printf("reference: no drift map, +100 ppm, 10 min\n");
    TimeMap map;
    const double dt = 1e-3, x0 = 1000.0;
    double y = x0 + 600.0;
    map.apply(&y, 1);
    const double truth = x0 + 600.0 / (1.0 + 1e-4);
    std::printf("  error after 10 min: %.2f ms\n", (y - truth) * 1e3);
    CHECK(std::fabs(y - truth) > 50e-3, "expected the uncorrected drift");
    (void)dt;
}

// The gap and knot walk against brute force, around a dropout and knots that share a time.
void testMapping() {
    std::printf("mapping: gaps and knots, batch against scalar, inverse, clamp inside a dropout\n");
    TimeMap map;
    map.addKnot(100.0, 0.0);
    map.addKnot(105.0, 2e-4);
    map.addGap(107.0, 1.5);
    map.addKnot(110.0, -3e-4);
    map.addGap(110.0, 0.5);   // at a knot
    map.addKnot(115.0, 1e-4);
    std::vector<double> xs;
    for (double x = 95.0; x < 125.0; x += 0.0137) xs.push_back(x);
    std::vector<double> ys(xs);
    map.apply(ys.data(), (int)ys.size());
    double worstInv = 0.0, prev = -1e300;
    bool monotone = true, same = true;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (map.realTime(xs[i]) != ys[i]) same = false;
        if (ys[i] <= prev) monotone = false;
        prev = ys[i];
        worstInv = std::max(worstInv, std::fabs(map.oldFrameTime(ys[i]) - xs[i]));
    }
    CHECK(same, "apply and realTime differ");
    CHECK(monotone, "display time not increasing");
    CHECK(worstInv < 1e-9, "inverse error %.3g", worstInv);
    // D at the gap and inside it.
    const double dGap = map.drift(107.0);
    std::vector<TimeMap::Gap> bands;
    map.bands(bands);
    CHECK(bands.size() == 2, "%zu bands", bands.size());
    if (bands.size() == 2) {
        CHECK(std::fabs(bands[0].first - (107.0 + dGap)) < 1e-12, "band start %.9f", bands[0].first);
        CHECK(std::fabs(bands[0].second - bands[0].first - 1.5) < 1e-12, "band length");
        // Inside a dropout: the first sample after it.
        CHECK(map.oldFrameTime(bands[0].first + 0.7) == 107.0, "clamp %.9f", map.oldFrameTime(bands[0].first + 0.7));
        CHECK(map.oldFrameTime(bands[1].first + 0.2) == 110.0, "clamp at knot %.9f", map.oldFrameTime(bands[1].first + 0.2));
        // Continuity of D across the knot at the gap.
        const double before = map.drift(std::nextafter(110.0, 0.0)), at = map.drift(110.0);
        CHECK(std::fabs(before - at) < 1e-15, "D jumps at a knot by %.3g", at - before);
    }
    CHECK(std::fabs(map.missingSeconds() - 2.0) < 1e-12, "missing %.9f", map.missingSeconds());
    // No knots: exactly the old gap-only mapping.
    TimeMap plain;
    plain.addGap(50.0, 0.75);
    double a = 49.0, b = 51.0;
    plain.apply(&a, 1); plain.apply(&b, 1);
    CHECK(a == 49.0 && b == 51.0 + 0.75, "gap-only mapping %.9f %.9f", a, b);
}

// Pruning keeps the segment that covers the cutoff, and keeps the list bounded.
void testPruning() {
    std::printf("pruning: knots older than the retention go, the covering segment stays\n");
    TimeMap map;
    double x = 0.0;
    map.addKnot(x, 0.0);
    std::size_t most = 0;
    for (int i = 0; i < 2000; ++i) {
        x += driftmap::kKnotSpacing;
        map.addKnot(x, (i % 7 - 3) * 1e-5);
        most = std::max(most, map.knots().size());
    }
    const double cutoff = x - driftmap::kRetention;
    CHECK(map.knots().front().x <= cutoff, "oldest kept knot %.1f > cutoff %.1f", map.knots().front().x, cutoff);
    CHECK(map.knots().size() >= 2 && map.knots()[1].x >= cutoff, "kept a knot it could drop");
    CHECK(most <= driftmap::kRetention / driftmap::kKnotSpacing + 2, "%zu knots", most);
    std::printf("  %zu knots kept at most\n", most);
}

// A replay whose stamps follow its speed: a rate error far past the clamp. The map holds its
// slope and flags the mismatch instead of following.
void testScaledReplay() {
    std::printf("scaled replay: stamps at 2x for 60 s\n");
    TimeMap map;
    DriftServo servo;
    const double dt = 1e-3;
    double x = 500.0;
    map.addKnot(x, 0.0);
    servo.start(x);
    bool flagged = false;
    for (int i = 0; i < 3000; ++i) {
        x += 0.02;
        const double r = (x - 500.0) * 1.0;   // stamps advance 2 s per data s
        double s = 0.0;
        if (servo.update(x, r, map, s)) map.addKnot(x, s);
        flagged = flagged || servo.mismatch();
    }
    CHECK(flagged, "no mismatch flagged");
    CHECK(std::fabs(map.headSlope()) <= driftmap::kMaxSlope, "slope %.6f", map.headSlope());
    std::printf("  mismatch %s, slope %+.1f ppm, D at head %+.1f ms\n", flagged ? "flagged" : "not flagged",
                map.headSlope() * 1e6, map.drift(x) * 1e3);
    (void)dt;
}

// ---------------------------------------------------------------------------
// Microbenchmark: the old gap-only mapping (copied from HfStreamSource before the drift map)
// against TimeMap, on the shape the spec names: 24 knots, 10 gaps, 4096 x values. The two run
// in alternating rounds in one process, so background load hits both alike.

struct OldGaps {
    std::vector<std::pair<double, double>> gaps_;
    double gapBase_ = 0.0;
    double realTime(double oldT) const {
        double off = gapBase_;
        for (auto& g : gaps_) if (g.first <= oldT) off += g.second;
        return oldT + off;
    }
    double oldFrameTime(double realT) const {
        double off = gapBase_;
        for (auto& g : gaps_) {
            if (realT >= g.first + off + g.second) { off += g.second; continue; }
            if (realT > g.first + off) return g.first;
            break;
        }
        return realT - off;
    }
    void applyGaps(double* x, int n) const {
        double off = gapBase_; std::size_t gi = 0;
        if (gaps_.empty() && off == 0.0) return;
        for (int i = 0; i < n; ++i) {
            while (gi < gaps_.size() && gaps_[gi].first <= x[i]) { off += gaps_[gi].second; ++gi; }
            x[i] += off;
        }
    }
};

template <class F>
double timeNs(F&& f, int reps) {
    const auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) f();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count() / reps;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int bench() {
    const double xBase = 250000.0, span = 120.0;
    OldGaps oldMap;
    TimeMap newMap, newNoKnots;
    newMap.addKnot(xBase, 0.0);
    for (int i = 1; i < 24; ++i) newMap.addKnot(xBase + i * span / 24.0, (i % 5 - 2) * 2e-5);
    for (int i = 0; i < 10; ++i) {
        const double gx = xBase + 6.0 + i * span / 10.0, g = 0.3 + 0.01 * i;
        oldMap.gaps_.push_back({gx, g});
        newMap.addGap(gx, g);
        newNoKnots.addGap(gx, g);
    }
    // The visible window: 4096 x values over the last 60 s (a stacked plot's shared axis).
    const int N = 4096;
    std::vector<double> base(N), buf(N);
    for (int i = 0; i < N; ++i) base[i] = xBase + 60.0 + 60.0 * i / N;
    double sink = 0.0;
    auto runOld = [&] { std::copy(base.begin(), base.end(), buf.begin()); oldMap.applyGaps(buf.data(), N); sink += buf[N / 2]; };
    auto runNew = [&] { std::copy(base.begin(), base.end(), buf.begin()); newMap.apply(buf.data(), N); sink += buf[N / 2]; };
    auto runNew0 = [&] { std::copy(base.begin(), base.end(), buf.begin()); newNoKnots.apply(buf.data(), N); sink += buf[N / 2]; };
    auto runCopy = [&] { std::copy(base.begin(), base.end(), buf.begin()); sink += buf[N / 2]; };
    // realTime: a few calls per frame (newest time, spectrogram columns, live edge).
    int ri = 0;
    auto rtOld = [&] { sink += oldMap.realTime(base[(ri = (ri + 977) % N)]); };
    auto rtNew = [&] { sink += newMap.realTime(base[(ri = (ri + 977) % N)]); };
    auto ofOld = [&] { sink += oldMap.oldFrameTime(base[(ri = (ri + 977) % N)] + 4.0); };
    auto ofNew = [&] { sink += newMap.oldFrameTime(base[(ri = (ri + 977) % N)] + 4.0); };

    std::vector<double> a, b, c0, cp, ra, rb, oa, of;
    for (int round = 0; round < 41; ++round) {
        a.push_back(timeNs(runOld, 2000));
        b.push_back(timeNs(runNew, 2000));
        c0.push_back(timeNs(runNew0, 2000));
        cp.push_back(timeNs(runCopy, 2000));
        ra.push_back(timeNs(rtOld, 200000));
        rb.push_back(timeNs(rtNew, 200000));
        oa.push_back(timeNs(ofOld, 200000));
        of.push_back(timeNs(ofNew, 200000));
    }
    const double copy = median(cp);
    std::printf("bench: 24 knots, 10 gaps, %d x values; median of 41 alternating rounds\n", N);
    std::printf("  copy of the x array alone        %8.0f ns\n", copy);
    std::printf("  applyGaps, old (gaps only)       %8.0f ns  (%.2f ns per x, copy excluded)\n", median(a), (median(a) - copy) / N);
    std::printf("  apply, new (knots + gaps)        %8.0f ns  (%.2f ns per x, copy excluded)\n", median(b), (median(b) - copy) / N);
    std::printf("  apply, new (gaps only, D = 0)    %8.0f ns  (%.2f ns per x, copy excluded)\n", median(c0), (median(c0) - copy) / N);
    std::printf("  realTime, old                    %8.1f ns per call\n", median(ra));
    std::printf("  realTime, new                    %8.1f ns per call\n", median(rb));
    std::printf("  oldFrameTime, old                %8.1f ns per call\n", median(oa));
    std::printf("  oldFrameTime, new                %8.1f ns per call\n", median(of));
    std::printf("  (checksum %.3f)\n", sink);
    return 0;
}

}  // namespace

// The same run over several seeds, one line each, for tuning the constants.
int seeds(int count) {
    for (double ppm : {+100.0, -100.0})
        for (int i = 0; i < count; ++i) {
            SimConfig c;
            c.ppm = ppm; c.seed = 1000u + (unsigned)i; c.recover = 120.0; c.recoverDrop = kDropWindow;
            c.recheck = false;
            const SimResult r = simulate(c);
            std::printf("%+5.0f ppm seed %u: max %.3f ms, startup %.1f s, dropout %.1f s (%.3f ms, "
                        "length %+.3f ms), step %.1f s, end %+.3f ppm, bit changes %lu\n", ppm, c.seed,
                        r.maxErr * 1e3, r.firstConverged, r.recoverDropout, r.maxErrDrop * 1e3, r.gapErr * 1e3,
                        r.recoverStep, r.finalPpm, r.mismatches);
        }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) return bench();
    if (argc > 2 && std::strcmp(argv[1], "--seeds") == 0) return seeds(std::atoi(argv[2]));
    testMapping();
    testPruning();
    testUncorrected();
    testScaledReplay();
    testLowRate();
    testSynthetic(+100.0);
    testSynthetic(-100.0);
    std::printf(g_failures ? "drift_map_test: %d FAILED\n" : "drift_map_test: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
