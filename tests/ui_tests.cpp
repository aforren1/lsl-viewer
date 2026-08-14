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
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <sys/time.h>           // timeval for SO_RCVTIMEO
#endif
#ifdef Yield
#undef Yield                    // winbase.h macro (via winsock2.h) vs ImGuiTestContext::Yield
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
// Sessions are reaped on their own threads, so the client roster the GUI reads settles
// shortly after a connect/disconnect rather than immediately.
static bool rcTestWaitClients(const RemoteControl& rc, std::size_t n) {
    for (int i = 0; i < 100; ++i) {
        if (rc.clients().size() == n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
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

    // Notch + low-pass biquads: the notch zeroes its center tone but passes a tone
    // 20 Hz away; the low-pass passes lows and attenuates a tone well above cutoff.
    t = IM_REGISTER_TEST(e, "filter", "biquad_notch_lowpass");
    t->TestFunc = [](ImGuiTestContext*) {
        const float  fs = 500.0f;
        const int    N  = 6000;
        const double TWO_PI = 6.283185307179586;
        auto tone = [&](float f) {
            std::vector<float> s(N);
            for (int i = 0; i < N; ++i) s[i] = std::sin((float)(TWO_PI * f * i / fs));
            return s;
        };
        auto pp = [&](const std::vector<float>& v) {   // peak-to-peak after settling
            float mn = 1e9f, mx = -1e9f;
            for (int i = N / 2; i < N; ++i) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
            return mx - mn;
        };
        const float full = 2.0f;   // a unit sine has pp = 2

        { auto s = tone(60.0f); Biquad b; b.init(1); b.setNotch(60.0, fs, 30.0);
          b.process(s.data(), (std::size_t)N); IM_CHECK_LT(pp(s), 0.3f * full); }   // 60 Hz removed
        { auto s = tone(40.0f); Biquad b; b.init(1); b.setNotch(60.0, fs, 30.0);
          b.process(s.data(), (std::size_t)N); IM_CHECK_GT(pp(s), 0.8f * full); }   // 40 Hz passes
        { auto s = tone(5.0f);  Biquad b; b.init(1); b.setLowpass(30.0, fs, 0.70710678);
          b.process(s.data(), (std::size_t)N); IM_CHECK_GT(pp(s), 0.8f * full); }   // 5 Hz passes
        { auto s = tone(80.0f); Biquad b; b.init(1); b.setLowpass(30.0, fs, 0.70710678);
          b.process(s.data(), (std::size_t)N); IM_CHECK_LT(pp(s), 0.3f * full); }   // 80 Hz attenuated
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
    // (Winsock on Windows CI, BSD sockets elsewhere). select/start/stop block until
    // a main loop applies them, so this stands in a fake one. See remote_control.hpp.
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

        // Stand-in for the viewer's frame loop: drain the queue and answer each request
        // the way main.cpp does. `pump` gates it so the timeout path can be tested too.
        std::atomic<bool> pump{true}, loopUp{true};
        std::vector<std::string> lastSelect;
        std::string lastFilename;
        std::thread loop([&] {
            while (loopUp) {
                if (pump) {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    if (st.setFilename) { lastFilename = *st.setFilename; st.setFilename.reset(); }
                    for (auto& req : st.queue) {
                        switch (req->kind) {
                        case RcRequest::Kind::Select:
                            lastSelect = req->keys;
                            req->msg = (req->keys.size() == 1 && req->keys[0] == "unknown-key")
                                     ? "error: unknown stream(s): unknown-key (see `streams`)"
                                     : "ok: connected " + std::to_string(req->keys.size()) + " stream(s)";
                            break;
                        case RcRequest::Kind::Start: req->msg = "ok: recording -> " + lastFilename; break;
                        case RcRequest::Kind::Stop:  req->msg = "ok: stopped -> " + lastFilename;   break;
                        }
                        req->done = true;
                    }
                    if (!st.queue.empty()) { st.queue.clear(); st.cv.notify_all(); }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        rc_socket_t fd = rcTestConnect(rc.port());   // the port it actually bound
        IM_CHECK(fd != RC_INVALID);
        if (fd == RC_INVALID) { loopUp = false; loop.join(); rc.stop(); return; }

        IM_CHECK(rcTestRecv(fd).find("remote control") != std::string::npos);   // hello banner

        // The roster the Recording panel shows: one peer, addressed the way the panel
        // prints it.
        IM_CHECK(rcTestWaitClients(rc, 1));
        IM_CHECK(rc.clients().front().rfind("127.0.0.1:", 0) == 0);

        rcTestSend(fd, "status\n");
        IM_CHECK(rcTestRecv(fd).find("recording=false") != std::string::npos);

        rcTestSend(fd, "streams\n");
        const std::string streams = rcTestRecv(fd);
        IM_CHECK(streams.find("mock-eeg") != std::string::npos);
        IM_CHECK(streams.find("mock-acc") != std::string::npos);

        // `select <key>` reaches the main loop as a request, and its reply is the
        // outcome the main loop wrote — not an optimistic ok.
        rcTestSend(fd, "select mock-eeg\n");
        IM_CHECK(rcTestRecv(fd).find("ok: connected 1") != std::string::npos);
        IM_CHECK(lastSelect.size() == 1);
        IM_CHECK_STR_EQ(lastSelect.front().c_str(), "mock-eeg");

        rcTestSend(fd, "select unknown-key\n");   // rejected whole, not half-applied
        IM_CHECK(rcTestRecv(fd).find("error: unknown stream(s)") != std::string::npos);

        // `start <path>` sets the filename first, so the request the loop applies
        // already sees it, and the reply names the file it opened.
        rcTestSend(fd, "start /tmp/rc_unit.xdf\n");
        IM_CHECK(rcTestRecv(fd).find("ok: recording -> /tmp/rc_unit.xdf") != std::string::npos);
        IM_CHECK_STR_EQ(lastFilename.c_str(), "/tmp/rc_unit.xdf");

        rcTestSend(fd, "stop\n");
        IM_CHECK(rcTestRecv(fd).find("ok: stopped") != std::string::npos);

        rcTestSend(fd, "frobnicate\n");        // unknown -> error, connection stays open
        IM_CHECK(rcTestRecv(fd).find("error") != std::string::npos);

        // A second client is served while the first stays connected (one session
        // thread each) — a script and a `nc` session can coexist.
        rc_socket_t fd2 = rcTestConnect(port);
        IM_CHECK(fd2 != RC_INVALID);
        if (fd2 != RC_INVALID) {
            IM_CHECK(rcTestRecv(fd2).find("remote control") != std::string::npos);
            rcTestSend(fd2, "status\n");
            IM_CHECK(rcTestRecv(fd2).find("recording=false") != std::string::npos);
            rcTestSend(fd, "status\n");        // the first connection still works
            IM_CHECK(rcTestRecv(fd).find("recording=false") != std::string::npos);
            IM_CHECK(rcTestWaitClients(rc, 2));
            rc_close(fd2);
            IM_CHECK(rcTestWaitClients(rc, 1));   // and the roster drops it again
        }

        // With no main loop draining the queue, a request must give up rather than
        // park the client forever, and must not stay queued to fire later.
        pump = false;
        rcTestSend(fd, "stop\n");
        std::string late;
        for (int i = 0; i < 4 && late.empty(); ++i) late = rcTestRecv(fd);   // 2 s server-side wait
        IM_CHECK(late.find("did not respond") != std::string::npos);
        { std::lock_guard<std::mutex> lk(st.mtx); IM_CHECK(st.queue.empty()); }
        pump = true;

        rcTestSend(fd, "quit\n");
        IM_CHECK(rcTestRecv(fd).find("bye") != std::string::npos);
        rc_close(fd);
        IM_CHECK(rcTestWaitClients(rc, 0));   // no phantom peers left on the roster

        loopUp = false; loop.join();
        rc.stop();                              // joins the server thread cleanly
        IM_CHECK(!rc.listening());
    };

    // Discovery beacon identity. Two viewers that both use 22345 must not publish the same
    // source_id (LSL would treat them as one logical stream), and the format is a contract:
    // xdf_record skips beacons by the prefix, clients read the port after the last colon.
    t = IM_REGISTER_TEST(e, "remote", "beacon_source_id");
    t->TestFunc = [](ImGuiTestContext*) {
        const std::string sid = rc_beacon_source_id(22345);
        IM_CHECK(sid.rfind("lsl-viewer-rc:", 0) == 0);              // xdf_record's filter
        IM_CHECK_STR_EQ(sid.substr(sid.rfind(':') + 1).c_str(), "22345");
        IM_CHECK(std::count(sid.begin(), sid.end(), ':') == 3);     // prefix:host:pid:port
        IM_CHECK(sid.find(std::to_string(rc_pid())) != std::string::npos);
        IM_CHECK(sid != rc_beacon_source_id(22346));                // the port distinguishes
        IM_CHECK(rc_hostname() != "unknown");                       // and so does the host
    };

    // Two viewers on one host: the second can't have the same port, so start() falls back
    // to an ephemeral one and announces that rather than going without a control port.
    // This also pins down the platform bind semantics — on Windows SO_REUSEADDR would let
    // the second bind SUCCEED on the live port and quietly split the connections, so the
    // no-fallback case failing is the assertion that matters most there.
    t = IM_REGISTER_TEST(e, "remote", "second_instance");
    t->TestFunc = [](ImGuiTestContext*) {
        RemoteState st1, st2;
        RemoteControl rc1, rc2;
        const int port = 22457;
        IM_CHECK(rc1.start(port, &st1));
        if (!rc1.listening()) return;            // bind failed (port busy?) — don't hang
        IM_CHECK_EQ(rc1.port(), port);

        IM_CHECK(!rc2.start(port, &st2, false, /*portFallback=*/false));   // pinned -> just fails
        IM_CHECK(!rc2.listening());
        IM_CHECK(rc2.error().find("bind") != std::string::npos);

        IM_CHECK(rc2.start(port, &st2, false, /*portFallback=*/true));     // default -> moves
        IM_CHECK(rc2.listening());
        if (rc2.listening()) {
            IM_CHECK(rc2.port() != 0);
            IM_CHECK(rc2.port() != port);
            rc_socket_t fd = rcTestConnect(rc2.port());   // and it's reachable there
            IM_CHECK(fd != RC_INVALID);
            if (fd != RC_INVALID) {
                IM_CHECK(rcTestRecv(fd).find("remote control") != std::string::npos);
                rc_close(fd);
            }
        }
        rc2.stop();
        rc1.stop();

        // The port is free again straight after a clean stop: no lingering listener, and
        // the accepted sockets' TIME_WAIT doesn't block a fresh bind.
        RemoteControl rc3;
        IM_CHECK(rc3.start(port, &st1));
        IM_CHECK_EQ(rc3.port(), port);
        rc3.stop();
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

    // ERP raster: channels x time heatmap of the trigger-averaged response. Run the
    // evoked demo so MockEvoked / MockEvokedMarkers are the default ERP stream+trigger
    // (index 0); the 32-ch MockEEG also feeds the multi-channel views.
    //   python tools/lsl_test_streams.py --streams eeg,evoked
    t = IM_REGISTER_TEST(e, "ui", "capture_erp_raster");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);    // wait for discovery + autoconnect
        if (ctx->GetWindowByRef("//MockEvoked") == nullptr) { ctx->LogInfo("no evoked stream; skip"); return; }
        ctx->MenuClick("//##MainMenuBar/View/New ERP (marker average)");
        ctx->Yield(2);
        if (ctx->GetWindowByRef("//ERP 1") == nullptr) { ctx->LogInfo("no ERP window; skip"); return; }
        ctx->WindowFocus("//ERP 1"); ctx->Yield(2);
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//ERP 1/cfg");   // controls live in a left child
        if (cfg.Window == nullptr) { ctx->LogInfo("no cfg child; skip"); return; }
        ctx->SetRef(cfg.Window);
        ctx->SleepNoSkip(25.0f, 1.0f / 30.0f);   // accumulate ~30 epochs (single channel + spaghetti)
        ctx->CaptureScreenshotWindow("//ERP 1", ImGuiCaptureFlags_HideMouseCursor);  // 1) single-ch lines + spaghetti
        ctx->ItemCheck("all channels");
        ctx->SleepNoSkip(8.0f, 1.0f / 30.0f);    // refill averages for every channel
        ctx->CaptureScreenshotWindow("//ERP 1", ImGuiCaptureFlags_HideMouseCursor);  // 2) multi-ch average lines
        ctx->ItemCheck("raster");
        ctx->MouseMoveToPos(ImVec2(5, 5));       // park cursor off the controls (no hover tooltip)
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//ERP 1", ImGuiCaptureFlags_HideMouseCursor);  // 3) channels x time raster
    };
}
