// Replays the streams of an XDF recording as live LSL outlets. A compiled port of
// tools/xdf_replay.py with the same flags and output, so a session can be checked
// at the rig right after it was recorded, without Python. The engine is
// src/xdf_player.hpp (shared with the viewer); this file is only the command line.
//
//   xdf_replay rec.xdf --list            show the streams, no replay
//   xdf_replay rec.xdf                   replay all streams once
//   xdf_replay rec.xdf --speed 10        check a long session quickly
//
// Run with --help for every option.

#include "xdf_player.hpp"   // src/ is on the include path (see CMakeLists)

#include <lsl_c.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
XdfPlayer*        g_player = nullptr;

void onSignal(int) {
    g_stop.store(true);
    if (g_player) g_player->cancel();   // aborts a scan or pre-pass in progress
}

const char* kUsage =
    "usage: xdf_replay [-h] [--list] [--streams STREAMS] [--prefix PREFIX] [--suffix SUFFIX]\n"
    "                  [--start START] [--duration DURATION] [--speed SPEED] [--loop]\n"
    "                  [--delay DELAY] [--chunk-ms CHUNK_MS] [--lookahead LOOKAHEAD]\n"
    "                  [--sync | --no-sync] [--dejitter | --no-dejitter]\n"
    "                  [--timestamps {recording,scaled}] [--source-id-prefix PREFIX]\n"
    "                  file\n";

const char* kHelp = R"(
Replay the streams of an XDF recording as live LSL outlets.

Each stream in the file becomes an outlet with the recorded name, type, channel
count, nominal rate, channel format, and the full <desc> subtree (channel labels,
units, locations, ...). The source_id gets the prefix "replay-" (see
--source-id-prefix), so a recorder that matches on it does not take the replay
for the live device. All outlets share one playback clock, so the relative
timing between streams is the same as in the recording. Time zero is the first
sample in the file.

The file is streamed: a reader thread decodes the chunks of the selected streams
in order of their start time and stays about --lookahead seconds ahead of
playback. Memory use depends on the read-ahead, not on the size of the file, and
playback starts within a second also for multi-GB files. --start seeks through an
index of the file. A chunk that the reader delivers after its time is pushed at
once with its recorded timestamps; the number of such late chunks is printed when
playback ends.

Timestamps are the recorded ones, shifted onto local_clock(). Regular streams go
out in chunks of about --chunk-ms; irregular streams (markers, events) go out at
the time of each sample. With --loop, each pass is shifted by the window length,
so timestamps keep increasing.

At --speed other than 1, --timestamps selects what the timestamps follow:
  recording  (default) the recorded spacing. A viewer that plots by sample index
             and places markers by timestamp (lsl_viewer does) then shows the
             recording's time axis, dropout widths, and marker positions. The
             timestamps run ahead of local_clock() above 1x.
  scaled     the recorded spacing divided by --speed, so the timestamps track
             local_clock(), as tools/xdf_replay.py does.
The nominal rate in the metadata is not changed; the effective rate is nominal
rate x speed.

Clock sync (default on) and --dejitter reproduce pyxdf.load_xdf with its default
settings, so the replayed timestamps are the ones pyxdf would return. Sync is on
by default because streams recorded from other hosts are on unrelated clocks
without it. Dejitter is off by default, so the replay sends the timestamps the
recorder received, jitter included, as a live inlet would see them. --dejitter
first reads all timestamps of the selected streams, which takes a few seconds on
large files.

A damaged file still plays. If the chunk structure is broken, the data up to the
next Boundary chunk (about 10 s in LabRecorder files) is lost. A single chunk
whose content is bad is skipped. A cut-off file plays up to the cut. Gzip-
compressed files (.xdfz) are not supported; decompress them first.

A stream with no samples (in the file or in the window) still gets an outlet, so
the stream list matches the recording.

