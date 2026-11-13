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

#include "combined_plot.hpp"     // trace binding, grouping and the workspace record, under test
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
extern void lslViewerErpLastPeak(double& ms, int& count);
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
extern bool lslViewerChannelGain(const char* name, int channel, float& gain);
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

// The newest combined plot: its lane labels, and per lane an index into the connected
// sources or -1 (not connected), -2 (channel missing), -3 (waiting for data).
extern void lslViewerCombinedProbe(int& id, std::vector<std::string>& ticks, std::vector<int>& state, int& groups);
extern void lslViewerCombinedEdges(std::vector<std::vector<double>>& edges);
struct CombinedProbeView { int id = 0, groups = 0; std::vector<std::string> ticks; std::vector<int> state; };
static CombinedProbeView combinedProbe() {
    CombinedProbeView p;
    lslViewerCombinedProbe(p.id, p.ticks, p.state, p.groups);
    return p;
}
// The combined plot window with the highest id ("<title>###combined<id>"), or null.
static ImGuiWindow* newestCombinedWindow() {
    ImGuiWindow* best = nullptr;
    int bestId = 0;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        const char* m = std::strstr(w->Name, "###combined");
        if (!w->WasActive || m == nullptr || std::strchr(w->Name, '/')) continue;
        if (const int id = std::atoi(m + 11); id > bestId) { bestId = id; best = w; }
    }
    return best;
}
// Adds channel `chan` of `stream` with the combined plot's Add popup (`cfg`: its config strip).
static void combinedAdd(ImGuiTestContext* ctx, ImGuiWindow* cfg, const char* stream, const char* chan) {
    ctx->SetRef(cfg);
    ctx->ItemClick("Add...");
    ctx->SetRef("//$FOCUSED");
    ctx->ComboClick((std::string("##stream/") + stream).c_str());
    ctx->ItemCheck((std::string("**/") + chan).c_str());
    ctx->ItemClick("Add selected");
    ctx->Yield(2);
}

extern std::string lslViewerAppSettingsOnly(const std::string& ini);
extern std::string lslViewerLayoutOnly(const std::string& ini);
extern int  lslViewerPageSlice(const std::vector<int>& enabled, int pageSize, int page, std::vector<int>& out);
extern std::string lslViewerSerializePaging(bool paginate, int pageSize, int page, int nch);
extern void lslViewerParsePaging(const std::string& payload, bool& paginate, int& pageSize, int& page, int& nch);

// The Y tick labels of a stream window's lane plot (stacked or raster) as drawn last frame,
// top lane first, without the "(1.5x)" gain suffix: an earlier test may have changed gains.
// Empty when the plot is not there.
static std::vector<std::string> laneTicks(ImGuiWindow* plt) {
    std::vector<std::string> out;
    ImPlotPlot* plot = plt ? GImPlot->Plots.GetByKey(ImHashStr("##plot", 0, plt->ID)) : nullptr;
    if (plot == nullptr) return out;
    const ImPlotTicker& tk = plot->Axes[ImAxis_Y1].Ticker;
    for (int i = 0; i < tk.Ticks.Size; ++i) {
        std::string s = tk.GetText(i);
        if (const std::size_t g = s.find(" ("); g != std::string::npos) s.resize(g);
        out.push_back(s);
    }
    return out;
}
static std::vector<std::string> sliceOf(const std::vector<std::string>& v, int b, int e) {
    return std::vector<std::string>(v.begin() + b, v.begin() + std::min(e, (int)v.size()));
}

