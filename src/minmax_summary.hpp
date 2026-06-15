#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Fixed-grid min/max summary keyed to ABSOLUTE sample index. Bin k always covers
// absolute samples [k*B, (k+1)*B), so a closed bin's (min, max) and its time never
// change. A scrolling viewer therefore just translates the bins (no shimmer), and
// per-frame work drops from O(window_samples) to O(visible_bins) per channel.
//
// Folded incrementally by the producer from the same chunk it just wrote to the raw
// ring (cache-hot). Layout is interleaved [bin][channel], mirroring the raw ring.
//
// Choose B so that at maximum zoom-out you get ~1 bin per pixel
// (e.g. window 10 s @ 8 kHz = 80k samples / 1500 px -> B ~ 32-64). When zoomed in
// past 1 sample/bin, read the raw ring instead. For a wide zoom range, keep several
// levels (B, 4B, 16B, ...) and pick the one nearest ~1 bin/pixel.
class MinMaxSummary {
public:
    void init(int channels, int bin_samples, std::size_t cap_bins) {
        C_ = channels; B_ = bin_samples; capB_ = cap_bins;
        mn_.assign(capB_ * C_,  kHi);
        mx_.assign(capB_ * C_,  kLo);
        accMn_.assign(C_, kHi);
        accMx_.assign(C_, kLo);
        accCount_ = 0; binAbs_ = 0;
        closed_.store(0, std::memory_order_relaxed);
    }

    int           binSamples() const { return B_; }
    std::uint64_t closedBins() const { return closed_.load(std::memory_order_acquire); }

    // PRODUCER thread only. Fold n interleaved samples (n*channels floats).
    void append(const float* s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const float* smp = s + i * (std::size_t)C_;
            for (int c = 0; c < C_; ++c) {
                if (smp[c] < accMn_[c]) accMn_[c] = smp[c];
                if (smp[c] > accMx_[c]) accMx_[c] = smp[c];
            }
            if (++accCount_ == B_) commit();
        }
    }

    // READER. `bins` closed bins for channel c ending at `endBin` (absolute bin
    // index, exclusive; 0 = the newest), ready for PlotShaded. x/mn/mx must each
    // hold >= bins floats. dt = 1/srate; t0 = time (s) of sample 0. Returns the
    // number of bins written. Pass a frozen endBin (e.g. head/B at pause) to keep
    // a paused view anchored instead of following new data.
    int read(int c, std::size_t bins, double dt, double t0,
             double* x, double* mn, double* mx, std::uint64_t endBin = 0) const {
        const std::uint64_t closed   = closed_.load(std::memory_order_acquire);
        const std::uint64_t end      = (endBin == 0 || endBin > closed) ? closed : endBin;
        const std::uint64_t resident = std::min<std::uint64_t>(closed, capB_ - 1); // guard slot
        const std::uint64_t oldest   = closed - resident;            // oldest still in buffer
        const std::uint64_t avail    = (end > oldest) ? end - oldest : 0;
        const std::uint64_t take     = std::min<std::uint64_t>(bins, avail);
        const std::uint64_t first    = end - take;
        int n = 0;
        for (std::uint64_t k = first; k < end; ++k) {
            const std::size_t slot = (std::size_t)(k % capB_) * C_ + c;
            x[n]  = t0 + dt * ((double)k * B_ + B_ * 0.5);   // double: absolute LSL time
            mn[n] = mn_[slot];
            mx[n] = mx_[slot];
            ++n;
        }
        return n;
    }

    // Copy another summary's committed-bin state (for the pause snapshot — read() only needs
    // these). The open accumulators are irrelevant to readers, so they're left as-is.
    void snapshotFrom(const MinMaxSummary& src) {
        C_ = src.C_; B_ = src.B_; capB_ = src.capB_;
        mn_ = src.mn_; mx_ = src.mx_;
        binAbs_ = src.binAbs_;
        closed_.store(src.closed_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    void commit() {
        const std::size_t base = (std::size_t)(binAbs_ % capB_) * C_;
        for (int c = 0; c < C_; ++c) { mn_[base + c] = accMn_[c]; mx_[base + c] = accMx_[c]; }
        ++binAbs_;
        closed_.store(binAbs_, std::memory_order_release);     // publish for readers
        std::fill(accMn_.begin(), accMn_.end(), kHi);
        std::fill(accMx_.begin(), accMx_.end(), kLo);
        accCount_ = 0;
    }

    static constexpr float kHi =  std::numeric_limits<float>::infinity();
    static constexpr float kLo = -std::numeric_limits<float>::infinity();

    int                        C_ = 0, B_ = 32;
    std::size_t                capB_ = 0;
    std::vector<float>         mn_, mx_;        // [capB_ * C_], interleaved [bin][channel]
    std::vector<float>         accMn_, accMx_;  // open-bin accumulators
    int                        accCount_ = 0;
    std::uint64_t              binAbs_ = 0;
    std::atomic<std::uint64_t> closed_{0};
};
