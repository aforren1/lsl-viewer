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
#include "heatmap_image.hpp"     // heatmaps as one textured quad, under test
#include "remote_control.hpp"   // TCP control server under test (+ its rc_socket_t layer)
#include "xdf_player.hpp"       // XdfPlayer::State, for the replay test
#include "xdf_writer.hpp"       // writes the replay test's file
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>   // strtoul
#include <cstring>   // strstr
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <random>
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
// Reads until `buf` holds `bytes` bytes (the receive timeout ends a short reply). A
// counted reply or a `get` body can span several segments, so one recv() is not enough.
static bool rcTestFill(rc_socket_t fd, std::string& buf, std::size_t bytes) {
    char tmp[2048];
    while (buf.size() < bytes) {
        const int n = (int)::recv(fd, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (std::size_t)n);
    }
    return true;
}
static bool rcTestFillLines(rc_socket_t fd, std::string& buf, std::size_t lines) {
    while ((std::size_t)std::count(buf.begin(), buf.end(), '\n') < lines)
        if (!rcTestFill(fd, buf, buf.size() + 1)) return false;
    return true;
}
// A counted reply read the way a client must: the header, then exactly the N lines it
// announces. `rest` is whatever came after them, and must be empty.
struct RcTestCounted { std::string head; std::size_t n = 0; std::vector<std::string> lines; std::string rest; };
static RcTestCounted rcTestCounted(rc_socket_t fd) {
    RcTestCounted r;
    std::string buf;
    if (!rcTestFillLines(fd, buf, 1)) { r.rest = buf; return r; }
    std::size_t at = buf.find('\n');
    r.head = buf.substr(0, at++);
    if (r.head.rfind("ok: ", 0) == 0) r.n = std::strtoul(r.head.c_str() + 4, nullptr, 10);
    rcTestFillLines(fd, buf, 1 + r.n);
    for (std::size_t i = 0, nl; i < r.n && (nl = buf.find('\n', at)) != std::string::npos; ++i, at = nl + 1)
        r.lines.push_back(buf.substr(at, nl - at));
    r.rest = buf.substr(at);
    return r;
}

// Replay hooks in main.cpp: open a file the way the Playback > Open XDF file dialog does,
// and read back the replay's state, its streams, and the dropouts and break marks of the
// streams it shows.
extern void lslViewerRequestReplay(const char* path);
extern void lslViewerReplayProbe(int& state, double& position, double& length, int& streams, int& shown,
                                 unsigned long long& dropouts, int& breakMarks, std::size_t& breaks);
struct ReplayProbeView {
    int state = -1; double position = 0.0, length = 0.0; int streams = 0, shown = 0;
    unsigned long long dropouts = 0; int breakMarks = 0; std::size_t breaks = 0;
};
static ReplayProbeView replayProbe() {
    ReplayProbeView p;
    lslViewerReplayProbe(p.state, p.position, p.length, p.streams, p.shown, p.dropouts, p.breakMarks, p.breaks);
    return p;
}

// A 30 s recording: a 2-channel 100 Hz sine and a string marker every 2 s. Small enough to
// write in a few milliseconds, long enough to seek in.
static void writeReplayTestFile(const std::string& path) {
    xdf::Writer w(path);
    w.stream_header(1, "<?xml version=\"1.0\"?><info><name>ReplayTestSine</name><type>EEG</type>"
                       "<channel_count>2</channel_count><nominal_srate>100</nominal_srate>"
                       "<channel_format>float32</channel_format><source_id>replaytest-sine</source_id></info>");
    w.stream_header(2, "<?xml version=\"1.0\"?><info><name>ReplayTestMarkers</name><type>Markers</type>"
                       "<channel_count>1</channel_count><nominal_srate>0</nominal_srate>"
                       "<channel_format>string</channel_format><source_id>replaytest-markers</source_id></info>");
    const double t0 = 1000.0;
    std::vector<double> ts(100);
    std::vector<float>  v(200);
    for (int c = 0; c < 30; ++c) {
        for (int i = 0; i < 100; ++i) {
            const int k = c * 100 + i;
            ts[i] = t0 + k / 100.0;
            v[2 * i]     = 50.0f * std::sin(6.2831853f * 5.0f * (float)k / 100.0f);
            v[2 * i + 1] = 30.0f * std::sin(6.2831853f * 11.0f * (float)k / 100.0f);
        }
        w.data_chunk(1, ts, v.data(), 100, 2);
        if (c % 2 == 0) {
            const std::vector<double> mts{t0 + c};
            const std::string m = "m" + std::to_string(c);
            w.data_chunk(2, mts, &m, 1, 1);
        }
        if (c % 10 == 0) { w.clock_offset(1, t0 + c, 0.0); w.clock_offset(2, t0 + c, 0.0); w.boundary(); }
    }
    w.stream_footer(1, "<?xml version=\"1.0\"?><info><first_timestamp>1000</first_timestamp>"
                       "<last_timestamp>1029.99</last_timestamp><sample_count>3000</sample_count></info>");
    w.stream_footer(2, "<?xml version=\"1.0\"?><info><first_timestamp>1000</first_timestamp>"
                       "<last_timestamp>1028</last_timestamp><sample_count>15</sample_count></info>");
}

// ERP hook in main.cpp: the newest ERP window's id, its bound streams, the events its
// trigger stream has received, and the epochs it averaged or left out.
extern void lslViewerErpProbe(int& id, std::string& stream, std::string& trigger, std::size_t& markerEvents,
                              int& count, int& rejected);
struct ErpProbeView { int id = 0, count = 0, rejected = 0; std::size_t markerEvents = 0; std::string stream, trigger; };
static ErpProbeView erpProbe() {
    ErpProbeView p;
    lslViewerErpProbe(p.id, p.stream, p.trigger, p.markerEvents, p.count, p.rejected);
    return p;
}

// A 20 s recording with a 2 s dropout, for the ERP: a 100 Hz sine with no samples in
// [8, 10) s, and markers placed to test each way an epoch can meet the gap. With the default
// epoch (-100 ms, +500 ms):
//   4, 5, 6, 7, 11 ... 18   clear of the gap: averaged (12)
//   7.7                     runs into the gap (data ends at 7.99): left out
//   9.5                     inside the gap: maps to the first sample after it, so its baseline
//                           lies before the gap: left out. Mapped as if no gap followed, it would
//                           land on data recorded 1.5 s after the resumption and be averaged.
// No marker before 4 s, so the test can bind the ERP before the trigger stream has any event.
static constexpr int kErpGapAveraged = 12, kErpGapLeftOut = 2;
static void writeErpGapFile(const std::string& path) {
    xdf::Writer w(path);
    w.stream_header(1, "<?xml version=\"1.0\"?><info><name>ReplayErpSine</name><type>EEG</type>"
                       "<channel_count>1</channel_count><nominal_srate>100</nominal_srate>"
                       "<channel_format>float32</channel_format><source_id>replayerp-sine</source_id></info>");
    w.stream_header(2, "<?xml version=\"1.0\"?><info><name>ReplayErpMarkers</name><type>Markers</type>"
                       "<channel_count>1</channel_count><nominal_srate>0</nominal_srate>"
                       "<channel_format>string</channel_format><source_id>replayerp-markers</source_id></info>");
    const double t0 = 2000.0;
    auto marker = [&](double t) {
        const std::vector<double> mts{t0 + t};
        const std::string m = "e";
        w.data_chunk(2, mts, &m, 1, 1);
    };
    std::vector<double> ts(100);
    std::vector<float>  v(100);
    for (int c = 0; c < 20; ++c) {
        if (c % 10 == 0) { w.clock_offset(1, t0 + c, 0.0); w.clock_offset(2, t0 + c, 0.0); w.boundary(); }
        if (c == 8 || c == 9) continue;                 // the dropout
        for (int i = 0; i < 100; ++i) {
            const int k = c * 100 + i;
            ts[i] = t0 + k / 100.0;
            v[i]  = 50.0f * std::sin(6.2831853f * 5.0f * (float)k / 100.0f);
        }
        w.data_chunk(1, ts, v.data(), 100, 1);
        if (c >= 4 && c <= 7) marker(c);
        if (c == 7)           { marker(7.7); marker(9.5); }
        if (c >= 11 && c <= 18) marker(c);
    }
    w.stream_footer(1, "<?xml version=\"1.0\"?><info><first_timestamp>2000</first_timestamp>"
                       "<last_timestamp>2019.99</last_timestamp><sample_count>1800</sample_count></info>");
    w.stream_footer(2, "<?xml version=\"1.0\"?><info><first_timestamp>2004</first_timestamp>"
                       "<last_timestamp>2018</last_timestamp><sample_count>14</sample_count></info>");
}

// Stream hooks in main.cpp: connect a stream by source_id, and read a connected stream's
// state by name (false if it is not connected).
extern void lslViewerRequestConnect(const char* sourceId);
extern bool lslViewerStreamProbe(const char* name, bool& anchored, unsigned long long& dropouts, double& gapSec,
                                 double& edge, double& newest, double& dt, double& rate, double& speed);
struct StreamProbeView { bool found = false, anchored = false; unsigned long long dropouts = 0; double gapSec = 0.0;
                         double edge = 0.0, newest = 0.0, dt = 0.0, rate = 0.0, speed = 0.0; };
static StreamProbeView streamProbe(const char* name) {
    StreamProbeView p;
    p.found = lslViewerStreamProbe(name, p.anchored, p.dropouts, p.gapSec, p.edge, p.newest, p.dt, p.rate, p.speed);
    return p;
}

