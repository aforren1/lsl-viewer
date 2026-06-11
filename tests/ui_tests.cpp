// Dear ImGui Test Engine suite for the LSL viewer.
//
// Drives the UI headlessly: `./lsl_viewer --tests` queues every test, runs them
// in fast mode, prints a pass/fail summary, and exits non-zero on failure.
// Capture tests write PNGs to ./output/captures/. Display settings are
// per-stream now, so those tests drive the controls inside each stream window's
// "Display" header (they skip when the stream isn't connected).
//
// Registered from main() via RegisterAppTests().

#include "imgui.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include "filter.hpp"
#include <cmath>
#include <vector>

void RegisterAppTests(ImGuiTestEngine* e) {
    ImGuiTest* t = nullptr;

    // High-pass (DC blocker) correctness: removes DC, preserves passband, and a
    // DC-only input decays to ~0. No UI — pure logic check.
    t = IM_REGISTER_TEST(e, "filter", "highpass_dc_blocker");
    t->TestFunc = [](ImGuiTestContext*) {
        const float  fs = 500.0f, dc = 1000.0f, amp = 10.0f, f = 40.0f;
        const int    N  = 5000;
        const double TWO_PI = 6.283185307179586;
        std::vector<float> in(N), out(N);
        for (int i = 0; i < N; ++i) in[i] = dc + amp * std::sin((float)(TWO_PI * f * i / fs));

        DcBlocker hp; hp.init(1, DcBlocker::cutoffToR(0.5, fs));   // 0.5 Hz cutoff
        hp.process(in.data(), out.data(), (std::size_t)N);

        // After settling, DC (1000) is gone and the 40 Hz tone (>> cutoff) survives.
        double mean = 0.0; float mn = 1e9f, mx = -1e9f;
        for (int i = N / 2; i < N; ++i) { mean += out[i]; mn = std::min(mn, out[i]); mx = std::max(mx, out[i]); }
        mean /= (N / 2);
        IM_CHECK_LT(std::fabs(mean), 1.0);                 // DC removed
        const float pp = mx - mn;                           // ~2*amp = 20
        IM_CHECK_GT(pp, 0.8f * 2.0f * amp);                 // passband preserved
        IM_CHECK_LT(pp, 1.2f * 2.0f * amp);

        // Pure DC input decays toward zero.
        DcBlocker hp2; hp2.init(1, DcBlocker::cutoffToR(0.5, fs));
        std::vector<float> din(N, 500.0f), dout(N);
        hp2.process(din.data(), dout.data(), (std::size_t)N);
        IM_CHECK_LT(std::fabs((double)dout[N - 1]), 1.0);
    };

    // Performance overlay (off by default; shown via View menu) exposes VSync.
    t = IM_REGISTER_TEST(e, "ui", "performance_window");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuCheck("//##MainMenuBar/Debug/Performance");
        ctx->Yield(2);
        ctx->SetRef("//Streams");                  // Performance is a section in the rail now
        IM_CHECK(ctx->ItemExists("VSync"));
    };

    // Screen capture of the browser + performance overlay (SDL_GPU readback).
    t = IM_REGISTER_TEST(e, "ui", "capture_ui");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuCheck("//##MainMenuBar/Debug/Performance");
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow("//Streams", ImGuiCaptureFlags_HideMouseCursor);
    };

    // Overlay/raw vs stacked/high-pass, driven from the stream's own controls.
    t = IM_REGISTER_TEST(e, "ui", "capture_eeg_plot");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(2.5f, 1.0f / 30.0f);   // allow discovery + autoconnect
        if (ctx->GetWindowByRef("//MockEEG") == nullptr) {
            ctx->LogInfo("MockEEG window not present; skipping plot capture");
            return;
        }
        ctx->WindowFocus("//MockEEG"); ctx->Yield(2);   // bring the docked tab forward
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//MockEEG/cfg");  // controls live in a child
        if (cfg.Window == nullptr) { ctx->LogInfo("no cfg child; skipping"); return; }
        ctx->SetRef(cfg.Window);
        ctx->ItemOpen("Display");
        ctx->ItemInputValue("History (s)", 5.0f);
        ctx->SleepNoSkip(5.0f, 1.0f / 30.0f);

        ctx->ItemUncheck("Stacked montage");          // overlay / raw — shows DC bunching
        ctx->ItemUncheck("High-pass");
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", ImGuiCaptureFlags_HideMouseCursor);

        ctx->ItemCheck("Stacked montage");            // stacked / high-pass — the default
        ctx->ItemCheck("High-pass");
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", ImGuiCaptureFlags_HideMouseCursor);
    };

    // Per-channel gain: EOG blow-out, then the Auto-gain fix. Best run with a
    // small channel count: python tools/lsl_test_streams.py --streams eeg --eeg-channels 8
    t = IM_REGISTER_TEST(e, "ui", "capture_eog_gain");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(2.5f, 1.0f / 30.0f);   // allow discovery + autoconnect
        if (ctx->GetWindowByRef("//MockEEG") == nullptr) {
            ctx->LogInfo("MockEEG window not present; skipping gain capture");
            return;
        }
        ctx->WindowFocus("//MockEEG"); ctx->Yield(2);   // bring the docked tab forward
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//MockEEG/cfg");
        if (cfg.Window == nullptr) { ctx->LogInfo("no cfg child; skipping"); return; }
        ctx->SetRef(cfg.Window);
        ctx->ItemOpen("Display");
        ctx->ItemInputValue("History (s)", 5.0f);
        ctx->ItemCheck("Stacked montage");
        ctx->ItemCheck("High-pass");
        ctx->SleepNoSkip(5.0f, 1.0f / 30.0f);

        ctx->ItemOpen("Channel gains");
        ctx->CaptureScreenshotWindow("//MockEEG", ImGuiCaptureFlags_HideMouseCursor); // before
        ctx->ItemClick("Auto");
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", ImGuiCaptureFlags_HideMouseCursor); // after
    };

    // Channel-list pattern filter: typing "EOG" should leave only EOG* channels.
    t = IM_REGISTER_TEST(e, "ui", "capture_chanfilter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(2.5f, 1.0f / 30.0f);
        if (ctx->GetWindowByRef("//MockEEG") == nullptr) {
            ctx->LogInfo("MockEEG not present; skipping");
            return;
        }
        ctx->WindowFocus("//MockEEG"); ctx->Yield(2);   // bring the docked tab forward
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//MockEEG/cfg");
        if (cfg.Window == nullptr) return;
        ctx->SetRef(cfg.Window);
        ctx->ItemInputValue("filter", "EOG");
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", ImGuiCaptureFlags_HideMouseCursor);
        ctx->ItemInputValue("filter", "");   // clear
    };

    // Spectrogram (STFT heatmap). Run with the chirp for a rising diagonal:
    //   python tools/lsl_test_streams.py --streams chirp
    t = IM_REGISTER_TEST(e, "ui", "capture_spectro");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuClick("//##MainMenuBar/View/New spectrogram");
        ctx->Yield(2);
        if (ctx->GetWindowByRef("//Spectrogram 1") == nullptr) {
            ctx->LogInfo("no spectrogram window; skipping");
            return;
        }
        ctx->SleepNoSkip(8.0f, 1.0f / 30.0f);   // accumulate STFT columns
        ctx->CaptureScreenshotWindow("//Spectrogram 1", ImGuiCaptureFlags_HideMouseCursor);
    };

    // Gap-aware spectrogram: a dropout shows as a contiguous red/blanked region
    // (dropout + STFT recovery latency). Run with the flaky stream (disconnects
    // ~3 s every ~8 s); ~13 s catches the first dropout. Captures both the
    // spectrogram and the MockFlaky time series (red band on each).
    //   python tools/lsl_test_streams.py --streams flaky
    t = IM_REGISTER_TEST(e, "ui", "capture_spectro_gap");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (ctx->GetWindowByRef("//MockFlaky") == nullptr) {
            ctx->LogInfo("no flaky stream; skipping gap spectrogram");
            return;
        }
        ctx->MenuClick("//##MainMenuBar/View/New spectrogram");
        ctx->Yield(2);
        ctx->SleepNoSkip(13.0f, 1.0f / 30.0f);   // catch the first dropout (~8 s in)
        ctx->CaptureScreenshotWindow("//Spectrogram 1", ImGuiCaptureFlags_HideMouseCursor);
        ctx->CaptureScreenshotWindow("//MockFlaky", ImGuiCaptureFlags_HideMouseCursor);
    };

    // Spectrogram motion flip-book — a burst of frames ~0.12 s apart to eyeball
    // smooth vs lurching scroll and the live-edge gap. Run with the chirp (moving
    // diagonal makes motion obvious): python tools/lsl_test_streams.py --streams chirp
    t = IM_REGISTER_TEST(e, "ui", "capture_spectro_motion");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuClick("//##MainMenuBar/View/New spectrogram");
        ctx->Yield(2);
        if (ctx->GetWindowByRef("//Spectrogram 1") == nullptr) { ctx->LogInfo("skip"); return; }
        ctx->SleepNoSkip(8.0f, 1.0f / 30.0f);   // accumulate columns
        for (int i = 0; i < 8; ++i) {
            ctx->CaptureScreenshotWindow("//Spectrogram 1", ImGuiCaptureFlags_HideMouseCursor);
            ctx->SleepNoSkip(0.12f, 1.0f / 60.0f);
        }
    };

    // FFT spectrum. Run with the pure-40 Hz sine (and/or chirp):
    //   python tools/lsl_test_streams.py --streams sine,chirp
    t = IM_REGISTER_TEST(e, "ui", "capture_fft");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (ctx->GetWindowByRef("//Spectrum") == nullptr) {
            ctx->LogInfo("Spectrum window not present; skipping");
            return;
        }
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);   // fill >= one FFT window
        ctx->CaptureScreenshotWindow("//Spectrum", ImGuiCaptureFlags_HideMouseCursor);
    };

}
