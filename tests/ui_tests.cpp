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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>   // strtoul
#include <cstring>   // strstr
#include <filesystem>
#include <fstream>
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

    // Performance overlay (off by default; shown via View menu) exposes VSync.
    t = IM_REGISTER_TEST(e, "ui", "performance_window");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->MenuCheck("//##MainMenuBar/Debug/Performance");
        ctx->Yield(2);
        ctx->SetRef("//Streams");                  // Performance is a section in the rail now
        IM_CHECK(ctx->ItemExists("VSync"));
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