The outlets are visible on the whole network, like those of any LSL source. A
recorder that selects streams by name and host (LabRecorder's RequiredStreams)
also selects a replay that runs on the host of the device; use --suffix to keep
the names apart.

Stream selection (--streams) is a comma list. Each item is a stream id (as
printed by --list), "type:<pattern>", or a name pattern. Patterns use shell-style
wildcards (* and ?). The reader only reads the chunks of the selected streams.

Examples:
    xdf_replay rec.xdf --list            # show streams, no replay
    xdf_replay rec.xdf                   # replay all streams once
    xdf_replay rec.xdf --loop --suffix _replay
    xdf_replay rec.xdf --streams 2,type:Markers --start 60 --duration 30
    xdf_replay rec.xdf --streams "Mock*" --speed 10

Stop with Ctrl+C.

positional arguments:
  file                  XDF file (.xdf)

options:
  -h, --help            show this help message and exit
  --list                print the streams in the file and exit (reads headers only)
  --streams STREAMS     comma list of stream ids, name patterns, or type:<pattern> (default: all)
  --prefix PREFIX       prepended to every outlet name
  --suffix SUFFIX       appended to every outlet name (avoid clashes with live devices)
  --start START         window start in seconds after the first sample in the file (default: 0)
  --duration DURATION   window length in seconds (default: to the end)
  --speed SPEED         playback speed factor (default: 1)
  --loop                repeat the window; timestamps keep increasing across passes
  --delay DELAY         seconds between outlet creation and the first sample, so inlets can
                        connect (default: 0.5)
  --chunk-ms CHUNK_MS   push period for regular streams in ms (default: 20)
  --lookahead LOOKAHEAD seconds the reader decodes ahead of playback (default: 2)
  --sync, --no-sync     apply pyxdf's clock synchronization (default: on)
  --dejitter, --no-dejitter
                        apply pyxdf's timestamp dejittering (default: off, so the replay keeps
                        the recorded jitter)
  --timestamps {recording,scaled}
                        what timestamps follow at --speed other than 1 (default: recording)
  --source-id-prefix PREFIX
                        prepended to every source_id; "" keeps the recorded ones
                        (default: replay-)
)";

[[noreturn]] void usageError(const std::string& msg) {
    std::fprintf(stderr, "%sxdf_replay: error: %s\n", kUsage, msg.c_str());
    std::exit(2);
}

// Python's format widths count characters, not bytes; names can be UTF-8.
std::size_t utf8Len(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
std::string utf8Head(std::string_view s, std::size_t chars) {
    std::size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { if (n == chars) break; ++n; }
    }
    return std::string(s.substr(0, i));
}
std::string padL(std::string_view s, std::size_t w) {   // left-aligned, like {:<w}
    std::string o(s);
    for (std::size_t n = utf8Len(s); n < w; ++n) o += ' ';
    return o;
}
std::string padR(std::string_view s, std::size_t w) {   // right-aligned, like {:>w}
    std::string o;
    for (std::size_t n = utf8Len(s); n < w; ++n) o += ' ';
    return o + std::string(s);
}
std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), f, v);
    return b;
}

void printList(const std::vector<xdf::StreamHeader>& hs, const std::vector<xdf::Timebase>& tbs) {
    std::vector<std::optional<double>> firsts;
    double t0 = 0.0;
    bool any = false;
    for (std::size_t k = 0; k < hs.size(); ++k) {
        firsts.push_back(xdf::firstTime(hs[k], tbs[k]));
        if (firsts.back()) { t0 = any ? std::min(t0, *firsts.back()) : *firsts.back(); any = true; }
    }
    std::printf("%s  %s %s %s %s %s %s %s %s\n", padR("id", 3).c_str(), padL("name", 28).c_str(),
                padL("type", 14).c_str(), padR("ch", 4).c_str(), padR("rate", 9).c_str(),
                padL("format", 8).c_str(), padR("samples", 10).c_str(), padR("start s", 9).c_str(),
                padR("dur s", 9).c_str());
    for (std::size_t k = 0; k < hs.size(); ++k) {
        const auto& s = hs[k];
        const std::string rate = s.srate == 0 ? std::string("irreg") : fmt("%g", s.srate);
        std::string begin = "-", dur = "-";
        if (firsts[k]) {
            begin = fmt("%.2f", *firsts[k] - t0);
            const double lastRaw = s.last && *s.last != 0.0 ? *s.last : *s.first;
            dur = fmt("%.2f", tbs[k].clock(lastRaw, s.count - 1) - *firsts[k]);
        }
        std::printf("%s  %s %s %s %s %s %s %s %s\n", padR(std::to_string(s.id), 3).c_str(),
                    padL(utf8Head(s.name, 28), 28).c_str(), padL(utf8Head(s.type, 14), 14).c_str(),
                    padR(std::to_string(s.channels), 4).c_str(), padR(rate, 9).c_str(),
                    padL(s.formatName, 8).c_str(), padR(std::to_string(s.count), 10).c_str(),
                    padR(begin, 9).c_str(), padR(dur, 9).c_str());
    }
    std::printf("'start' is relative to the earliest sample in the file, the time zero of --start.\n");
}

double toDouble(const std::string& flag, const std::string& v) {
    std::istringstream in(v);
    in.imbue(std::locale::classic());
    double d = 0;
    in >> d;
    if (in.fail() || in.peek() != std::char_traits<char>::eof())
        usageError("argument " + flag + ": invalid float value: '" + v + "'");
    return d;
}

