#pragma once
// Model of a combined plot: channels from several streams in one plot. Kept free of ImGui and
// liblsl so the tests can check binding, grouping and the workspace record without streams.
//
// A combined plot never owns a stream. Each trace names its stream by key (streamKeyOf: stable
// across reconnects and workspace loads) and is resolved to a connected source every frame, so
// no source pointer outlives the frame it was found in: a disconnected source is moved to the
// reap list and freed later.

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct TraceRef {
    std::string streamKey;         // streamKeyOf() of the stream
    int         channel  = 0;      // index in that stream; follows the label on a rebind
    std::string label;             // channel label when added; checked again on every bind
    bool        filtered = true;   // the stream's display-filter output, or the raw samples
    bool        autoFit  = false;  // stacked: scale the lane to its visible min / max
    // Not saved:
    std::string streamName;        // for the lane label while the stream is not connected
    float       fitLo = 0.0f, fitHi = 0.0f;   // auto-fit range in use (hysteresis)
    bool        fitValid = false;
};

// Name of the stream in a key, for a trace loaded from a workspace whose stream has not
// connected yet. A key is "source_id|name", or the uid when there is no source_id.
inline std::string streamNameOfKey(const std::string& key) {
    const std::size_t bar = key.find('|');
    return bar == std::string::npos ? key : key.substr(bar + 1);
}

// The channel a trace binds to in a stream with these labels: its index while the label there
// still matches, else the first channel with its label, else -1 (the channel is missing). A
// stream that reconnects with another channel layout so keeps showing the same signal.
inline int bindTraceChannel(const TraceRef& t, const std::vector<std::string>& labels) {
    if (t.channel >= 0 && t.channel < (int)labels.size() && labels[(std::size_t)t.channel] == t.label)
        return t.channel;
    for (std::size_t c = 0; c < labels.size(); ++c)
        if (labels[c] == t.label) return (int)c;
    return -1;
}

// A raw and a filtered trace of one channel are a valid comparison; the same one twice is not.
inline bool hasTrace(const std::vector<TraceRef>& traces, const std::string& key, int channel, bool filtered) {
    for (const TraceRef& t : traces)
        if (t.channel == channel && t.filtered == filtered && t.streamKey == key) return true;
    return false;
}

// Traces that are drawn together: one source, read the same way (raw or filtered) and on the
// same Y axis. `order[first .. first + count)` are the group's trace indices in lane order.
struct TraceGroup { int src = -1, kind = 0, first = 0, count = 0; };

// Groups the traces with src[i] >= 0 by (src[i], kind[i]), in order of first appearance, each
// group's traces in lane order. One read and one x axis per group then serve all its lanes.
// Reuses `groups` and `order`: no allocation once they have grown.
inline void groupTraces(std::span<const int> src, std::span<const int> kind,
                        std::vector<TraceGroup>& groups, std::vector<int>& order) {
    groups.clear();
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] < 0) continue;
        std::size_t g = 0;
        while (g < groups.size() && !(groups[g].src == src[i] && groups[g].kind == kind[i])) ++g;
        if (g == groups.size()) groups.push_back({src[i], kind[i], 0, 0});
        ++groups[g].count;
    }
    int at = 0;
    for (TraceGroup& g : groups) { g.first = at; at += g.count; g.count = 0; }
    order.resize((std::size_t)at);
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] < 0) continue;
        for (TraceGroup& g : groups)
            if (g.src == src[i] && g.kind == kind[i]) { order[(std::size_t)(g.first + g.count++)] = (int)i; break; }
    }
}

// ---- Workspace record ---------------------------------------------------------------------
// Fields are tab-separated and list entries space-separated, so every free-text field (title,
// stream key, label, unit) is percent-encoded: stream keys and labels can hold spaces, tabs
// and '|', which the trace entry uses between its own fields.
inline std::string pctEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c <= 0x20 || c == '%' || c == '|' || c == ',' || c == '=' || c == 0x7F) {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}
inline std::string pctDecode(std::string_view s) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && nib(s[i + 1]) >= 0 && nib(s[i + 2]) >= 0) {
            out += (char)(nib(s[i + 1]) * 16 + nib(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// One combined plot in a workspace. `view` is the serializeOpts() payload, kept opaque here.
struct CombinedRecord {
    int                                        id = 0;
    std::string                                title;
    std::string                                view;
    std::vector<std::pair<std::string, float>> unitScale;
    std::vector<TraceRef>                      traces;
};

// "C <id> <title> <view> <unit=scale ...> <key|channel|label|f-or-r|autofit ...>", tab-separated.
inline std::string toLine(const CombinedRecord& r) {
    std::string s = "C\t" + std::to_string(r.id) + "\t" + pctEncode(r.title) + "\t" + r.view + "\t";
    char b[48];
    for (std::size_t i = 0; i < r.unitScale.size(); ++i) {
        if (i) s += ' ';
        std::snprintf(b, sizeof b, "=%.6g", (double)r.unitScale[i].second);
        s += pctEncode(r.unitScale[i].first) + b;
    }
    s += '\t';
    for (std::size_t i = 0; i < r.traces.size(); ++i) {
        const TraceRef& t = r.traces[i];
        if (i) s += ' ';
        s += pctEncode(t.streamKey) + "|" + std::to_string(t.channel) + "|" + pctEncode(t.label) + "|" +
             (t.filtered ? "f" : "r") + "|" + (t.autoFit ? "1" : "0");
    }
    return s;
}

// Parses a line written by toLine (false if it is not one). Unknown trailing fields are ignored.
inline bool fromLine(const std::string& line, CombinedRecord& r) {
    std::vector<std::string_view> f;
    for (std::size_t a = 0;;) {
        const std::size_t b = line.find('\t', a);
        f.emplace_back(std::string_view(line).substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (f.size() < 6 || f[0] != "C") return false;
    r = CombinedRecord{};
    r.id    = std::atoi(std::string(f[1]).c_str());
    r.title = pctDecode(f[2]);
    r.view  = std::string(f[3]);
    auto each = [](std::string_view list, auto&& fn) {
        for (std::size_t a = 0; a < list.size();) {
            std::size_t b = list.find(' ', a);
            if (b == std::string::npos) b = list.size();
            if (b > a) fn(list.substr(a, b - a));
            a = b + 1;
        }
    };
    each(f[4], [&](std::string_view e) {
        const std::size_t eq = e.rfind('=');
        if (eq == std::string::npos) return;
        r.unitScale.emplace_back(pctDecode(e.substr(0, eq)), (float)std::atof(std::string(e.substr(eq + 1)).c_str()));
    });
    each(f[5], [&](std::string_view e) {
        std::string_view part[5];
        int n = 0;
        for (std::size_t a = 0; n < 5;) {
            const std::size_t b = e.find('|', a);
            part[n++] = e.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (n < 4) return;
        TraceRef t;
        t.streamKey  = pctDecode(part[0]);
        t.channel    = std::atoi(std::string(part[1]).c_str());
        t.label      = pctDecode(part[2]);
        t.filtered   = part[3] != "r";
        t.autoFit    = n >= 5 && part[4] == "1";
        t.streamName = streamNameOfKey(t.streamKey);
        r.traces.push_back(std::move(t));
    });
    return true;
}
