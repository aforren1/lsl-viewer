#pragma once
// Thin profiling layer. Two backends, picked at build time:
//
//   -DLSL_TRACY=ON   LSL_ZONE forwards to the Tracy client (interactive timeline).
//   (default)        LSL_ZONE feeds a lightweight built-in TEXT profiler that
//                    accumulates per-zone count/total/avg/max and prints a sorted
//                    table to stdout — readable without a GUI. Opt-in at runtime
//                    (near-zero cost when off): LSL_PROFILE_ENABLE(true) or env
//                    LSL_PROFILE=1; dump with LSL_PROFILE_DUMP(seconds).
//
//   LSL_ZONE("name")          instrument the enclosing scope
//   LSL_FRAME_MARK()          mark end-of-frame
//   LSL_PLOT("n", val)        publish a numeric time series (Tracy only)
//   LSL_PROFILE_ENABLE(bool)  turn the text profiler on/off
//   LSL_PROFILE_DUMP(seconds) print the table for the elapsed window, then reset

#if defined(LSL_TRACY)
  #include <tracy/Tracy.hpp>
  #define LSL_ZONE(name)         ZoneScopedN(name)
  #define LSL_FRAME_MARK()       FrameMark
  #define LSL_PLOT(name, val)    TracyPlot(name, (double)(val))
  #define LSL_PROFILE_ENABLE(b)  ((void)(b))
  #define LSL_PROFILE_DUMP(s)    ((void)(s))
#else
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstdio>
  #include <mutex>
  #include <string>
  #include <unordered_map>
  #include <vector>

namespace lslprof {

struct Stat { std::uint64_t count = 0, total_ns = 0, max_ns = 0; };

inline std::atomic<bool>                       g_on{false};
inline std::mutex                              g_mtx;
inline std::unordered_map<std::string, Stat>   g_stats;

inline void enable(bool e) { g_on.store(e, std::memory_order_relaxed); }

inline void record(const char* name, std::uint64_t ns) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Stat& s = g_stats[name];
    ++s.count; s.total_ns += ns; if (ns > s.max_ns) s.max_ns = ns;
}

// Print zones sorted by total time (descending), then clear for the next window.
inline void dump(double elapsed_s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_stats.empty()) return;
    std::vector<std::pair<std::string, Stat>> v(g_stats.begin(), g_stats.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second.total_ns > b.second.total_ns; });
    std::printf("[profile] --- %.1fs window ---  %-16s %8s %10s %9s %9s %7s\n",
                elapsed_s, "zone", "count", "total_ms", "avg_us", "max_us", "%frame");
    const double frame_total_ms = elapsed_s > 0 ? elapsed_s * 1000.0 : 0.0;
    for (const auto& [name, s] : v) {
        const double total_ms = s.total_ns / 1e6;
        std::printf("[profile]                          %-16s %8llu %10.2f %9.2f %9.2f %6.1f%%\n",
                    name.c_str(), (unsigned long long)s.count, total_ms,
                    s.count ? s.total_ns / 1e3 / s.count : 0.0, s.max_ns / 1e3,
                    frame_total_ms > 0 ? 100.0 * total_ms / frame_total_ms : 0.0);
    }
    std::fflush(stdout);
    g_stats.clear();
}

struct Scope {
    const char* name;
    bool        on;
    std::chrono::steady_clock::time_point t0;
    explicit Scope(const char* n)
        : name(n), on(g_on.load(std::memory_order_relaxed)) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!on) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        record(name, (std::uint64_t)ns);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace lslprof

  #define LSL_PROF_CAT2(a, b) a##b
  #define LSL_PROF_CAT(a, b)  LSL_PROF_CAT2(a, b)
  #define LSL_ZONE(name)      lslprof::Scope LSL_PROF_CAT(_lslprof_, __LINE__)(name)
  #define LSL_FRAME_MARK()    ((void)0)
  #define LSL_PLOT(name, val) ((void)0)
  #define LSL_PROFILE_ENABLE(b) lslprof::enable(b)
  #define LSL_PROFILE_DUMP(s)   lslprof::dump(s)
#endif