// Hidden test mode: --control "t:op[ arg];..." runs control calls at t seconds after
// play(), to exercise the API that a GUI would use. Ops: pause, resume, seek S,
// speed X, loop on|off, stop.
struct ControlStep { double t; std::string op; double arg; };
std::vector<ControlStep> parseControl(const std::string& spec) {
    std::vector<ControlStep> out;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.empty()) continue;
        const auto colon = item.find(':');
        if (colon == std::string::npos) usageError("--control: expected t:op in '" + item + "'");
        ControlStep st{toDouble("--control", item.substr(0, colon)), {}, 0.0};
        std::istringstream rest(item.substr(colon + 1));
        rest.imbue(std::locale::classic());
        std::string arg;
        rest >> st.op >> arg;
        if (st.op == "loop") st.arg = arg == "on" ? 1.0 : 0.0;
        else if (!arg.empty()) st.arg = toDouble("--control", arg);
        out.push_back(st);
    }
    std::sort(out.begin(), out.end(), [](const ControlStep& a, const ControlStep& b) { return a.t < b.t; });
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string file;
    bool list = false;
    XdfPlayer::Options opt;
    std::string control;
    auto needValue = [&](int& i, const std::string& flag, std::string& inlineVal, bool hasInline) -> std::string {
        if (hasInline) return inlineVal;
        if (i + 1 >= argc) usageError("argument " + flag + ": expected one argument");
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], val;
        bool hasVal = false;
        if (a.rfind("--", 0) == 0) {
            if (const auto eq = a.find('='); eq != std::string::npos) { val = a.substr(eq + 1); a = a.substr(0, eq); hasVal = true; }
        }
        if (a == "-h" || a == "--help") { std::printf("%s%s", kUsage, kHelp); return 0; }
        else if (a == "--list")         list = true;
        else if (a == "--streams")      opt.streams = needValue(i, a, val, hasVal);
        else if (a == "--prefix")       opt.prefix = needValue(i, a, val, hasVal);
        else if (a == "--suffix")       opt.suffix = needValue(i, a, val, hasVal);
        else if (a == "--start")        opt.start = toDouble(a, needValue(i, a, val, hasVal));
        else if (a == "--duration")     opt.duration = toDouble(a, needValue(i, a, val, hasVal));
        else if (a == "--speed")        opt.speed = toDouble(a, needValue(i, a, val, hasVal));
        else if (a == "--loop")         opt.loop = true;
        else if (a == "--delay")        opt.delay = toDouble(a, needValue(i, a, val, hasVal));
        else if (a == "--chunk-ms")     opt.chunkMs = toDouble(a, needValue(i, a, val, hasVal));
        else if (a == "--lookahead")    opt.lookahead = toDouble(a, needValue(i, a, val, hasVal));
        else if (a == "--sync")         opt.sync = true;
        else if (a == "--no-sync")      opt.sync = false;
        else if (a == "--dejitter")     opt.dejitter = true;
        else if (a == "--no-dejitter")  opt.dejitter = false;
        else if (a == "--source-id-prefix") opt.sourceIdPrefix = needValue(i, a, val, hasVal);
        else if (a == "--timestamps") {
            const std::string v = needValue(i, a, val, hasVal);
            if (v == "recording") opt.stamps = XdfPlayer::Stamps::recording;
            else if (v == "scaled") opt.stamps = XdfPlayer::Stamps::scaled;
            else usageError("argument --timestamps: invalid choice: '" + v + "' (choose from 'recording', 'scaled')");
        }
        else if (a == "--control")      control = needValue(i, a, val, hasVal);
        else if (a.size() > 1 && a[0] == '-' && !(a[1] >= '0' && a[1] <= '9')) usageError("unrecognized arguments: " + a);
        else if (file.empty())          file = a;
        else usageError("unrecognized arguments: " + a);
    }
    if (file.empty()) usageError("the following arguments are required: file");
    if (!(opt.speed > 0)) usageError("--speed must be positive");
    if (opt.duration && !(*opt.duration > 0)) usageError("--duration must be positive");
    if (!(opt.lookahead > 0)) usageError("--lookahead must be positive");
    const std::vector<ControlStep> steps = parseControl(control);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onSignal);    // Ctrl+Break, and what a script can send on Windows
#endif
    const auto tStart = std::chrono::steady_clock::now();
    auto since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    };
    opt.onWarning = [](const std::string& m) { std::fprintf(stderr, "warning: %s\n", m.c_str()); std::fflush(stderr); };
    // LSL_PROFILE=1 prints the time spent in the reader and push zones at exit (profiler.hpp).
