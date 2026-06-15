#pragma once
// Built-in demo source. Publishes a curated set of synthetic LSL streams on loopback so a
// new user can see every view light up — montage / raster, display filters + CAR, spectrum,
// spectrogram, ERP — with one click, no Python / uv to install. This is a trimmed C++ port
// of tools/lsl_test_streams.py, which stays the fuller tool for testing (high channel
// counts, flaky / dropout streams, mouse, configurable rates, clock skew). Streams here:
//
//   MockEEG      32ch @ 500 Hz   — 10-20 labels + scalp positions; last 2 are ~10x-scale EOG
//                                  (alpha + mains line noise + movement transients / blinks)
//   MockChirp     1ch @ 1000 Hz  — 1 -> 120 Hz sweep (spectrogram)
//   MockAudio     2ch @ 48 kHz   — 440 / 660 Hz stereo tone (spectrum / high-rate stress)
//   MockEvoked    1ch @ 250 Hz   + MockEvokedMarkers — P100 every trial, P300 on "target" (ERP)
//
// One background thread builds the outlets and paces them to their nominal rates (LSL fills
// in per-sample timestamps from the rate, given the time of the last sample in each chunk).

#include <lsl_cpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <thread>
#include <vector>

class MockStreams {
public:
    ~MockStreams() { stop(); }

    bool running() const { return running_.load(std::memory_order_acquire); }

    void start() {
        if (running_.exchange(true)) return;          // already on
        worker_ = std::thread([this] { run(); });
    }
    void stop() {
        if (!running_.exchange(false)) return;         // already off
        if (worker_.joinable()) worker_.join();
    }

private:
    std::atomic<bool> running_{false};
    std::thread       worker_;

    void run();
};