// Sets up a paged stacked plot on MockEEG: every channel enabled, with the reference tick
// list taken before paging. Returns false (the test then skips) without the stream. The ref
// is left on the cfg child.
struct PagedPlot { ImGuiWindow* cfg = nullptr; ImGuiWindow* plt = nullptr; std::vector<std::string> all; };
static bool pagedPlotSetup(ImGuiTestContext* ctx, int perPage, PagedPlot& pp) {
    ctx->SleepNoSkip(2.5f, 1.0f / 30.0f);
    if (ctx->GetWindowByRef("//MockEEG") == nullptr) { ctx->LogInfo("MockEEG not present; skipping"); return false; }
    ctx->WindowFocus("//MockEEG"); ctx->Yield(2);
    pp.cfg = ctx->WindowInfo("//MockEEG/cfg").Window;
    pp.plt = ctx->WindowInfo("//MockEEG/plt").Window;
    if (pp.cfg == nullptr || pp.plt == nullptr) { ctx->LogInfo("no cfg/plt child; skipping"); return false; }
    ctx->SetRef(pp.cfg);
    ctx->ItemOpen("Display");
    ctx->ItemCheck("Stacked montage");
    ctx->ItemUncheck("Raster");
    // Closed, so the channel list below fits in a short window: the test engine scrolls the
    // list to a checkbox but not the cfg child around it.
    ctx->ItemClose("Display");
    ctx->ItemOpen("Channels");
    ctx->ItemUncheck("Paginate");        // a failed run may have left it on
    ctx->ItemInputValue("filter", "");   // All acts on the filtered list
    ctx->ItemClick("All");
    ctx->Yield(3);
    pp.all = laneTicks(pp.plt);
    ctx->LogInfo("%d channels enabled", (int)pp.all.size());
    ctx->ItemCheck("Paginate");
    ctx->ItemInputValue("per page", perPage);
    ctx->Yield(3);
    return true;
}
// Back to the default: every channel enabled, 16 per page, first page.
static void pagedPlotRestore(ImGuiTestContext* ctx, PagedPlot& pp) {
    ctx->SetRef(pp.cfg);
    ctx->ItemCheck("Paginate");
    ctx->ItemInputValue("per page", 16);
    ctx->ItemClick("All");
    ctx->ItemClick("##pgprev");
    ctx->ItemClick("##pgprev");
    ctx->ItemClick("##pgprev");
    ctx->Yield(2);
}

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

    // Channel pagination math: pages of N over the enabled list, the last one short, and the
    // page clamped into range. No UI.
    t = IM_REGISTER_TEST(e, "pages", "slice");
    t->TestFunc = [](ImGuiTestContext*) {
        std::vector<int> en, out;
        for (int c = 0; c < 64; ++c) if (c % 11 != 5) en.push_back(c);   // 58 enabled, with holes
        IM_CHECK_EQ((int)en.size(), 58);
        for (int p = 0; p < 4; ++p) {
            IM_CHECK_EQ(lslViewerPageSlice(en, 16, p, out), p);
            IM_CHECK_EQ((int)out.size(), p < 3 ? 16 : 10);
            IM_CHECK_EQ(out.front(), en[p * 16]);
            IM_CHECK_EQ(out.back(), en[p * 16 + (int)out.size() - 1]);
        }
        IM_CHECK_EQ(lslViewerPageSlice(en, 16, 4, out), 3);              // past the end
        IM_CHECK_EQ((int)out.size(), 10);
        IM_CHECK_EQ(lslViewerPageSlice(en, 16, 1000, out), 3);
        IM_CHECK_EQ(lslViewerPageSlice(en, 16, -2, out), 0);
        IM_CHECK_EQ(out.front(), en[0]);

        const std::vector<int> none;
        IM_CHECK_EQ(lslViewerPageSlice(none, 16, 2, out), 0);           // one empty page
        IM_CHECK(out.empty());

        IM_CHECK_EQ(lslViewerPageSlice(en, 1, 57, out), 57);             // N = 1: a channel per page
        IM_CHECK_EQ((int)out.size(), 1);
        IM_CHECK_EQ(out[0], en[57]);
        IM_CHECK_EQ(lslViewerPageSlice(en, 1, 58, out), 57);

        IM_CHECK_EQ(lslViewerPageSlice(en, 100, 1, out), 0);             // N > |E|: one page of all
        IM_CHECK(out == en);
    };

    // Workspaces keep the pagination fields, and a payload saved before they existed loads
    // with pagination off. 256 channels, all enabled: the longest line serializeOpts writes
    // into its fixed buffer (the vis= list is appended outside it), so a cut would lose nch.
    t = IM_REGISTER_TEST(e, "pages", "workspace_roundtrip");
    t->TestFunc = [](ImGuiTestContext*) {
        bool pg = false; int n = 0, p = 0, nch = 0;
        const std::string s = lslViewerSerializePaging(true, 8, 3, 256);
        IM_CHECK(s.find(" pg=1 pgn=8 pgi=3 ") != std::string::npos);
        lslViewerParsePaging(s, pg, n, p, nch);
        IM_CHECK(pg);
        IM_CHECK_EQ(n, 8);
        IM_CHECK_EQ(p, 3);
        IM_CHECK_EQ(nch, 256);

        lslViewerParsePaging(lslViewerSerializePaging(false, 256, 0, 1), pg, n, p, nch);
        IM_CHECK(!pg);
        IM_CHECK_EQ(n, 256);
        IM_CHECK_EQ(p, 0);

        const std::string old = "hist=10.000 stk=1 ras=0 gain=150.000 spc=0.000 hp=1 hphz=0.5000 no=0 "
                                "nohz=60.0 lp=0 lphz=40.0 ref=0 refc=0 lw=1.00 ovm=0 cfg=1 nch=32 vis=0,1,2,";
        lslViewerParsePaging(old, pg, n, p, nch);
        IM_CHECK(!pg);
        IM_CHECK_EQ(n, 16);
        IM_CHECK_EQ(p, 0);
        IM_CHECK_EQ(nch, 32);

        lslViewerParsePaging("pg=1 pgn=0 pgi=-4", pg, n, p, nch);       // hand-edited: clamped
        IM_CHECK_EQ(n, 1);
        IM_CHECK_EQ(p, 0);
        lslViewerParsePaging("pg=1 pgn=9999", pg, n, p, nch);
        IM_CHECK_EQ(n, 256);
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

    // Combined plot model, no UI: a trace binds by index while the label there matches, else by
    // label; traces group by (source, kind) in lane order; the workspace record survives keys
    // and labels with spaces, tabs, '|' and '%'.
    t = IM_REGISTER_TEST(e, "combined", "model");
    t->TestFunc = [](ImGuiTestContext*) {
        const std::vector<std::string> labels = {"Fp1", "Cz", "Pz"};
        TraceRef tr;
        tr.label = "Cz"; tr.channel = 1;
        IM_CHECK_EQ(bindTraceChannel(tr, labels), 1);                 // index match
        tr.channel = 2;
        IM_CHECK_EQ(bindTraceChannel(tr, labels), 1);                 // index moved: by label
        const std::vector<std::string> moved = {"Cz", "Fp1"};
        tr.channel = 1;
        IM_CHECK_EQ(bindTraceChannel(tr, moved), 0);                  // reconnect, new layout
        tr.channel = 7; tr.label = "Pz";
        IM_CHECK_EQ(bindTraceChannel(tr, labels), 2);                 // index out of range
        tr.label = "Oz";
        IM_CHECK_EQ(bindTraceChannel(tr, labels), -1);                // channel missing
        IM_CHECK_EQ(bindTraceChannel(tr, {}), -1);

        // Lanes 0..5 from sources 2, 0, 2, (none), 0, 2; lane 4 is raw (kind 0).
        const std::vector<int> src = {2, 0, 2, -1, 0, 2}, kind = {1, 1, 1, 1, 0, 1};
        std::vector<TraceGroup> groups;
        std::vector<int> order;
        groupTraces(src, kind, groups, order);
        IM_CHECK_EQ((int)groups.size(), 3);
        IM_CHECK(groups[0].src == 2 && groups[0].kind == 1 && groups[0].first == 0 && groups[0].count == 3);
        IM_CHECK(groups[1].src == 0 && groups[1].kind == 1 && groups[1].first == 3 && groups[1].count == 1);
        IM_CHECK(groups[2].src == 0 && groups[2].kind == 0 && groups[2].first == 4 && groups[2].count == 1);
        IM_CHECK((order == std::vector<int>{0, 2, 5, 1, 4}));
        groupTraces(std::vector<int>{-1, -2}, std::vector<int>{1, 1}, groups, order);
        IM_CHECK(groups.empty() && order.empty());

        CombinedRecord r;
        r.id = 7; r.title = "EEG + chirp: 50% | left";
        r.view = "hist=12.000 stk=1 lw=1.50";
        r.unitScale = {{"microvolts", 150.0f}, {"a.u. (x|y)", 0.25f}};
        TraceRef a;
        a.streamKey = "amp 1 serial|EEG amplifier"; a.channel = 3; a.label = "Cz"; a.filtered = true;
        TraceRef b;
        b.streamKey = "odd%key\twith|bars"; b.channel = 0; b.label = "chirp 1|2 %"; b.filtered = false; b.autoFit = true;
        r.traces = {a, b};
        const std::string line = toLine(r);
        IM_CHECK(line.find('\n') == std::string::npos);
        IM_CHECK_EQ((int)std::count(line.begin(), line.end(), '\t'), 5);
        CombinedRecord q;
        IM_CHECK(fromLine(line, q));
        IM_CHECK_EQ(q.id, 7);
        IM_CHECK_STR_EQ(q.title.c_str(), r.title.c_str());
        IM_CHECK_STR_EQ(q.view.c_str(), r.view.c_str());
        IM_CHECK_EQ((int)q.unitScale.size(), 2);
        IM_CHECK_STR_EQ(q.unitScale[1].first.c_str(), "a.u. (x|y)");
        IM_CHECK(std::fabs(q.unitScale[1].second - 0.25f) < 1e-6f);
        IM_CHECK_EQ((int)q.traces.size(), 2);
        IM_CHECK_STR_EQ(q.traces[0].streamKey.c_str(), a.streamKey.c_str());
        IM_CHECK_STR_EQ(q.traces[0].streamName.c_str(), "EEG amplifier");   // shown until it connects
        IM_CHECK(q.traces[0].channel == 3 && q.traces[0].filtered && !q.traces[0].autoFit);
        IM_CHECK_STR_EQ(q.traces[1].streamKey.c_str(), b.streamKey.c_str());
        IM_CHECK_STR_EQ(q.traces[1].label.c_str(), b.label.c_str());
        IM_CHECK(q.traces[1].channel == 0 && !q.traces[1].filtered && q.traces[1].autoFit);
        IM_CHECK(!fromLine("S\tkey\thist=10", q));
        r.traces.clear(); r.unitScale.clear();                        // an empty plot round-trips too
        IM_CHECK(fromLine(toLine(r), q) && q.traces.empty() && q.unitScale.empty());

        // Raw and filtered of one channel side by side are allowed; the same trace twice is not.
        IM_CHECK(hasTrace({a}, a.streamKey, 3, true));
        IM_CHECK(!hasTrace({a}, a.streamKey, 3, false));
        IM_CHECK(!hasTrace({a}, "other", 3, true));
    };

    // Combined plot on the demo streams: add Cz of MockEEG (500 Hz) and the chirp of MockChirp
    // (1 kHz) with the Add popup; disconnect the chirp (its lane stays, as a placeholder) and
    // connect it again (the lane binds again). Needs LSL_DEMO=1; skips without the streams.
    t = IM_REGISTER_TEST(e, "combined", "demo_streams");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);
        if (ctx->GetWindowByRef("//MockEEG") == nullptr || ctx->GetWindowByRef("//MockChirp") == nullptr) {
            ctx->LogInfo("MockEEG / MockChirp not present; skipping");
            return;
        }
        ctx->MenuClick("//##MainMenuBar/View/New combined plot");
        ctx->Yield(3);
        ImGuiWindow* w = newestCombinedWindow();
        IM_CHECK_RETV(w != nullptr, );
        const int id = combinedProbe().id;
        IM_CHECK(combinedProbe().ticks.empty());
        ctx->SetRef(w);
        ImGuiTestItemInfo cfg = ctx->WindowInfo("cfg");
        IM_CHECK_RETV(cfg.Window != nullptr, );
        auto add = [&](const char* stream, const char* chan) { combinedAdd(ctx, cfg.Window, stream, chan); };
        add("MockEEG", "Cz");
        add("MockChirp", "chirp");
        ctx->SleepNoSkip(2.0f, 1.0f / 30.0f);
        CombinedProbeView p = combinedProbe();
        IM_CHECK_EQ(p.id, id);
        IM_CHECK_EQ((int)p.ticks.size(), 2);
        if (p.ticks.size() != 2) return;
        ctx->LogInfo("lanes: '%s', '%s'", p.ticks[0].c_str(), p.ticks[1].c_str());
        IM_CHECK_STR_EQ(p.ticks[0].c_str(), "Cz (MockEEG)");
        IM_CHECK_STR_EQ(p.ticks[1].c_str(), "chirp (MockChirp)");
        IM_CHECK(p.state[0] >= 0 && p.state[1] >= 0 && p.state[0] != p.state[1]);
        IM_CHECK_EQ(p.groups, 2);                                      // one read per stream
        ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor);

        ctx->WindowClose("//MockChirp");                               // closing the window disconnects
        ctx->SleepNoSkip(1.0f, 1.0f / 30.0f);
        p = combinedProbe();
        IM_CHECK_EQ((int)p.ticks.size(), 2);
        if (p.ticks.size() != 2) return;
        IM_CHECK_STR_EQ(p.ticks[1].c_str(), "chirp (MockChirp, not connected)");
        IM_CHECK_EQ(p.state[1], -1);
        IM_CHECK(p.state[0] >= 0);
        IM_CHECK_EQ(p.groups, 1);
        ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor);

        lslViewerRequestConnect("mock-chirp");
        for (int i = 0; i < 300 && combinedProbe().state.size() == 2 && combinedProbe().state[1] < 0; ++i)
            ctx->SleepNoSkip(0.05f, 1.0f / 30.0f);
        ctx->SleepNoSkip(1.0f, 1.0f / 30.0f);
        p = combinedProbe();
        IM_CHECK(p.state.size() == 2 && p.state[1] >= 0);
        if (p.ticks.size() == 2) IM_CHECK_STR_EQ(p.ticks[1].c_str(), "chirp (MockChirp)");
        ctx->WindowFocus(w->ID);                                       // the reconnected stream's tab took focus
        ctx->Yield(2);
        ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor);

        // A 48 kHz trace takes the envelope path on its own; the overlay gives each unit an axis.
        add("MockAudio", "L");
        ctx->SleepNoSkip(2.0f, 1.0f / 30.0f);
        p = combinedProbe();
        IM_CHECK_EQ((int)p.ticks.size(), 3);
        IM_CHECK_EQ(p.groups, 3);
        ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor);
        ctx->SetRef(cfg.Window);
        ctx->ItemOpen("Display");
        ctx->ItemUncheck("Stacked lanes");
        ctx->SleepNoSkip(1.0f, 1.0f / 30.0f);
        ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor);
        ctx->ItemCheck("Stacked lanes");
        ctx->WindowClose(w->ID);
        ctx->Yield(2);
    };

    // A combined plot survives a workspace save and load: its trace (raw here) binds again.
    // Needs LSL_DEMO=1; skips without MockEEG.
    t = IM_REGISTER_TEST(e, "combined", "workspace");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);
        if (ctx->GetWindowByRef("//MockEEG") == nullptr) { ctx->LogInfo("MockEEG not present; skipping"); return; }
        ctx->MenuClick("//##MainMenuBar/View/New combined plot");
        ctx->Yield(3);
        ImGuiWindow* w = newestCombinedWindow();
        IM_CHECK_RETV(w != nullptr, );
        ctx->SetRef(w);
        ImGuiTestItemInfo cfg = ctx->WindowInfo("cfg");
        IM_CHECK_RETV(cfg.Window != nullptr, );
        combinedAdd(ctx, cfg.Window, "MockEEG", "Cz");
        ctx->SetRef(cfg.Window);
        ctx->ItemClick("**/filt");                                     // the trace shows raw samples
        ctx->Yield(3);
        IM_CHECK(combinedProbe().ticks.size() == 1 && combinedProbe().ticks[0] == "Cz (MockEEG, raw)");
        // Auto-fit on and off: its button is tinted while on, and the push and pop of that tint
        // must not follow the flag the same click flips (ImGui reports an unbalanced style stack).
        ctx->ItemClick("**/fit");
        ctx->Yield(2);
        ctx->ItemClick("**/fit");
        ctx->Yield(2);

        ctx->MenuClick("//##MainMenuBar/Workspaces");
        ctx->SetRef("//$FOCUSED");
        ctx->ItemInputValue("##wsname", "combined_ws_test");
        ctx->ItemClick("Save");
        ctx->WindowClose(w->ID);
        ctx->Yield(3);
        IM_CHECK_EQ(combinedProbe().id, 0);                            // no combined plot left

        ctx->MenuClick("//##MainMenuBar/Workspaces");
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("**/combined_ws_test");
        ctx->SleepNoSkip(2.0f, 1.0f / 30.0f);
        const CombinedProbeView p = combinedProbe();
        IM_CHECK(p.id > 0);
        IM_CHECK_EQ((int)p.ticks.size(), 1);
        if (p.ticks.size() == 1) {
            IM_CHECK_STR_EQ(p.ticks[0].c_str(), "Cz (MockEEG, raw)");
            IM_CHECK(p.state[0] >= 0);
        }
        ctx->MenuClick("//##MainMenuBar/Workspaces");                  // clean up the saved file
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("**/combined_ws_test/\xc3\x97");
        ctx->KeyPress(ImGuiKey_Escape);
        if (ImGuiWindow* w2 = newestCombinedWindow()) ctx->WindowClose(w2->ID);
        ctx->Yield(2);
    };

    // Alignment of two streams in a combined plot (design test 4). Both carry a square wave that
    // rises at the same true times, every 2 s; one is a device running 500 ppm fast that declares
    // the same 250 Hz. Both stamp the true time, as liblsl does. Without the drift map the fast
    // stream's edges walk 30 ms a minute; with it, once converged (after the first minute), the
    // two lanes' rising edges must lie within one sample (4 ms) of each other on the plot's time
    // axis. Read through a probe of the drawn x axis, not pixels. Four minutes of stamps are sent
    // at 25x, so the test takes about 10 s. LSL_DRIFT_CORR=0 checks the drift instead.
    t = IM_REGISTER_TEST(e, "combined", "alignment");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto waitFor = [&](auto pred, double seconds) {   // wall clock: LSL runs on the real one
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        const char* env = std::getenv("LSL_DRIFT_CORR");
        const bool corrected = !(env && std::strcmp(env, "0") == 0);
        constexpr double kRate = 250.0, kPpm = 500.0, kSpeed = 25.0, kSpan = 240.0, kSettle = 60.0;
        auto info = [](const char* name, const char* sid) {
            lsl::stream_info i(name, "Signal", 1, kRate, lsl::cf_float32, sid);
            i.desc().append_child("channels").append_child("channel").append_child_value("label", "sq");
            return i;
        };
        lsl::stream_outlet ref(info("AlignRef", "aligntest-ref")), fast(info("AlignFast", "aligntest-fast"));
        lslViewerRequestConnect("aligntest-ref");
        IM_CHECK_RETV(waitFor([&] { return streamProbe("AlignRef").found && ref.have_consumers(); }, 15.0), );
        lslViewerRequestConnect("aligntest-fast");
        IM_CHECK_RETV(waitFor([&] { return streamProbe("AlignFast").found && fast.have_consumers(); }, 15.0), );

        ctx->MenuClick("//##MainMenuBar/View/New combined plot");
        ctx->Yield(3);
        ImGuiWindow* w = newestCombinedWindow();
        IM_CHECK_RETV(w != nullptr, );
        ctx->SetRef(w);
        ImGuiTestItemInfo cfg = ctx->WindowInfo("cfg");
        IM_CHECK_RETV(cfg.Window != nullptr, );
        combinedAdd(ctx, cfg.Window, "AlignRef", "sq");
        combinedAdd(ctx, cfg.Window, "AlignFast", "sq");
        ctx->SetRef(cfg.Window);
        ctx->ItemClick("**/filt");                       // both raw: the high-pass would bend the steps
        ctx->ItemClick("**/filt");
        ctx->Yield(2);
        IM_CHECK_RETV(combinedProbe().ticks.size() == 2, );
        // For review: the two lanes overlaid, 3 s wide (a capture is taken near the end).
        ctx->ItemOpen("Display");
        ctx->ItemInputValue("History (s)", 3.0f);
        ctx->ItemUncheck("Stacked lanes");
        ctx->WindowFocus(w->ID);
        ctx->MouseMoveToPos(ImVec2(5, 5));
        bool shot = false;

        // Device sample k of a stream with rate `r` is taken at true time k / r after `base`.
        const double base = lsl::local_clock();
        const double rates[2] = {kRate, kRate * (1.0 + kPpm * 1e-6)};
        lsl::stream_outlet* outs[2] = {&ref, &fast};
        long long k[2] = {0, 0};
        std::vector<float> v(5);
        double lateMax = 0.0, earlyMax = 0.0;
        int    matched = 0;
        std::vector<std::vector<double>> edges;
        const auto wall0 = std::chrono::steady_clock::now();
        for (;;) {
            const double virt = kSpeed * std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
            if (virt > kSpan) break;
            for (int o = 0; o < 2; ++o)   // chunks of 5, stamped with the newest sample, as liblsl does
                for (const long long due = (long long)(virt * rates[o]); k[o] + 5 <= due; k[o] += 5) {
                    for (int i = 0; i < 5; ++i)
                        v[(std::size_t)i] = std::fmod((double)(k[o] + i) / rates[o], 2.0) < 1.0 ? 1.0f : 0.0f;
                    outs[o]->push_chunk_multiplexed(v, base + (double)(k[o] + 4) / rates[o]);
                }
            ctx->Yield();
            lslViewerCombinedEdges(edges);
            if (edges.size() == 2)
                for (double a : edges[0]) {   // each reference edge against the fast lane's nearest
                    double best = 1e9;
                    for (double b : edges[1]) if (std::fabs(b - a) < std::fabs(best)) best = b - a;
                    if (std::fabs(best) > 0.5) continue;   // that edge is not in the other lane yet
                    if (virt > kSettle) { lateMax = std::max(lateMax, std::fabs(best)); ++matched; }
                    else                  earlyMax = std::max(earlyMax, std::fabs(best));
                }
            if (!shot && virt > kSpan - 30.0) { ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor); shot = true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ctx->LogInfo("alignment %s: before %.0f s up to %.2f ms apart; after, up to %.2f ms (%d edge pairs)",
                     corrected ? "corrected" : "NOT corrected", kSettle, earlyMax * 1e3, lateMax * 1e3, matched);
        IM_CHECK_GT(matched, 20);
        if (corrected) IM_CHECK_LE(lateMax, 1.0 / kRate + 1e-5);   // one sample of the slower stream
        else           IM_CHECK_GT(lateMax, 20.0 / 1000.0);        // 500 ppm: 30 ms a minute

        ctx->WindowClose(w->ID);
        ctx->WindowClose("//AlignRef");                  // disconnect before the outlets go
        ctx->WindowClose("//AlignFast");
        ctx->Yield(3);
    };

    // Drag and drop into a combined plot: a channel row of a stream plot onto the empty plot, a
    // lane label onto the trace list (inserted at that row), and a channel onto a stream plot
    // (a new combined plot with that stream's enabled channels, then the dropped one). Needs
    // LSL_DEMO=1; skips without MockEEG.
    t = IM_REGISTER_TEST(e, "combined", "drag_drop");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        constexpr float uiFreeLeft = 320.0f;   // px of MockEEG's plot (labels included) left uncovered
        ctx->SleepNoSkip(3.0f, 1.0f / 30.0f);
        if (ctx->GetWindowByRef("//MockEEG") == nullptr) { ctx->LogInfo("MockEEG not present; skipping"); return; }
        ctx->MenuClick("//##MainMenuBar/View/New combined plot");
        ctx->Yield(3);
        ImGuiWindow* w = newestCombinedWindow();
        IM_CHECK_RETV(w != nullptr, );
        ctx->WindowFocus("//MockEEG");
        ctx->Yield(2);
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//MockEEG/cfg");
        ImGuiTestItemInfo plt = ctx->WindowInfo("//MockEEG/plt");
        IM_CHECK_RETV(cfg.Window != nullptr && plt.Window != nullptr, );
        // Float it over the right part of MockEEG's plot, so that both are on screen for the drag
        // and the left part of that plot stays free to drop on.
        ctx->UndockWindow((std::string("//") + w->Name).c_str());   // absolute: the ref may be a child
        const ImVec2 wp = ImGui::GetMainViewport()->WorkPos, ws = ImGui::GetMainViewport()->WorkSize;
        const float  wx = plt.Window->Pos.x + uiFreeLeft;
        // Set directly: a title-bar drag across the dock space would dock it again.
        ImGui::SetWindowPos(w->Name, ImVec2(wx, plt.Window->Pos.y + 20.0f));
        ImGui::SetWindowSize(w->Name, ImVec2(std::max(300.0f, wp.x + ws.x - wx - 10.0f), ws.y * 0.6f));
        ctx->Yield(2);
        ctx->WindowFocus("//MockEEG");
        ctx->Yield(2);
        ctx->LogInfo("MockEEG plot at x %.0f, combined plot at x %.0f", plt.Window->Pos.x, w->Pos.x);
        ctx->SetRef(cfg.Window);
        ctx->ItemOpen("Display");
        ctx->ItemCheck("Stacked montage");
        ctx->ItemClose("Display");
        ctx->ItemOpen("Channels");
        ctx->Yield(2);
        auto dragTo = [&](ImVec2 to, const char* shot = nullptr) {
            ctx->MouseDown(ImGuiMouseButton_Left);
            ctx->MouseMoveToPos(ImVec2(ImGui::GetIO().MousePos.x + 30.0f, ImGui::GetIO().MousePos.y));
            ctx->MouseMoveToPos(to);
            ctx->Yield(2);
            if (shot) ctx->CaptureScreenshotWindow(shot, 0);   // the drop tooltip, before the release
            ctx->MouseUp(ImGuiMouseButton_Left);
            ctx->Yield(3);
        };
        auto center = [](ImGuiWindow* win) { return ImVec2(win->Pos.x + win->Size.x * 0.65f, win->Pos.y + win->Size.y * 0.5f); };

        // 1) The "Pz" row of MockEEG's channel list onto the empty combined plot.
        ctx->SetRef(w);
        ImGuiTestItemInfo cplt = ctx->WindowInfo("plt");
        IM_CHECK_RETV(cplt.Window != nullptr, );
        ctx->SetRef(cfg.Window);
        ctx->MouseMove("**/Pz");
        dragTo(center(cplt.Window));
        CombinedProbeView p = combinedProbe();
        IM_CHECK_EQ((int)p.ticks.size(), 1);
        if (!p.ticks.empty()) IM_CHECK_STR_EQ(p.ticks[0].c_str(), "Pz (MockEEG)");

        // 1b) The same from the row's label text, not its box: a label is the larger target.
        {
            ctx->MouseMove("**/Cz");   // scrolls the row into view; then aim at its text
            const ImGuiTestItemInfo cz = ctx->ItemInfo("**/Cz");
            IM_CHECK_RETV(cz.ID != 0, );
            ctx->MouseMoveToPos(ImVec2(cz.RectFull.Max.x - 3.0f, cz.RectFull.GetCenter().y));
            dragTo(center(cplt.Window));
            p = combinedProbe();
            IM_CHECK_EQ((int)p.ticks.size(), 2);
            if (p.ticks.size() == 2) IM_CHECK_STR_EQ(p.ticks[1].c_str(), "Cz (MockEEG)");
            // A drag must not also toggle the channel it started on.
            ctx->SetRef(cfg.Window);
            IM_CHECK(ctx->ItemIsChecked("**/Cz"));
            // Back to one trace, so the steps below see what they expect.
            ctx->SetRef(w);
            ctx->ItemClick("**/$$1/x");
            ctx->SetRef(cfg.Window);
            ctx->Yield(2);
            IM_CHECK_EQ((int)combinedProbe().ticks.size(), 1);
        }

        // 2) MockEEG's first lane label (Fp1) onto the trace list's first row: inserted before Pz.
        ctx->MouseMoveToPos(ImVec2(plt.Window->Pos.x + 22.0f, plt.Window->Pos.y + 10.0f + (plt.Window->Size.y - 50.0f) * (0.5f / 16.0f)));
        ctx->Yield(2);
        ctx->SetRef(w);
        ImGuiTestItemInfo row = ctx->ItemInfo("**/MockEEG: Pz");
        IM_CHECK_RETV(row.ID != 0, );
        dragTo(row.RectFull.GetCenter());
        p = combinedProbe();
        IM_CHECK_EQ((int)p.ticks.size(), 2);
        if (p.ticks.size() == 2) {
            IM_CHECK_STR_EQ(p.ticks[0].c_str(), "Fp1 (MockEEG)");
            IM_CHECK_STR_EQ(p.ticks[1].c_str(), "Pz (MockEEG)");
        }
        ctx->CaptureScreenshotWindow(w->ID, ImGuiCaptureFlags_HideMouseCursor);
        const int firstId = p.id;

        // 3) The "Pz" row onto MockEEG's own plot, with Pz disabled there: a new combined plot,
        //    MockEEG's enabled channels (all 32 but Pz, as the default enables all) and then Pz.
        //    MockEEG's plot is unchanged.
        ctx->WindowFocus("//MockEEG");
        ctx->Yield(2);
        ctx->SetRef(cfg.Window);
        ctx->ItemUncheck("**/Pz");
        ctx->MouseMove("**/Pz");
        dragTo(ImVec2(plt.Window->Pos.x + 0.7f * uiFreeLeft, plt.Window->Pos.y + plt.Window->Size.y * 0.5f), "//MockEEG");
        ctx->Yield(3);
        p = combinedProbe();
        IM_CHECK(p.id != firstId);
        IM_CHECK_EQ((int)p.ticks.size(), 32);
        if (p.ticks.size() == 32) {
            IM_CHECK_STR_EQ(p.ticks[0].c_str(), "Fp1 (MockEEG)");
            IM_CHECK_STR_EQ(p.ticks[31].c_str(), "Pz (MockEEG)");
        }
        ctx->WindowFocus("//MockEEG");                 // the new combined plot came up in front
        ctx->Yield(2);
        ctx->SetRef(cfg.Window);
        ctx->ItemCheck("**/Pz");
        if (ImGuiWindow* w2 = newestCombinedWindow(); w2 && w2 != w) ctx->WindowClose(w2->ID);
        ctx->WindowClose(w->ID);
        ctx->Yield(2);
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

    // Channel pagination on a stacked plot: each page's lane labels, the next arrow stopping
    // on the last page, the corner label wrapping, PgDn on the hovered plot, and disabling
    // channels clamping the page. Needs MockEEG (demo streams); skips without it.
    t = IM_REGISTER_TEST(e, "ui", "pages_stacked");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        constexpr int N = 8;
        PagedPlot pp;
        if (!pagedPlotSetup(ctx, N, pp)) return;
        const int C = (int)pp.all.size();
        if (C <= N) { ctx->LogInfo("only %d channels; skipping", C); pagedPlotRestore(ctx, pp); return; }
        const int P = (C + N - 1) / N;
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, 0, N));
        for (int p = 1; p < P; ++p) {
            ctx->ItemClick("##pgnext");
            ctx->Yield(3);
            IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, p * N, p * N + N));
        }
        ctx->ItemClick("##pgnext");                              // past the last page: stays
        ctx->Yield(3);
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, (P - 1) * N, C));

        ctx->SetRef(pp.plt);
        ctx->ItemClick("##pgcorner");                            // the corner label wraps
        ctx->Yield(3);
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, 0, N));
        ctx->MouseMoveToPos(pp.plt->Rect().GetCenter());
        ctx->KeyPress(ImGuiKey_PageDown);
        ctx->Yield(3);
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, N, 2 * N));
        ctx->KeyPress(ImGuiKey_PageUp);
        ctx->Yield(3);
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, 0, N));

        // Back to the last page, then disable its channels: the page holds while one is
        // left on it, and clamps to the page before once none is.
        ctx->SetRef(pp.cfg);
        for (int p = 1; p < P; ++p) ctx->ItemClick("##pgnext");
        ctx->Yield(3);
        ctx->SetRef(ctx->WindowInfo("chsel").Window);
        for (int c = (P - 1) * N; c < C - 1; ++c) ctx->ItemUncheck(pp.all[c].c_str());
        ctx->Yield(3);
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, C - 1, C));
        ctx->ItemUncheck(pp.all[C - 1].c_str());
        ctx->Yield(3);
        IM_CHECK(laneTicks(pp.plt) == sliceOf(pp.all, (P - 2) * N, (P - 1) * N));

        pagedPlotRestore(ctx, pp);
    };

    // A paged stacked plot on its short last page: the corner label, and the empty lane slots
    // at the bottom that keep the lane height of the full pages.
    t = IM_REGISTER_TEST(e, "ui", "capture_pages");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        PagedPlot pp;
        if (!pagedPlotSetup(ctx, 12, pp)) return;
        const int P = ((int)pp.all.size() + 11) / 12;
        for (int p = 1; p < P; ++p) ctx->ItemClick("##pgnext");
        ctx->MouseMoveToPos(ImVec2(ImGui::GetMainViewport()->WorkSize.x - 5.0f, 5.0f));   // no tooltip
        ctx->SleepNoSkip(1.0f, 1.0f / 30.0f);
        ctx->CaptureScreenshotWindow("//MockEEG", ImGuiCaptureFlags_HideMouseCursor);
        pagedPlotRestore(ctx, pp);
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

    // One stream that mixes EEG (100 uV) with a microphone input (+/- 600000 uV), as some amps
    // do. Auto-gain must bring both to the lane: the microphone needs a gain near 1e-4.
    t = IM_REGISTER_TEST(e, "stream", "auto_gain_wide");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto waitFor = [&](auto pred, double seconds) {   // wall clock: LSL runs on the real one
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ctx->Yield();
            }
            return pred();
        };
        const char* kName = "GainWide";
        constexpr double kRate = 250.0;
        constexpr float kEeg = 100.0f, kMic = 600000.0f;
        lsl::stream_outlet out(lsl::stream_info(kName, "EEG", 2, kRate, lsl::cf_float32, "gaintest-wide"));
        lslViewerRequestConnect("gaintest-wide");
        IM_CHECK(waitFor([&] { return streamProbe(kName).found && out.have_consumers(); }, 15.0));
        std::vector<float> v;
        const auto start = std::chrono::steady_clock::now();
        long long sent = 0;
        auto pushDue = [&] {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            v.clear();
            for (; (double)sent / kRate <= el; ++sent) {
                const double ph = 6.283185307179586 * (double)sent / kRate;
                v.push_back(kEeg * (float)std::sin(10.0 * ph));
                v.push_back(kMic * (float)std::sin(3.0 * ph));
            }
            if (!v.empty()) out.push_chunk_multiplexed(v.data(), v.size());
        };
        IM_CHECK(waitFor([&] { pushDue(); return (double)sent / kRate > 3.0; }, 10.0));

        ctx->WindowFocus("//GainWide"); ctx->Yield(2);
        ImGuiTestItemInfo cfg = ctx->WindowInfo("//GainWide/cfg");
        IM_CHECK(cfg.Window != nullptr);
        ctx->SetRef(cfg.Window);
        ctx->ItemOpen("Display");
        ctx->ItemUncheck("High-pass");       // keep the sines as sent
        ctx->ItemOpen("Channel gains");
        ctx->ItemClick("Auto");
        waitFor([&] { pushDue(); return false; }, 0.2);   // a few frames for Auto to measure and apply
        float gEeg = 0.0f, gMic = 0.0f;
        IM_CHECK(lslViewerChannelGain(kName, 0, gEeg) && lslViewerChannelGain(kName, 1, gMic));
        ctx->LogInfo("%s: auto gains %.4g (EEG) and %.4g (mic)", kName, gEeg, gMic);
        // Auto puts the amplitude at 0.4 lane height (150 uV a lane): 0.6 and 1e-4.
        IM_CHECK(gEeg > 0.4f && gEeg < 0.9f);
        IM_CHECK(gMic > 0.5e-4f && gMic < 2e-4f);
        IM_CHECK_GT(gEeg / gMic, 3000.0f);
        // Dragging the gain keeps the old feel: 100 px from 1x is about 0.04 decades (1.1x).
        ctx->ItemClick("Reset");
        ctx->ItemDragWithDelta("$$0/ch 0", ImVec2(100.0f, 0.0f));
        ctx->Yield(2);
        IM_CHECK(lslViewerChannelGain(kName, 0, gEeg));
        ctx->LogInfo("%s: gain %.4g after a 100 px drag from 1x", kName, gEeg);
        IM_CHECK(gEeg > 1.03f && gEeg < 1.2f);
        ctx->ItemCheck("High-pass");
        ctx->WindowClose("//GainWide");
        ctx->Yield(3);
    };

    // A device off its nominal rate must not move the ERP. The data stream declares 250 Hz but
    // runs 500 ppm fast, with a peak 100 ms after each marker; the markers carry the true time.
    // Without the drift map the peak walks 30 ms a minute; with it, every epoch after the first
    // minute has its peak within one sample of 100 ms. Ten minutes of stamps are sent at 25x, so
    // the test takes 24 s. With LSL_DRIFT_CORR=0 it checks the drift instead (the A/B reference).
    t = IM_REGISTER_TEST(e, "stream", "drift_erp_latency");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto waitFor = [&](auto pred, double seconds) {   // wall clock: LSL runs on the real one
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (!pred() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx->Yield();
            }
            return pred();
        };
        const char* env = std::getenv("LSL_DRIFT_CORR");
        const bool corrected = !(env && std::strcmp(env, "0") == 0);
        constexpr double kRate = 250.0, kPpm = 500.0, kSpeed = 25.0, kSpan = 600.0, kLatency = 0.100;
        lsl::stream_outlet dout(lsl::stream_info("DriftErpTest", "EEG", 1, kRate, lsl::cf_float32, "drifttest-data"));
        lsl::stream_outlet mout(lsl::stream_info("DriftErpMarkers", "Markers", 1, lsl::IRREGULAR_RATE,
                                                 lsl::cf_string, "drifttest-markers"));
        lslViewerRequestConnect("drifttest-data");
        IM_CHECK(waitFor([&] { return streamProbe("DriftErpTest").found && dout.have_consumers(); }, 15.0));
        lslViewerRequestConnect("drifttest-markers");
        IM_CHECK(waitFor([&] { return mout.have_consumers(); }, 15.0));

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
        ctx->ComboClick("stream/DriftErpTest");
        ctx->ComboClick("trigger/DriftErpMarkers");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(erpProbe().stream.c_str(), "DriftErpTest");
        IM_CHECK_STR_EQ(erpProbe().trigger.c_str(), "DriftErpMarkers");

        // True time of device sample k, from `base`: the device makes kRate * (1 + ppm) samples a second.
        const double base = lsl::local_clock(), devRate = kRate * (1.0 + kPpm * 1e-6);
        std::vector<double> peaks;                       // true peak times still ahead of the data
        std::vector<float>  v(5);
        long long k = 0;
        double nextMarker = 1.0;
        double lateLo = 1e9, lateHi = -1e9, firstMs = 0.0, lastMs = 0.0;
        int lastCount = 0, late = 0;
        const auto wall0 = std::chrono::steady_clock::now();
        for (;;) {
            const double virt = kSpeed * std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
            if (virt > kSpan) break;
            for (; nextMarker <= virt; nextMarker += 0.8) {
                mout.push_sample(std::vector<std::string>{"stim"}, base + nextMarker);
                peaks.push_back(nextMarker + kLatency);
            }
            // Chunks of 5 samples, stamped like liblsl does: the newest sample, the rest back-filled.
            for (const long long due = (long long)(virt * devRate); k + 5 <= due; k += 5) {
                for (int i = 0; i < 5; ++i) {
                    const double tk = (double)(k + i) / devRate;
                    float y = 0.0f;
                    for (double tp : peaks) { const double z = (tk - tp) / 0.008; y += 100.0f * (float)std::exp(-0.5 * z * z); }
                    v[(std::size_t)i] = y;
                }
                dout.push_chunk_multiplexed(v, base + (double)(k + 4) / devRate);
                const double tNow = (double)(k + 4) / devRate;
                std::erase_if(peaks, [&](double tp) { return tp < tNow - 0.1; });
            }
            double ms = 0.0; int count = 0;
            lslViewerErpLastPeak(ms, count);
            if (count > lastCount) {
                if (lastCount == 0) firstMs = ms;
                lastMs = ms; lastCount = count;
                if (virt > 60.0) { lateLo = std::min(lateLo, ms); lateHi = std::max(lateHi, ms); ++late; }
            }
            ctx->Yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ctx->LogInfo("drift %s: %d epochs, peak %.1f ms first, %.1f ms last; after 60 s %.1f .. %.1f ms (%d epochs)",
                     corrected ? "corrected" : "NOT corrected", lastCount, firstMs, lastMs, lateLo, lateHi, late);
        IM_CHECK_GT(late, 100);
        if (corrected) {
            IM_CHECK_LE(lateHi - lateLo, 1000.0 / kRate + 0.01);           // one sample
            IM_CHECK_LT(std::fabs(0.5 * (lateLo + lateHi) - kLatency * 1000.0), 1000.0 / kRate);
        } else {
            IM_CHECK_GT(lateHi - lateLo, 100.0);                            // 500 ppm over 9 min: ~270 ms
        }
        ctx->WindowClose(ref);
        ctx->WindowClose("//DriftErpTest");               // disconnect before the outlets go
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
    // The same list also takes the images of the usage pages (see the block in the test), which
    // tools/make_doc_screenshots.py runs and copies to docs/images/.
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
        auto shot = [&](ImGuiTestRef window, const char* name) {   // empty ref: the whole viewport
            parkMouse();
            ctx->CaptureReset();
            std::snprintf(ctx->CaptureArgs->InOutputFile, sizeof ctx->CaptureArgs->InOutputFile,
                          "output/captures/readme_%s.png", name);
            if (!window.IsEmpty()) ctx->CaptureAddWindow(window);
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

        // The images of the usage pages in docs/: overview, plot, combined, markers, recording,
        // playback. These run on the built-in demo streams (LSL_DEMO=1), which connect themselves,
        // so tools/make_doc_screenshots.py can take them with no other process. Each shot leaves
        // the view as the next one expects it, so the order here is the order they are taken in.
        if (want("overview") || want("plot") || want("combined") || want("markers") ||
            want("recording") || want("playback")) {
            auto have = [&](const char* w) { return ctx->GetWindowByRef(w) != nullptr; };
            if (!waitFor([&] { return have("//MockEEG") && have("//MockChirp") && have("//MockAudio") &&
                                      have("//MockEvoked"); }, 20.0)) {
                IM_ERRORF("%s", "the demo streams did not connect: set LSL_DEMO=1");
                return;
            }
            pause(2.0);                                   // the marker stream opens no window to wait on
            ctx->SetRef("//Streams");                     // the rail would show this machine's folder
            ctx->ItemInputValue("##recdir", "");
            if (want("recording")) {
                ctx->ItemOpen("filename fields");
                shot("//Streams", "recording");
                ctx->SetRef("//Streams");
                ctx->ItemClose("filename fields");
            }
            if (want("overview") || want("plot")) {
                const std::string spectrum = newWindow("New spectrum", "Spectrum ");
                if (!bindSpectrum(spectrum, "MockAudio")) return;
                if (!openHeaders("MockEEG", {"Display"})) return;
                ctx->ItemCheck("High-pass");              // else the drift takes the lanes off center
                pause(11.0);                              // fill the 10 s history
                if (!cfgOf(spectrum)) return;
                ctx->ItemClick("Fit Hz");
                ctx->WindowFocus("//MockEEG");
                if (want("overview")) shot(ImGuiTestRef(), "overview");
                ctx->WindowClose(spectrum.c_str());       // the plot then has the full height
                ctx->Yield(3);
                if (want("plot")) {
                    if (!openHeaders("MockEEG", {"Channels"})) return;
                    ctx->ScrollToTop(ctx->GetRef());      // the cfg child scrolled down to reach it
                    shot("//MockEEG", "plot");
                }
            }
            if (want("combined")) {
                ctx->MenuClick("//##MainMenuBar/View/New combined plot");
                ctx->Yield(3);
                ImGuiWindow* w = newestCombinedWindow();
                if (w == nullptr) { IM_ERRORF("%s", "no combined plot window"); return; }
                ctx->SetRef(w);
                ImGuiTestItemInfo cfg = ctx->WindowInfo("cfg");
                if (cfg.Window == nullptr) { IM_ERRORF("%s", "no cfg child in the combined plot"); return; }
                combinedAdd(ctx, cfg.Window, "MockEEG", "Cz");
                combinedAdd(ctx, cfg.Window, "MockEEG", "Pz");
                combinedAdd(ctx, cfg.Window, "MockChirp", "chirp");
                combinedAdd(ctx, cfg.Window, "MockEvoked", "Cz");
                ctx->SetRef(cfg.Window);
                ctx->ItemOpen("Traces");
                ctx->WindowFocus(w->ID);
                pause(11.0);                              // fill the 10 s history
                shot(w->ID, "combined");
                ctx->WindowClose(w->ID);
            }
            if (want("markers")) {
                ctx->MenuCheck("//##MainMenuBar/View/Marker events");
                ctx->Yield(2);
                ctx->SetRef("//Marker events");
                if (!ctx->ItemExists("**/Clear all")) { IM_ERRORF("%s", "no marker stream connected"); return; }
                ctx->ItemOpen("**/###mk");
                pause(1.0);
                shot("//Marker events", "markers");
            }
            if (want("playback")) {
                // The rail shows the file name, so give the file a name a recording would have.
                const std::filesystem::path file =
                    std::filesystem::temp_directory_path() / "sub-P001_ses-S001_task-Default_run-001_eeg.xdf";
                writeReplayTestFile(file.string());
                lslViewerRequestReplay(file.string().c_str());
                const bool playing = waitFor([] { const auto p = replayProbe();
                                                  return p.state == (int)XdfPlayer::State::playing && p.shown == 2; }, 15.0);
                if (!playing) IM_ERRORF("%s", "the replay did not start");
                pause(3.0);                               // some samples sent, for the counts in the rows
                if (playing) shot("//Streams", "playback");
                ctx->SetRef("//Streams");
                ctx->ItemClick("Stop");
                waitFor([] { return replayProbe().shown == 0; }, 3.0);
                ctx->ItemClick("###replayclose");
                ctx->Yield(3);
                std::error_code ec; std::filesystem::remove(file, ec);
            }
        }
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
                ctx->ItemUncheck("Paginate");             // the raster shows every channel at once
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
    // Frame cost of a combined plot against the stream plots it combines: 4 streams x 16
    // channels at 500 Hz (published here), as four stream plots tiled over the dock area
    // (LSL_PERF_COMBINED=streams) or as one combined plot of all 64 channels docked in front of
    // them (=combined), so the stream plots behind it are not drawn. Control strips hidden.
    // LSL_PERF_HISTORY sets the time window (s). Runs for LSL_PERF_SOAK seconds (default 15);
    // profile with LSL_PROFILE=1 / LSL_BENCH=1. Does nothing without LSL_PERF_COMBINED, so the
    // normal suite is not slower.
    t = IM_REGISTER_TEST(e, "bench", "combined_vs_streams");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* mode = std::getenv("LSL_PERF_COMBINED");
        if (!mode) { ctx->LogInfo("LSL_PERF_COMBINED not set; skipping"); return; }
        const bool   combinedMode = std::strcmp(mode, "combined") == 0;
        const double secs = std::getenv("LSL_PERF_SOAK") ? std::atof(std::getenv("LSL_PERF_SOAK")) : 15.0;
        constexpr int kStreams = 4, kCh = 16;
        constexpr double kRate = 500.0;
        std::vector<std::unique_ptr<lsl::stream_outlet>> outs;
        for (int i = 0; i < kStreams; ++i) {
            char name[32], sid[32];
            std::snprintf(name, sizeof name, "PerfCombined%c", 'A' + i);
            std::snprintf(sid, sizeof sid, "perf-combined-%c", 'a' + i);
            lsl::stream_info info(name, "EEG", kCh, kRate, lsl::cf_float32, sid);
            lsl::xml_element chns = info.desc().append_child("channels");
            for (int c = 0; c < kCh; ++c) {
                char lab[8]; std::snprintf(lab, sizeof lab, "C%02d", c);
                lsl::xml_element ch = chns.append_child("channel");
                ch.append_child_value("label", lab);
                ch.append_child_value("unit", "microvolts");
            }
            outs.push_back(std::make_unique<lsl::stream_outlet>(info));
        }
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> sent{0};
        std::thread gen([&] {   // real time, stamped on the local clock like a live device
            std::mt19937 rng(7);
            std::normal_distribution<float> noise(0.0f, 5.0f);
            std::vector<float>  buf;
            std::vector<double> ts;
            const double t0 = lsl::local_clock();
            while (!stop.load()) {
                const std::uint64_t due = (std::uint64_t)((lsl::local_clock() - t0) * kRate), s0 = sent.load();
                if (due > s0) {
                    const std::size_t n = (std::size_t)(due - s0);
                    buf.resize(n * kCh); ts.resize(n);
                    for (std::size_t j = 0; j < n; ++j) ts[j] = t0 + (double)(s0 + j) / kRate;
                    for (int o = 0; o < kStreams; ++o) {
                        for (std::size_t j = 0; j < n; ++j)
                            for (int c = 0; c < kCh; ++c)
                                buf[j * kCh + c] = 20.0f * std::sin(0.0628f * (float)((s0 + j) % 100000) * (float)(1 + c % 4)) + noise(rng);
                        outs[(std::size_t)o]->push_chunk_multiplexed(buf.data(), ts.data(), n * kCh, true);   // elements, not samples
                    }
                    sent = due;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        struct Join { std::atomic<bool>& stop; std::thread& t; ~Join() { stop = true; if (t.joinable()) t.join(); } } join{stop, gen};

        std::vector<std::string> wins;
        for (int i = 0; i < kStreams; ++i) {
            char sid[32], ref[40];
            std::snprintf(sid, sizeof sid, "perf-combined-%c", 'a' + i);
            std::snprintf(ref, sizeof ref, "//PerfCombined%c", 'A' + i);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (ctx->GetWindowByRef(ref) == nullptr && std::chrono::steady_clock::now() < until) {
                lslViewerRequestConnect(sid);
                for (int k = 0; k < 10; ++k) ctx->Yield();
            }
            IM_CHECK_RETV(ctx->GetWindowByRef(ref) != nullptr, );
            wins.push_back(ref);
        }
        // Wall clock: SleepNoSkip advances simulated time, and the data comes in real time.
        auto allAnchored = [&] {
            for (const std::string& w : wins) if (!streamProbe(w.c_str() + 2).anchored) return false;
            return true;
        };
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!allAnchored() && std::chrono::steady_clock::now() < until) ctx->Yield();
        IM_CHECK_RETV(allAnchored(), );
        const ImVec2 wp = ImGui::GetMainViewport()->WorkPos, ws = ImGui::GetMainViewport()->WorkSize;
        const ImVec2 a0(wp.x + 300.0f, wp.y + 30.0f), area(ws.x - 310.0f, ws.y - 40.0f);
        auto place = [&](ImGuiWindow* w, ImVec2 pos, ImVec2 size) {
            ctx->UndockWindow((std::string("//") + w->Name).c_str());   // absolute: the ref may be a child
            ImGui::SetWindowPos(w->Name, pos);   // directly: a title-bar drag would dock it again
            ImGui::SetWindowSize(w->Name, size);
            ctx->Yield(2);
        };
        const char* hist = std::getenv("LSL_PERF_HISTORY");
        auto hideCfg = [&](ImGuiWindow* w) {
            ctx->SetRef(w);
            ImGuiTestItemInfo cfg = ctx->WindowInfo("cfg");
            if (cfg.Window == nullptr) return;
            ctx->SetRef(cfg.Window);
            if (hist) {
                ctx->ItemOpen("Display");
                ctx->ItemInputValue("History (s)", (float)std::atof(hist));
                ctx->ItemClose("Display");
            }
            ctx->ItemClick("< hide");
        };
        if (combinedMode) {
            ctx->MenuClick("//##MainMenuBar/View/New combined plot");
            ctx->Yield(3);
            ImGuiWindow* w = newestCombinedWindow();
            IM_CHECK_RETV(w != nullptr, );
            ctx->SetRef(w);
            ImGuiTestItemInfo cfg = ctx->WindowInfo("cfg");
            IM_CHECK_RETV(cfg.Window != nullptr, );
            for (int i = 0; i < kStreams; ++i) {
                ctx->SetRef(cfg.Window);
                ctx->ItemClick("Add...");
                ctx->SetRef("//$FOCUSED");
                ctx->ComboClick((std::string("##stream/") + (wins[(std::size_t)i].c_str() + 2)).c_str());
                ctx->ItemClick("Add all enabled");
                ctx->Yield(2);
            }
            IM_CHECK_EQ((int)combinedProbe().ticks.size(), kStreams * kCh);
            ctx->SetRef(cfg.Window);
            ctx->ItemUncheck("**/Paginate");   // all 64 lanes, as the four stream plots show
            ctx->WindowFocus(w->ID);   // docked with the stream plots, in front of them
            hideCfg(w);
        } else {
            for (int i = 0; i < kStreams; ++i) {
                ImGuiWindow* w = ctx->GetWindowByRef(wins[(std::size_t)i].c_str());
                place(w, ImVec2(a0.x + (i % 2) * area.x * 0.5f, a0.y + (i / 2) * area.y * 0.5f),
                      ImVec2(area.x * 0.5f, area.y * 0.5f));
                hideCfg(w);
            }
        }
        ctx->MouseMoveToPos(ImVec2(5, 5));
        ctx->Yield(10);
        if (std::getenv("LSL_PERF_CAPTURE")) {
            ctx->CaptureReset();
            ctx->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor);
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
        while (std::chrono::steady_clock::now() < end) ctx->Yield();
        ctx->LogInfo("published %llu samples per stream", (unsigned long long)sent.load());
    };

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
            ctx->ItemUncheck("Paginate");               // every channel at once, not a page
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
