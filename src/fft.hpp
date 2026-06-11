#pragma once
// Self-contained power spectral density for the FFT view.
//
// One-sided PSD of a strided channel: detrend (remove mean) -> Hann window ->
// radix-2 FFT -> |X|^2 with Welch scaling. Self-contained (no dependency) and
// plenty fast for a handful of channels at a few kHz; swap in pffft/KissFFT
// later if many channels or a spectrogram need it. N must be a power of two.

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

class Psd {
public:
    void init(int N, float fs) {
        N_ = N; fs_ = fs;
        re_.assign(N, 0.0f);
        im_.assign(N, 0.0f);
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
        double mean = 0.0;
        for (int i = 0; i < N_; ++i) mean += src[(std::size_t)i * stride];
        mean /= N_;
        for (int i = 0; i < N_; ++i) {
            re_[i] = (float)(src[(std::size_t)i * stride] - mean) * win_[i];
            im_[i] = 0.0f;
        }
        fft(re_, im_);
        out.resize(bins());
        const float norm = 2.0f / (fs_ * winPow_);     // Welch one-sided
        for (int k = 0; k < bins(); ++k) {
            float p = (re_[k] * re_[k] + im_[k] * im_[k]) * norm;
            if (k == 0 || k == N_ / 2) p *= 0.5f;       // DC/Nyquist not doubled
            out[k] = p;
        }
    }

private:
    static constexpr float kPi = std::numbers::pi_v<float>;

    // In-place iterative radix-2 Cooley-Tukey (incremental twiddles).
    static void fft(std::vector<float>& re, std::vector<float>& im) {
        const int n = (int)re.size();
        for (int i = 1, j = 0; i < n; ++i) {            // bit reversal
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const float ang = -2.0f * kPi / len;
            const float wr = std::cos(ang), wi = std::sin(ang);
            for (int i = 0; i < n; i += len) {
                float cwr = 1.0f, cwi = 0.0f;
                for (int k = 0; k < len / 2; ++k) {
                    const int a = i + k, b = i + k + len / 2;
                    const float vr = re[b] * cwr - im[b] * cwi;
                    const float vi = re[b] * cwi + im[b] * cwr;
                    re[b] = re[a] - vr; im[b] = im[a] - vi;
                    re[a] += vr;        im[a] += vi;
                    const float nwr = cwr * wr - cwi * wi;
                    cwi = cwr * wi + cwi * wr; cwr = nwr;
                }
            }
        }
    }

    int   N_ = 0;
    float fs_ = 0.0f, winPow_ = 0.0f;
    std::vector<float> re_, im_, win_;
};