inline void MockStreams::run() {
    using lsl::stream_info;
    using lsl::stream_outlet;
    constexpr double TWO_PI = 6.283185307179586;

    std::mt19937 rng(0xC0FFEEu);
    auto uni   = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto gauss = [&] { return std::normal_distribution<float>(0.0f, 1.0f)(rng); };
    auto chance = [&](double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p; };

    // ---- MockEEG: 32 ch, 10-20 labels + approximate scalp positions (top view, unit disc;
    // nose = +Y, right ear = +X). The last 2 channels are EOG — a different modality on the
    // same montage at ~10x amplitude — tagged so the viewer excludes them from CAR. -------
    constexpr int    EEG_C = 32, N_EOG = 2;
    constexpr double EEG_SR = 500.0, LINE_HZ = 50.0;
    static const char* MONTAGE[EEG_C] = {
        "Fp1","Fp2","F7","F3","Fz","F4","F8","FC5","FC1","FC2","FC6",
        "T7","C3","Cz","C4","T8","CP5","CP1","CP2","CP6","P7","P3",
        "Pz","P4","P8","PO3","PO4","O1","Oz","O2","TP9","TP10" };
    struct Pos { const char* label; float x, y; };
    static const Pos POS[] = {
        {"Fp1",-0.31f,0.95f},{"Fp2",0.31f,0.95f},
        {"F7",-0.81f,0.59f},{"F3",-0.42f,0.62f},{"Fz",0.0f,0.64f},{"F4",0.42f,0.62f},{"F8",0.81f,0.59f},
        {"FC5",-0.64f,0.31f},{"FC1",-0.22f,0.34f},{"FC2",0.22f,0.34f},{"FC6",0.64f,0.31f},
        {"T7",-1.0f,0.0f},{"C3",-0.5f,0.0f},{"Cz",0.0f,0.0f},{"C4",0.5f,0.0f},{"T8",1.0f,0.0f},
        {"CP5",-0.64f,-0.31f},{"CP1",-0.22f,-0.34f},{"CP2",0.22f,-0.34f},{"CP6",0.64f,-0.31f},
        {"P7",-0.81f,-0.59f},{"P3",-0.42f,-0.62f},{"Pz",0.0f,-0.64f},{"P4",0.42f,-0.62f},{"P8",0.81f,-0.59f},
        {"PO3",-0.33f,-0.84f},{"PO4",0.33f,-0.84f},
        {"O1",-0.31f,-0.95f},{"Oz",0.0f,-1.0f},{"O2",0.31f,-0.95f},
        {"TP9",-0.95f,-0.32f},{"TP10",0.95f,-0.32f} };

    std::vector<std::string> eegLabels(EEG_C);
    for (int c = 0; c < EEG_C - N_EOG; ++c) eegLabels[c] = MONTAGE[c];
    for (int j = 0; j < N_EOG; ++j)         eegLabels[EEG_C - N_EOG + j] = "EOG" + std::to_string(j + 1);

    stream_info eegInfo("MockEEG", "EEG", EEG_C, EEG_SR, lsl::cf_float32, "mock-eeg");
    {
        lsl::xml_element chns = eegInfo.desc().append_child("channels");
        for (int c = 0; c < EEG_C; ++c) {
            const bool eog = c >= EEG_C - N_EOG;
            lsl::xml_element ch = chns.append_child("channel");
            ch.append_child_value("label", eegLabels[c]);
            ch.append_child_value("unit", "microvolts");
            ch.append_child_value("type", eog ? "EOG" : "EEG");
            for (const Pos& p : POS)
                if (eegLabels[c] == p.label) {
                    lsl::xml_element loc = ch.append_child("location");
                    loc.append_child_value("X", std::to_string(p.x));
                    loc.append_child_value("Y", std::to_string(p.y));
                    loc.append_child_value("Z", "0.000");
                    break;
                }
        }
    }
    stream_outlet eegOut(eegInfo, int(EEG_SR * 0.02), 360);
    std::vector<float> dc(EEG_C), alphaGain(EEG_C), chanPhase(EEG_C);
    for (int c = 0; c < EEG_C; ++c) { dc[c] = uni(-2000, 2000); alphaGain[c] = 15 + 10 * uni(0, 1); chanPhase[c] = uni(0, TWO_PI); }
    float eogFreq[N_EOG], eogPhase[N_EOG];
    for (int j = 0; j < N_EOG; ++j) { eogFreq[j] = uni(0.2f, 0.6f); eogPhase[j] = uni(0, TWO_PI); }

    // ---- MockChirp: a 1 -> 120 Hz sawtooth sweep, continuous phase across chunks. --------
    constexpr double CHIRP_SR = 1000.0, CHIRP_F0 = 1.0, CHIRP_F1 = 120.0, CHIRP_SWEEP = 8.0;
    stream_info chirpInfo("MockChirp", "Signal", 1, CHIRP_SR, lsl::cf_float32, "mock-chirp");
    chirpInfo.desc().append_child("channels").append_child("channel").append_child_value("label", "chirp");
    stream_outlet chirpOut(chirpInfo);
    double chirpPhase = 0.0;

    // ---- MockAudio: 440 / 660 Hz stereo tone at 48 kHz (high-rate spectrum stress). ------
    constexpr double AUDIO_SR = 48000.0;
    stream_info audioInfo("MockAudio", "Audio", 2, AUDIO_SR, lsl::cf_float32, "mock-audio");
    {
        lsl::xml_element chns = audioInfo.desc().append_child("channels");
        for (const char* lab : {"L", "R"}) chns.append_child("channel").append_child_value("label", lab);
    }
    stream_outlet audioOut(audioInfo, int(AUDIO_SR * 0.02), 360);

    // ---- MockEvoked: a data channel where every "stim" gets a P100 (~120 ms) and "target"
    // trials additionally get a P300 (~300 ms), plus its own marker stream. The ERP window
    // triggers on the markers and averages "target" vs "standard". --------------------------
    constexpr double EV_SR = 250.0;
    stream_info evInfo("MockEvoked", "EEG", 1, EV_SR, lsl::cf_float32, "mock-evoked");
    evInfo.desc().append_child("channels").append_child("channel").append_child_value("label", "Cz");
    stream_outlet evOut(evInfo);
    stream_info evMarkInfo("MockEvokedMarkers", "Markers", 1, lsl::IRREGULAR_RATE, lsl::cf_string, "mock-evoked-markers");
    stream_outlet evMarkOut(evMarkInfo);

    // ---- pacing: stamp each chunk with the time of its last sample; LSL derives the rest -
    const double start = lsl::local_clock();
    long long eegN = 0, chirpN = 0, audioN = 0, evN = 0;
    struct Onset { long long sample; bool target; };
    std::vector<Onset> onsets;
    double nextStim = start + uni(0.6f, 1.0f);

    while (running_.load(std::memory_order_acquire)) {
        const double now = lsl::local_clock();

        // MockEEG
        if (long long want = (long long)((now - start) * EEG_SR); want > eegN) {
            const int n = int(want - eegN);
            std::vector<float> buf((size_t)n * EEG_C);
            for (int s = 0; s < n; ++s) {
                const double t = double(eegN + s) / EEG_SR;
                const float alpha = std::sin(TWO_PI * 10.0 * t);
                const float burst = 0.5f * (1.0f + std::sin(TWO_PI * 0.1 * t));
                const float line  = std::sin(TWO_PI * LINE_HZ * t);
                for (int c = 0; c < EEG_C; ++c)
                    buf[(size_t)s * EEG_C + c] =
                        dc[c] + alpha * burst * alphaGain[c] * std::cos(chanPhase[c]) + line * 8.0f + gauss() * 6.0f;
            }
            // Rare movement transient (~2/s, ~500 uV) on an EEG channel.
            if (chance(2.0 * n / EEG_SR)) {
                const int s = int(uni(0, (float)n)), c = int(uni(0, (float)(EEG_C - N_EOG)));
                buf[(size_t)s * EEG_C + c] += (chance(0.5) ? 1.0f : -1.0f) * uni(400, 800);
            }
            // EOG channels: ~10x scale — slow ocular drift overwrites the EEG content.
            for (int s = 0; s < n; ++s) {
                const double t = double(eegN + s) / EEG_SR;
                for (int j = 0; j < N_EOG; ++j) {
                    const int c = EEG_C - N_EOG + j;
                    buf[(size_t)s * EEG_C + c] = dc[c] + 300.0f * std::sin(TWO_PI * eogFreq[j] * t + eogPhase[j]) + gauss() * 15.0f;
                }
            }
            // Blink: a large (~1.2 mV) decaying deflection shared across the EOG channels, ~1/3 s.
            if (chance((double)n / EEG_SR / 3.0)) {
                const int r = int(uni(0, (float)n));
                const int w = std::max(1, std::min(n - r, int(0.15 * EEG_SR)));
                for (int i = 0; i < w; ++i) {
                    const float shape = 0.5f * (1.0f - std::cos(TWO_PI * (w + i) / (2 * w)));  // hann tail: peak -> 0
                    for (int j = 0; j < N_EOG; ++j)
                        buf[(size_t)(r + i) * EEG_C + (EEG_C - N_EOG + j)] += 1200.0f * shape;
                }
            }
            eegOut.push_chunk_multiplexed(buf, now);
            eegN = want;
        }

        // MockChirp
        if (long long want = (long long)((now - start) * CHIRP_SR); want > chirpN) {
            const int n = int(want - chirpN);
            std::vector<float> buf(n);
            for (int s = 0; s < n; ++s) {
                const double t = double(chirpN + s) / CHIRP_SR;
                const double u = std::fmod(t, CHIRP_SWEEP) / CHIRP_SWEEP;
                const double finst = CHIRP_F0 + (CHIRP_F1 - CHIRP_F0) * u;
                chirpPhase += TWO_PI * finst / CHIRP_SR;
                buf[s] = 100.0f * float(std::sin(chirpPhase));
            }
            chirpPhase = std::fmod(chirpPhase, TWO_PI);
            chirpOut.push_chunk_multiplexed(buf, now);
            chirpN = want;
        }

        // MockAudio
        if (long long want = (long long)((now - start) * AUDIO_SR); want > audioN) {
            const int n = int(want - audioN);
            std::vector<float> buf((size_t)n * 2);
            for (int s = 0; s < n; ++s) {
                const double t = double(audioN + s) / AUDIO_SR;
                buf[(size_t)s * 2 + 0] = 30.0f * float(std::sin(TWO_PI * 440.0 * t));
                buf[(size_t)s * 2 + 1] = 30.0f * float(std::sin(TWO_PI * 660.0 * t));
            }
            audioOut.push_chunk_multiplexed(buf, now);
            audioN = want;
        }

        // MockEvoked (+ markers)
        if (now >= nextStim) {
            const bool target = chance(0.4);
            evMarkOut.push_sample(std::vector<std::string>{ target ? "target" : "standard" }, now);
            onsets.push_back({ evN, target });
            nextStim = now + uni(0.6f, 1.0f);
        }
        if (long long want = (long long)((now - start) * EV_SR); want > evN) {
            const int n = int(want - evN);
            std::vector<float> buf(n);
            for (int s = 0; s < n; ++s) {
                const long long k = evN + s;
                float v = gauss() * 4.0f;                                  // background noise
                for (const Onset& o : onsets) {
                    const double dp1 = (double(k) - (o.sample + 0.12 * EV_SR)) / (0.025 * EV_SR);
                    v += 6.0f * float(std::exp(-0.5 * dp1 * dp1));          // P100
                    if (o.target) {
                        const double dp3 = (double(k) - (o.sample + 0.30 * EV_SR)) / (0.05 * EV_SR);
                        v += 10.0f * float(std::exp(-0.5 * dp3 * dp3));     // P300
                    }
                }
                buf[s] = v;
            }
            evOut.push_chunk_multiplexed(buf, now);
            evN = want;
            const long long cutoff = evN - (long long)EV_SR;               // drop bumps now in the past
            onsets.erase(std::remove_if(onsets.begin(), onsets.end(),
                            [&](const Onset& o) { return o.sample <= cutoff; }), onsets.end());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}
