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
#include "fft.hpp"               // Psd (KissFFT-backed) under test
#include "remote_control.hpp"   // TCP control server under test (+ its rc_socket_t layer)
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/time.h>           // timeval for SO_RCVTIMEO
#endif

// ---- Tiny loopback TCP client for the remote-control roundtrip test ---------
// Uses the same rc_socket_t aliases as the server (Winsock on Windows, BSD
// elsewhere). RemoteControl::start() has already done WSAStartup on Windows.
static rc_socket_t rcTestConnect(int port) {
    rc_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == RC_INVALID) return RC_INVALID;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(0x7F000001);   // 127.0.0.1 (avoids inet_pton portability)
    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { rc_close(fd); return RC_INVALID; }
#if defined(_WIN32)
    DWORD tv = 2000;            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv{2, 0};          ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
    return fd;
}
static void rcTestSend(rc_socket_t fd, const std::string& s) { ::send(fd, s.data(), (int)s.size(), 0); }
static std::string rcTestRecv(rc_socket_t fd) {   // one reply burst (small, single loopback segment)
    char buf[2048];
    const int n = (int)::recv(fd, buf, (int)sizeof(buf), 0);
    return (n > 0) ? std::string(buf, (std::size_t)n) : std::string();
}

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

    // PSD correctness (KissFFT backend): a pure 40 Hz sine must peak in the 40 Hz
    // bin and dwarf an off-tone bin. Guards the FFT scaling/packing. No UI.
    t = IM_REGISTER_TEST(e, "fft", "psd_peak");
    t->TestFunc = [](ImGuiTestContext*) {
        const float  fs = 500.0f, f0 = 40.0f;
        const int    N  = 1024;
        const double TWO_PI = 6.283185307179586;
        Psd psd; psd.init(N, fs);
        std::vector<float> sig(N);
        for (int i = 0; i < N; ++i) sig[i] = std::sin((float)(TWO_PI * f0 * i / fs));
        std::vector<float> out;
        psd.compute(sig.data(), /*stride=*/1, out);
        IM_CHECK_EQ((int)out.size(), N / 2 + 1);
        int peak = 0;
        for (int k = 1; k < (int)out.size(); ++k) if (out[k] > out[peak]) peak = k;
        IM_CHECK_LT(std::fabs(psd.binHz(peak) - f0), fs / N + 1.0f);   // within a bin of 40 Hz
        const int kFar = (int)(150.0f * N / fs);                       // a 150 Hz bin
        IM_CHECK_GT(out[peak], 100.0f * out[kFar]);                    // tone dominates
    };

    // Remote-control server roundtrip: start the TCP server, drive it from a
    // loopback client, and assert both the text replies AND the RemoteState the
    // server hands back to the main loop. No UI — exercises the real socket path
    // (Winsock on Windows CI, BSD sockets elsewhere). See remote_control.hpp.
    t = IM_REGISTER_TEST(e, "remote", "roundtrip");
    t->TestFunc = [](ImGuiTestContext*) {
        RemoteState st;
        st.statusText  = "recording=false file=x.xdf seconds=0.0 streams=0 bytes=0";
        st.streamsText = "mock-eeg | MockEEG | EEG | 8ch | 500\n"
                         "mock-acc | MockAcc | ACC | 3ch | 100\n";
        RemoteControl rc;
        const int port = 22456;                 // SO_REUSEADDR set, so re-runs rebind fine
        IM_CHECK(rc.start(port, &st));
        if (!rc.listening()) return;            // bind failed (port busy?) — don't hang

        rc_socket_t fd = rcTestConnect(port);
        IM_CHECK(fd != RC_INVALID);
        if (fd == RC_INVALID) { rc.stop(); return; }

        IM_CHECK(rcTestRecv(fd).find("remote control") != std::string::npos);   // hello banner

        rcTestSend(fd, "status\n");
        IM_CHECK(rcTestRecv(fd).find("recording=false") != std::string::npos);

        rcTestSend(fd, "streams\n");
        const std::string streams = rcTestRecv(fd);
        IM_CHECK(streams.find("mock-eeg") != std::string::npos);
        IM_CHECK(streams.find("mock-acc") != std::string::npos);

        // `select <key>` must mutate RemoteState.setSelection (set under st.mtx by
        // the server thread; the reply is sent after, so it's visible once we read it).
        rcTestSend(fd, "select mock-eeg\n");
        IM_CHECK(rcTestRecv(fd).find("ok") != std::string::npos);
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            IM_CHECK(st.setSelection.has_value());
            IM_CHECK(st.setSelection->size() == 1);
            IM_CHECK_STR_EQ(st.setSelection->front().c_str(), "mock-eeg");
        }

        rcTestSend(fd, "start /tmp/rc_unit.xdf\n");
        IM_CHECK(rcTestRecv(fd).find("starting") != std::string::npos);
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            IM_CHECK(st.startReq);
            IM_CHECK(st.setFilename.has_value());
            IM_CHECK_STR_EQ(st.setFilename->c_str(), "/tmp/rc_unit.xdf");
        }

        rcTestSend(fd, "stop\n");
        IM_CHECK(rcTestRecv(fd).find("stopping") != std::string::npos);
        { std::lock_guard<std::mutex> lk(st.mtx); IM_CHECK(st.stopReq); }

        rcTestSend(fd, "frobnicate\n");        // unknown -> error, connection stays open
        IM_CHECK(rcTestRecv(fd).find("error") != std::string::npos);

        rcTestSend(fd, "quit\n");
        IM_CHECK(rcTestRecv(fd).find("bye") != std::string::npos);
        rc_close(fd);

        rc.stop();                              // joins the server thread cleanly
        IM_CHECK(!rc.listening());
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
