#pragma once
// Power spectral density for the Spectrum / Spectrogram views.
//
// One-sided PSD of a strided channel: detrend (remove mean) -> Hann window ->
// real FFT -> |X|^2 with Welch scaling. Uses KissFFT (BSD-3) for the transform;
// it was a hand-rolled radix-2 before. N must be even (we only use powers of two).

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include <kiss_fftr.h>   // KISSFFT_DATATYPE=float -> kiss_fft_scalar is float

class Psd {
public:
    Psd() = default;
    ~Psd() { if (cfg_) kiss_fftr_free(cfg_); }
    Psd(const Psd&) = delete;             // owns a malloc'd kiss config
    Psd& operator=(const Psd&) = delete;

    void init(int N, float fs) {
        N_ = N; fs_ = fs;
        if (cfg_) kiss_fftr_free(cfg_);
        cfg_ = kiss_fftr_alloc(N, /*inverse=*/0, nullptr, nullptr);  // real -> complex
        in_.assign(N, 0.0f);
        spec_.assign(N / 2 + 1, {});
        win_.resize(N);
        winPow_ = 0.0f;
        for (int i = 0; i < N; ++i) {                 // Hann
            win_[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * i / (N - 1)));
            winPow_ += win_[i] * win_[i];
        }
    }

    int   size()   const { return N_; }
    float fs()     const { return fs_; }
    int   bins()   const { return N_ / 2 + 1; }        // one-sided
    float binHz(int k) const { return (float)k * fs_ / (float)N_; }

    // Most-recent N samples of one channel: src[i*stride], i in [0, N).
    // Writes bins() power values into out.
    void compute(const float* src, int stride, std::vector<float>& out) {
        if (!cfg_) return;
        double mean = 0.0;
        for (int i = 0; i < N_; ++i) mean += src[(std::size_t)i * stride];
        mean /= N_;
        for (int i = 0; i < N_; ++i)
            in_[i] = (float)(src[(std::size_t)i * stride] - mean) * win_[i];
        kiss_fftr(cfg_, in_.data(), spec_.data());     // unnormalized, same convention as before
        out.resize(bins());
        const float norm = 2.0f / (fs_ * winPow_);     // Welch one-sided
        for (int k = 0; k < bins(); ++k) {
            float p = (spec_[k].r * spec_[k].r + spec_[k].i * spec_[k].i) * norm;
            if (k == 0 || k == N_ / 2) p *= 0.5f;       // DC/Nyquist not doubled
            out[k] = p;
        }
    }

private:
    static constexpr float kPi = std::numbers::pi_v<float>;

    int   N_ = 0;
    float fs_ = 0.0f, winPow_ = 0.0f;
    kiss_fftr_cfg              cfg_ = nullptr;
    std::vector<float>         in_, win_;
    std::vector<kiss_fft_cpx>  spec_;
};
