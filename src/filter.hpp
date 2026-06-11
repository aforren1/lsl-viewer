#pragma once
// Per-channel one-pole DC blocker (running high-pass):
//     y[n] = x[n] - x[n-1] + R * y[n-1],   R = exp(-2*pi*fc/fs)
// Primed on the first sample so a large DC offset doesn't ring in. Standalone
// (no SDL/LSL) so it can be unit-tested directly.

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

struct DcBlocker {
    void init(int channels, float R) {
        C_ = channels; R_ = R; primed_ = false;
        x1_.assign(channels, 0.0f);
        y1_.assign(channels, 0.0f);
    }
    void setR(float R) { R_ = R; }

    // Cutoff (Hz) -> pole radius. fc<=0 or fs<=0 disables (R=1, pure integrator-free passthrough of AC).
    static float cutoffToR(double fc, double fs) {
        if (fs <= 0.0 || fc <= 0.0) return 1.0f;
        return (float)std::exp(-2.0 * std::numbers::pi * fc / fs);
    }

    // Filter n interleaved samples (C channels) from `in` into `out`.
    void process(const float* in, float* out, std::size_t n) {
        if (!primed_) { for (int c = 0; c < C_; ++c) x1_[c] = in[c]; primed_ = true; }
        for (std::size_t i = 0; i < n; ++i) {
            const float* x = in  + i * (std::size_t)C_;
            float*       o = out + i * (std::size_t)C_;
            for (int c = 0; c < C_; ++c) {
                const float xc = x[c];
                const float y  = xc - x1_[c] + R_ * y1_[c];
                x1_[c] = xc; y1_[c] = y; o[c] = y;
            }
        }
    }

private:
    int   C_ = 0;
    float R_ = 1.0f;
    bool  primed_ = false;
    std::vector<float> x1_, y1_;
};