// Spectrum windows are numbered as they open and never renumbered, so which ones exist
// depends on the tests before. Returns how many are open; *newest gets the ref of the
// highest-numbered one. WasActive, not Active: the test runs after NewFrame clears Active.
// `prefix` selects the window family ("Spectrogram " for spectrograms).
static int openSpectra(std::string* newest = nullptr, const char* prefix = "Spectrum ") {
    int n = 0, best = 0;
    const std::size_t plen = std::strlen(prefix);
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        // Docked windows are child windows of the dock host, so skip children by name.
        if (!w->WasActive || std::strncmp(w->Name, prefix, plen) != 0 || std::strchr(w->Name, '/')) continue;
        ++n;
        if (const int id = std::atoi(w->Name + plen); id > best) {
            best = id;
            if (newest) *newest = std::string("//") + w->Name;
        }
    }
    return n;
}

extern std::string lslViewerAppSettingsOnly(const std::string& ini);
extern std::string lslViewerLayoutOnly(const std::string& ini);

void RegisterAppTests(ImGuiTestEngine* e) {
    ImGuiTest* t = nullptr;

    // Launch reads only the app's own section of imgui.ini, so the layout always starts from
    // the default while the theme and recording settings persist; workspaces take the rest.
    // CRLF too: a hand-edited file on Windows. No UI.
    t = IM_REGISTER_TEST(e, "settings", "launch_skips_layout");
    t->TestFunc = [](ImGuiTestContext*) {
        const std::string ini =
            "[Window][Spectrum]\nPos=10,20\nSize=300,200\nDockId=0x00000002,0\n\n"
            "[LSLViewer][State]\r\nlight=1\r\nrectmpl=sub-{subject}/x.xdf\r\nstreamkind=1 a|b\r\n\r\n"
            "[Table][0x1234ABCD,3]\nColumn 0  Width=80\n\n"
            "[Docking][Data]\nDockSpace ID=0x8B93E3BD Window=0xA787BDB4 Pos=280,19 Size=1000,781 Split=Y\n";
        const std::string app = lslViewerAppSettingsOnly(ini);
        IM_CHECK(app.find("[LSLViewer][State]") == 0);
        IM_CHECK(app.find("light=1") != std::string::npos);
        IM_CHECK(app.find("rectmpl=sub-{subject}/x.xdf") != std::string::npos);
        IM_CHECK(app.find("streamkind=1 a|b") != std::string::npos);
        IM_CHECK(app.find("[Window]") == std::string::npos);
        IM_CHECK(app.find("DockId=") == std::string::npos);
        IM_CHECK(app.find("[Table]") == std::string::npos);
        IM_CHECK(app.find("[Docking]") == std::string::npos);
        IM_CHECK(app.find("DockSpace") == std::string::npos);
        IM_CHECK(lslViewerAppSettingsOnly("").empty());

        // The inverse is what a workspace saves and loads: the layout without the app settings.
        const std::string layout = lslViewerLayoutOnly(ini);
        IM_CHECK(layout.find("[Window][Spectrum]") == 0);
        IM_CHECK(layout.find("DockId=0x00000002,0") != std::string::npos);
        IM_CHECK(layout.find("[Table]") != std::string::npos);
        IM_CHECK(layout.find("DockSpace") != std::string::npos);
        IM_CHECK(layout.find("[LSLViewer]") == std::string::npos);
        IM_CHECK(layout.find("light=") == std::string::npos);
        IM_CHECK(layout.find("rectmpl=") == std::string::npos);
    };

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

    // The texture heatmap colors every value as PlotHeatmap does: the same colormap table entry,
    // including values clamped at either end of the scale, for each colormap the views use.
    // A NaN takes the low end. No UI.
    t = IM_REGISTER_TEST(e, "heat", "colorscale");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_UNUSED(ctx);
        ImPlotContext& gp = *GImPlot;
        for (ImPlotColormap cm : { (ImPlotColormap)ImPlotColormap_Plasma, (ImPlotColormap)ImPlotColormap_Viridis,
                                   (ImPlotColormap)ImPlotColormap_Deep }) {   // Deep: a qualitative map
            ImPlot::PushColormap(cm);
            const double lo = -40.0, hi = 20.0;
            const heat::ColorScale cs(lo, hi);
            int bad = 0;
            for (int i = -200; i <= 1200; ++i) {
                const double v = lo + (hi - lo) * i / 1000.0;
                const float  t01 = ImClamp((float)((v - lo) / (hi - lo)), 0.0f, 1.0f);   // as GetterHeatmap
                if (cs(v) != gp.ColormapData.LerpTable(cm, t01)) ++bad;
            }
            IM_CHECK_EQ(bad, 0);
            IM_CHECK_EQ(cs(std::nan("")), gp.ColormapData.GetTableColor(cm, 0));
            ImPlot::PopColormap();
        }
    };

    // The texture heatmap next to PlotHeatmap on the same values: a gradient, one row past each
    // end of the scale, and a checkerboard that shows the cell edges. Captured for a visual check.
    t = IM_REGISTER_TEST(e, "heat", "capture_compare");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        IM_UNUSED(ctx);
        constexpr int R = 6, C = 24;
        static float v[R * C];
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < C; ++c)
                v[r * C + c] = r == 0 ? -0.5f : r == 1 ? 1.5f : r == 2 ? (float)((r + c) % 2)
                                                              : (float)c / (C - 1) * (float)(r - 2) / 3.0f;
        ImGui::SetNextWindowSize(ImVec2(660, 280), ImGuiCond_Always);
        ImGui::Begin("Heat compare", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImPlot::PushColormap(ImPlotColormap_Plasma);
        const ImPlotFlags      pf = ImPlotFlags_CanvasOnly;
        const ImPlotAxisFlags  af = ImPlotAxisFlags_NoDecorations;
        if (ImPlot::BeginPlot("##quads", ImVec2(310, 230), pf)) {
            ImPlot::SetupAxes(nullptr, nullptr, af, af);
            ImPlot::SetupAxesLimits(0, 1, 0, 1, ImPlotCond_Always);
            ImPlot::PlotHeatmap("##q", v, R, C, 0.0, 1.0, nullptr, ImPlotPoint(0, 0), ImPlotPoint(1, 1));
            ImPlot::EndPlot();
        }
        ImGui::SameLine();
        if (ImPlot::BeginPlot("##texture", ImVec2(310, 230), pf)) {
            ImPlot::SetupAxes(nullptr, nullptr, af, af);
            ImPlot::SetupAxesLimits(0, 1, 0, 1, ImPlotCond_Always);
            heat::plotValues("##t", v, R, C, 0.0, 1.0, ImPlotPoint(0, 0), ImPlotPoint(1, 1));
            ImPlot::EndPlot();
        }
        ImPlot::PopColormap();
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//Heat compare", ImGuiCaptureFlags_HideMouseCursor);
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
    // server hands back to the main loop. No UI: exercises the real socket path
    // (Winsock on Windows CI, BSD sockets elsewhere). select/start/stop block until
    // a main loop applies them, so this stands in a fake one. The bodies are built with
    // the same helpers main.cpp publishes with, so their format is under test too. Replies
    // that LabRecorder RCS clients parse are compared byte for byte. See remote_control.hpp.
    t = IM_REGISTER_TEST(e, "remote", "roundtrip");
    t->TestFunc = [](ImGuiTestContext*) {
        RemoteState st;
        const std::string spaced = "C:/rc data/sub 01 run.xdf";   // file= must survive spaces
        rc_status_text(st.statusText, false, 0.0, 0, 0, spaced);
        // A '|' in a key is legal (keys are "<source_id>|<name>"); a newline in a name must
        // not cost the reply its framing.
        rc_append_stream_line(st.streamsText, "mock-eeg", "MockEEG", "EEG", 8, 500.0, false);
        rc_append_stream_line(st.streamsText, "src-7|MockAcc", "MockAcc", "ACC", 3, 0.0, true);
        rc_append_stream_line(st.streamsText, "mock-x", "Bad\nName", "Misc", 1, 100.0, false);
        rc_append_line(st.selectedText, "src-7|MockAcc");
        { std::string s; rc_status_text(s, true, 12.34, 2, 5000000000ULL, "a\nb");   // > 32-bit bytes
          IM_CHECK_STR_EQ(s.c_str(), "recording=true seconds=12.3 streams=2 bytes=5000000000 file=a b"); }
        RemoteControl rc;
        const int port = 22456;                 // SO_REUSEADDR set, so re-runs rebind fine
        IM_CHECK(rc.start(port, &st));
        if (!rc.listening()) return;            // bind failed (port busy?): don't hang

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
        // A failed IM_CHECK returns from this lambda; a still-joinable std::thread would
        // then call std::terminate and take the whole suite down instead of one test.
        struct LoopJoin { std::atomic<bool>& up; std::thread& t;
                          ~LoopJoin() { up = false; if (t.joinable()) t.join(); } } loopJoin{loopUp, loop};

        rc_socket_t fd = rcTestConnect(rc.port());   // the port it actually bound
        IM_CHECK(fd != RC_INVALID);
        if (fd == RC_INVALID) { loopUp = false; loop.join(); rc.stop(); return; }

        // Clients find the version with "protocol (\d+)"; this is the line they parse.
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok: lsl-viewer remote control, protocol 2. type `help`.\n");

        // The roster the Recording panel shows: one peer, addressed the way the panel
        // prints it.
        IM_CHECK(rcTestWaitClients(rc, 1));
        IM_CHECK(rc.clients().front().rfind("127.0.0.1:", 0) == 0);

        // The fields before file= are k=v tokens; everything after it is the path.
        rcTestSend(fd, "status\n");
        const std::string status = rcTestRecv(fd);
        IM_CHECK_STR_EQ(status.c_str(),
                        ("ok: recording=false seconds=0.0 streams=0 bytes=0 file=" + spaced + "\n").c_str());
        const std::size_t fileAt = status.find(" file=");
        IM_CHECK(fileAt != std::string::npos);
        if (fileAt != std::string::npos)
            IM_CHECK_STR_EQ(status.substr(fileAt + 6, status.size() - fileAt - 7).c_str(), spaced.c_str());

        rcTestSend(fd, "streams\n");
        {
            const RcTestCounted r = rcTestCounted(fd);
            IM_CHECK_STR_EQ(r.head.c_str(), "ok: 3 streams");
            IM_CHECK_EQ(r.lines.size(), (std::size_t)3);
            if (r.lines.size() == 3) {
                IM_CHECK_STR_EQ(r.lines[0].c_str(), "mock-eeg | MockEEG | EEG | 8ch | 500");
                IM_CHECK_STR_EQ(r.lines[1].c_str(), "src-7|MockAcc | MockAcc | ACC | 3ch | irregular  [rec]");
                IM_CHECK_STR_EQ(r.lines[2].c_str(), "mock-x | Bad Name | Misc | 1ch | 100");
                IM_CHECK_STR_EQ(r.lines[1].substr(0, r.lines[1].find(" | ")).c_str(), "src-7|MockAcc");
            }
            IM_CHECK(r.rest.empty());
        }
        rcTestSend(fd, "selected\n");
        {
            const RcTestCounted r = rcTestCounted(fd);
            IM_CHECK_STR_EQ(r.head.c_str(), "ok: 1 selected");
            IM_CHECK(r.lines.size() == 1 && r.lines[0] == "src-7|MockAcc");
            IM_CHECK(r.rest.empty());
        }
        // Empty lists still answer with a header, so a client reading lines never waits
        // out its timeout; and nothing follows it, so the next reply starts clean.
        { std::lock_guard<std::mutex> lk(st.mtx); st.streamsText.clear(); st.selectedText.clear(); }
        rcTestSend(fd, "streams\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok: 0 streams\n");
        rcTestSend(fd, "selected\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok: 0 selected\n");
        rcTestSend(fd, "status\n");
        IM_CHECK(rcTestRecv(fd).rfind("ok: recording=false ", 0) == 0);

        rcTestSend(fd, "help\n");
        {
            const RcTestCounted r = rcTestCounted(fd);
            IM_CHECK(r.head.rfind("ok: ", 0) == 0 && r.head.size() > 10 &&
                     r.head.compare(r.head.size() - 6, 6, " lines") == 0);
            IM_CHECK_GT(r.n, (std::size_t)0);
            IM_CHECK_EQ(r.lines.size(), r.n);
            IM_CHECK(r.rest.empty());
            bool mentionsGet = false;
            for (auto& l : r.lines) if (l.find("get") != std::string::npos) mentionsGet = true;
            IM_CHECK(mentionsGet);
        }
        rcTestSend(fd, "status\n");             // the help body did not overrun its count
        IM_CHECK(rcTestRecv(fd).rfind("ok: recording=", 0) == 0);

        // The LabRecorder RCS commands keep their protocol-1 replies exactly.
        rcTestSend(fd, "filename /tmp/rc_name.xdf\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok\n");
        rcTestSend(fd, "set subject 01\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok\n");
        { std::lock_guard<std::mutex> lk(st.mtx);
          IM_CHECK(st.setVars.size() == 1 && st.setVars[0].first == "subject" && st.setVars[0].second == "01"); }
        rcTestSend(fd, "set\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "error: set requires <field> <value>\n");

        // `select <key>` reaches the main loop as a request, and its reply is the
        // outcome the main loop wrote, not an optimistic ok.
        rcTestSend(fd, "select mock-eeg\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok: connected 1 stream(s)\n");
        IM_CHECK(lastSelect.size() == 1);
        IM_CHECK_STR_EQ(lastSelect.front().c_str(), "mock-eeg");

        rcTestSend(fd, "select unknown-key\n");   // rejected whole, not half-applied
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "error: unknown stream(s): unknown-key (see `streams`)\n");

        // `start <path>` sets the filename first, so the request the loop applies
        // already sees it, and the reply names the file it opened.
        rcTestSend(fd, "start /tmp/rc_unit.xdf\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok: recording -> /tmp/rc_unit.xdf\n");
        IM_CHECK_STR_EQ(lastFilename.c_str(), "/tmp/rc_unit.xdf");

        rcTestSend(fd, "stop\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "ok: stopped -> /tmp/rc_unit.xdf\n");

        // `get`: "ok: <bytes> <name>", the name runs to the end of the line (spaces and
        // all), then exactly <bytes> of data. Larger than one recv() so the body spans reads.
        const std::filesystem::path getPath = std::filesystem::temp_directory_path() / "rc get test.xdf";
        std::string payload(5000, '\0');
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(i * 31 + 7);
        { std::ofstream(getPath, std::ios::binary).write(payload.data(), (std::streamsize)payload.size()); }
        { std::lock_guard<std::mutex> lk(st.mtx); st.lastFile = getPath.string(); }
        rcTestSend(fd, "get\n");
        {
            std::string buf;
            rcTestFillLines(fd, buf, 1);
            const std::size_t nl = buf.find('\n');
            IM_CHECK(nl != std::string::npos);
            if (nl != std::string::npos) {
                IM_CHECK_STR_EQ(buf.substr(0, nl).c_str(), "ok: 5000 rc get test.xdf");
                rcTestFill(fd, buf, nl + 1 + payload.size());
                IM_CHECK(buf.size() == nl + 1 + payload.size());   // exactly <bytes>, nothing after
                IM_CHECK(buf.compare(nl + 1, std::string::npos, payload) == 0);
            }
        }
        { std::lock_guard<std::mutex> lk(st.mtx); st.recording = true; }
        rcTestSend(fd, "get\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "error: stop the recording before `get`\n");
        { std::lock_guard<std::mutex> lk(st.mtx); st.recording = false; st.lastFile.clear(); }
        rcTestSend(fd, "get\n");
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "error: no completed recording yet\n");
        std::error_code ec; std::filesystem::remove(getPath, ec);

        rcTestSend(fd, "frobnicate\n");        // unknown -> error, connection stays open
        IM_CHECK(rcTestRecv(fd).rfind("error: ", 0) == 0);

        // A second client is served while the first stays connected (one session
        // thread each): a script and a `nc` session can coexist.
        rc_socket_t fd2 = rcTestConnect(port);
        IM_CHECK(fd2 != RC_INVALID);
        if (fd2 != RC_INVALID) {
            IM_CHECK(rcTestRecv(fd2).find("protocol 2") != std::string::npos);
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
        IM_CHECK_STR_EQ(rcTestRecv(fd).c_str(), "bye\n");
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

        // The beacon a client resolves: it can reject a protocol it doesn't speak from
        // desc() alone, before it connects.
        lsl::stream_info bi = rc_beacon_info(22345, false);
        IM_CHECK_STR_EQ(bi.name().c_str(), "LSLViewerControl");
        IM_CHECK_STR_EQ(bi.type().c_str(), "ViewerControl");
        IM_CHECK_STR_EQ(bi.source_id().c_str(), sid.c_str());
        IM_CHECK_STR_EQ(bi.desc().child_value("protocol_version"), "2");
        IM_CHECK_STR_EQ(bi.desc().child_value("protocol"), "tcp-text-lines");
        IM_CHECK_STR_EQ(bi.desc().child_value("port"), "22345");
        IM_CHECK_STR_EQ(bi.desc().child_value("pid"), std::to_string(rc_pid()).c_str());
        IM_CHECK_STR_EQ(bi.desc().child_value("bind"), "loopback");
        lsl::stream_info ba = rc_beacon_info(22345, true);
        IM_CHECK_STR_EQ(ba.desc().child_value("bind"), "all");
    };

    // Two viewers on one host: the second can't have the same port, so start() falls back
    // to an ephemeral one and announces that rather than going without a control port.
    // This also pins down the platform bind semantics: on Windows SO_REUSEADDR would let
    // the second bind SUCCEED on the live port and quietly split the connections, so the
    // no-fallback case failing is the assertion that matters most there.
    t = IM_REGISTER_TEST(e, "remote", "second_instance");
    t->TestFunc = [](ImGuiTestContext*) {
        RemoteState st1, st2;
        RemoteControl rc1, rc2;
        const int port = 22457;
        IM_CHECK(rc1.start(port, &st1));
        if (!rc1.listening()) return;            // bind failed (port busy?): don't hang
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
                IM_CHECK(rcTestRecv(fd).rfind("ok: lsl-viewer remote control, protocol ", 0) == 0);
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

    // Performance window (off by default; Debug menu) exposes VSync, and floats: a docked
    // diagnostic would take a slot from the plots.
    t = IM_REGISTER_TEST(e, "ui", "performance_window");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuCheck("//##MainMenuBar/Debug/Performance");
        ctx->Yield(2);
        ctx->SetRef("//Performance");
        IM_CHECK(ctx->ItemExists("VSync"));
        IM_CHECK(!ctx->GetWindowByRef("//Performance")->DockIsActive);
        ctx->MenuUncheck("//##MainMenuBar/Debug/Performance");
    };

    // Marker log "Clear" buttons: the clear is deferred to the end of the frame (the log
    // rows and the plot overlays alias the source caches clear() rebuilds), so this drives
    // both buttons and checks the events are actually gone after each.
    //
    // Like the capture tests, it needs streams and SKIPS without them: run it with the
    // demo emitter (LSL_DEMO=1 ./lsl_viewer --tests marker_clear) or the mock script
    // (python tools/lsl_test_streams.py --streams evoked). It deliberately does NOT switch
    // the emitter on itself: connected stream windows outlive the emitter, so a test that
    // started it would leave every later capture test finding streams and running instead
    // of skipping.
    t = IM_REGISTER_TEST(e, "ui", "marker_clear");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        // The stream header reads "<name>  .  N events  .  R/s" and is the only observable
        // the test engine can read back; DebugLabel truncates at 32 chars, so a long stream
        // name can cut the count off (-> -1, the test then skips its checks).
        auto headerCount = [](ImGuiTestContext* c) -> int {
            ImGuiTestItemInfo hi = c->ItemInfo("**/###mk");
            const char* end = strstr(hi.DebugLabel, " events");
            if (end == nullptr) return -1;
            const char* begin = end;
            while (begin > hi.DebugLabel && begin[-1] >= '0' && begin[-1] <= '9') begin--;
            return (begin == end) ? -1 : atoi(begin);
        };
        ctx->SleepNoSkip(6.0f, 1.0f / 30.0f);          // discovery + autoconnect + a few markers
        ctx->MenuCheck("//##MainMenuBar/View/Marker events");
        ctx->Yield(2);
        ctx->SetRef("//Marker events");
        // Controls sit in the left "cfg" child window: reach them with the **/ wildcard.
        if (!ctx->ItemExists("**/Clear all")) { ctx->LogInfo("no marker stream connected; skip"); return; }
        ctx->ItemOpen("**/###mk");                     // first stream's log (collapsed by default)
        ctx->Yield(2);

        // Markers keep arriving (the demo emits ~1/s), so the count is only momentarily 0:
        // assert the drop, not an exact zero.
        const int beforeOne = headerCount(ctx);
        ctx->ItemClick("**/Clear");                    // per-stream clear
        ctx->Yield(3);
        const int afterOne = headerCount(ctx);
        ctx->LogInfo("per-stream clear: %d -> %d events", beforeOne, afterOne);
        IM_CHECK(ctx->ItemExists("**/Clear all"));        // window survived the deferred clear
        if (beforeOne > 1) IM_CHECK(afterOne < beforeOne);

        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);          // let the log fill again
        const int beforeAll = headerCount(ctx);
        ctx->ItemClick("**/Clear all");
        ctx->Yield(3);
        const int afterAll = headerCount(ctx);
        ctx->LogInfo("clear all: %d -> %d events", beforeAll, afterAll);
        IM_CHECK(ctx->ItemExists("**/Clear all"));
        if (beforeAll > 1) IM_CHECK(afterAll < beforeAll);
    };

    // Marker events log: left controls strip + one expanded per-stream log. Needs a marker
    // stream (demo emitter or the mock script); skips without one, like the other captures.
    t = IM_REGISTER_TEST(e, "ui", "capture_markers");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(6.0f, 1.0f / 30.0f);          // discovery + autoconnect + a few markers
        ctx->MenuCheck("//##MainMenuBar/View/Marker events");
        ctx->Yield(2);
        ctx->SetRef("//Marker events");
        if (!ctx->ItemExists("**/Clear all")) { ctx->LogInfo("no marker stream connected; skip"); return; }
        ctx->ItemOpen("**/###mk");
        ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//Marker events", ImGuiCaptureFlags_HideMouseCursor);
    };

    // Screen capture of the rail and the Performance window (SDL_GPU readback).
    t = IM_REGISTER_TEST(e, "ui", "capture_ui");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuCheck("//##MainMenuBar/Debug/Performance");
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow("//Streams", ImGuiCaptureFlags_HideMouseCursor);
        ctx->CaptureScreenshotWindow("//Performance", ImGuiCaptureFlags_HideMouseCursor);
        ctx->MenuUncheck("//##MainMenuBar/Debug/Performance");
    };

    // The rail at its fixed width: idle, during a replay (its rows tagged), with one replay
    // stream closed (listed under Playback), with more streams than fit (the list scrolls,
    // Recording and Playback stay put), and after Stop. Needs no other streams: it publishes
    // its own for the list.
    t = IM_REGISTER_TEST(e, "ui", "capture_rail");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        using State = XdfPlayer::State;
        auto waitFor = [&](auto pred, double seconds) {   // wall clock, as in replay_open_seek
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        auto shot = [&] {                                // no tooltip in the shot
            ctx->MouseMoveToPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 5.0f, 5.0f));
            ctx->Yield(3);
            ctx->CaptureScreenshotWindow("//Streams", ImGuiCaptureFlags_HideMouseCursor);
        };
        shot();
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "lsl_viewer_rail_test.xdf";
        writeReplayTestFile(file.string());
        lslViewerRequestReplay(file.string().c_str());
        IM_CHECK(waitFor([] { const auto p = replayProbe(); return p.state == (int)State::playing && p.shown == 2; }, 15.0));
        waitFor([] { return false; }, 1.0);
        shot();
        ctx->WindowClose("//ReplayTestSine");
        waitFor([] { return replayProbe().shown == 1; }, 3.0);
        ctx->MenuCheck("//##MainMenuBar/View/Pause display");
        shot();
        ctx->MenuUncheck("//##MainMenuBar/View/Pause display");
        // Reconnect it: a closed stream stays closed for the next replay of the same file.
        lslViewerRequestConnect("replay-replaytest-sine");
        IM_CHECK(waitFor([] { return replayProbe().shown == 2; }, 3.0));

        // Enough outlets to overflow the list on a 1080p screen. Discovery lists them unconnected.
        const float recY0 = ctx->ItemInfo("//Streams/Record").RectFull.Min.y;
        std::vector<std::unique_ptr<lsl::stream_outlet>> outs;
        for (int i = 0; i < 24; ++i) {
            char name[32], sid[32];
            std::snprintf(name, sizeof name, "RailTest%02d", i);
            std::snprintf(sid, sizeof sid, "railtest-%02d", i);
            outs.push_back(std::make_unique<lsl::stream_outlet>(lsl::stream_info(name, "EEG", 8, 250.0, lsl::cf_float32, sid)));
        }
        waitFor([] { return false; }, 4.0);
        shot();
        // The pinned block does not move for the stream count.
        const float recY1 = ctx->ItemInfo("//Streams/Record").RectFull.Min.y;
        ctx->LogInfo("Record button at y %.0f -> %.0f with 24 more streams", recY0, recY1);
        IM_CHECK_EQ(recY0, recY1);

        ctx->SetRef("//Streams");
        ctx->ItemClick("Stop");
        waitFor([] { return replayProbe().shown == 0; }, 3.0);
        shot();
        ctx->ItemClick("###replayclose");
        ctx->Yield(3);
        IM_CHECK_EQ(replayProbe().state, -1);
        std::error_code ec; std::filesystem::remove(file, ec);
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

    // Lane-label highlight: hover a stacked-montage channel name, click to pin it, move to
    // the data (pin holds), then right-click the names to release. One capture per state.
    t = IM_REGISTER_TEST(e, "ui", "capture_lane_highlight");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(2.5f, 1.0f / 30.0f);
        if (ctx->GetWindowByRef("//MockEEG") == nullptr) {
            ctx->LogInfo("MockEEG not present; skipping");
            return;
        }
        ctx->WindowFocus("//MockEEG"); ctx->Yield(2);   // bring the docked tab forward
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//MockEEG/cfg");
        ImGuiTestItemInfo plt = ctx->WindowInfo("//MockEEG/plt");
        if (cfg.Window == nullptr || plt.Window == nullptr) { ctx->LogInfo("no cfg/plt child; skipping"); return; }
        ctx->SetRef(cfg.Window);
        ctx->ItemOpen("Display");
        ctx->ItemCheck("Stacked montage");
        ctx->SleepNoSkip(2.0f, 1.0f / 30.0f);

        // Labels sit just inside the plot child's left edge; aim at the third lane from the top.
        const ImVec2 p = plt.Window->Pos, sz = plt.Window->Size;
        const ImVec2 label(p.x + 22.0f, p.y + 10.0f + (sz.y - 50.0f) * (2.5f / 16.0f));
        ctx->MouseMoveToPos(label); ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", 0);                                // hover
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->MouseMoveToPos(ImVec2(p.x + sz.x * 0.6f, p.y + sz.y * 0.7f)); ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", 0);                                // pinned
        ctx->MouseMoveToPos(label);
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->MouseMoveToPos(ImVec2(p.x + sz.x * 0.6f, p.y + sz.y * 0.7f)); ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", 0);                                // released

        ctx->SetRef(cfg.Window); ctx->ItemCheck("**/Raster"); ctx->Yield(3);
        ctx->MouseMoveToPos(label); ctx->Yield(3);
        ctx->CaptureScreenshotWindow("//MockEEG", 0);                                // raster hover
        ctx->ItemUncheck("**/Raster");
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
        std::string ref;
        if (openSpectra(&ref) == 0) {
            ctx->LogInfo("no spectrum window open; skipping");
            return;
        }
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);   // fill >= one FFT window
        ctx->CaptureScreenshotWindow(ref.c_str(), ImGuiCaptureFlags_HideMouseCursor);
    };

    // View > New spectrum adds an independent window each time, and closing one leaves the
    // others open. Needs no streams: an unbound spectrum still opens, with a hint to connect.
    // App > About opens, shows each license tab, and closes.
    t = IM_REGISTER_TEST(e, "ui", "about");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuClick("//##MainMenuBar/App/About");
        ctx->Yield(2);
        ImGuiWindow* w = ctx->GetWindowByRef("//About");
        IM_CHECK(w != nullptr && w->WasActive);
        ctx->SetRef("//About");
        IM_CHECK(ctx->ItemExists("Copy version info"));
        ctx->CaptureScreenshotWindow("//About", ImGuiCaptureFlags_HideMouseCursor);
        ctx->ItemClick("##licenses/Third-party licenses");
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow("//About", ImGuiCaptureFlags_HideMouseCursor);
        ctx->WindowClose("//About");
        ctx->Yield(2);
        w = ctx->GetWindowByRef("//About");
        IM_CHECK(w == nullptr || !w->WasActive);
    };

    t = IM_REGISTER_TEST(e, "ui", "spectrum_multi");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const int before = openSpectra();
        std::string a, b, newest;
        ctx->MenuClick("//##MainMenuBar/View/New spectrum");
        ctx->Yield(2);
        IM_CHECK_EQ(openSpectra(&a), before + 1);
        ctx->MenuClick("//##MainMenuBar/View/New spectrum");
        ctx->Yield(2);
        IM_CHECK_EQ(openSpectra(&b), before + 2);
        IM_CHECK(a != b);
        ctx->WindowClose(a.c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(openSpectra(&newest), before + 1);
        IM_CHECK_STR_EQ(newest.c_str(), b.c_str());
        ctx->WindowClose(b.c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(openSpectra(), before);
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

    // Panel accents: every panel family docked side by side, one spectrogram floating (its accent
    // goes on the title bar, not a tab), in both themes. A whole-viewport capture, because the
    // accents are only judged against each other. Run with LSL_DEMO=1 for the signal and marker
    // windows; the playback accent is on the rail's Playback header and the replay's stream rows,
    // for a file the test writes.
    t = IM_REGISTER_TEST(e, "ui", "capture_panel_accents");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        using State = XdfPlayer::State;
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "lsl_viewer_accent_test.xdf";
        writeReplayTestFile(file.string());
        lslViewerRequestReplay(file.string().c_str());
        auto waitFor = [&](auto pred, double seconds) {   // wall clock, as in replay_open_seek
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        IM_CHECK(waitFor([] { return replayProbe().state == (int)State::playing; }, 15.0));
        std::string spectrumRef;                         // reuse the startup spectrum, if still open
        if (openSpectra(&spectrumRef) == 0) {
            ctx->MenuClick("//##MainMenuBar/View/New spectrum");
            ctx->Yield(2);
            openSpectra(&spectrumRef);
        }
        ctx->MenuClick("//##MainMenuBar/View/Marker events");
        ctx->MenuClick("//##MainMenuBar/View/New ERP (marker average)");
        ctx->MenuClick("//##MainMenuBar/View/New spectrogram");
        ctx->MenuClick("//##MainMenuBar/View/New spectrogram");
        ctx->Yield(3);
        ctx->UndockWindow("Spectrogram 2");
        ctx->WindowMove("//Spectrogram 2", ImVec2(ImGui::GetMainViewport()->WorkSize.x * 0.55f, 120.0f));
        ctx->WindowResize("//Spectrogram 2", ImVec2(420.0f, 260.0f));
        ctx->WindowFocus(spectrumRef.c_str());
        ctx->MouseMoveToPos(ImVec2(5, 5));
        waitFor([] { return false; }, 3.0);              // fill the plots
        ctx->CaptureReset();
        ctx->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor);
        ctx->MenuCheck("//##MainMenuBar/App/Light theme");
        ctx->MouseMoveToPos(ImVec2(5, 5));
        ctx->Yield(3);
        ctx->CaptureReset();
        ctx->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor);
        ctx->MenuUncheck("//##MainMenuBar/App/Light theme");

        ctx->SetRef("//Streams");
        ctx->ItemClick("Stop");
        waitFor([] { return replayProbe().shown == 0; }, 3.0);
        ctx->ItemClick("###replayclose");
        ctx->Yield(3);
        std::error_code ec; std::filesystem::remove(file, ec);
    };

    // Playback > Open XDF file, end to end on a file it writes itself: the streams connect, a
    // seek from the rail moves playback and is marked as a break (not a dropout), a replay
    // pause leaves no dropout either, and Stop removes the streams. Needs no other streams.
    t = IM_REGISTER_TEST(e, "replay", "replay_open_seek");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        using State = XdfPlayer::State;
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "lsl_viewer_replay_test.xdf";
        writeReplayTestFile(file.string());
        lslViewerRequestReplay(file.string().c_str());
        // Wall-clock waits: in fast mode the engine's sleeps run on simulated time, and the
        // replay, LSL, and the inlets all run on the real clock.
        auto waitFor = [&](auto pred, double seconds) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        auto sleepReal = [&](double seconds) { waitFor([] { return false; }, seconds); };
        IM_CHECK(waitFor([] { const auto p = replayProbe(); return p.state == (int)State::playing && p.shown == 2; }, 15.0));
        ReplayProbeView p = replayProbe();
        IM_CHECK_EQ(p.streams, 2);
        IM_CHECK_GT(p.length, 25.0);
        IM_CHECK(ctx->GetWindowByRef("//ReplayTestSine") != nullptr);
        sleepReal(1.0);                                  // some data before the seek
        // Seek at 20x, where one push period of the player spans 0.4 s of recording: a seek
        // that dropped what was already due, or a dropout check against the previous sample,
        // would show the join as missing data. Back to 1x right after, so the rest of the
        // file lasts through the pause below.
        ctx->SetRef("//Streams");                       // Playback is a section of the rail
        ctx->ComboClick("##replayspeed/20x");
        sleepReal(0.3);
        const double before = replayProbe().position;
        ctx->ItemInputValue("##replaypos", 15.0f);       // Ctrl+click the slider, type, Enter
        ctx->ComboClick("##replayspeed/1x");
        IM_CHECK(waitFor([] { return replayProbe().breakMarks >= 1; }, 5.0));
        p = replayProbe();
        ctx->LogInfo("seek: %.2f s -> %.2f s, breaks %zu, marks %d, dropouts %llu",
                     before, p.position, p.breaks, p.breakMarks, p.dropouts);
        IM_CHECK_GE(p.position, 15.0);
        IM_CHECK_LT(p.position, 25.0);
        IM_CHECK_EQ(p.breaks, (std::size_t)1);
        IM_CHECK_EQ(p.dropouts, 0ull);                   // the join is not missing data

        // A replay pause sends nothing for a while, which must not read as a dropout either.
        ctx->ItemClick("Pause replay");
        IM_CHECK(waitFor([] { return replayProbe().state == (int)State::paused; }, 2.0));
        sleepReal(1.5);
        ctx->ItemClick("Resume replay");
        sleepReal(1.5);
        p = replayProbe();
        IM_CHECK_EQ(p.state, (int)State::playing);
        IM_CHECK_EQ(p.dropouts, 0ull);

        // Stop closes the outlets and removes the replay's plots.
        ctx->ItemClick("Stop");
        IM_CHECK(waitFor([] { return replayProbe().shown == 0; }, 3.0));
        IM_CHECK_EQ(replayProbe().state, (int)State::stopped);
        ctx->Yield(3);
        IM_CHECK(ctx->GetWindowByRef("//ReplayTestSine") == nullptr || !ctx->GetWindowByRef("//ReplayTestSine")->Active);
        ctx->ItemClick("###replayclose");                // and the close button forgets the session
        ctx->Yield(3);
        IM_CHECK_EQ(replayProbe().state, -1);
        std::error_code ec; std::filesystem::remove(file, ec);
    };

    // ERP across a dropout, on a replayed file (see writeErpGapFile): epochs that touch the gap
    // are left out and counted, a marker inside the gap is not matched to data recorded after
    // it, and the first marker of a trigger stream that had no events when the ERP was bound
    // is averaged, not swallowed by the ERP's first sync pass.
    t = IM_REGISTER_TEST(e, "replay", "replay_erp_gap");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        using State = XdfPlayer::State;
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "lsl_viewer_erp_gap_test.xdf";
        writeErpGapFile(file.string());
        lslViewerRequestReplay(file.string().c_str());
        auto waitFor = [&](auto pred, double seconds) {   // wall clock, as in replay_open_seek
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        IM_CHECK(waitFor([] { const auto p = replayProbe(); return p.state == (int)State::playing && p.shown == 2; }, 15.0));

        // Bind a new ERP to the replay's streams by name: other streams may be connected too.
        ctx->MenuClick("//##MainMenuBar/View/New ERP (marker average)");
        ctx->Yield(2);
        const int id = erpProbe().id;
        char ref[32];
        std::snprintf(ref, sizeof ref, "//ERP %d", id);
        IM_CHECK(ctx->GetWindowByRef(ref) != nullptr);
        ctx->WindowFocus(ref);
        ctx->Yield(2);
        ImGuiTestItemInfo cfg = ctx->WindowInfo((std::string(ref) + "/cfg").c_str());
        IM_CHECK(cfg.Window != nullptr);
        ctx->SetRef(cfg.Window);
        ctx->ComboClick("stream/ReplayErpSine");
        ctx->ComboClick("trigger/ReplayErpMarkers");
        ctx->Yield(2);
        ErpProbeView p = erpProbe();
        IM_CHECK_STR_EQ(p.stream.c_str(), "ReplayErpSine");
        IM_CHECK_STR_EQ(p.trigger.c_str(), "ReplayErpMarkers");
        // The first-event case needs a trigger stream with no events yet at the bind. The first
        // marker is 4 s into the file; if the UI was slower than that, say so, not a false pass.
        IM_CHECK_EQ(p.markerEvents, (std::size_t)0);

        // The rest at 10x, then wait for the end of the file. Leave the ERP showing while the
        // file plays, since it collects epochs as it draws.
        ctx->SetRef("//Streams");
        ctx->ComboClick("##replayspeed/10x");
        ctx->WindowFocus(ref);
        IM_CHECK(waitFor([] { return replayProbe().state == (int)State::finished; }, 20.0));
        // Not asserted: if an epoch went missing this times out, and the counts below say which.
        waitFor([] { return erpProbe().count + erpProbe().rejected >= kErpGapAveraged + kErpGapLeftOut; }, 3.0);
        p = erpProbe();
        ctx->LogInfo("ERP %d: %d averaged, %d left out, %zu trigger events", id, p.count, p.rejected, p.markerEvents);
        IM_CHECK_EQ(p.markerEvents, (std::size_t)(kErpGapAveraged + kErpGapLeftOut));
        IM_CHECK_EQ(p.count, kErpGapAveraged);
        IM_CHECK_EQ(p.rejected, kErpGapLeftOut);

        ctx->SetRef("//Streams");
        ctx->ItemClick("Stop");
        IM_CHECK(waitFor([] { return replayProbe().shown == 0; }, 3.0));
        ctx->ItemClick("###replayclose");
        ctx->WindowClose(ref);
        ctx->Yield(3);
        std::error_code ec; std::filesystem::remove(file, ec);
    };

    // A dropout inside one pull. The sender pushes the samples on both sides of a 1.5 s gap
    // as one chunk, so the viewer reads them in a single pull: the jump is between two samples
    // of the pull, not at its start, and must still be recorded as a dropout of that length.
    t = IM_REGISTER_TEST(e, "stream", "dropout_within_pull");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto waitFor = [&](auto pred, double seconds) {   // wall clock: LSL runs on the real one
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        const char* kName = "GapInPullTest";
        lsl::stream_outlet out(lsl::stream_info(kName, "EEG", 1, 100.0, lsl::cf_float32, "gaptest-inpull"));
        lslViewerRequestConnect("gaptest-inpull");
        // Data pushed before the inlet subscribes is not delivered, so wait for it first.
        IM_CHECK(waitFor([&] { return streamProbe(kName).found && out.have_consumers(); }, 15.0));

        const double base = lsl::local_clock();
        std::vector<float>  v;
        std::vector<double> ts;
        auto add = [&](double from, int n) {
            for (int i = 0; i < n; ++i) { ts.push_back(base + from + i / 100.0); v.push_back((float)i); }
        };
        add(0.0, 100);                                   // 1 s of continuous data anchors the stream
        out.push_chunk_multiplexed(v.data(), ts.data(), v.size(), true);
        IM_CHECK(waitFor([&] { return streamProbe(kName).anchored; }, 5.0));
        v.clear(); ts.clear();
        add(1.0, 50);                                    // 1.00 .. 1.49 s
        add(3.0, 50);                                    // 3.00 .. 3.49 s: 1.5 s after 1.49 + 1/100
        out.push_chunk_multiplexed(v.data(), ts.data(), v.size(), true);
        waitFor([&] { return streamProbe(kName).dropouts > 0; }, 3.0);
        const StreamProbeView p = streamProbe(kName);
        ctx->LogInfo("%s: %llu dropouts, %.3f s missing", kName, p.dropouts, p.gapSec);
        IM_CHECK_EQ(p.dropouts, 1ull);
        IM_CHECK(p.gapSec > 1.45 && p.gapSec < 1.55);

        ctx->WindowClose("//GapInPullTest");             // disconnect before the outlet goes
        ctx->Yield(3);
    };

    // Changing the replay speed must scroll smoothly: the plot edge may slow down or speed up,
    // but must not jump back, and must not run past the newest data (that strip is painted as a
    // live dropout). Measured frame by frame across a 5x -> 1x and a 1x -> 5x change.
    t = IM_REGISTER_TEST(e, "replay", "replay_speed_change");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        using State = XdfPlayer::State;
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "lsl_viewer_speed_test.xdf";
        writeReplayTestFile(file.string());
        lslViewerRequestReplay(file.string().c_str());
        auto waitFor = [&](auto pred, double seconds) {   // wall clock, as in replay_open_seek
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        IM_CHECK(waitFor([] { const auto p = replayProbe(); return p.state == (int)State::playing && p.shown == 2; }, 15.0));
        const char* kName = "ReplayTestSine";
        ctx->SetRef("//Streams");
        ctx->ComboClick("##replayspeed/5x");
        waitFor([] { return false; }, 1.5);              // settle at 5x (7.5 s of data)

        // Record every new edge for `seconds` of wall time after a speed change.
        // Every frame: the edge's step and its velocity (data seconds per wall second, from the
        // frame's own time). Judder is the velocity jumping from frame to frame; a stall is a
        // frame where the edge stops while data keeps coming.
        // A jolt is a jump in velocity that is large in itself (dv) AND large for the time it
        // took (dv per wall second, over the mean of the two frames). During a speed change
        // the edge really accelerates at about 13 to 17 data s/s^2, so one long frame in that
        // ramp (60 to 130 ms under load) makes dv exceed 0.5 with a smooth scroll.
        constexpr double kJoltDv    = 0.5;    // data s per wall s
        constexpr double kJoltAccel = 30.0;   // data s/s^2: about twice the ramp's acceleration
        struct Stats { int frames = 0, back = 0, ahead = 0, stalls = 0, jolts = 0;
                       double maxBack = 0.0, maxStep = 0.0, maxAhead = 0.0, maxDv = 0.0,
                              maxAccel = 0.0; };   // maxAccel: the worst dv/dt among dv > kJoltDv
        auto record = [&](double seconds) {
            Stats st;
            double prev = streamProbe(kName).edge, prevV = -1.0, prevDt = 0.0;
            int frame = ImGui::GetFrameCount();
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < end) {
                ctx->Yield();
                if (ImGui::GetFrameCount() == frame) continue;
                frame = ImGui::GetFrameCount();
                const StreamProbeView p = streamProbe(kName);
                const double step = p.edge - prev, dt = std::max(1e-4, p.dt);   // the step's own frame time
                const double v = step / dt;
                ++st.frames;
                if (step < 0.0) { ++st.back; st.maxBack = std::max(st.maxBack, -step); }
                st.maxStep = std::max(st.maxStep, step);
                if (v < 0.2) ++st.stalls;
                if (prevV >= 0.0) {
                    const double dv = std::fabs(v - prevV);
                    st.maxDv = std::max(st.maxDv, dv);
                    if (dv > kJoltDv) {
                        const double accel = dv / (0.5 * (dt + prevDt));
                        st.maxAccel = std::max(st.maxAccel, accel);
                        if (accel > kJoltAccel) ++st.jolts;
                    }
                }
                const double ahead = p.edge - p.newest;
                if (ahead > 0.05) { ++st.ahead; st.maxAhead = std::max(st.maxAhead, ahead); }
                prev = p.edge; prevV = v; prevDt = dt;
            }
            return st;
        };
        auto report = [&](const char* what, const Stats& st) {
            ctx->LogInfo("%s: %d frames, %d stalls, %d jolts (max dv %.2f; worst dv/dt above dv %.1f: %.0f /s^2, "
                         "limit %.0f), %d back (max %.3f s), max step %.3f s, %d ahead of the data (max %.3f s)",
                         what, st.frames, st.stalls, st.jolts, st.maxDv, kJoltDv, st.maxAccel, kJoltAccel,
                         st.back, st.maxBack, st.maxStep, st.ahead, st.maxAhead);
        };
        ctx->ComboClick("##replayspeed/1x");
        const Stats down = record(2.5);
        report("5x -> 1x", down);
        ctx->ComboClick("##replayspeed/5x");
        const Stats up = record(1.5);
        report("1x -> 5x", up);
        // Smooth: never back, never ahead of the data, never stopping while data comes, and
        // no jump in speed (a change of speed is a ramp over ~20 frames; see kJoltAccel).
        for (const Stats* st : {&down, &up}) {
            IM_CHECK_LT(st->maxBack, 0.001);
            IM_CHECK_EQ(st->ahead, 0);
            IM_CHECK_EQ(st->stalls, 0);
            IM_CHECK_EQ(st->jolts, 0);
        }
        // The shown rate is in the stream's own clock, so a replay at 5x still reads its
        // recorded 100 Hz; a wall-clock rate would read about 500.
        {
            const StreamProbeView p = streamProbe(kName);
            ctx->LogInfo("measured %.2f Hz, arriving at %.2fx", p.rate, p.speed);
            IM_CHECK(std::fabs(p.rate - 100.0) < 1.0);
        }

        ctx->ItemClick("Stop");
        IM_CHECK(waitFor([] { return replayProbe().shown == 0; }, 3.0));
        ctx->ItemClick("###replayclose");
        ctx->Yield(3);
        std::error_code ec; std::filesystem::remove(file, ec);
    };

    // The measured rate must be readable: steady to a fraction of a percent although the sender
    // delivers chunks of varying size at jittered times (a per-delivery rate bounces by several
    // percent around 1 kHz).
    t = IM_REGISTER_TEST(e, "stream", "measured_rate_steady");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto waitFor = [&](auto pred, double seconds) {   // wall clock: LSL runs on the real one
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ctx->Yield();
            }
            return pred();
        };
        const char* kName = "RateTest";
        lsl::stream_outlet out(lsl::stream_info(kName, "EEG", 1, 1000.0, lsl::cf_float32, "ratetest-1k"));
        lslViewerRequestConnect("ratetest-1k");
        IM_CHECK(waitFor([&] { return streamProbe(kName).found && out.have_consumers(); }, 15.0));
        // Real-time 1 kHz in chunks of 10..30 samples, pushed whenever they are due.
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> chunk(10, 30);
        std::vector<float> v(30, 0.0f);
        std::vector<double> ts(30);
        const double base = lsl::local_clock();
        long long sent = 0;
        int next = chunk(rng);
        double lo = 1e9, hi = 0.0;
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (el > 9.0) break;
            while ((double)(sent + next) / 1000.0 <= el) {
                for (int i = 0; i < next; ++i) ts[(std::size_t)i] = base + (double)(sent + i) / 1000.0;
                out.push_chunk_multiplexed(v.data(), ts.data(), (std::size_t)next, true);
                sent += next;
                next = chunk(rng);
            }
            ctx->Yield();
            if (el > 7.0) {                               // past the 5 s window: sample the shown rate
                const double r = streamProbe(kName).rate;
                lo = std::min(lo, r); hi = std::max(hi, r);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ctx->LogInfo("%s: shown rate %.2f .. %.2f Hz", kName, lo, hi);
        IM_CHECK(lo > 998.0 && hi < 1002.0);              // within 0.2% of 1 kHz
        IM_CHECK_LT(hi - lo, 1.0);                        // and steady to read
        ctx->WindowClose("//RateTest");
        ctx->Yield(3);
    };

    // The README images in docs/images/, opt-in: LSL_README_SHOTS lists the ones to take, from
    // montage,spectrum,spectrogram,erp,live (unset: the test returns at once). Streams are connected
    // by source_id, not autoconnect, so a stray stream on the network neither shows up as a tab nor
    // takes an analysis window. Files: output/captures/readme_<name>.png, and readme_live.mp4 (needs
    // LSL_FFMPEG; convert it to the GIF afterwards). The README images were taken as below; the
    // analysis views and the video are fullscreen on a 1920x1200 display, so their plots get room:
    //   uv run tools/lsl_test_streams.py --streams highdensity,eeg,drift,audio,chirp,evoked
    //   LSL_WINDOW=1280x800 LSL_README_SHOTS=montage lsl_viewer --tests capture_readme
    //   LSL_FULLSCREEN=1 LSL_README_SHOTS=spectrum,spectrogram,erp lsl_viewer --tests capture_readme
    //   uv run tools/lsl_test_streams.py --streams evoked,chirp,eeg,drift,audio,markers  (no highdensity)
    //   LSL_FULLSCREEN=1 LSL_README_SHOTS=live LSL_FFMPEG=<ffmpeg> lsl_viewer --tests capture_readme
    // The rail lists every stream on the network, so for the video, limit discovery to this machine:
    // LSLAPICFG=<file> with "[multicast] ResolveScope = machine" and "[lab] KnownPeers = {127.0.0.1}".
    // Then, for the 800x500 GIF:
    //   ffmpeg -i readme_live.mp4 -vf "trim=start_frame=2:end_frame=74,setpts=PTS-STARTPTS,
    //     scale=800:500:flags=lanczos,split[a][b];[a]palettegen=stats_mode=full[p];
    //     [b][p]paletteuse=dither=sierra2_4a" -r 12 -loop 0 live.gif
    t = IM_REGISTER_TEST(e, "ui", "capture_readme");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* shotsEnv = std::getenv("LSL_README_SHOTS");
        if (!shotsEnv) { ctx->LogInfo("LSL_README_SHOTS not set; skipping"); return; }
        const std::string shots = std::string(",") + shotsEnv + ",";
        auto want = [&](const char* s) { return shots.find(std::string(",") + s + ",") != std::string::npos; };
        auto waitFor = [&](auto pred, double seconds) {   // wall clock: the streams run in real time
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        auto pause = [&](double seconds) { waitFor([] { return false; }, seconds); };
        // A marker stream opens no window, so there is nothing to wait on: give discovery time.
        auto connect = [&](const char* sid, const char* window) {
            lslViewerRequestConnect(sid);
            if (window == nullptr) { pause(2.0); return true; }
            const std::string ref = std::string("//") + window;
            const bool ok = waitFor([&] { return ctx->GetWindowByRef(ref.c_str()) != nullptr; }, 15.0);
            if (!ok) IM_ERRORF("%s did not connect", window);
            return ok;
        };
        auto cfgOf = [&](const std::string& ref) {        // the window's left controls child
            ctx->WindowFocus(ref.c_str()); ctx->Yield(2);
            ImGuiTestItemInfo cfg = ctx->WindowInfo((ref + "/cfg").c_str());
            if (cfg.Window == nullptr) { IM_ERRORF("no cfg child in %s", ref.c_str()); return false; }
            ctx->SetRef(cfg.Window);
            return true;
        };
        auto parkMouse = [&] {                            // the menu bar's empty end: no hover state
            ctx->MouseMoveToPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 5.0f, 5.0f));
            ctx->Yield(3);
        };
        auto shot = [&](const char* window, const char* name) {
            parkMouse();
            ctx->CaptureReset();
            std::snprintf(ctx->CaptureArgs->InOutputFile, sizeof ctx->CaptureArgs->InOutputFile,
                          "output/captures/readme_%s.png", name);
            ctx->CaptureAddWindow(window);
            ctx->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor);
        };
        auto newWindow = [&](const char* menu, const char* prefix) {
            ctx->MenuClick((std::string("//##MainMenuBar/View/") + menu).c_str());
            ctx->Yield(3);
            std::string ref;
            openSpectra(&ref, prefix);
            return ref;
        };
        auto bindSpectrum = [&](const std::string& ref, const char* stream) {
            if (!cfgOf(ref)) return false;
            ctx->ComboClick((std::string("##stream/") + stream).c_str());
            ctx->ComboClick("N/4096");
            ctx->ItemClick("All");
            return true;
        };
        auto bindSpectrogram = [&](const std::string& ref, const char* stream, float span) {
            if (!cfgOf(ref)) return false;
            ctx->ComboClick((std::string("##stream/") + stream).c_str());
            ctx->ItemInputValue("span (s)", span);
            return true;
        };
        auto bindErp = [&](const std::string& ref) {
            if (!waitFor([&] { return ctx->WindowInfo((ref + "/cfg").c_str(), ImGuiTestOpFlags_NoError).Window != nullptr; }, 10.0)) {
                IM_ERRORF("%s has no controls: no marker stream?", ref.c_str());
                return false;
            }
            if (!cfgOf(ref)) return false;
            ctx->ComboClick("stream/MockEvoked");
            ctx->ComboClick("trigger/MockEvokedMarkers");
            ctx->ItemInputValue("match", "target");
            return true;
        };
        auto openHeaders = [&](const char* window, std::initializer_list<const char*> headers) {
            if (!cfgOf(std::string("//") + window)) return false;
            for (const char* h : headers) ctx->ItemOpen(h);
            return true;
        };

        if (want("montage") || want("spectrum")) {
            // The tab order is the connect order.
            if (!connect("mock-hd", "MockHighDensity") || !connect("mock-eeg", "MockEEG") ||
                !connect("mock-drift", "MockDrift"))
                return;
            // An analysis row below gives the time series the height they have in the README.
            const std::string spectrum = newWindow("New spectrum", "Spectrum ");
            if (want("montage")) {
                if (!openHeaders("MockHighDensity", {"Display", "Channels"})) return;
                ctx->ItemClick("All");
                ctx->ItemCheck("Raster");
                if (!openHeaders("MockDrift", {"Display", "Channels"})) return;
                pause(11.0);                              // fill the 10 s history
                shot("//MockDrift", "montage_drift");
                ctx->WindowFocus("//MockHighDensity");
                shot("//MockHighDensity", "montage_raster");
            }
            if (want("spectrum")) {
                if (!connect("mock-audio", "MockAudio")) return;
                if (!bindSpectrum(spectrum, "MockAudio")) return;
                pause(3.0);                               // fill one 4096-point window
                ctx->ItemClick("Fit Hz");
                pause(1.0);
                shot(spectrum.c_str(), "spectrum");
            }
        }
        if (want("spectrogram")) {
            if (!connect("mock-chirp", "MockChirp")) return;
            const std::string spectro = newWindow("New spectrogram", "Spectrogram ");
            if (!bindSpectrogram(spectro, "MockChirp", 8.0f)) return;
            pause(9.0);                                   // one full 8 s sweep across the span
            ctx->ItemClick("Fit Hz");
            pause(1.0);
            shot(spectro.c_str(), "spectrogram");
        }
        if (want("erp")) {
            if (!connect("mock-evoked", "MockEvoked")) return;
            connect("mock-evoked-markers", nullptr);
            const std::string erp = newWindow("New ERP (marker average)", "ERP ");
            if (!bindErp(erp)) return;
            waitFor([] { return erpProbe().count >= 30; }, 90.0);
            ctx->LogInfo("ERP: %d epochs", erpProbe().count);
            shot(erp.c_str(), "erp");
        }
        if (want("live")) {
            if (!connect("mock-evoked", "MockEvoked") || !connect("mock-chirp", "MockChirp") ||
                !connect("mock-eeg", "MockEEG") || !connect("mock-drift", "MockDrift") ||
                !connect("mock-audio", "MockAudio"))
                return;
            connect("mock-evoked-markers", nullptr);
            connect("mock-markers", nullptr);
            // One analysis row, split three ways: spectrogram, spectrum, ERP.
            const std::string spectro  = newWindow("New spectrogram", "Spectrogram ");
            const std::string spectrum = newWindow("New spectrum", "Spectrum ");
            const std::string erp      = newWindow("New ERP (marker average)", "ERP ");
            ctx->DockInto(spectrum.c_str(), spectro.c_str(), ImGuiDir_Right);
            ctx->DockInto(erp.c_str(), spectrum.c_str(), ImGuiDir_Right);
            ctx->Yield(3);
            // Docking halves a node, which leaves the ERP a quarter of the row: too narrow for its
            // plot beside the controls. Drag the splitters to near thirds.
            auto nodeOf = [&](const std::string& ref) { return ctx->GetWindowByRef(ref.c_str())->DockNode; };
            const float rowX = nodeOf(spectro)->Pos.x;
            const float rowW = nodeOf(erp)->Pos.x + nodeOf(erp)->Size.x - rowX;
            auto dragSplitter = [&](const std::string& left, float frac) {
                const ImGuiDockNode* n = nodeOf(left);
                const float y = n->Pos.y + n->Size.y * 0.5f;
                ctx->MouseMoveToPos(ImVec2(n->Pos.x + n->Size.x + 1.0f, y));
                ctx->MouseDown(ImGuiMouseButton_Left);
                ctx->MouseMoveToPos(ImVec2(rowX + rowW * frac, y));
                ctx->MouseUp(ImGuiMouseButton_Left);
                ctx->Yield(2);
            };
            dragSplitter(spectro, 0.33f);
            dragSplitter(spectrum, 0.67f);
            if (!bindSpectrogram(spectro, "MockChirp", 2.0f) || !bindSpectrum(spectrum, "MockAudio") || !bindErp(erp))
                return;
            if (!openHeaders("MockEEG", {"Display", "Channels", "Markers"})) return;
            ctx->ItemCheck("Overlay markers");
            ctx->ScrollToTop(ctx->GetRef());              // the cfg child scrolled down to reach it
            waitFor([] { return erpProbe().count >= 15; }, 60.0);
            if (!cfgOf(spectro)) return;
            ctx->ItemClick("Fit Hz");
            if (!cfgOf(spectrum)) return;
            ctx->ItemClick("Fit Hz");
            ctx->SetRef("//Streams");                     // the rail would show this machine's folder
            ctx->ItemInputValue("##recdir", "");
            ctx->WindowFocus("//MockEEG");
            parkMouse();
            pause(1.0);
            // One frame per video frame, in fixed steps of the frame period, paced to the wall
            // clock: the video then plays at the speed the streams run. A full-window readback per
            // frame is slow, so the rate is the GIF's (12 fps), not 30 or 60.
            constexpr int kFps = 12;
            std::snprintf(ctx->EngineIO->VideoCaptureEncoderParams, sizeof ctx->EngineIO->VideoCaptureEncoderParams, "%s",
                          "-hide_banner -loglevel error -r $FPS -f rawvideo -pix_fmt rgba -s $WIDTHx$HEIGHT -i - "
                          "-threads 0 -y -c:v libx264 -preset ultrafast -crf 0 -pix_fmt yuv444p $OUTPUT");
            ctx->CaptureReset();
            std::snprintf(ctx->CaptureArgs->InOutputFile, sizeof ctx->CaptureArgs->InOutputFile,
                          "output/captures/readme_live.mp4");
            ctx->CaptureArgs->InFlags = ImGuiCaptureFlags_HideMouseCursor;
            ctx->CaptureArgs->InRecordFPSTarget = kFps;
            if (!ctx->CaptureBeginVideo()) { IM_ERRORF("%s", "no video capture: set LSL_FFMPEG"); return; }
            // A hair over the period, so float rounding never makes the capture skip a frame.
            ctx->EngineIO->ConfigFixedDeltaTime = 1.0f / kFps + 1e-5f;
            const auto t0 = std::chrono::steady_clock::now();
            const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / kFps));
            int frames = 0, late = 0;
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(6500)) {
                if (std::chrono::steady_clock::now() > t0 + step * (frames + 1)) ++late;
                std::this_thread::sleep_until(t0 + step * (frames + 1));
                ctx->Yield();
                ++frames;
            }
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ctx->CaptureEndVideo();
            ctx->LogInfo("video: %d frames in %.2f s (%.1f Hz, %d late; %d plays at real speed)",
                         frames, wall, frames / wall, late, kFps);
        }
    };

    // Profiling soak, opt-in so the normal suite stays fast: a default layout exercises only the
    // time series of 16 channels, which hides the analysis windows and the many-channel paths.
    //   LSL_PERF_SOAK=<seconds>  run time after setup (unset: the test returns at once)
    //   LSL_PERF_STREAM=<name>   show every channel of this stream window
    //   LSL_PERF_RASTER=1        and draw it as the raster heatmap
    //   LSL_PERF_OPEN=<list>     windows to open, from spectrum,spectrogram,erp,markers (default all;
    //                            each opens twice except erp and markers)
    //   LSL_PERF_SPECTRUM_ALL=1  select every channel in the spectrum windows (of LSL_PERF_STREAM)
    //   LSL_PERF_SPEED=<5x>      replay speed, as the Playback combo shows it (with LSL_REPLAY)
    //   LSL_PERF_SPECTRO_SPAN=<s> spectrogram span in seconds (and bind them to LSL_PERF_STREAM)
    //   LSL_PERF_CAPTURE=1       save a screenshot of the viewport at the end (output/captures)
    // Run it with LSL_PROFILE=1 LSL_AUTOCONNECT=1 and a heavy sender, e.g.
    //   uv run tools/lsl_test_streams.py --hd-channels 256 --hd-rate 8000
    //   lsl_viewer --tests soak
    t = IM_REGISTER_TEST(e, "bench", "soak");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* soak = std::getenv("LSL_PERF_SOAK");
        if (!soak) { ctx->LogInfo("LSL_PERF_SOAK not set; skipping"); return; }
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);           // discovery + autoconnect
        if (const char* name = std::getenv("LSL_PERF_STREAM")) {
            const std::string win = std::string("//") + name;
            // A replay (LSL_REPLAY) can take longer than the fixed wait above to prepare and connect.
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (ctx->GetWindowByRef(win.c_str()) == nullptr && std::chrono::steady_clock::now() < until) ctx->Yield();
            IM_CHECK_RETV(ctx->GetWindowByRef(win.c_str()) != nullptr, void());
            ctx->WindowFocus(win.c_str()); ctx->Yield(2);
            ImGuiTestItemInfo cfg = ctx->WindowInfo((win + "/cfg").c_str());
            if (cfg.Window == nullptr) { ctx->LogError("no cfg child in %s", win.c_str()); return; }
            ctx->SetRef(cfg.Window);
            ctx->ItemOpen("Channels");
            ctx->ItemClick("All");
            ctx->ItemClose("Channels");
            if (std::getenv("LSL_PERF_RASTER")) {
                ctx->ItemOpen("Display");
                ctx->ItemCheck("Raster");
                ctx->ItemClose("Display");
            }
        }
        if (const char* speed = std::getenv("LSL_PERF_SPEED")) {
            ctx->SetRef("//Streams");                    // Playback is a section of the rail
            ctx->ComboClick((std::string("##replayspeed/") + speed).c_str());
        }
        const char* openEnv = std::getenv("LSL_PERF_OPEN");
        const std::string open = openEnv ? openEnv : "spectrum,spectrogram,erp,markers";
        auto want = [&](const char* w) { return open.find(w) != std::string::npos; };
        // "spectrum" is also a prefix of "spectrogram": match it with its separator or at the end.
        const bool spectrum = (open + ",").find("spectrum,") != std::string::npos;
        for (int i = 0; i < 2 && spectrum; ++i) {
            ctx->MenuClick("//##MainMenuBar/View/New spectrum");
            ctx->Yield(2);
            std::string ref;
            if (std::getenv("LSL_PERF_SPECTRUM_ALL") && openSpectra(&ref) > 0) {
                ImGuiTestItemInfo cfg = ctx->WindowInfo((ref + "/cfg").c_str());   // a child window
                if (cfg.Window == nullptr) { ctx->LogError("no cfg child in %s", ref.c_str()); return; }
                ctx->SetRef(cfg.Window);
                // A new spectrum takes the first connected stream, and the connect order varies.
                if (const char* name = std::getenv("LSL_PERF_STREAM"))
                    ctx->ComboClick((std::string("##stream/") + name).c_str());
                ctx->ItemClick("All");
            }
        }
        for (int i = 0; i < 2 && want("spectrogram"); ++i) {
            ctx->MenuClick("//##MainMenuBar/View/New spectrogram");
            ctx->Yield(2);
            std::string ref;
            const char* span = std::getenv("LSL_PERF_SPECTRO_SPAN");
            if (span && openSpectra(&ref, "Spectrogram ") > 0) {
                ImGuiTestItemInfo cfg = ctx->WindowInfo((ref + "/cfg").c_str());   // a child window
                if (cfg.Window == nullptr) { ctx->LogError("no cfg child in %s", ref.c_str()); return; }
                ctx->SetRef(cfg.Window);
                if (const char* name = std::getenv("LSL_PERF_STREAM"))
                    ctx->ComboClick((std::string("##stream/") + name).c_str());
                ctx->ItemInputValue("span (s)", (float)std::atof(span));
            }
        }
        if (want("erp"))     ctx->MenuClick("//##MainMenuBar/View/New ERP (marker average)");
        if (want("markers")) ctx->MenuClick("//##MainMenuBar/View/Marker events");
        ctx->MouseMoveToPos(ImVec2(5, 5));               // no hover tooltips during the soak
        // Wall clock, not SleepNoSkip: that one advances simulated time, so unthrottled frames
        // would end the soak long before the producers had delivered its worth of data.
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::atof(soak));
        while (std::chrono::steady_clock::now() < end) ctx->Yield();
        if (std::getenv("LSL_PERF_CAPTURE")) {
            ctx->CaptureReset();
            ctx->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor);
        }
    };
}
