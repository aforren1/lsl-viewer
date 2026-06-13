#pragma once
// Per-channel one-pole DC blocker (running high-pass):
//     y[n] = x[n] - x[n-1] + R * y[n-1],   R = exp(-2*pi*fc/fs)
// Primed on the first sample so a large DC offset doesn't ring in. Standalone
// (no SDL/LSL) so it can be unit-tested directly.

#include <algorithm>
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
    // Re-prime on the next chunk (used when the stage is re-enabled after a bypass, so it
    // starts clean from the current signal instead of ringing on stale x1_/y1_).
    void reset() { primed_ = false; }

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

// Per-channel RBJ biquad (Direct-Form II transposed: 2 state words/channel, good
// numerics). Used for the optional notch (mains 50/60 Hz) and low-pass stages of the
// display filter. Coefficients are recomputed by the producer when the UI changes
// them; the z-state is producer-only. Standalone (no SDL/LSL) for unit testing.
struct Biquad {
    void init(int channels) {
        C_ = channels;
        z1_.assign(channels, 0.0f);
        z2_.assign(channels, 0.0f);
    }
    void reset() { std::fill(z1_.begin(), z1_.end(), 0.0f); std::fill(z2_.begin(), z2_.end(), 0.0f); }

    // RBJ cookbook low-pass at fc (Hz), quality Q (0.7071 = Butterworth/maximally flat).
    void setLowpass(double fc, double fs, double Q) {
        if (fs <= 0.0 || fc <= 0.0) { setIdentity(); return; }
        fc = std::min(fc, 0.45 * fs);                 // keep below Nyquist
        const double w0 = 2.0 * std::numbers::pi * fc / fs;
        const double cw = std::cos(w0), sw = std::sin(w0), a = sw / (2.0 * Q);
        const double b0 = (1.0 - cw) * 0.5, b1 = 1.0 - cw, b2 = (1.0 - cw) * 0.5;
        const double a0 = 1.0 + a, a1 = -2.0 * cw, a2 = 1.0 - a;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }
    // RBJ cookbook band-reject (notch) at f0 (Hz); higher Q = narrower notch.
    void setNotch(double f0, double fs, double Q) {
        if (fs <= 0.0 || f0 <= 0.0 || f0 >= 0.5 * fs) { setIdentity(); return; }
        const double w0 = 2.0 * std::numbers::pi * f0 / fs;
        const double cw = std::cos(w0), sw = std::sin(w0), a = sw / (2.0 * Q);
        const double b0 = 1.0, b1 = -2.0 * cw, b2 = 1.0;
        const double a0 = 1.0 + a, a1 = -2.0 * cw, a2 = 1.0 - a;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }

    // Filter n interleaved samples (C channels) in place.
    void process(float* x, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            float* s = x + i * (std::size_t)C_;
            for (int c = 0; c < C_; ++c) {
                const float in = s[c];
                const float y  = b0_ * in + z1_[c];
                z1_[c] = b1_ * in - a1_ * y + z2_[c];
                z2_[c] = b2_ * in - a2_ * y;
                s[c] = y;
            }
        }
    }

    bool enabled = false;   // owner gates process() on this

private:
    void setIdentity() { b0_ = 1.0f; b1_ = b2_ = a1_ = a2_ = 0.0f; reset(); }
    void setCoeffs(double b0, double b1, double b2, double a0, double a1, double a2) {
        b0_ = (float)(b0 / a0); b1_ = (float)(b1 / a0); b2_ = (float)(b2 / a0);
        a1_ = (float)(a1 / a0); a2_ = (float)(a2 / a0);
    }
    int   C_ = 0;
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    std::vector<float> z1_, z2_;
};
