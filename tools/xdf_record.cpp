// Minimal headless XDF recorder CLI — uses the same Recorder/xdf_writer as the
// viewer, so it's a no-GUI parallel to LabRecorder's clirecorder (handy for tests
// and scripted captures).
//
//   xdf_record <out.xdf> [--seconds N] [--streams name1,name2] [--resolve-wait S]
//
// Records every resolved LSL stream (or just the named ones) for N seconds (or until
// SIGINT/SIGTERM if N is omitted).

#include "recorder.hpp"   // src/ is on the include path (see CMakeLists)

#include <lsl_cpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(tok);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <out.xdf> [--seconds N] [--streams a,b] [--resolve-wait S]\n", argv[0]);
        return 2;
    }
    const std::string out = argv[1];
    double seconds = 0.0, resolve_wait = 2.0;
    std::vector<std::string> want;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--seconds"      && i + 1 < argc) seconds      = std::atof(argv[++i]);
        else if (a == "--streams"      && i + 1 < argc) want         = split_csv(argv[++i]);
        else if (a == "--resolve-wait" && i + 1 < argc) resolve_wait = std::atof(argv[++i]);
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
    }
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    std::vector<lsl::stream_info> infos;
    for (auto& info : lsl::resolve_streams(resolve_wait)) {
        if (info.source_id().rfind("lsl-viewer-rc", 0) == 0) continue;   // skip RC beacons
        if (want.empty() || std::find(want.begin(), want.end(), info.name()) != want.end())
            infos.push_back(info);
    }
    if (infos.empty()) { spdlog::error("no matching streams resolved"); return 1; }

    Recorder rec;
    if (!rec.start(out, infos)) {
        spdlog::error("failed to start recording: {}", rec.error());
        return 1;
    }
    spdlog::info("recording {} stream(s) -> {}", infos.size(), out);
    const double t0 = lsl::local_clock();
    while (!g_stop && (seconds <= 0.0 || lsl::local_clock() - t0 < seconds))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t bytes = rec.bytes();
    const double dur = rec.seconds();
    rec.stop();
    spdlog::info("done: {:.1f}s, {} bytes", dur, bytes);
    return 0;
}