#ifdef _WIN32
    char* profEnv = nullptr;   // _dupenv_s: MSVC warns (C4996) on getenv
    std::size_t profLen = 0;
    const bool profile = _dupenv_s(&profEnv, &profLen, "LSL_PROFILE") == 0 && profEnv != nullptr;
    std::free(profEnv);
#else
    const bool profile = std::getenv("LSL_PROFILE") != nullptr;
#endif
    LSL_PROFILE_ENABLE(profile);

    if (list) {
        try {
            const auto hs = xdf::scan(file, opt.onWarning);
            std::vector<xdf::Timebase> tbs;
            for (const auto& h : hs) tbs.emplace_back(h, opt.sync, opt.onWarning);
            printList(hs, tbs);
            std::printf("(scanned in %.2f s)\n", since(tStart));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
        return 0;
    }

    XdfPlayer player;
    g_player = &player;
    if (!player.prepare(file, opt)) {
        g_player = nullptr;
        if (g_stop) { std::printf("\nstopped.\n"); return 0; }
        std::fprintf(stderr, "%s\n", player.error().c_str());
        return 1;
    }
    if (player.prepassStreams())
        std::printf("read all timestamps of %zu streams in %.2f s\n", player.prepassStreams(), player.prepassSeconds());
    std::printf("outlets:\n");
    for (std::size_t k : player.selected()) {
        const auto& h = player.headers()[k];
        const std::string rate = h.srate == 0 ? std::string("irregular") : fmt("%g", h.srate) + " Hz";
        std::printf("  %s %s %s ch  %s %s %s samples%s\n", padL(opt.prefix + h.name + opt.suffix, 32).c_str(),
                    padL(h.type, 12).c_str(), padR(std::to_string(h.channels), 3).c_str(), padL(rate, 10).c_str(),
                    padL(h.formatName, 8).c_str(), padR(std::to_string(h.count), 9).c_str(),
                    h.count ? "" : "  (no samples: outlet stays idle)");
    }
    player.play();
    const auto tPlay = std::chrono::steady_clock::now();
    std::string window = fmt("%g", opt.start) + " s";
    if (opt.duration) window += " for " + fmt("%g", *opt.duration) + " s";
    std::printf("ready in %.2f s; playing from %s%s at %sx. Press Ctrl+C to stop.\n", since(tStart),
                window.c_str(), opt.loop ? " in a loop" : "", fmt("%g", opt.speed).c_str());
    std::fflush(stdout);

    std::size_t nextStep = 0;
    auto profileDump = [&] { if (profile) LSL_PROFILE_DUMP(since(tPlay)); };
    auto interrupted = [&] {
        std::printf("\nstopping... %llu late chunks so far.\n", (unsigned long long)player.status().late);
        std::fflush(stdout);
        player.stop();
        g_player = nullptr;
        profileDump();
        return 0;
    };
    // Control steps can follow the end (a seek or resume from there), so the loop
    // runs until playback has finished and no step is left.
    for (;;) {
        const bool finished = player.waitFinished(0.05);
        if (finished && nextStep >= steps.size()) break;
        if (finished) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (g_stop) return interrupted();
        if (player.status().state == XdfPlayer::State::stopped) break;
        while (nextStep < steps.size() && since(tPlay) >= steps[nextStep].t) {
            const ControlStep& st = steps[nextStep++];
            if (st.op == "pause") player.pause();
            else if (st.op == "resume") player.resume();
            else if (st.op == "seek") player.seek(st.arg);
            else if (st.op == "speed") player.setSpeed(st.arg);
            else if (st.op == "loop") player.setLoop(st.arg != 0.0);
            else if (st.op == "stop") { player.stop(); break; }
            const auto s = player.status();
            std::printf("control: %.3f %s %g -> position %.3f of %.3f, pass %llu, local_clock %.6f\n", since(tPlay),
                        st.op.c_str(), st.arg, s.position, s.length, (unsigned long long)s.pass, lsl_local_clock());
            std::fflush(stdout);
        }
    }
    const auto s = player.status();
    if (s.state == XdfPlayer::State::stopped && !g_stop) {   // a --control stop
        std::printf("stopped. %llu late chunks.\n", (unsigned long long)s.late);
        g_player = nullptr;
        return 0;
    }
    if (s.empty) {
        std::printf("no samples in the selected window (see --list for stream times)\n");
    } else {
        // Give connected inlets time to drain the last chunks before the outlets close.
        for (int i = 0; i < 20; ++i) {
            if (g_stop) return interrupted();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("done. %llu late chunks.\n", (unsigned long long)s.late);
    }
    std::fflush(stdout);
    player.stop();
    g_player = nullptr;
    profileDump();
    return 0;
}
