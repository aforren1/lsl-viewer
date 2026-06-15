// LSL Stream Viewer — SDL3 + SDL_GPU + Dear ImGui + ImPlot
//
// Render-loop structure follows imgui/examples/example_sdl3_sdlgpu3/main.cpp.
// Key SDL_GPU rule: ImGui_ImplSDLGPU3_PrepareDrawData() MUST run before
// SDL_BeginGPURenderPass (copy ops are illegal inside a render pass).
//
// Display path (DESIGN "core"):
//   - normal view : MinMaxSummary::read() -> ImPlot::PlotShaded (envelope)
//   - zoomed in   : InterleavedRing::recent() -> ImPlot::PlotLine (raw)
// The switch is per channel-stream by samples-per-pixel vs the summary bin size.
//
// Env knobs (handy for profiling without touching the UI):
//   LSL_GPU_DEBUG=1   enable the Vulkan validation layer (slow; off by default)
//   LSL_AUTOCONNECT=1 connect every discovered stream automatically
//   LSL_NOVSYNC=1     start with VSync off (uncapped — measure real render cost)
//   LSL_BENCH=1       print FPS / frame-time stats to stdout once a second
//   LSL_PROFILE=1     print a per-zone CPU table (count/total/avg/max/%frame) every 3 s
//   SPDLOG_LEVEL=...  log verbosity (e.g. debug, info, warn)

#include "imgui.h"
#include "imgui_internal.h"   // BringWindowToDisplayFront (keep Streams on top)
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "implot.h"

#include <SDL3/SDL.h>

#include "hf_stream_source.hpp"
#include "mock_streams.hpp"
#include "recorder.hpp"
#include "remote_control.hpp"
#include <ctime>
#include <optional>
#include <set>
#include "fft.hpp"
#include "profiler.hpp"
#include "theme.hpp"       // applyTheme() + loadEmbeddedFont() (UI palette + Roboto)

#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>     // SPDLOG_LEVEL env var

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef LSL_TESTS
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_ui.h"
#include "imgui_test_engine/imgui_capture_tool.h"   // default ffmpeg video/gif params
extern void RegisterAppTests(ImGuiTestEngine* engine);

// State the screen-capture callback needs. We render the UI into an offscreen
// texture each frame (then blit it to the swapchain), because the swapchain
// texture is not readable after present. The callback reads back a region of
// that offscreen texture on demand.
struct CaptureCtx {
    SDL_GPUDevice*  gpu  = nullptr;
    SDL_GPUTexture* tex  = nullptr;   // current-frame offscreen color target
    int             w = 0, h = 0;
    bool            bgra = false;     // swapchain is B8G8R8A8 -> swizzle to RGBA
};

// Matches ImGuiScreenCaptureFunc: fill `pixels` (w*h RGBA8) from the framebuffer.
static bool captureFunc(ImGuiID, int x, int y, int w, int h,
                        unsigned int* pixels, void* user_data) {
    CaptureCtx* c = (CaptureCtx*)user_data;
    if (!c->tex || w <= 0 || h <= 0) return false;

    SDL_GPUTransferBufferCreateInfo tci = {};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tci.size  = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(c->gpu, &tci);
    if (!tb) return false;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(c->gpu);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = c->tex;
    src.x = (Uint32)x; src.y = (Uint32)y; src.w = (Uint32)w; src.h = (Uint32)h; src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = tb; dst.pixels_per_row = (Uint32)w; dst.rows_per_layer = (Uint32)h;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(c->gpu, true, &fence, 1);
    SDL_ReleaseGPUFence(c->gpu, fence);

    bool ok = false;
    if (Uint8* m = (Uint8*)SDL_MapGPUTransferBuffer(c->gpu, tb, false)) {
        Uint8* out = (Uint8*)pixels;
        const int n = w * h;
        for (int i = 0; i < n; ++i) {
            out[i * 4 + 0] = c->bgra ? m[i * 4 + 2] : m[i * 4 + 0];
            out[i * 4 + 1] = m[i * 4 + 1];
            out[i * 4 + 2] = c->bgra ? m[i * 4 + 0] : m[i * 4 + 2];
            out[i * 4 + 3] = 255;
        }
        SDL_UnmapGPUTransferBuffer(c->gpu, tb);
        ok = true;
    }
    SDL_ReleaseGPUTransferBuffer(c->gpu, tb);
    return ok;
}
#endif

// Rolling frame-time history for the FPS overlay.
struct FrameStats {
    static constexpr int N = 180;
    float ms[N] = {};
    int   idx = 0;
    void  push(float v) { ms[idx] = v; idx = (idx + 1) % N; }
    float avg() const { float s = 0; for (float v : ms) s += v; return s / N; }
    float maxv() const { float m = 0; for (float v : ms) m = std::max(m, v); return m; }
};

static double pcSeconds(Uint64 a, Uint64 b) {
    return (double)(b - a) / (double)SDL_GetPerformanceFrequency();
}

// Is this connected source the same stream as `info`? Prefer source_id (globally
// unique and stable across reconnects) so a reconnected stream — which gets a new
// uid — isn't treated as a brand-new stream and double-connected. Templated so it
// works for both HfStreamSource and MarkerSource (both expose sourceId()/uid()).
template <class Src>
static bool sameStream(const Src& s, lsl::stream_info& info) {
    const std::string sid = info.source_id();
    return !sid.empty() ? (s.sourceId() == sid) : (s.uid() == info.uid());
}

// Marker/event streams are string-typed; they get a MarkerSource (overlay), not a
// waveform plot.
static bool isMarkerStream(lsl::stream_info& info) {
    return info.channel_format() == lsl::cf_string;
}

// Stable key for "the user dismissed this stream" bookkeeping: source_id (globally
// unique) when set, else uid. Works for a stream_info or any connected source.
static std::string streamKey(lsl::stream_info& info) {
    return !info.source_id().empty() ? info.source_id() : info.uid();
}
template <class Src>
static std::string streamKeyOf(const Src& s) {
    return !s.sourceId().empty() ? s.sourceId() : s.uid();
}

// BIDS-ish filename templating: substitute {subject}/{session}/{task}/{run}/{acq}/{modality}
// and the built-ins {datetime}/{date}/{time} in `tmpl`. Unknown {keys} are left literal so a
// forgotten field is visible in the resulting name. The template may include subdirectories
// (created at record time).
struct RecVars { std::string subject, session, task, run, acq, modality; };
static std::string resolveTemplate(const std::string& tmpl, const RecVars& v) {
    std::time_t tt = std::time(nullptr); std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char dt[32], dd[16], hh[16];
    std::strftime(dt, sizeof(dt), "%Y%m%d_%H%M%S", &tm);
    std::strftime(dd, sizeof(dd), "%Y%m%d", &tm);
    std::strftime(hh, sizeof(hh), "%H%M%S", &tm);
    auto val = [&](const std::string& k) -> std::string {
        if (k == "subject")  return v.subject;
        if (k == "session")  return v.session;
        if (k == "task")     return v.task;
        if (k == "run")      return v.run;
        if (k == "acq")      return v.acq;
        if (k == "modality") return v.modality;
        if (k == "datetime") return dt;
        if (k == "date")     return dd;
        if (k == "time")     return hh;
        return "{" + k + "}";
    };
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const std::size_t e = tmpl.find('}', i);
            if (e != std::string::npos) { out += val(tmpl.substr(i + 1, e - i - 1)); i = e + 1; continue; }
        }
        out += tmpl[i++];
    }
    return out;
}

// A marker event ready to draw: display-time position, text (points into a
// MarkerStreamView that outlives the draw), source color, and a STABLE label row
// (derived from the event's fixed sequence number, so a label keeps its row as the
// plot pans instead of being reshuffled by greedy packing).
constexpr int kMarkerRows = 4;
struct MarkerEvent { double t; const std::string* text; ImU32 col; int row; };

// Per-frame snapshot of one connected marker stream: its identity (for per-plot
// toggles), color, and the events within the visible time window.
struct MarkerStreamView {
    std::string                     id;       // sourceId (or uid) — stable toggle key
    std::string                     name;
    ImU32                           col;
    const std::vector<MarkerSource::Event>* events = nullptr;  // -> source's cached snapshot
};

// Reusable per-frame scratch so the plot path allocates nothing steady-state.
struct PlotScratch {
    // x is the time axis: it MUST be double. LSL timestamps are absolute local_clock seconds
    // (tens of thousands of seconds and up), where float32's ~0.01 s resolution is coarser than
    // a sample step — collapsing many samples onto one x and drawing the trace as a staircase.
    // mn/mx/y share x's type because ImPlot's PlotLine/PlotShaded take one type for both axes.
    std::vector<double>      x, mn, mx, y, mid;
    std::vector<double>      tvals;   // stacked-mode Y tick positions
    std::vector<const char*> tlabs;   // stacked-mode Y tick labels (-> tstr)
    std::vector<std::string> tstr;    // backing storage for tick labels
    std::vector<int>         visIdx;  // indices of visible channels
    std::vector<MarkerEvent> markers; // merged enabled marker events for this plot
    std::vector<float>       raster;  // channels x pixels p2p image (raster mode)
    std::vector<int>         pmap;    // raster: pixel-column -> summary-bin index
};

struct DisplayOpts {
    float history     = 10.0f;
    bool  stacked     = true;   // EEG-style montage (vs shared-axis overlay)
    bool  raster      = false;  // many-channel mode: one heatmap (color=p2p) vs N line series
    float gainUv      = 150.0f; // µV mapped to one lane height (stacked)
    float spacing     = 0.0f;   // vertical offset between channels (overlay)
    bool  highpass    = true;   // high-pass STAGE on/off (independent of notch/low-pass/reference)
    float hpHz        = 0.5f;   // running high-pass cutoff (Hz)
    bool  notch       = false;  // mains line-noise notch (display filter stage)
    float notchHz     = 60.0f;  // notch center (50 or 60 Hz)
    bool  lowpass     = false;  // low-pass (anti-EMG / smoothing) display filter stage
    float lpHz        = 40.0f;  // low-pass cutoff (Hz)
    int   refMode     = 0;      // re-reference: 0 none, 1 common-average, 2 single channel
    int   refChan     = 0;      // reference channel when refMode == 2
    float lineWidth   = 1.0f;   // trace line weight (px)
    std::vector<unsigned char> visible;  // per-channel show flags (sized lazily)
    ImGuiTextFilter            chanFilter;  // pattern filter for the channel list
    bool overlayMarkers = false;           // master marker-overlay toggle for this plot (off by default)
    std::unordered_map<std::string, bool> markerOn;  // per-stream (id -> show; absent = on)
    bool cfgShown = true;                  // show the left config strip (else plot is full-width)
    float lastPlotPx = 0.0f;               // last frame's plot width (px) — for edge pixel-snapping
    bool  overlayYFit = true;              // overlay mode: auto-fit Y once (on entry / scale change), then free
    bool  synced      = false;             // pushed our filter/reference state to the source yet?
};

// ---- Workspace (de)serialization: DisplayOpts <-> a compact "k=v ..." payload --
// Used by the named-workspace save/load (see main). Values never contain spaces, so the
// whole thing is one space-separated token list; channel visibility is an index list plus
// the channel count it was saved at (so it only re-applies to a matching stream).
static double wsField(const std::string& p, const char* key, double def) {
    const std::string hay = " " + p, k = std::string(" ") + key + "=";
    const size_t pos = hay.find(k);
    return (pos == std::string::npos) ? def : std::atof(hay.c_str() + pos + k.size());
}
static std::string serializeOpts(const DisplayOpts& o) {
    char b[320];
    std::snprintf(b, sizeof b,
        "hist=%.3f stk=%d ras=%d gain=%.3f spc=%.3f hp=%d hphz=%.4f no=%d nohz=%.1f "
        "lp=%d lphz=%.1f ref=%d refc=%d lw=%.2f ovm=%d cfg=%d nch=%zu",
        o.history, o.stacked ? 1 : 0, o.raster ? 1 : 0, o.gainUv, o.spacing, o.highpass ? 1 : 0,
        o.hpHz, o.notch ? 1 : 0, o.notchHz, o.lowpass ? 1 : 0, o.lpHz, o.refMode, o.refChan,
        o.lineWidth, o.overlayMarkers ? 1 : 0, o.cfgShown ? 1 : 0, o.visible.size());
    std::string s = b;
    s += " vis=";
    for (std::size_t i = 0; i < o.visible.size(); ++i) if (o.visible[i]) { s += std::to_string(i); s += ','; }
    return s;
}
static DisplayOpts parseOpts(const std::string& p) {
    DisplayOpts o;
    o.history   = (float)wsField(p, "hist", o.history);
    o.stacked   = wsField(p, "stk",  1) != 0;
    o.raster    = wsField(p, "ras",  0) != 0;
    o.gainUv    = (float)wsField(p, "gain", o.gainUv);
    o.spacing   = (float)wsField(p, "spc",  o.spacing);
    o.highpass  = wsField(p, "hp",   1) != 0;
    o.hpHz      = (float)wsField(p, "hphz", o.hpHz);
    o.notch     = wsField(p, "no",   0) != 0;
    o.notchHz   = (float)wsField(p, "nohz", o.notchHz);
    o.lowpass   = wsField(p, "lp",   0) != 0;
    o.lpHz      = (float)wsField(p, "lphz", o.lpHz);
    o.refMode   = (int)wsField(p, "ref",  0);
    o.refChan   = (int)wsField(p, "refc", 0);
    o.lineWidth = (float)wsField(p, "lw",   o.lineWidth);
    o.overlayMarkers = wsField(p, "ovm", 0) != 0;
    o.cfgShown  = wsField(p, "cfg", 1) != 0;
    const int nch = (int)wsField(p, "nch", 0);
    if (nch > 0 && nch < 100000) {
        o.visible.assign(nch, 0);
        const size_t vp = p.find("vis=");
        if (vp != std::string::npos)
            for (const char* c = p.c_str() + vp + 4; *c && *c != ' '; ) {
                int idx = std::atoi(c);
                if (idx >= 0 && idx < nch) o.visible[idx] = 1;
                while (*c && *c != ',' && *c != ' ') ++c;   // skip to next index
                if (*c == ',') ++c;
            }
    }
    o.synced = false;   // force the filter/reference state to be re-pushed to the worker
    return o;
}

// Stable per-channel color (by absolute channel index, so a channel keeps its
// color across stacked/overlay and regardless of which others are selected).
// ImPlot v1.0 takes styling via ImPlotSpec on the plot call. We draw a light
// min/max fill (transients) plus a crisp center line (always visible, never
// collapses below ~1px when zoomed out).
static ImPlotSpec bandSpec(int c) {
    ImPlotSpec sp;
    sp.FillColor = ImPlot::GetColormapColor(c);
    sp.FillAlpha = 0.55f;   // visible enough to read on its own when zoomed out
    return sp;
}
static ImPlotSpec lineSpec(int c, float w) {
    ImPlotSpec sp;
    sp.LineColor  = ImPlot::GetColormapColor(c);
    sp.LineWeight = w;
    return sp;
}

// Missing-data red, OPAQUE so the hue is identical regardless of what's underneath
// (time-series plot background vs the spectrogram's dark colormap floor) — they must
// match across views.
static constexpr ImU32 kDropoutRed = IM_COL32(200, 45, 45, 255);

// Paint opaque red over every missing-data span. Unifies the recorded gaps (each
// [a, b], with `extraEnd` added for the spectrogram's STFT recovery latency) and the
// LIVE edge gap [newestTime, view edge] (drawn whenever the gliding edge has advanced
// past the last data — geometry, not a stale-time threshold, so it hands off to the
// recorded band without a flicker). Adjacent intervals within `mergeGap` are merged
// (use 0 to only fuse exactly-touching spans, avoiding a seam, while still SHOWING any
// real data between two dropouts rather than masking it). Producer records the gap
// before publishing samples, so resumed data is never transiently covered by the red.
// Call inside an active plot, after the data. Spans the full Y extent.
static void drawDropoutRed(HfStreamSource& s, double extraEnd, double mergeGap) {
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    std::vector<std::pair<double, double>> iv;
    auto [base, gv] = s.gapSnapshot();    // {folded base offset, recent gaps}, one lock
    double prior = base;
    for (auto& g : gv) {
        const double a = g.first + prior;
        prior += g.second;
        iv.push_back({a, a + g.second + extraEnd});   // recorded gap (display time)
    }
    const double live = s.newestTime();               // live edge gap, if the edge ran ahead
    if (lim.X.Max - live > 0.05) iv.push_back({live, lim.X.Max});
    if (iv.empty()) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    auto flush = [&](double s0, double s1) {
        if (s1 < lim.X.Min || s0 > lim.X.Max) return;
        const double x0 = std::max(s0, lim.X.Min), x1 = std::min(s1, lim.X.Max);
        if (x1 > x0) dl->AddRectFilled(ImPlot::PlotToPixels(x0, lim.Y.Max),
                                       ImPlot::PlotToPixels(x1, lim.Y.Min), kDropoutRed);
    };
    double cs = iv[0].first, ce = iv[0].second;       // iv is time-ordered (gaps then live)
    for (std::size_t i = 1; i < iv.size(); ++i) {
        if (iv[i].first - ce <= mergeGap) ce = std::max(ce, iv[i].second);  // merge
        else { flush(cs, ce); cs = iv[i].first; ce = iv[i].second; }
    }
    flush(cs, ce);
}

// Overlay marker events on the current plot as vertical lines + labels. `evs` must be
// time-sorted. For readability over the trace: lines are semi-transparent (data shows
// through); labels are packed into a few stacked rows at the top, each on a dark
// backing so the text stays legible; and hovering near a line shows a tooltip of the
// event(s) there (with time) — handy where labels are too dense to all fit.
// Call inside an active plot, after the data.
// `contrast` is for drawing over a filled heatmap (the raster): a dim see-through line
// vanishes against the colormap, so the line gets a black halo + opaque core (its hue is
// kept, so per-label colors still read). On a normal trace it stays dim so data shows through.
static void drawMarkers(const std::vector<MarkerEvent>& evs, bool contrast = false) {
    if (evs.empty()) return;
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    ImDrawList*      dl  = ImPlot::GetPlotDrawList();
    const float yTop = ImPlot::PlotToPixels(lim.X.Min, lim.Y.Max).y;
    const float yBot = ImPlot::PlotToPixels(lim.X.Min, lim.Y.Min).y;
    const float lh   = ImGui::GetTextLineHeight();
    const bool  hov  = ImPlot::IsPlotHovered();
    const float mx   = ImGui::GetIO().MousePos.x;

    float rowEnd[kMarkerRows];               // last drawn label's right edge, per row
    for (float& r : rowEnd) r = -1e9f;
    float lastLine = -1e9f;
    const MarkerEvent* hit = nullptr;        // first event under the cursor

    constexpr float kCollapsePx = 3.0f;      // a denser cluster reads as one line anyway
    for (const auto& e : evs) {
        if (e.t < lim.X.Min || e.t > lim.X.Max) continue;
        const float x = ImPlot::PlotToPixels(e.t, lim.Y.Max).x;
        const bool drewLine = (x - lastLine >= kCollapsePx);
        if (drewLine) {                       // collapse sub-3px clusters (caps the vertex budget)
            if (contrast) {                   // over a heatmap fill: black halo + opaque colored core
                dl->AddLine(ImVec2(x, yTop), ImVec2(x, yBot), IM_COL32(0, 0, 0, 200), 3.0f);
                const ImU32 lc = (e.col & 0x00FFFFFFu) | (235u << 24);
                dl->AddLine(ImVec2(x, yTop), ImVec2(x, yBot), lc, 1.5f);
            } else {
                const ImU32 lc = (e.col & 0x00FFFFFFu) | (110u << 24);   // dim so the trace shows
                dl->AddLine(ImVec2(x, yTop), ImVec2(x, yBot), lc, 1.0f);
            }
            lastLine = x;
        }
        if (hov && !hit && std::abs(x - mx) <= 3.0f) hit = &e;
        // Label only on a drawn line (so dense streams don't pile up labels either), in
        // its FIXED row (stable across pan), skipped if it would land on the previous.
        if (drewLine && e.text && !e.text->empty() && x > rowEnd[e.row] + 3.0f) {
            const float  w = ImGui::CalcTextSize(e.text->c_str()).x;
            const ImVec2 p(x + 2.0f, yTop + 1.0f + (float)e.row * lh);
            dl->AddRectFilled(ImVec2(p.x - 1.0f, p.y), ImVec2(p.x + w + 1.0f, p.y + lh),
                              IM_COL32(0, 0, 0, 150));                // backing for legibility
            dl->AddText(p, e.col, e.text->c_str());
            rowEnd[e.row] = x + 2.0f + w;
        }
    }
    if (hit) {   // list every event at the hovered x (with time)
        ImGui::BeginTooltip();
        for (const auto& e : evs) {
            if (e.t < lim.X.Min || e.t > lim.X.Max) continue;
            const float x = ImPlot::PlotToPixels(e.t, lim.Y.Max).x;
            if (std::abs(x - mx) <= 3.0f && e.text)
                ImGui::Text("%.3f s  \xc2\xb7  %s", e.t, e.text->c_str());
        }
        ImGui::EndTooltip();
    }
}

// Draw one connected stream. `edge` is the smoothed right-edge time; `followX`
// is false while paused (so the X axis stops auto-following and can be panned).
// `headFreeze` (0 = live) anchors the data read to a frozen absolute sample index
// while paused, so the plotted window doesn't slide off as new data keeps arriving.
// Read up to `want` most-recent samples from a ring. When `endHead` is 0 the live edge
// is read (ring's own head); otherwise the window ENDS at `endHead` (a frozen/paused
// head). Either way the start is clamped to what's still resident, using THIS ring's own
// head as the lower bound — so it's correct for any ring (the raw ring and the filtered
// ring publish their heads at slightly different times). Returns a contiguous pointer
// (the magic ring guarantees no wrap split) and sets count/start. Shared by fillRaw + FFT.
static const float* readWindow(InterleavedRing& rg, std::uint64_t endHead,
                               std::size_t want, std::size_t& count, std::uint64_t& start) {
    if (endHead == 0) return rg.recent(want, count, start);                 // live edge
    const std::size_t   cap  = rg.capacity();
    const std::uint64_t head = rg.head();
    const std::uint64_t oldest = (head > cap) ? head - cap : 0;             // this ring's resident floor
    start = (endHead > want) ? endHead - want : 0;
    if (start < oldest) start = oldest;
    count = (endHead >= start) ? (std::size_t)(endHead - start) : 0;
    return rg.windowAt(start);
}

// "Fit once, then free": snap an axis to its limits the frame something CHANGED (mode /
// stream / range edit), then leave it Once so the mouse can zoom/pan freely afterward.
// The per-site change detection differs (a flag, a value compare); this just names the
// shared cond. (Overlay-mode Y is a separate AutoFit mechanism — see overlayYFit.)
static ImPlotCond fitCond(bool changed) { return changed ? ImPlotCond_Always : ImPlotCond_Once; }

static void drawStream(HfStreamSource& s, DisplayOpts& o, double edge, bool followX,
                       std::uint64_t headFreeze,
                       const std::vector<MarkerStreamView>& markerViews, PlotScratch& sc) {
    LSL_ZONE("drawStream");
    constexpr float kCfgWidth = 230.0f;
    // Above this many visible line points (samples-in-view x channels) the stacked/overlay
    // views fall back to the min/max envelope; below it they draw the raw trace (which looks
    // better but costs a point per sample). ~200k keeps even a 32 ch x 10 s @ 500 Hz montage
    // on the raw path while still capping audio / high-density streams.
    constexpr double kRawPointBudget = 200000.0;

    const std::string err = s.error();
    if (!err.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "error: %s", err.c_str());

    if (s.srate() <= 0.0) {
        ImGui::TextUnformatted("irregular / marker stream — marker view not yet implemented");
        return;
    }

    const int C = s.channels();
    // Push our filter/reference state to the source ONCE, so the producer's conditioning
    // matches the UI regardless of whether the defaults happen to line up (the UI setters
    // otherwise only fire on a toggle).
    if (!o.synced) {
        s.setHighpass(o.highpass); s.setHighpassHz(o.hpHz);
        s.setNotch(o.notch);       s.setNotchHz(o.notchHz);
        s.setLowpass(o.lowpass);   s.setLowpassHz(o.lpHz);
        s.setReference(o.refMode); s.setRefChannel(o.refChan);
        o.synced = true;
    }
    const auto& labels = s.labels();   // lock-free ref (see HfStreamSource::labels)
    const auto& units  = s.units();
    const char* streamUnit = "a.u.";           // representative unit for labels
    for (auto& u : units) if (!u.empty()) { streamUnit = u.c_str(); break; }
    if ((int)o.visible.size() != C) {          // default: first 16 channels visible
        o.visible.assign(C, 0);
        for (int c = 0; c < std::min(C, 16); ++c) o.visible[c] = 1;
    }
    std::vector<float>& gain = s.chanGain();
    std::vector<float>& amp  = s.chanAmp();

    // Visible-channel index list — needed by both the config UI and the plot, so
    // compute it before the (collapsible) config strip.
    sc.visIdx.clear();
    for (int c = 0; c < C; ++c) if (o.visible[c]) sc.visIdx.push_back(c);
    const int show = (int)sc.visIdx.size();

    // ---- left column: config (collapsible so the plot can go full-width) ----
    if (!o.cfgShown) {
        if (ImGui::SmallButton(">")) o.cfgShown = true;   // restore the config strip
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("show controls");
        ImGui::SameLine();
    }
    if (o.cfgShown) {
    ImGui::BeginChild("cfg", ImVec2(kCfgWidth, 0), ImGuiChildFlags_Borders);
    if (ImGui::SmallButton("< hide")) o.cfgShown = false;   // hide to widen the plot
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("History (s)", &o.history, 1.0f, 60.0f, "%.0f");
        ImGui::Checkbox("Stacked montage", &o.stacked);
        ImGui::BeginDisabled(!o.stacked);          // raster is a stacked-montage render style
        ImGui::Checkbox("Raster", &o.raster);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("render the stacked montage as one heatmap (color = activity)\n"
                              "instead of N line lanes — far cheaper at high channel counts");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!o.stacked);          // lane gain: stacked montage only
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("Gain/lane", &o.gainUv, 5.0f, 2000.0f, "%.0f",
                           ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("per-lane gain (%s)", streamUnit);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(o.stacked);           // channel spacing: overlay only
        ImGui::SetNextItemWidth(110);
        if (ImGui::SliderFloat("Spacing", &o.spacing, 0.0f, 5000.0f, "%.0f")) o.overlayYFit = true;
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("Line width", &o.lineWidth, 0.5f, 4.0f, "%.1f");
        // Conditioning stages (re-reference -> high-pass -> notch -> low-pass) are each
        // INDEPENDENT — press any combination. The plot shows the conditioned signal
        // whenever ANY stage is on, and the raw signal when all are off (no master toggle).
        // High-pass stage (own toggle + cutoff)
        if (ImGui::Checkbox("High-pass", &o.highpass)) { s.setHighpass(o.highpass); o.overlayYFit = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.highpass);
        ImGui::SetNextItemWidth(90);
        if (ImGui::SliderFloat("##hpcut", &o.hpHz, 0.1f, 5.0f, "%.2f Hz",
                               ImGuiSliderFlags_Logarithmic))
            s.setHighpassHz(o.hpHz);
        ImGui::EndDisabled();
        // Notch stage
        if (ImGui::Checkbox("Notch", &o.notch)) { s.setNotch(o.notch); o.overlayYFit = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("band-reject the mains line frequency (50/60 Hz) and its hum");
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.notch);
        ImGui::SetNextItemWidth(60);
        if (ImGui::SliderFloat("##notchhz", &o.notchHz, 45.0f, 65.0f, "%.0f Hz")) s.setNotchHz(o.notchHz);
        ImGui::SameLine();
        const bool is50 = o.notchHz < 55.0f;
        if (ImGui::SmallButton(is50 ? "60" : "50")) {   // quick mains toggle (label = target)
            o.notchHz = is50 ? 60.0f : 50.0f; s.setNotchHz(o.notchHz);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("jump to %s Hz mains", is50 ? "60" : "50");
        ImGui::EndDisabled();
        // Low-pass stage
        if (ImGui::Checkbox("Low-pass", &o.lowpass)) { s.setLowpass(o.lowpass); o.overlayYFit = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("smooth / anti-EMG: attenuate everything above the cutoff");
        ImGui::SameLine();
        ImGui::BeginDisabled(!o.lowpass);
        ImGui::SetNextItemWidth(90);
        if (ImGui::SliderFloat("##lphz", &o.lpHz, 5.0f, 120.0f, "%.0f Hz",
                               ImGuiSliderFlags_Logarithmic)) s.setLowpassHz(o.lpHz);
        ImGui::EndDisabled();
        // Re-reference montage — the FIRST stage of the conditioned chain (reference ->
        // high-pass -> notch -> low-pass), so it lives with the filter controls.
        const char* refModes[] = { "None", "Avg (CAR)", "Channel" };
        ImGui::SetNextItemWidth(110);
        if (ImGui::Combo("Reference", &o.refMode, refModes, 3)) { s.setReference(o.refMode); o.overlayYFit = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("re-reference the montage: subtract the common average (CAR,\n"
                              "over the EEG channels only) or a chosen channel from every channel");
        if (o.refMode == 2) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(90);
            if (o.refChan >= C) o.refChan = 0;
            const char* rc = (o.refChan < (int)labels.size()) ? labels[o.refChan].c_str() : "ch";
            if (ImGui::BeginCombo("##refch", rc)) {
                for (int c = 0; c < C; ++c)
                    if (ImGui::Selectable(labels[c].c_str(), c == o.refChan)) {
                        o.refChan = c; s.setRefChannel(c);
                    }
                ImGui::EndCombo();
            }
        }
    }
    if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen)) {
        // All/None act on the visible (filtered) set, so "Cz" + All selects all
        // Cz* channels; clear the filter and All selects everything.
        auto pass = [&](int c) { return o.chanFilter.PassFilter(labels[c].c_str()); };
        const bool filtering = o.chanFilter.IsActive();
        if (ImGui::SmallButton("All"))  for (int c = 0; c < C; ++c) if (pass(c)) o.visible[c] = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(filtering ? "select all channels matching the filter"
                                        : "select all channels");
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) for (int c = 0; c < C; ++c) if (pass(c)) o.visible[c] = 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(filtering ? "deselect channels matching the filter"
                                        : "deselect all channels");
        o.chanFilter.Draw("filter", 140.0f);       // own line; e.g. "Cz" or "Cz,Pz" or "-EOG"
        ImGui::BeginChild("chsel", ImVec2(0, 120), ImGuiChildFlags_Borders);
        for (int c = 0; c < C; ++c) {
            if (!pass(c)) continue;
            bool on = o.visible[c] != 0;
            if (ImGui::Checkbox(labels[c].c_str(), &on)) o.visible[c] = on ? 1 : 0;
        }
        ImGui::EndChild();
    }

    if (o.stacked && ImGui::CollapsingHeader("Channel gains")) {
        if (ImGui::SmallButton("Auto"))          // fit each lane from measured amplitude
            for (int j = 0; j < show; ++j) {
                const int c = sc.visIdx[j];
                gain[c] = std::clamp(o.gainUv * 0.4f / std::max(amp[c], 1e-3f), 0.01f, 100.0f);
            }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) for (int j = 0; j < show; ++j) gain[sc.visIdx[j]] = 1.0f;
        for (int j = 0; j < show; ++j) {
            const int c = sc.visIdx[j];
            ImGui::PushID(c);
            ImGui::SetNextItemWidth(120);
            ImGui::DragFloat(labels[c].c_str(), &gain[c], 0.01f, 0.01f, 100.0f, "%.2fx",
                             ImGuiSliderFlags_Logarithmic);
            ImGui::PopID();
        }
    }
    // ---- Markers: overlay event lines from selected marker streams ----------
    if (!markerViews.empty() && ImGui::CollapsingHeader("Markers")) {
        ImGui::Checkbox("Overlay markers", &o.overlayMarkers);
        ImGui::BeginDisabled(!o.overlayMarkers);
        for (const auto& mv : markerViews) {
            bool on = o.markerOn.count(mv.id) ? o.markerOn[mv.id] : true;  // default on
            ImGui::PushStyleColor(ImGuiCol_Text, mv.col);
            if (ImGui::Checkbox(mv.name.c_str(), &on)) o.markerOn[mv.id] = on;
            ImGui::PopStyleColor();
        }
        ImGui::EndDisabled();
    }
    // ---- Info / health: stream metadata + live delivery quality -------------
    if (ImGui::CollapsingHeader("Info")) {
        const ImVec4 warn(1.0f, 0.6f, 0.2f, 1.0f);
        ImGui::Text("type: %s", s.type().empty() ? "(none)" : s.type().c_str());
        ImGui::TextWrapped("source id: %s", s.sourceId().empty() ? "(none)" : s.sourceId().c_str());
        ImGui::Text("channels: %d", C);
        ImGui::Text("unit: %s", streamUnit);
        // Per-channel modality types (distinct, with counts) from the metadata — one
        // pass over channels accumulating into an ordered (key,count) list.
        {
            const auto& types = s.types();
            static const std::string kNone("(none)");
            std::vector<std::pair<std::string, int>> tc;
            for (int c = 0; c < C; ++c) {
                const std::string& key = (c < (int)types.size() && !types[c].empty()) ? types[c] : kNone;
                bool found = false;
                for (auto& p : tc) if (p.first == key) { ++p.second; found = true; break; }
                if (!found) tc.push_back({key, 1});
            }
            std::string ts;
            for (auto& p : tc) { if (!ts.empty()) ts += ", "; ts += p.first + " x" + std::to_string(p.second); }
            ImGui::TextWrapped("ch types: %s", ts.c_str());
        }
        // Sensor positions (the basis for any spatial/topographic view). Surfaced so we
        // can tell at a glance whether a stream actually carries a layout.
        {
            const auto& locs = s.locs();
            int withPos = 0; for (const auto& l : locs) if (l.valid) ++withPos;
            if (withPos > 0)
                ImGui::Text("positions: %d/%d channels", withPos, C);
            else
                ImGui::TextDisabled("positions: none in metadata");
        }
        ImGui::Separator();
        const double nom = s.srate(), meas = s.measuredRate();
        if (s.irregular()) {
            ImGui::Text("rate: irregular (resampled @ %.0f Hz)", nom);
        } else {
            ImGui::Text("nominal: %.0f Hz", nom);
            if (meas > 0.0) {
                const double pct = (nom > 0.0) ? (meas - nom) / nom * 100.0 : 0.0;
                const bool bad = std::fabs(pct) > 5.0;
                ImGui::TextColored(bad ? warn : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                                   "measured: %.1f Hz (%+.1f%%)", meas, pct);
            } else {
                ImGui::TextDisabled("measured: --");
            }
        }
        ImGui::Text("clock offset: %+.2f ms", s.clockOffset() * 1000.0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("LSL time_correction (remote -> local clock)");
        ImGui::Text("dropouts: %llu", (unsigned long long)s.dropouts());
        const double st = s.staleSeconds();
        if (st > 0.5) ImGui::TextColored(warn, "no data for %.1f s", st);
    }

    // Merge the enabled streams' events into one time-sorted list for decluttered
    // drawing (mv.events points into the MarkerSource's cache, which outlives this call).
    sc.markers.clear();
    if (o.overlayMarkers) {
        for (std::size_t vi = 0; vi < markerViews.size(); ++vi) {
            const auto& mv = markerViews[vi];
            const bool on = o.markerOn.count(mv.id) ? o.markerOn[mv.id] : true;
            if (!on) continue;
            for (const auto& e : *mv.events)           // stable row from event seq (+stream offset)
                sc.markers.push_back({e.t, &e.text, mv.col, (int)((e.seq + vi) % kMarkerRows)});
        }
        std::sort(sc.markers.begin(), sc.markers.end(),
                  [](const MarkerEvent& a, const MarkerEvent& b) { return a.t < b.t; });
    }

    ImGui::EndChild();  // cfg
    ImGui::SameLine();
    }  // if (o.cfgShown)

    // ---- right column: the plot --------------------------------------------
    ImGui::BeginChild("plt", ImVec2(0, 0));
    if (!s.anchored() || s.head() == 0) {
        ImGui::TextUnformatted("waiting for data...");
        ImGui::EndChild();
        return;
    }
    if (show == 0) {
        ImGui::TextUnformatted("no channels selected");
        ImGui::EndChild();
        return;
    }

    const double dt = s.dt(), t0 = s.time0(), rate = s.srate();
    const int    B  = s.binSamples();
    // Paused: anchor the summary read to the bin holding the frozen head.
    const std::uint64_t endBin = headFreeze ? headFreeze / (std::uint64_t)B : 0;
    const double viewSamples   = (o.history + 0.5) * rate;  // read past both edges
    // Show the conditioned (filtered) signal when ANY stage is enabled, else raw.
    const bool   filtered = o.highpass || o.notch || o.lowpass || (o.refMode != 0);
    MinMaxSummary& summ = filtered ? s.summaryHp() : s.summary();
    auto envelopeBins = [&]() {
        return std::max<std::size_t>(1, (std::size_t)(viewSamples / B) + 4);
    };

    // Fill xs (real time) + ys (raw, or high-pass filtered on the fly) for one
    // channel over the visible window. Used when zoomed in enough to resolve the
    // actual waveform — the min/max band can't (its midline is flat for signals
    // oscillating faster than a bin, e.g. a 40 Hz sine).
    auto fillRaw = [&](int c, std::vector<double>& xs, std::vector<double>& ys) -> int {
        // Read the pre-filtered ring when the display filter is on (full chain:
        // high-pass -> notch -> low-pass), else the raw ring. Both rings share indices
        // (written in lockstep), so no per-frame filtering or warm-up is needed.
        InterleavedRing& rg = filtered ? s.ringHp() : s.ring();
        const std::size_t want = std::min<std::size_t>((std::size_t)viewSamples, rg.capacity());
        std::size_t count = 0; std::uint64_t start = 0;
        const float* p = readWindow(rg, headFreeze, want, count, start);
        if (count == 0) return 0;
        const int vis = (int)count;
        xs.resize(vis); ys.resize(vis);
        for (int j = 0; j < vis; ++j) ys[j] = p[(std::size_t)j * (std::size_t)C + c];
        for (int j = 0; j < vis; ++j) xs[j] = t0 + (double)(start + (std::size_t)j) * dt;  // double: absolute LSL time
        s.applyGaps(xs.data(), vis);
        return vis;
    };

    // Pixel-snap the scrolling right edge (using last frame's plot width) so the
    // trace, gridlines, and time-axis labels travel in whole-pixel steps instead
    // of shimmering by a sub-pixel each frame as they scroll.
    double xedge = edge;
    if (followX && o.lastPlotPx > 1.0f && o.history > 0.0f) {
        const double secPerPx = (double)o.history / (double)o.lastPlotPx;
        xedge = std::round(edge / secPerPx) * secPerPx;
    }

    // Arm the overlay Y auto-fit while we're in a non-overlay mode, so switching INTO
    // overlay fits once before the axis is left free (the overlay branch consumes it).
    if (o.stacked) o.overlayYFit = true;

    // ---- Raster: a render style OF the stacked montage — all visible channels as one
    // heatmap (color = per-bin peak-to-peak activity), a single PlotHeatmap drawcall
    // regardless of channel count. The scalable view for many (32-256) channels, where
    // line traces are too short to read. Only applies to the stacked layout (there's no
    // meaningful overlay heatmap), so overlay mode below ignores it. Reuses the envelope.
    if (o.stacked && o.raster) {
        if (ImPlot::BeginPlot("##plot", ImVec2(-70, -1), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("time (s)", nullptr,
                              ImPlotAxisFlags_None, ImPlotAxisFlags_NoGridLines);
            if (followX)
                ImPlot::SetupAxisLimits(ImAxis_X1, xedge - o.history, xedge, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, (double)show, ImPlotCond_Always);
            sc.tvals.resize(show); sc.tlabs.resize(show); sc.tstr.resize(show);
            for (int j = 0; j < show; ++j) {                  // channel names at row centers
                sc.tvals[j] = (double)show - 0.5 - (double)j;  // row j (0 = top = first visible)
                sc.tstr[j]  = labels[sc.visIdx[j]];
                sc.tlabs[j] = sc.tstr[j].c_str();
            }
            ImPlot::SetupAxisTicks(ImAxis_Y1, sc.tvals.data(), show, sc.tlabs.data());
            const int px = std::max(64, (int)ImPlot::GetPlotSize().x);
            o.lastPlotPx = ImPlot::GetPlotSize().x;

            // Build ONE heatmap cell per pixel column, mapped to the pixel-snapped axis
            // range, so cells align to the pixel grid and don't shimmer as the view
            // scrolls (the native ~0.03 s summary bins are ~2 px wide -> sub-pixel drift).
            const std::size_t maxBins = envelopeBins();
            const float  inv      = (o.gainUv > 0.0f) ? 1.0f / o.gainUv : 0.0f;
            const double bMin     = xedge - o.history, bMax = xedge;        // snapped axis range
            const double secPerPx = o.history / (double)px;
            sc.x.resize(maxBins); sc.mn.resize(maxBins); sc.mx.resize(maxBins);
            // Channel 0 also builds the pixel-column -> summary-bin resample map (the bin
            // grid is shared by all channels, so it's computed once).
            const int n0 = (show > 0)
                ? summ.read(sc.visIdx[0], maxBins, dt, t0, sc.x.data(), sc.mn.data(), sc.mx.data(), endBin) : 0;
            if (n0 > 0) {
                s.applyGaps(sc.x.data(), n0);
                sc.pmap.resize(px);
                int bi = 0;
                for (int k = 0; k < px; ++k) {
                    const double tk = bMin + ((double)k + 0.5) * secPerPx;     // pixel-center time
                    while (bi < n0 - 1 && (double)sc.x[bi + 1] <= tk) ++bi;     // nearest bin
                    sc.pmap[k] = (bi < n0 - 1 && (tk - sc.x[bi]) > (sc.x[bi + 1] - tk)) ? bi + 1 : bi;
                }
                sc.raster.resize((std::size_t)show * px);                       // row-major, row 0 = top
                for (int k = 0; k < px; ++k) sc.raster[k] = (sc.mx[sc.pmap[k]] - sc.mn[sc.pmap[k]]) * inv;
                for (int j = 1; j < show; ++j) {
                    summ.read(sc.visIdx[j], maxBins, dt, t0, sc.x.data(), sc.mn.data(), sc.mx.data(), endBin);
                    float* dst = sc.raster.data() + (std::size_t)j * px;
                    for (int k = 0; k < px; ++k) dst[k] = (sc.mx[sc.pmap[k]] - sc.mn[sc.pmap[k]]) * inv;
                }
                // Plasma here (vs the spectrogram's Viridis) so the two heatmaps read as
                // different views at a glance — both perceptually uniform, distinct hues.
                ImPlot::PushColormap(ImPlotColormap_Plasma);
                ImPlot::PlotHeatmap("##r", sc.raster.data(), show, px, 0.0, 1.0, nullptr,
                                    ImPlotPoint(bMin, 0.0), ImPlotPoint(bMax, (double)show));
                drawDropoutRed(s, 0.0, 0.0);
                drawMarkers(sc.markers, /*contrast=*/true);   // over the heatmap fill
                ImPlot::EndPlot();
                ImGui::SameLine();
                ImPlot::ColormapScale("p2p", 0.0, (double)o.gainUv, ImVec2(60, -1), "%.0f");
                ImPlot::PopColormap();
            } else {
                ImPlot::EndPlot();
            }
        }
        ImGui::EndChild();  // plt
        return;
    }

    // ---- Stacked montage: one lane per visible channel, gain-scaled ----------
    if (o.stacked) {
        if (ImPlot::BeginPlot("##plot", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("time (s)", nullptr,
                              ImPlotAxisFlags_None, ImPlotAxisFlags_NoGridLines);
            if (followX)
                ImPlot::SetupAxisLimits(ImAxis_X1, xedge - o.history, xedge, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -0.6, (double)show - 0.4, ImPlotCond_Always);

            // Y ticks at lane centers labelled with channel names (top = first
            // visible). Append "(Gx)" when scaled, so the display stays honest.
            sc.tvals.resize(show); sc.tlabs.resize(show); sc.tstr.resize(show);
            for (int j = 0; j < show; ++j) {
                const int c = sc.visIdx[j];
                sc.tvals[j] = (double)(show - 1 - j);
                const char* nm = labels[c].c_str();
                if (gain[c] > 1.005f || gain[c] < 0.995f) {
                    char buf[48]; std::snprintf(buf, sizeof(buf), "%s (%.2gx)", nm, gain[c]);
                    sc.tstr[j] = buf;
                } else {
                    sc.tstr[j] = nm;
                }
                sc.tlabs[j] = sc.tstr[j].c_str();
            }
            ImPlot::SetupAxisTicks(ImAxis_Y1, sc.tvals.data(), show, sc.tlabs.data());

            const int    px     = std::max(64, (int)ImPlot::GetPlotSize().x);
            o.lastPlotPx = ImPlot::GetPlotSize().x;   // for next frame's edge snap
            // Raw-vs-envelope decision. Use the min/max band only when drawing the raw trace
            // would be genuinely heavy: the visible point count (samples-in-view x channels)
            // exceeds a budget. Below that, raw is cheap and looks far better — the coarse
            // ~0.03 s band reads as a staircase for clean / low-frequency signals and a jagged
            // ribbon for noise. The visible range (not the whole history) is what's measured, so
            // zooming in also drops to raw. Live, the visible range == history.
            const double visSamples = ImPlot::GetPlotLimits().X.Size() * rate;
            const bool   useEnv = visSamples > 3.0 * (double)px && visSamples * (double)show > kRawPointBudget;
            const float  inv    = (o.gainUv > 0.0f) ? 1.0f / o.gainUv : 0.0f;
            const std::size_t bins = envelopeBins();
            for (int j = 0; j < show; ++j) {
                const int   c    = sc.visIdx[j];
                const float lane = (float)(show - 1 - j);
                const float g    = gain[c] * inv;
                if (useEnv) {
                    LSL_ZONE("envelope");
                    sc.x.resize(bins); sc.mn.resize(bins); sc.mx.resize(bins); sc.y.resize(bins);
                    const int n = summ.read(c, bins, dt, t0, sc.x.data(), sc.mn.data(),
                                            sc.mx.data(), endBin);
                    s.applyGaps(sc.x.data(), n);
                    for (int i = 0; i < n; ++i) {
                        sc.y[i]  = 0.5f * (sc.mx[i] - sc.mn[i]);   // half-range, for Auto
                        sc.mn[i] = lane + sc.mn[i] * g;
                        sc.mx[i] = lane + sc.mx[i] * g;
                    }
                    if (n > 0) {
                        const int k = std::min(n - 1, (int)(0.8 * n));
                        std::nth_element(sc.y.begin(), sc.y.begin() + k, sc.y.begin() + n);
                        amp[c] = sc.y[k];
                    }
                    char id[12]; std::snprintf(id, sizeof(id), "##b%d", c);
                    ImPlot::PlotShaded(id, sc.x.data(), sc.mn.data(), sc.mx.data(), n, bandSpec(c));
                } else {
                    LSL_ZONE("raw");
                    const int n = fillRaw(c, sc.x, sc.y);
                    for (int i = 0; i < n; ++i) sc.y[i] = lane + sc.y[i] * g;
                    char id[12]; std::snprintf(id, sizeof(id), "##l%d", c);
                    ImPlot::PlotLine(id, sc.x.data(), sc.y.data(), n, lineSpec(c, o.lineWidth));
                }
            }

            // Amplitude scale bar near the left == gainUv units (unity-gain lanes).
            const double xbar = ImPlot::GetPlotLimits().X.Min + 0.03 * o.history;
            const ImU32  col  = ImGui::GetColorU32(ImGuiCol_Text);   // adapts to theme
            ImDrawList*  dl   = ImPlot::GetPlotDrawList();
            dl->AddLine(ImPlot::PlotToPixels(xbar, -0.5), ImPlot::PlotToPixels(xbar, 0.5), col, 2.0f);
            const int   c0   = sc.visIdx[0];
            const char* unit = (c0 < (int)units.size() && !units[c0].empty())
                                   ? units[c0].c_str() : "a.u.";
            ImPlot::Annotation(xbar, 0.0, ImGui::ColorConvertU32ToFloat4(col), ImVec2(6, 0),
                               false, "%.0f %s", o.gainUv, unit);
            drawDropoutRed(s, 0.0, 0.0);
            drawMarkers(sc.markers);
            ImPlot::EndPlot();
        }
        ImGui::EndChild();  // plt
        return;
    }

    // ---- Overlay: shared amplitude axis (raw values, optional offset) ---------
    if (ImPlot::BeginPlot("##plot", ImVec2(-1, -1))) {
        // Auto-fit Y only when armed (entering overlay / a scale change); then leave it
        // FREE so the mouse can zoom/pan the amplitude axis (double-click re-fits).
        const ImPlotAxisFlags yf = o.overlayYFit ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
        ImPlot::SetupAxes("time (s)", streamUnit, ImPlotAxisFlags_None, yf);
        o.overlayYFit = false;
        if (followX)
            ImPlot::SetupAxisLimits(ImAxis_X1, xedge - o.history, xedge, ImPlotCond_Always);

        const int    px     = std::max(64, (int)ImPlot::GetPlotSize().x);
        o.lastPlotPx = ImPlot::GetPlotSize().x;   // for next frame's edge snap
        // Same point-budget decision as the stacked branch (raw unless it'd be heavy).
        const double visSamples = ImPlot::GetPlotLimits().X.Size() * rate;
        const bool   useEnv = visSamples > 3.0 * (double)px && visSamples * (double)show > kRawPointBudget;
        const std::size_t bins = envelopeBins();

        for (int j = 0; j < show; ++j) {
            const int   c     = sc.visIdx[j];
            const float yoff  = (float)j * o.spacing;
            const char* label = labels[c].c_str();
            if (useEnv) {
                LSL_ZONE("envelope");
                sc.x.resize(bins); sc.mn.resize(bins); sc.mx.resize(bins);
                const int n = summ.read(c, bins, dt, t0, sc.x.data(), sc.mn.data(),
                                        sc.mx.data(), endBin);
                s.applyGaps(sc.x.data(), n);
                if (yoff != 0.0f) for (int i = 0; i < n; ++i) { sc.mn[i] += yoff; sc.mx[i] += yoff; }
                ImPlot::PlotShaded(label, sc.x.data(), sc.mn.data(), sc.mx.data(), n, bandSpec(c));
            } else {
                LSL_ZONE("raw");
                const int n = fillRaw(c, sc.x, sc.y);
                if (yoff != 0.0f) for (int i = 0; i < n; ++i) sc.y[i] += yoff;
                ImPlot::PlotLine(label, sc.x.data(), sc.y.data(), n, lineSpec(c, o.lineWidth));
            }
        }
        drawDropoutRed(s, 0.0, 0.0);
        drawMarkers(sc.markers);
        ImPlot::EndPlot();
    }
    ImGui::EndChild();  // plt
}

// Apply VSync vs uncapped present mode, falling back gracefully if the requested
// mode is unsupported by the swapchain.
static void applyPresentMode(SDL_GPUDevice* gpu, SDL_Window* window, bool vsync) {
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;  // always supported
    if (!vsync) {
        if (SDL_WindowSupportsGPUPresentMode(gpu, window, SDL_GPU_PRESENTMODE_IMMEDIATE))
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        else if (SDL_WindowSupportsGPUPresentMode(gpu, window, SDL_GPU_PRESENTMODE_MAILBOX))
            mode = SDL_GPU_PRESENTMODE_MAILBOX;
    }
    SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

// ---- Spectrogram: rolling STFT heatmap for one channel ----------------------
struct Spectro {
    Psd                psd;
    std::string        wantSid;   // workspace restore: bind to this stream source_id once it appears
    int                streamIdx = 0;
    int                channel   = 0;
    int                nfft      = 256;
    bool               db        = true;
    float              dbMin = -40.0f, dbMax = 20.0f;
    float              fMin = 0.0f, fMax = 0.0f; // displayed frequency range (Y axis); fMax reset to Nyquist
    bool               yDirty = false;          // freq-range control edited -> reapply axis limits this frame
    float              spanSec   = 2.0f;        // displayed time width (X axis)
    int                cols      = 240;
    std::string        uid;                     // stream-change detection
    int                freqBins  = 0;
    std::vector<float> data;     // cols*freqBins COLUMN-major ring (col c contiguous; r=0 = Nyquist)
    std::vector<float> drawBuf;  // visible columns de-rotated -> row-major, for PlotHeatmap
    std::vector<float> psdOut;
    int                filled    = 0;
    int                writeCol  = 0;           // ring cursor: physical index of the next column
    int                lastDrawWriteCol = -1, lastDrawNdraw = -1;  // drawBuf dirty-check (skip redundant transpose)
    std::uint64_t      nextEnd   = 0;           // absolute sample index = end of next window
    int                hopSamples = 0;          // window stride (decoupled from nfft for smooth scroll)
    double             hopSec    = 0.0;
    double             lastGapInserted = -1e18; // oldTime of the last dropout already blanked in
    double             colNewestTime = 0.0;     // real time of the newest column (data is pinned here)
    double             smoothNow     = 0.0;     // continuously-advancing view edge (smooth scroll)
    bool               smoothInit    = false;
    ImGuiTextFilter    chanFilter;              // searchable channel dropdown
    ImGuiTextFilter    streamFilter;            // searchable stream dropdown
    bool               filtered = false;        // read the display-filtered ring vs raw
    int                id    = 0;               // unique window id (stable ImGui title)
    bool               open  = true;            // window open flag (closed -> removed)
    bool               focus = true;            // one-shot raise request
};

static void spectroReset(Spectro& sp, HfStreamSource& s) {
    constexpr double kMaxSpan   = 120.0;  // buffer holds up to this much; span slider just views it
    constexpr double kTargetHop = 0.03;   // s/column target — small hop = fast columns = smooth scroll
    sp.psd.init(sp.nfft, (float)s.srate());
    sp.freqBins = sp.psd.bins();
    // Y (frequency) range: seed to full 0..Nyquist on first init AND whenever the stream
    // changes (its Nyquist can differ wildly — 125 Hz EEG vs 24 kHz audio — so a preserved
    // zoom would be meaningless). For same-stream resets (channel/NFFT change, Filtered
    // toggle) PRESERVE the user's zoom, only clamping it to Nyquist.
    const float ny = (float)(s.srate() * 0.5);
    const bool  streamChanged = (sp.uid != s.uid());     // sp.uid is still the previous stream here
    if (sp.fMax <= 0.0f || streamChanged) { sp.fMin = 0.0f; sp.fMax = ny; }
    else { sp.fMin = std::clamp(sp.fMin, 0.0f, ny); sp.fMax = std::clamp(sp.fMax, sp.fMin, ny); }
    sp.yDirty = true;                                      // apply it on the next frame
    // Stride toward ~0.06 s columns regardless of sample rate, but never coarser
    // than 50% overlap (low-rate streams overlap more, so the edge stays fed).
    sp.hopSamples = (int)std::min((double)(sp.nfft / 2),
                                  std::max(1.0, std::round(kTargetHop / s.dt())));
    sp.hopSec     = sp.hopSamples * s.dt();
    sp.cols       = std::clamp((int)(kMaxSpan / std::max(sp.hopSec, 1e-6)), 30, 1500);
    sp.data.assign((std::size_t)sp.freqBins * sp.cols, sp.dbMin);
    sp.filled   = 0;
    sp.writeCol = 0;
    sp.lastDrawNdraw = -1; sp.lastDrawWriteCol = -1;   // force a drawBuf rebuild (freqBins may have changed)
    sp.nextEnd  = s.head();
    sp.uid      = s.uid();
    sp.lastGapInserted = -1e18;
    sp.colNewestTime   = s.newestTime();
    sp.smoothInit      = false;
}

// Compute any STFT columns whose window has fully arrived, appending each on the
// right (older columns shift left). Reads overlapping windows at exact hop
// positions from the raw ring.
static void spectroUpdate(Spectro& sp, HfStreamSource& s) {
    LSL_ZONE("spectrogram");
    const std::uint64_t hop  = (std::uint64_t)std::max(1, sp.hopSamples);
    const int           ch   = s.channels();
    const std::uint64_t head = s.head();
    const std::uint64_t cap  = s.ring().capacity();
    const double        t0   = s.time0();
    const double        dt   = s.dt();
    // Append a column by writing into the ring cursor (no memmove — the old
    // shift-everything-left was up to GBs/frame after a long dropout). Column-major
    // so the write is one contiguous run; the draw de-rotates the visible slice.
    auto writeColumn = [&](bool blank, double colTime) {
        float* col = &sp.data[(std::size_t)sp.writeCol * sp.freqBins];
        for (int r = 0; r < sp.freqBins; ++r)
            col[r] = blank ? sp.dbMin : sp.psdOut[sp.freqBins - 1 - r];   // r=0 = Nyquist
        sp.writeCol = (sp.writeCol + 1) % sp.cols;
        if (sp.filled < sp.cols) sp.filled++;
        sp.colNewestTime = colTime;   // pin the newest column to its real time
    };
    const double latency = (double)sp.nfft * dt;   // until a full post-gap window exists
    const auto   gaps    = s.gaps();
    int added = 0;
    while (sp.nextEnd <= head && added < 48) {
        if (sp.nextEnd < (std::uint64_t)sp.nfft) { sp.nextEnd += hop; continue; }
        // Has the advancing window reached an as-yet-unblanked dropout? If so, fill
        // the dropout + STFT latency as one blank block and resume with the first
        // window that is entirely post-gap (skip the straddling windows, which would
        // mix pre/post-gap samples into garbage).
        bool consumed = false;
        for (const auto& g : gaps) {
            if (g.first <= sp.lastGapInserted) continue;
            const std::uint64_t G = (std::uint64_t)((g.first - t0) / dt + 0.5);  // gap sample
            if (sp.nextEnd >= G) {
                const int nb = std::min(sp.cols, (int)((g.second + latency) / sp.hopSec + 0.5));
                for (int b = 0; b < nb; ++b) writeColumn(true, sp.colNewestTime + sp.hopSec);
                sp.lastGapInserted = g.first;
                sp.nextEnd = G + (std::uint64_t)sp.nfft;   // first fully post-gap window
                consumed = true;
            }
            break;   // gaps are time-ordered; only the earliest unconsumed one matters
        }
        if (consumed) { ++added; continue; }

        const std::uint64_t startAbs = sp.nextEnd - (std::uint64_t)sp.nfft;
        if (head > cap && startAbs < head - cap) { sp.nextEnd = head; break; }
        const float* p = (sp.filtered ? s.ringHp() : s.ring()).windowAt(startAbs);
        sp.psd.compute(p + sp.channel, ch, sp.psdOut);
        for (auto& v : sp.psdOut) v = sp.db ? 10.0f * std::log10(v + 1e-12f) : v;
        writeColumn(false, s.realTime(t0 + (double)sp.nextEnd * dt));
        sp.nextEnd += hop;
        ++added;
    }
}

// ===========================================================================
// ERP / marker-aligned averaging. On each trigger event (a marker from a chosen
// marker stream, optionally label-filtered) we cut an epoch [-pre, +post] ms from
// one channel of a data stream, baseline-correct it (subtract the pre-event mean),
// and fold it into a running average. The window shows the average (bold) over the
// recent individual epochs (faint "spaghetti").
// ===========================================================================
struct Erp {
    std::string wantSid, wantMsid;   // workspace restore: bind to these source_ids once present
    int    streamIdx = 0, channel = 0, markerIdx = 0;
    float  preMs = 100.0f, postMs = 500.0f;
    bool   baseline = true;
    bool   allCh   = false;                     // average ALL channels (up to maxCh) vs the single 'channel'
    int    maxCh   = 32;                         // channel cap when allCh (matches the time-series raster)
    bool   raster  = false;                     // render as a channels x time heatmap (one row per channel)
    float  imgHalf = 50.0f;                       // raster color half-range (from the averages)
    ImGuiTextFilter streamFilter, chanFilter, markerFilter, labelFilter;

    // epoch shape (recomputed on reset)
    int    nbins = 0, pre = 0, nchan = 0;       // nchan = active channel count (1 = single, else allCh set)
    std::vector<int>                chans;      // active channel indices (size nchan)
    std::vector<float>              taxis;      // ms, -pre .. +post
    std::vector<double>            sumv;        // running sum, nchan x nbins row-major (double for accuracy)
    int                            count = 0;   // epochs folded
    std::vector<std::vector<float>> epochs;     // recent SINGLE-channel epochs (capped) for spaghetti
    std::vector<float>             avg;         // scratch: sum/count, nchan x nbins row-major

    std::vector<double>  pending;               // event display-times awaiting post-data
    std::vector<double>  stillScratch;          // erpUpdate: events not yet ready (reused, not per-frame allocated)
    std::uint64_t        lastSeq = 0;
    bool                 seqInit = false;
    std::string          sUid, mUid;            // identity (auto-reset on change)
    float                yHalf = 50.0f;         // robust Y half-range (excludes spike outliers)
    float                yHalfApplied = -1.0f;  // last yHalf forced onto the axis (auto-fit while evolving)
    std::vector<float>   yscratch;              // |sample| buffer for the percentile
    std::vector<double>      tvals;             // raster Y-tick positions (reused each frame)
    std::vector<const char*> tlabs;             // raster Y-tick labels (-> into labels(), per-frame valid)
    int    id    = 0;                           // unique window id (stable ImGui title)
    bool   open  = true;                        // window open flag (closed -> removed)
    bool   focus = true;                        // one-shot raise request
};

static void erpClear(Erp& e) {
    e.sumv.assign((std::size_t)e.nchan * e.nbins, 0.0);
    e.avg.assign((std::size_t)e.nchan * e.nbins, 0.0f);
    e.count = 0;
    e.epochs.clear();
    e.pending.clear();
}

static void erpReset(Erp& e, HfStreamSource& s, const std::string& mUid) {
    const double rate = s.srate(), dt = s.dt();
    e.pre   = std::max(1, (int)(e.preMs  / 1000.0 * rate));
    const int post = std::max(1, (int)(e.postMs / 1000.0 * rate));
    e.nbins = e.pre + post;
    e.taxis.resize(e.nbins);
    for (int i = 0; i < e.nbins; ++i) e.taxis[i] = (float)((double)(i - e.pre) * dt * 1000.0);
    // Active channel set: all channels (capped) when allCh, else just the selected one.
    e.chans.clear();
    if (e.allCh) {
        const int n = std::min(s.channels(), std::max(1, e.maxCh));
        for (int c = 0; c < n; ++c) e.chans.push_back(c);
    } else {
        e.chans.push_back(std::min(e.channel, std::max(0, s.channels() - 1)));
    }
    e.nchan = (int)e.chans.size();
    erpClear(e);
    e.lastSeq = 0; e.seqInit = false;
    e.sUid = s.uid(); e.mUid = mUid;
}

static constexpr int kErpMaxSpaghetti = 60;

// Diverging colormap for signed amplitude (ERP raster): RdBu reversed to blue-white-red
// so POSITIVE = red/warm and NEGATIVE = blue, the neuroimaging convention (MNE, nilearn),
// with white at 0. Built once from the built-in RdBu; used with a plain ascending
// [-half, +half] scale so the heatmap and its colorbar agree (no scale inversion).
static ImPlotColormap divergingPosRed() {
    static ImPlotColormap cm = -1;
    if (cm < 0) {
        const int n = ImPlot::GetColormapSize(ImPlotColormap_RdBu);
        std::vector<ImVec4> cols((std::size_t)n);
        for (int i = 0; i < n; ++i) cols[i] = ImPlot::GetColormapColor(n - 1 - i, ImPlotColormap_RdBu);
        cm = ImPlot::AddColormap("RdBu_r", cols.data(), n, /*qual=*/false);
    }
    return cm;
}

static void erpUpdate(Erp& e, HfStreamSource& s, MarkerSource& mk) {
    // 1) ingest new trigger events (by stable seq) into the pending queue. The first
    // pass after a reset only SYNCS lastSeq (no backfill) so we accumulate from now on.
    const bool firstPass = !e.seqInit;
    for (const auto& ev : mk.cachedEvents()) {   // const-ref to source cache (no per-frame copy)
        if (e.seqInit && ev.seq <= e.lastSeq) continue;
        e.lastSeq = std::max(e.lastSeq, ev.seq);
        if (firstPass) continue;
        if (e.labelFilter.IsActive() && !e.labelFilter.PassFilter(ev.text.c_str())) continue;
        e.pending.push_back(ev.t);
    }
    e.seqInit = true;
    // 2) fold pending events whose full post-window has now arrived
    const double t0 = s.time0(), dt = s.dt(), newest = s.newestTime();
    const std::uint64_t head = s.head(), cap = s.ring().capacity();
    const int C = s.channels();
    const double postSec = e.postMs / 1000.0;
    std::vector<double>& still = e.stillScratch;   // reused buffer (no per-frame heap alloc)
    still.clear();
    bool added = false;
    for (double T : e.pending) {
        if (newest < T + postSec) { still.push_back(T); continue; }      // post data not in yet
        const long long idxEvent = (long long)std::llround((T - t0) / dt);
        const long long start    = idxEvent - e.pre;
        if (start < 0) continue;                                         // before stream start: drop
        if (head > cap && (std::uint64_t)start < head - cap) continue;   // scrolled out of ring: drop
        if ((std::uint64_t)start + (std::uint64_t)e.nbins > head) { still.push_back(T); continue; }
        const float* p = s.ring().windowAt((std::uint64_t)start);
        // Fold this epoch into every active channel's running sum (baseline-corrected
        // per channel). One window read, strided by channel.
        for (int ci = 0; ci < e.nchan; ++ci) {
            const int ch = e.chans[ci];
            double* sv = e.sumv.data() + (std::size_t)ci * e.nbins;
            float b = 0.0f;
            if (e.baseline) {                                           // subtract pre-event mean
                double m = 0.0; for (int i = 0; i < e.pre; ++i) m += p[(std::size_t)i * C + ch];
                b = (float)(m / std::max(1, e.pre));
            }
            for (int i = 0; i < e.nbins; ++i) sv[i] += p[(std::size_t)i * C + ch] - b;
        }
        ++e.count;
        if (e.nchan == 1) {                                             // keep epochs for spaghetti (single channel only)
            const int ch = e.chans[0];
            std::vector<float> ep(e.nbins);
            float b = 0.0f;
            if (e.baseline) { double m = 0.0; for (int i = 0; i < e.pre; ++i) m += p[(std::size_t)i * C + ch];
                              b = (float)(m / std::max(1, e.pre)); }
            for (int i = 0; i < e.nbins; ++i) ep[i] = p[(std::size_t)i * C + ch] - b;
            e.epochs.push_back(std::move(ep));
            if ((int)e.epochs.size() > kErpMaxSpaghetti) e.epochs.erase(e.epochs.begin());
        }
        added = true;
    }
    e.pending.swap(still);

    // Robust ranges, recomputed when an epoch folds in:
    //  - yHalf  (line Y axis): single channel uses the spread of individual epochs so the
    //    spaghetti fits; multi-channel uses the average spread (no spaghetti there).
    //  - imgHalf (raster color): always the average spread (the raster shows averages).
    if (added && e.count > 0) {
        for (std::size_t k = 0; k < e.avg.size(); ++k) e.avg[k] = (float)(e.sumv[k] / e.count);
        // nth_element in place on the (rebuilt-each-call) scratch — no per-fold copy.
        auto pct95 = [](std::vector<float>& absv) {
            if (absv.empty()) return 1.0f;
            const std::size_t k = (std::size_t)(0.95 * (absv.size() - 1));
            std::nth_element(absv.begin(), absv.begin() + k, absv.end());
            return std::max(1e-3f, absv[k] * 1.2f);
        };
        e.yscratch.clear();
        for (float v : e.avg) e.yscratch.push_back(std::fabs(v));
        e.imgHalf = pct95(e.yscratch);
        if (e.nchan == 1 && !e.epochs.empty()) {
            e.yscratch.clear();
            for (const auto& ep : e.epochs) for (float v : ep) e.yscratch.push_back(std::fabs(v));
            e.yHalf = pct95(e.yscratch);
        } else {
            e.yHalf = e.imgHalf;
        }
    }
}

// Persisted in imgui.ini (via the settings handler below): theme + recording path.
static bool g_light    = false;
static char g_recDir[512]  = "";                          // output directory ("" = cwd)
// Default follows LabRecorder's BIDS layout (forward slashes for cross-platform paths;
// the optional acq entity is left out of the default — add `{acq}` to the template if needed).
static char g_recTmpl[512] =
    "sub-{subject}/ses-{session}/{modality}/sub-{subject}_ses-{session}_task-{task}_run-{run}_{modality}.xdf";

static void* SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*) { return (void*)1; }
static void SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    int v; char b[512];
    if      (std::sscanf(line, "light=%d", &v) == 1)        g_light = (v != 0);
    else if (std::sscanf(line, "recdir=%511[^\n]", b) == 1) std::snprintf(g_recDir,  sizeof(g_recDir),  "%s", b);
    else if (std::sscanf(line, "rectmpl=%511[^\n]", b) == 1) std::snprintf(g_recTmpl, sizeof(g_recTmpl), "%s", b);
}
static void SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    buf->appendf("[%s][State]\n", h->TypeName);
    buf->appendf("light=%d\n",   g_light ? 1 : 0);
    buf->appendf("recdir=%s\n",  g_recDir);
    buf->appendf("rectmpl=%s\n", g_recTmpl);
    buf->append("\n");
}
// SDL folder-picker callback → sets the output directory.
static void onFolderPicked(void*, const char* const* list, int) {
    if (list && list[0]) { std::snprintf(g_recDir, sizeof(g_recDir), "%s", list[0]); ImGui::MarkIniSettingsDirty(); }
}

// Rate-limit per-frame work: returns true at most once per `period` seconds,
// advancing `last`. (`last` starts at a large negative so the first call fires.)
static bool dueEvery(double& last, double period) {
    const double now = ImGui::GetTime();
    if (now - last < period) return false;
    last = now;
    return true;
}

int main(int argc, char** argv) {
    spdlog::cfg::load_env_levels();                 // override via SPDLOG_LEVEL=debug, etc.
    spdlog::set_pattern("%H:%M:%S.%e [%^%l%$] %v");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        spdlog::critical("SDL_Init: {}", SDL_GetError());
        return 1;
    }

    const bool gpuDebug    = std::getenv("LSL_GPU_DEBUG")   != nullptr;
    const bool autoConnect = std::getenv("LSL_AUTOCONNECT") != nullptr;
    const bool bench       = std::getenv("LSL_BENCH")       != nullptr;
    const bool profile     = std::getenv("LSL_PROFILE")     != nullptr;
    bool       vsync       = std::getenv("LSL_NOVSYNC")     == nullptr;
    LSL_PROFILE_ENABLE(profile);   // built-in text zone profiler (no-op under Tracy)

    // Initial window size; override with LSL_WINDOW=WxH (used for higher-res capture).
    int winW = 1280, winH = 800;
    if (const char* ws = std::getenv("LSL_WINDOW")) {
        int w = 0, h = 0;
        if (std::sscanf(ws, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { winW = w; winH = h; }
    }
    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (std::getenv("LSL_FULLSCREEN")) wflags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow("LSL Stream Viewer", winW, winH, wflags);
    if (!window) { spdlog::critical("SDL_CreateWindow: {}", SDL_GetError()); return 1; }

    SDL_GPUDevice* gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
        SDL_GPU_SHADERFORMAT_MSL  | SDL_GPU_SHADERFORMAT_METALLIB,
        gpuDebug, nullptr);
    if (!gpu) { spdlog::critical("SDL_CreateGPUDevice: {}", SDL_GetError()); return 1; }

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        spdlog::critical("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
        return 1;
    }
    applyPresentMode(gpu, window, vsync);
    spdlog::info("GPU backend: {}", SDL_GetGPUDeviceDriver(gpu));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // drag windows together to dock

    // Data layout. Default: config/state (imgui.ini + saved workspaces) in the OS app-data dir
    // (AppData / Application Support / ~/.local/share), recordings under ~/Documents. Portable:
    // everything beside the binary instead, for a self-contained folder or USB stick — enabled
    // by LSL_PORTABLE or a `portable.txt` dropped next to the executable / .AppImage.
    std::string prefBase, iniPath, recDefault;
    {
        std::string base;                                  // dir of the .AppImage, else the exe
        if (const char* ai = std::getenv("APPIMAGE")) {
            const std::string p = ai; const auto s = p.find_last_of("/\\");
            base = (s == std::string::npos) ? std::string() : p.substr(0, s + 1);
        } else if (const char* bp = SDL_GetBasePath()) { base = bp; }   // SDL-owned, don't free

        const bool portable = std::getenv("LSL_PORTABLE") != nullptr ||
                              (!base.empty() && std::filesystem::exists(base + "portable.txt"));
        if (portable && !base.empty()) {
            prefBase   = base + "lsl_viewer_data/";
            std::error_code ec; std::filesystem::create_directories(prefBase, ec);
            recDefault = prefBase + "recordings";
        } else {
            if (char* pref = SDL_GetPrefPath("", "lsl_viewer")) { prefBase = pref; SDL_free(pref); }
            if (const char* doc = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS))   recDefault = std::string(doc)  + "lsl-recordings";
            else if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME))  recDefault = std::string(home) + "lsl-recordings";
        }
        if (!prefBase.empty()) { iniPath = prefBase + "imgui.ini"; io.IniFilename = iniPath.c_str(); }
    }

    loadEmbeddedFont(io, window);   // embedded Roboto, HiDPI-crisp (see theme.hpp)
    // Persist theme + recording path in imgui.ini (handler must be added before the
    // ini is auto-loaded on the first NewFrame).
    g_light = (std::getenv("LSL_LIGHT") != nullptr);   // env default; ini overrides on load
    {
        ImGuiSettingsHandler sh;
        sh.TypeName   = "LSLViewer";
        sh.TypeHash   = ImHashStr("LSLViewer");
        sh.ReadOpenFn = SettingsReadOpen;
        sh.ReadLineFn = SettingsReadLine;
        sh.WriteAllFn = SettingsWriteAll;
        ImGui::AddSettingsHandler(&sh);
    }
    // Apply the recordings default (computed above) unless the ini, loaded on the first frame,
    // overrides it; clearing the field in the UI falls back to the working directory.
    if (g_recDir[0] == '\0' && !recDefault.empty())
        std::snprintf(g_recDir, sizeof g_recDir, "%s", recDefault.c_str());
    applyTheme(g_light);
    bool themeApplied = false;   // re-applied once after the ini loads (first NewFrame)

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device            = gpu;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    init_info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

#ifdef LSL_TESTS
    const bool runTests = (argc > 1 && std::strcmp(argv[1], "--tests") == 0);
    // Optional filter: `--tests <query>` runs only matching tests (e.g. "ui/capture_*").
    const char* testFilter = (runTests && argc > 2) ? argv[2] : nullptr;
    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
    tio.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Info;
    tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    tio.ConfigLogToTTY            = true;
    tio.ConfigRunSpeed            = runTests ? ImGuiTestRunSpeed_Fast : ImGuiTestRunSpeed_Normal;
    tio.ConfigNoThrottle          = runTests;
    tio.ConfigWatchdogKillTest    = 180.0f;   // capture tests sleep to fill plots / accumulate ERPs
    // Video/GIF capture (CaptureBeginVideo/EndVideo) via ffmpeg: the encoder path + params are
    // applied from LSL_FFMPEG once after the first frame (the settings load clobbers pre-Start
    // values), so they live in the render loop below rather than here.
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    ImGuiTestEngine_InstallDefaultCrashHandler();
    RegisterAppTests(engine);
    if (runTests)
        ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, testFilter,
                                   ImGuiTestRunFlags_RunFromCommandLine);

    const SDL_GPUTextureFormat swapFmt = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    SDL_GPUTexture* offscreen = nullptr;
    int offW = 0, offH = 0;
    CaptureCtx capCtx;
    capCtx.gpu  = gpu;
    capCtx.bgra = (swapFmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                   swapFmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);
    tio.ScreenCaptureFunc     = captureFunc;
    tio.ScreenCaptureUserData = &capCtx;
#else
    (void)argc; (void)argv;
#endif

    Discovery discovery;   // resolves streams continuously in the background
    MockStreams mockStreams;   // built-in demo: publishes synthetic streams on loopback
    if (std::getenv("LSL_DEMO")) mockStreams.start();   // auto-emit on launch (also via the UI toggle)
    std::vector<std::unique_ptr<HfStreamSource>> sources;
    std::vector<std::unique_ptr<HfStreamSource>> closing;  // disconnected, awaiting worker exit
    std::vector<std::unique_ptr<MarkerSource>>   markerSources;  // string/event streams
    std::vector<std::unique_ptr<MarkerSource>>   closingMrk;     // disconnected, awaiting worker exit
    std::unordered_set<std::string>              dismissed;      // user-disconnected (don't auto-reconnect)
    Recorder recorder;                                           // XDF recording
    // Which streams to record (keys); nullopt = all, empty set = none. Default none so
    // we never start recording every stream on the network by accident.
    RemoteControl remote;                                        // TCP control server
    RemoteState   rcState;
    int           rcPort = 22345;
    if (const char* p = std::getenv("LSL_RC_PORT")) {            // auto-start from env
        rcPort = std::atoi(p);
        if (remote.start(rcPort, &rcState)) spdlog::info("remote control listening on tcp:{}", rcPort);
    }
    RecVars recVars;                                     // g_recTmpl/g_recDir are file-scope (persisted)
    // BIDS entity defaults so the default template yields a valid name out of the box; the
    // user overrides them in the Recording panel.
    char    recSubject[64] = "P001", recSession[64] = "S001", recTask[64] = "Default",
            recRun[64] = "001", recAcq[64] = "", recModality[64] = "eeg";
    // Full output path: resolve the template, then prefix the directory (unless the
    // template is already absolute).
    auto recFullPath = [&]() -> std::string {
        std::string fn = resolveTemplate(g_recTmpl,
            RecVars{recSubject, recSession, recTask, recRun, recAcq, recModality});
        const bool abs = !fn.empty() &&
                         (fn[0] == '/' || fn[0] == '\\' || (fn.size() > 1 && fn[1] == ':'));
        if (!abs && g_recDir[0]) {
            std::string d = g_recDir;
            if (d.back() != '/' && d.back() != '\\') d += '/';
            fn = d + fn;
        }
        // Normalize to the OS-native separator (backslashes on Windows) — the template and the
        // dir join above use '/', which is valid but looks foreign in the preview / saved file.
        return std::filesystem::path(fn).make_preferred().string();
    };
    PlotScratch scratch;

    std::unordered_map<const HfStreamSource*, DisplayOpts> dispOpts;  // per-stream settings
    // Per-stream settings restored from a workspace, keyed by source_id; consumed (applied
    // + erased) the first time that stream's dispOpts is touched, so it works whether the
    // stream is already connected at load time or reconnects later.
    std::unordered_map<std::string, DisplayOpts>          savedOpts;
    std::unordered_map<const HfStreamSource*, double>      edgeMap;   // smoothed X right-edge
    std::unordered_map<const HfStreamSource*, std::uint64_t> pauseHead; // frozen head at pause
    bool showPerf     = false;   // Performance overlay (View menu; hidden by default)
    bool showSpectrum = true;    // Spectrum window (View menu)
    // Spectrogram and ERP are multi-instance: "New ..." in the View menu adds a window;
    // closing one removes it. Heatmaps/averages don't overlay, so separate windows let
    // you compare channels/streams/conditions side by side.
    std::vector<std::unique_ptr<Spectro>> spectros;
    std::vector<std::unique_ptr<Erp>>     erps;
    int nextSpectroId = 1, nextErpId = 1;
    bool showMetrics  = false;   // ImGui metrics/debugger (Debug menu) — vertex counts, draw calls
    // one-shot "raise this window to the front" requests, set from the menu
    bool focusSpectrum = false, focusMetrics = false;
    // one-shot: a new analysis window was just opened — ensure a bottom dock row
    // exists for it (ImGui prunes the row once its last window closes).
    bool wantBottom = false;
    bool paused       = false;   // freeze the scrolling plots (App menu / P)
    bool pausedPrev   = false;

    // FFT / PSD view state.
    Psd                        psd;
    int                        fftStream = 0;
    std::string                fftWantSid;   // workspace restore: bind to this source_id once present
    std::vector<int>           fftWantSel;   // workspace restore: selected-channel indices, applied on bind
    int                        fftN = 512;   // small default = fast fill / immediate feedback (bump for finer Hz resolution)
    bool                       fftDb = true;
    bool                       fftFiltered = false;  // apply the stream's display filter chain
    bool                       fftRefit = true;   // force-fit axes after a mode change
    bool                       fftFitHz = false;  // pending request: zoom X to significant energy
    std::string                fftUid;
    std::vector<unsigned char> fftSel;
    ImGuiTextFilter            fftFilter;
    int                        psdN = 0;
    float                      psdFs = 0.0f;
    std::vector<float>         psdFreq, psdOut;
    std::vector<float>         fftCache;          // [channel*bins] last computed spectra
    double                     fftLastCompute = -1e18;  // wall-clock of last recompute (throttle)
    bool                       fftPausedPrev  = false;  // detect pause entry (recompute once on the frozen window)

    FrameStats fps;
    bool   curVsync = vsync;
    double uiMs = 0.0, gpuMs = 0.0;
    double benchAccum = 0.0;
    double profAccum  = 0.0;     // text-profiler dump cadence
    Uint64 tPrev = SDL_GetPerformanceCounter();

    // ---- Workspaces: named snapshots of the whole view (per-stream settings, analysis
    // windows, and the dock layout). Streams are referenced by source_id so a workspace
    // re-applies itself when the same streams reconnect. Stored as ./workspaces/<name>.lslws.
    const std::filesystem::path wsDir =
        prefBase.empty() ? std::filesystem::path("workspaces")
                         : std::filesystem::path(prefBase) / "workspaces";
    char        wsNameBuf[128] = "";
    std::string pendingWs;            // a load deferred to end-of-frame (safe ImGui ini point)
    // Streams a loaded workspace needs (source_id, name): auto-connected as they appear and
    // listed in a "waiting for…" notice until present. Cleared on the next load or dismiss.
    std::vector<std::pair<std::string, std::string>> wsRequired;
    std::unordered_set<std::string>                  wsConnectedOnce;   // don't re-fight a manual disconnect

    auto wsSerialize = [&]() -> std::string {
        std::string s = "# lsl-sdl workspace v1\n";
        // "R" lines list the streams this workspace needs (source_id + name) so loading it can
        // re-connect them and report any that aren't on the network. Deduplicated.
        std::unordered_set<std::string> reqSeen;
        auto require = [&](const std::string& sid, const std::string& name) {
            if (reqSeen.insert(sid).second) s += "R\t" + sid + "\t" + name + "\n";
        };
        for (auto& src : sources) {
            require(streamKeyOf(*src), src->name());
            auto it = dispOpts.find(src.get());
            if (it != dispOpts.end())
                s += "S\t" + streamKeyOf(*src) + "\t" + serializeOpts(it->second) + "\n";
        }
        // Every connected marker stream is "required" too — recording captures all connected
        // streams, so restoring the workspace must re-connect them even if no ERP/overlay uses one.
        for (auto& mk : markerSources)
            require(streamKeyOf(*mk), mk->name());
        if (showSpectrum && fftStream < (int)sources.size()) {
            std::string sel;
            for (std::size_t c = 0; c < fftSel.size(); ++c) if (fftSel[c]) sel += std::to_string(c) + ",";
            char b[96]; std::snprintf(b, sizeof b, "n=%d db=%d filt=%d vis=", fftN, fftDb ? 1 : 0, fftFiltered ? 1 : 0);
            s += "F\t" + streamKeyOf(*sources[fftStream]) + "\t" + b + sel + "\n";
        }
        for (auto& sp : spectros) {
            if (sp->streamIdx >= (int)sources.size()) continue;
            char b[176]; std::snprintf(b, sizeof b, "ch=%d nfft=%d span=%.1f fmin=%.2f fmax=%.2f db=%d filt=%d",
                sp->channel, sp->nfft, sp->spanSec, sp->fMin, sp->fMax, sp->db ? 1 : 0, sp->filtered ? 1 : 0);
            s += "G\t" + streamKeyOf(*sources[sp->streamIdx]) + "\t" + b + "\n";
        }
        for (auto& ep : erps) {
            if (ep->streamIdx >= (int)sources.size() || ep->markerIdx >= (int)markerSources.size()) continue;
            char b[176]; std::snprintf(b, sizeof b, "ch=%d all=%d maxc=%d pre=%.0f post=%.0f base=%d ras=%d",
                ep->channel, ep->allCh ? 1 : 0, ep->maxCh, ep->preMs, ep->postMs, ep->baseline ? 1 : 0, ep->raster ? 1 : 0);
            s += "E\t" + streamKeyOf(*sources[ep->streamIdx]) + "\t" + streamKeyOf(*markerSources[ep->markerIdx])
               + "\t" + b + "\t" + ep->labelFilter.InputBuf + "\n";
        }
        s += "---IMGUI---\n";
        s += ImGui::SaveIniSettingsToMemory();
        return s;
    };

    auto wsApply = [&](const std::string& blob) {
        const std::string mark = "\n---IMGUI---\n";
        const std::size_t sep = blob.find(mark);
        const std::string app = blob.substr(0, sep);
        const std::string ini = (sep == std::string::npos) ? std::string() : blob.substr(sep + mark.size());
        // Rebuild analysis windows + pending per-stream settings from the workspace.
        spectros.clear(); erps.clear(); savedOpts.clear();
        wsRequired.clear(); wsConnectedOnce.clear();
        nextSpectroId = nextErpId = 1;
        auto split = [](const std::string& l) {              // tab-delimited fields
            std::vector<std::string> f; std::string cur; std::istringstream is(l);
            while (std::getline(is, cur, '\t')) f.push_back(cur);
            return f;
        };
        std::istringstream is(app); std::string line;
        while (std::getline(is, line)) {
            if (line.empty() || line[0] == '#') continue;
            const auto f = split(line);
            if (f[0] == "R" && f.size() >= 3) {
                wsRequired.emplace_back(f[1], f[2]);   // (source_id, name) to re-connect / report
            } else if (f[0] == "S" && f.size() >= 3) {
                savedOpts[f[1]] = parseOpts(f[2]);
            } else if (f[0] == "F" && f.size() >= 3) {
                showSpectrum = true;
                fftWantSid = f[1];
                fftN = (int)wsField(f[2], "n", fftN);
                fftDb = wsField(f[2], "db", 1) != 0;
                fftFiltered = wsField(f[2], "filt", 0) != 0;
                fftWantSel.clear();
                if (std::size_t vp = f[2].find("vis="); vp != std::string::npos)
                    for (const char* c = f[2].c_str() + vp + 4; *c && *c != ' '; ) {
                        fftWantSel.push_back(std::atoi(c));
                        while (*c && *c != ',' && *c != ' ') ++c; if (*c == ',') ++c;
                    }
            } else if (f[0] == "G" && f.size() >= 3) {
                auto sp = std::make_unique<Spectro>();
                sp->id = nextSpectroId++; sp->wantSid = f[1];
                sp->channel = (int)wsField(f[2], "ch", 0);
                sp->nfft    = (int)wsField(f[2], "nfft", sp->nfft);
                sp->spanSec = (float)wsField(f[2], "span", sp->spanSec);
                sp->fMin    = (float)wsField(f[2], "fmin", 0);
                sp->fMax    = (float)wsField(f[2], "fmax", 0);
                sp->db      = wsField(f[2], "db", 1) != 0;
                sp->filtered = wsField(f[2], "filt", 0) != 0;
                spectros.push_back(std::move(sp));
            } else if (f[0] == "E" && f.size() >= 4) {
                auto ep = std::make_unique<Erp>();
                ep->id = nextErpId++; ep->wantSid = f[1]; ep->wantMsid = f[2];
                ep->channel  = (int)wsField(f[3], "ch", 0);
                ep->allCh    = wsField(f[3], "all", 0) != 0;
                ep->maxCh    = (int)wsField(f[3], "maxc", ep->maxCh);
                ep->preMs    = (float)wsField(f[3], "pre", ep->preMs);
                ep->postMs   = (float)wsField(f[3], "post", ep->postMs);
                ep->baseline = wsField(f[3], "base", 1) != 0;
                ep->raster   = wsField(f[3], "ras", 0) != 0;
                if (f.size() >= 5) { std::snprintf(ep->labelFilter.InputBuf, sizeof ep->labelFilter.InputBuf, "%s", f[4].c_str()); ep->labelFilter.Build(); }
                erps.push_back(std::move(ep));
            }
        }
        if (!ini.empty()) ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    };

    auto wsList = [&]() -> std::vector<std::string> {
        std::vector<std::string> names; std::error_code ec;
        if (std::filesystem::exists(wsDir, ec))
            for (auto& de : std::filesystem::directory_iterator(wsDir, ec))
                if (de.path().extension() == ".lslws") names.push_back(de.path().stem().string());
        std::sort(names.begin(), names.end());
        return names;
    };
    auto wsSave = [&](const std::string& name) {
        std::error_code ec; std::filesystem::create_directories(wsDir, ec);
        std::ofstream(wsDir / (name + ".lslws")) << wsSerialize();
    };
    auto wsLoad = [&](const std::string& name) {
        std::ifstream f(wsDir / (name + ".lslws"));
        if (f) { std::ostringstream ss; ss << f.rdbuf(); pendingWs = ss.str(); }   // applied end-of-frame
    };

    bool done = false;
    unsigned long frameCounter = 0;
    while (!done) {
        ++frameCounter;
        Uint64 tNow = SDL_GetPerformanceCounter();
        const float frameMs = (float)(pcSeconds(tPrev, tNow) * 1000.0);
        tPrev = tNow;
        fps.push(frameMs);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) done = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                e.window.windowID == SDL_GetWindowID(window))
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        if (vsync != curVsync) { applyPresentMode(gpu, window, vsync); curVsync = vsync; }

        const Uint64 tUi0 = SDL_GetPerformanceCounter();
        {
            LSL_ZONE("ui build");
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (!themeApplied) { themeApplied = true; applyTheme(g_light); }  // after ini load

            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("App")) {
                    ImGui::MenuItem("Pause", "P", &paused);
                    if (ImGui::MenuItem("Light theme", nullptr, &g_light)) {
                        applyTheme(g_light); ImGui::MarkIniSettingsDirty();
                    }
                    if (ImGui::MenuItem("Quit", "Ctrl+Q")) done = true;
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    // These toggle visibility, but a click also raises the window so it
                    // can't stay buried behind a stream plot.
                    if (ImGui::MenuItem("Spectrum", nullptr, showSpectrum))    { showSpectrum = true; focusSpectrum = true; wantBottom = true; }
                    if (ImGui::MenuItem("New spectrogram")) {
                        auto s = std::make_unique<Spectro>(); s->id = nextSpectroId++; spectros.push_back(std::move(s));
                        wantBottom = true;
                    }
                    if (ImGui::MenuItem("New ERP (marker average)")) {
                        auto e = std::make_unique<Erp>(); e->id = nextErpId++; erps.push_back(std::move(e));
                        wantBottom = true;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Workspaces")) {
                    ImGui::TextDisabled("save the current view (per-stream settings,");
                    ImGui::TextDisabled("analysis windows, layout) under a name");
                    ImGui::SetNextItemWidth(160);
                    ImGui::InputTextWithHint("##wsname", "name", wsNameBuf, sizeof wsNameBuf);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(wsNameBuf[0] == '\0');
                    if (ImGui::Button("Save")) { wsSave(wsNameBuf); wsNameBuf[0] = '\0'; ImGui::CloseCurrentPopup(); }
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    const auto names = wsList();
                    if (names.empty()) ImGui::TextDisabled("(no saved workspaces)");
                    // Each row: click the name to load (closes the menu); click the × to delete
                    // in place (keeps the menu open so several can be pruned). The name column is
                    // sized to the widest entry so the × buttons line up.
                    float nameW = 0.0f;
                    for (const auto& n : names) nameW = std::max(nameW, ImGui::CalcTextSize(n.c_str()).x);
                    for (const auto& n : names) {
                        ImGui::PushID(n.c_str());
                        if (ImGui::Selectable(n.c_str(), false, ImGuiSelectableFlags_None, ImVec2(nameW, 0.0f))) {
                            wsLoad(n); ImGui::CloseCurrentPopup();   // applied next frame
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("\xc3\x97")) {        // × — delete this workspace
                            std::error_code ec; std::filesystem::remove(wsDir / (n + ".lslws"), ec);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete \"%s\"", n.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Debug")) {
                    ImGui::MenuItem("Performance", nullptr, &showPerf);   // a section in the Streams rail
                    if (ImGui::MenuItem("Metrics (ImGui)", nullptr, showMetrics)) { showMetrics = true; focusMetrics = true; wantBottom = true; }
                    // Built-in demo: publish a synthetic EEG / chirp / audio / evoked set on loopback
                    // (auto-connected below), so every view can be explored with no external source.
                    const bool demo = mockStreams.running();
                    if (ImGui::MenuItem("Emit demo streams", nullptr, demo)) {
                        if (demo) { mockStreams.stop();  spdlog::info("demo streams stopped"); }
                        else      { mockStreams.start(); spdlog::info("demo streams started"); }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Synthetic EEG (+EOG), a 1->120 Hz chirp, a 48 kHz stereo\n"
                                          "tone, and an evoked-response stream with markers.");
                    ImGui::Separator();
                    ImGui::TextDisabled("GPU backend: %s", SDL_GetGPUDeviceDriver(gpu));
                    ImGui::EndMenu();
                }
                if (recorder.active()) {     // always-visible recording indicator (right side)
                    char rb[64];
                    std::snprintf(rb, sizeof(rb), "[REC %.0fs  %.1f MB]",
                                  recorder.seconds(), recorder.bytes() / 1e6);
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(rb).x - 12.0f);
                    const bool on = ((int)(recorder.seconds() * 2)) % 2 == 0;   // blink
                    ImGui::TextColored(on ? ImVec4(1, 0.30f, 0.30f, 1) : ImVec4(0.65f, 0.18f, 0.18f, 1),
                                       "%s", rb);
                }
                ImGui::EndMainMenuBar();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q)) done = true;
            if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_P)) paused = !paused;

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            // discovery.snapshot() deep-copies every resolved stream_info (XML included);
            // the set changes slowly, so refresh at ~4 Hz and reuse the cache otherwise.
            static std::vector<lsl::stream_info> found;
            static double lastDiscovery = -1e18;
            if (dueEvery(lastDiscovery, 0.25)) {
                found = discovery.snapshot();
                found.erase(std::remove_if(found.begin(), found.end(),   // hide our own RC beacon
                    [](lsl::stream_info& i) { return i.source_id().rfind("lsl-viewer-rc", 0) == 0; }),
                    found.end());
            }
            // Connect / disconnect a stream — ONE implementation, shared by autoconnect,
            // the row-click handler, the window-close (X), and the remote `select` command.
            auto connected = [&](lsl::stream_info& info) {
                for (auto& s : sources)       if (sameStream(*s, info)) return true;
                for (auto& m : markerSources) if (sameStream(*m, info)) return true;
                return false;
            };
            auto connectStream = [&](lsl::stream_info& info) {
                dismissed.erase(streamKey(info));
                if (isMarkerStream(info)) {
                    auto m = std::make_unique<MarkerSource>(info); m->start();
                    markerSources.push_back(std::move(m));
                } else {
                    const auto t = std::chrono::steady_clock::now();
                    auto s = std::make_unique<HfStreamSource>(info, 10.0); s->start();
                    sources.push_back(std::move(s));
                    spdlog::debug("connect '{}' {:.2f} ms", info.name(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
                }
            };
            auto disconnectKey = [&](const std::string& key) {
                dismissed.insert(key);   // and don't auto-reconnect
                auto it = std::find_if(sources.begin(), sources.end(),
                    [&](const std::unique_ptr<HfStreamSource>& p) { return streamKeyOf(*p) == key; });
                if (it != sources.end()) {   // data stream: deferred reap (markers reaped by the dismissed-sweep)
                    edgeMap.erase(it->get()); pauseHead.erase(it->get()); dispOpts.erase(it->get());
                    (*it)->requestStop(); closing.push_back(std::move(*it)); sources.erase(it);
                }
            };
            if (autoConnect)
                for (auto& info : found)
                    if (!dismissed.count(streamKey(info)) && !connected(info))
                        connectStream(info);

            // Built-in demo: while it's emitting, auto-connect its own streams (source_id
            // "mock-*") so one click both publishes and shows them — but still honor a manual
            // disconnect (dismissed), so a row's X stays closed without fighting it.
            if (mockStreams.running())
                for (auto& info : found)
                    if (info.source_id().rfind("mock-", 0) == 0 &&
                        !dismissed.count(streamKey(info)) && !connected(info))
                        connectStream(info);

            // Workspace restore: connect each required stream the FIRST time it appears (once,
            // so a later manual disconnect isn't fought). connectStream() clears `dismissed`.
            if (!wsRequired.empty())
                for (auto& info : found) {
                    const std::string k = streamKey(info);
                    for (auto& req : wsRequired)
                        if (req.first == k && !wsConnectedOnce.count(k) && !connected(info)) {
                            connectStream(info); wsConnectedOnce.insert(k);
                        }
                }

            // Snapshot each connected marker stream once per frame (only events within
            // the widest plot window, so a long recording stays cheap). Each gets a
            // distinct color; plots pick which streams to overlay.
            std::vector<MarkerStreamView> markerViews;
            for (std::size_t i = 0; i < markerSources.size(); ++i) {
                MarkerStreamView v;
                v.id   = !markerSources[i]->sourceId().empty() ? markerSources[i]->sourceId()
                                                               : markerSources[i]->uid();
                v.name = markerSources[i]->name();
                v.col  = ImGui::GetColorU32(ImPlot::SampleColormap(
                    (markerSources.size() > 1 ? (float)i / (markerSources.size() - 1) : 0.0f),
                    ImPlotColormap_Pastel));
                v.events = &markerSources[i]->cachedEvents();   // pointer to source cache (no copy)
                markerViews.push_back(std::move(v));
            }

            // Recording captures exactly the streams the viewer is connected to (a data
            // window or a marker overlay) — "what you're viewing is what you record".
            auto startRecording = [&]() {
                std::vector<lsl::stream_info> rec;
                for (auto& info : found) if (connected(info)) rec.push_back(info);
                const std::string path = recFullPath();
                if (recorder.start(path, rec))
                    spdlog::info("recording {} stream(s) -> {}", rec.size(), path);
                else
                    spdlog::error("failed to start recording: {}", recorder.error());
            };

            // ---- Remote control: publish state, apply requests ----------
            if (remote.listening()) {
                std::lock_guard<std::mutex> lk(rcState.mtx);
                // Publish state at ~4 Hz (clients poll; rebuilding these strings every
                // frame is the costly part). Requests below are still applied per frame.
                static double lastPublish = -1e18;
                if (dueEvery(lastPublish, 0.25)) {
                    std::string s;
                    for (auto& info : found) {
                        char ln[320];
                        const double sr = info.nominal_srate();
                        std::snprintf(ln, sizeof(ln), "%s | %s | %s | %dch | %s%s\n",
                                      streamKey(info).c_str(), info.name().c_str(), info.type().c_str(),
                                      info.channel_count(),
                                      sr > 0 ? std::to_string((int)sr).c_str() : "irregular",
                                      connected(info) ? "  [rec]" : "");
                        s += ln;
                    }
                    rcState.streamsText = s;
                    const std::string fnow = recorder.active() ? recorder.path() : recFullPath();
                    char st[420];
                    std::snprintf(st, sizeof(st),
                                  "recording=%s file=%s seconds=%.1f streams=%d bytes=%llu",
                                  recorder.active() ? "true" : "false", fnow.c_str(), recorder.seconds(),
                                  recorder.streams(), (unsigned long long)recorder.bytes());
                    rcState.statusText = st;
                    rcState.recording  = recorder.active();
                    // Expose the last recording for `get` only once it's fully flushed/closed.
                    rcState.lastFile = (!recorder.active() && recorder.fileFlushed() && !recorder.path().empty())
                                       ? recorder.path() : std::string();
                    rcState.selectedText = [&]{
                        std::string j;
                        for (auto& info : found) if (connected(info)) j += streamKey(info) + " ";
                        return j.empty() ? std::string("none") : j;
                    }();
                }
                if (rcState.setFilename) {
                    std::snprintf(g_recTmpl, sizeof(g_recTmpl), "%s", rcState.setFilename->c_str());
                    ImGui::MarkIniSettingsDirty();
                    rcState.setFilename.reset();
                }
                for (auto& [k, val] : rcState.setVars) {
                    char* dst = (k == "subject")  ? recSubject : (k == "session")  ? recSession
                              : (k == "task")     ? recTask    : (k == "run")      ? recRun
                              : (k == "acq")      ? recAcq     : (k == "modality") ? recModality : nullptr;
                    if (dst) std::snprintf(dst, 64, "%s", val.c_str());
                }
                rcState.setVars.clear();
                // `select` now drives CONNECTIONS (recording captures all connected):
                // "*" = connect all discovered, empty = disconnect all, else make the
                // connected set exactly the given keys.
                if (rcState.setSelection) {
                    const auto& sel = *rcState.setSelection;
                    const bool all = (sel.size() == 1 && sel[0] == "*");
                    const std::set<std::string> want(sel.begin(), sel.end());
                    for (auto& info : found) {
                        const std::string key = streamKey(info);
                        const bool shouldConn = all || want.count(key) != 0;
                        const bool isConn = connected(info);
                        if (shouldConn && !isConn)      connectStream(info);
                        else if (!shouldConn && isConn) disconnectKey(key);
                    }
                    rcState.setSelection.reset();
                }
                if (rcState.stopReq)  { rcState.stopReq = false; if (recorder.active()) { recorder.stop(); spdlog::info("recording stopped (remote)"); } }
                if (rcState.startReq) { rcState.startReq = false; if (!recorder.active()) startRecording(); }
            }

            // ---- Two-column layout: fixed Streams sidebar + a dockspace -----
            // The plots live in a dockspace that fills the area right of the sidebar,
            // so they open as tabs and can be split/rearranged but never bury the rail.
            const float sidebarW = 280.0f;
            ImGuiID dockId = 0, dockCenter = 0, dockBottom = 0;   // time-series center, analysis bottom
            {
                ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + sidebarW, vp->WorkPos.y), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - sidebarW, vp->WorkSize.y), ImGuiCond_Always);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::Begin("##dockhost", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                    ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking);
                ImGui::PopStyleVar();
                dockId = ImGui::GetID("MainDockSpace");
                // Build the default layout ONCE, and only if there's no saved one (so we
                // never clobber a user's arrangement): central = time series, a bottom
                // row for analysis. The stable-named windows are pre-docked there.
                static bool layoutInit = false;
                if (!layoutInit) {
                    layoutInit = true;
                    if (ImGui::DockBuilderGetNode(dockId) == nullptr) {
                        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                        ImGui::DockBuilderSetNodeSize(dockId, ImVec2(vp->WorkSize.x - sidebarW, vp->WorkSize.y));
                        ImGuiID top = dockId;
                        ImGuiID bottom = ImGui::DockBuilderSplitNode(top, ImGuiDir_Down, 0.32f, nullptr, &top);
                        ImGui::DockBuilderDockWindow("Spectrum", bottom);
                        ImGui::DockBuilderFinish(dockId);
                    }
                }
                // Resolve the central + bottom node ids (works across runs): the
                // central node is always queryable; the bottom analysis row is its
                // sibling. ImGui prunes the bottom node once its last window closes,
                // so when a NEW analysis window is being opened with no row present,
                // re-split one off the central node — otherwise it falls back to the
                // central area and the window opens up top with the time series.
                auto resolveNodes = [&]() {
                    dockCenter = dockBottom = 0;
                    if (ImGuiDockNode* c = ImGui::DockBuilderGetCentralNode(dockId)) {
                        dockCenter = c->ID;
                        if (ImGuiDockNode* p = c->ParentNode) {
                            ImGuiDockNode* o = (p->ChildNodes[0] == c) ? p->ChildNodes[1] : p->ChildNodes[0];
                            if (o) dockBottom = o->ID;
                        }
                    }
                };
                resolveNodes();
                if (wantBottom && dockBottom == 0 && dockCenter != 0) {
                    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.32f,
                                                &dockBottom, &dockCenter);
                    ImGui::DockBuilderFinish(dockId);
                }
                wantBottom = false;
                if (dockCenter == 0) dockCenter = dockId;
                if (dockBottom == 0) dockBottom = dockId;
                ImGui::DockSpace(dockId);
                ImGui::End();
            }
            // Streams: a fixed left rail, deliberately NOT part of the docking system.
            ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(sidebarW, vp->WorkSize.y), ImGuiCond_Always);
            ImGui::Begin("Streams", nullptr,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
            ImGuiWindow* streamsWin = ImGui::GetCurrentWindow();  // kept on top below
            // Slightly larger section headers for the sidebar so the Recording/Streams/
            // Performance groups stand out (SetWindowFontScale is per-window, reset after).
            auto sectionHeader = [](const char* label) {
                ImGui::SetWindowFontScale(1.2f);
                ImGui::SeparatorText(label);
                ImGui::SetWindowFontScale(1.0f);
            };
            ImGui::TextDisabled("live \xc2\xb7 %d stream%s on the network",
                                (int)found.size(), found.size() == 1 ? "" : "s");

            // ---- Workspace restore: a loaded workspace auto-connects its streams; list any
            // that aren't on the network yet (clears itself once they all arrive). While some
            // are still missing, recording is held (below) so you don't capture a session that's
            // silently short a stream — until they connect or you dismiss the notice.
            int wsMissing = 0;
            if (!wsRequired.empty()) {
                auto connKey = [&](const std::string& key) {
                    for (auto& s : sources)       if (streamKeyOf(*s) == key) return true;
                    for (auto& m : markerSources) if (streamKeyOf(*m) == key) return true;
                    return false;
                };
                std::string miss;
                for (auto& req : wsRequired)
                    if (!connKey(req.first)) { if (wsMissing++) miss += ", "; miss += req.second; }
                if (wsMissing == 0) {
                    wsRequired.clear(); wsConnectedOnce.clear();   // all present -> stop tracking
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.20f, 1.0f));
                    ImGui::TextWrapped("\xe2\x9a\xa0 workspace: waiting for %d stream%s \xe2\x80\x94 %s",
                                       wsMissing, wsMissing == 1 ? "" : "s", miss.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::SmallButton("Dismiss##wswait")) { wsRequired.clear(); wsConnectedOnce.clear(); }
                }
            }

            // ---- Recording: pinned at the top so it has a stable location (always
            // visible, never shifts with the stream count). Records every connected
            // stream — no per-stream selection.
            sectionHeader("Recording");
            {
                int nConn = 0; for (auto& info : found) if (connected(info)) ++nConn;
                if (!recorder.active()) {
                    const float bw = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2;
                    ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
                    if (ImGui::InputTextWithHint("##recdir", "folder (blank = cwd)", g_recDir, sizeof(g_recDir)))
                        ImGui::MarkIniSettingsDirty();
                    ImGui::SameLine();
                    if (ImGui::Button("Browse"))
                        SDL_ShowOpenFolderDialog(onFolderPicked, nullptr, window,
                                                 g_recDir[0] ? g_recDir : nullptr, false);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::InputTextWithHint("##recpath", "sub-{subject}/…/{modality}.xdf",
                                                 g_recTmpl, sizeof(g_recTmpl)))
                        ImGui::MarkIniSettingsDirty();
                    if (ImGui::TreeNodeEx("filename fields", ImGuiTreeNodeFlags_SpanAvailWidth)) {
                        const float fw = (ImGui::GetContentRegionAvail().x
                                          - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                        ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##sub", "{subject}", recSubject, sizeof(recSubject));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##ses", "{session}", recSession, sizeof(recSession));
                        ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##task", "{task}", recTask, sizeof(recTask));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##run", "{run}", recRun, sizeof(recRun));
                        ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##mod", "{modality}", recModality, sizeof(recModality));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##acq", "{acq} (optional)", recAcq, sizeof(recAcq));
                        ImGui::TreePop();
                    }
                    // Preview path: recFullPath() does strftime + template substitution;
                    // refresh it at ~4 Hz instead of every frame (it's just a label).
                    static std::string recPreview;
                    static double recPreviewAt = -1e18;
                    if (dueEvery(recPreviewAt, 0.25)) recPreview = recFullPath();
                    ImGui::TextDisabled("-> %s", recPreview.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(recPreview.c_str());
                    // Hold recording while a restored workspace is still missing streams, so a
                    // session can't quietly start short a stream (recording = every connected
                    // stream). Resolved by the streams connecting or dismissing the notice above.
                    ImGui::BeginDisabled(nConn == 0 || wsMissing > 0);
                    if (ImGui::Button("Record", ImVec2(-1, 0))) startRecording();
                    ImGui::EndDisabled();
                    // Tooltip on the disabled button explaining how to proceed (disabled items
                    // don't hover by default — AllowWhenDisabled re-enables it).
                    if ((nConn == 0 || wsMissing > 0) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(wsMissing > 0
                            ? "Recording is held until your workspace's streams are present.\n"
                              "Wait for the missing stream(s) to connect, or click Dismiss in the\n"
                              "notice above to record without them."
                            : "Connect at least one stream to record.");
                    if (nConn == 0) {
                        ImGui::TextDisabled("Connect a stream to record.");
                    } else if (wsMissing > 0) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        ImGui::TextWrapped("recording held: %d workspace stream%s missing (connect them or dismiss above)",
                                           wsMissing, wsMissing == 1 ? "" : "s");
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::TextDisabled("records %d connected stream%s", nConn, nConn == 1 ? "" : "s");
                    }
                } else {
                    if (ImGui::Button("Stop recording", ImVec2(-1, 0))) {
                        recorder.stop(); spdlog::info("recording stopped ({:.1f}s, {:.1f} MB)",
                                                      recorder.seconds(), recorder.bytes() / 1e6);
                    }
                    ImGui::TextColored(ImVec4(1, 0.30f, 0.30f, 1),   // hard to miss while live
                                       "REC %.0fs \xc2\xb7 %d stream%s \xc2\xb7 %.2f MB",
                                       recorder.seconds(), recorder.streams(),
                                       recorder.streams() == 1 ? "" : "s", recorder.bytes() / 1e6);
                }
                const std::string rerr = recorder.error();
                if (!rerr.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "rec error: %s", rerr.c_str());
                bool rc = remote.listening();
                if (ImGui::Checkbox("Remote control", &rc)) {
                    if (rc) {
                        if (remote.start(rcPort, &rcState)) spdlog::info("remote control listening on tcp:{}", rcPort);
                        else spdlog::warn("remote control unavailable: {}", remote.error());  // e.g. Windows
                    } else { remote.stop(); spdlog::info("remote control stopped"); }
                }
                if (remote.listening()) { ImGui::SameLine(); ImGui::TextDisabled("tcp :%d", remote.port()); }
                else {
                    // Wrap on its own line — the rail is too narrow for a SameLine note,
                    // which clipped messages like "not supported on Windows yet".
                    const std::string re = remote.error();
                    if (!re.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        ImGui::TextWrapped("%s", re.c_str());
                        ImGui::PopStyleColor();
                    }
                }
            }
            sectionHeader("Streams");

            for (auto& info : found) {
                ImGui::PushID(info.uid().c_str());
                const float rowH = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
                char label[256];
                if (isMarkerStream(info)) {
                    // Marker/event stream: connects to a MarkerSource that overlays
                    // event lines on the time-series plots (no waveform window).
                    MarkerSource* m = nullptr;
                    for (auto& ms : markerSources)
                        if (sameStream(*ms, info)) { m = ms.get(); break; }
                    // NOTE: end with "###r" so the per-frame rate text doesn't change the
                    // Selectable's ID (which would break click press/release tracking —
                    // this was why marker rows couldn't be clicked to disconnect).
                    if (m) std::snprintf(label, sizeof(label),
                                         "[on] %s\n     markers \xc2\xb7 %.1f/s###r",
                                         info.name().c_str(), m->rate());
                    else   std::snprintf(label, sizeof(label),
                                         "%s\n     markers \xc2\xb7 string events###r",
                                         info.name().c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        m ? ImVec4(0.85f, 0.8f, 0.45f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                    const bool clicked = ImGui::Selectable(label, false, 0, ImVec2(0, rowH));
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        if (m) ImGui::SetTooltip("%s  (host: %s)\n%zu events total \xc2\xb7 overlay on"
                                                 " the time-series plots (toggle per plot)"
                                                 "\nclick to disconnect",
                                                 info.uid().c_str(), info.hostname().c_str(), m->count());
                        else   ImGui::SetTooltip("%s  (host: %s)\nclick to connect",
                                                 info.uid().c_str(), info.hostname().c_str());
                    }
                    if (clicked) {
                        if (!m) connectStream(info);            // connect
                        else    disconnectKey(streamKey(info)); // disconnect (no window to close)
                    }
                } else {
                    HfStreamSource* csrc = nullptr;
                    for (auto& s : sources)
                        if (sameStream(*s, info)) { csrc = s.get(); break; }
                    const bool  stale = csrc && csrc->staleSeconds() > 1.0;
                    const char* tag   = !csrc ? "" : (stale ? "[no data] " : "[on] ");
                    // Whole entry is clickable: connect (or focus its plot if connected).
                    char rate[24];
                    if (info.nominal_srate() > 0.0)
                        std::snprintf(rate, sizeof(rate), "@ %.0f Hz", info.nominal_srate());
                    else
                        std::snprintf(rate, sizeof(rate), "irregular");   // resampled on connect
                    std::snprintf(label, sizeof(label), "%s%s\n     %s \xc2\xb7 %dch \xc2\xb7 %s###r",
                                  tag, info.name().c_str(), info.type().c_str(),
                                  info.channel_count(), rate);
                    const ImVec4 tc = !csrc ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                      : (stale ? ImVec4(1.0f, 0.7f, 0.2f, 1)
                                               : ImVec4(0.40f, 0.85f, 0.45f, 1));
                    ImGui::PushStyleColor(ImGuiCol_Text, tc);
                    const bool clicked = ImGui::Selectable(label, false, 0, ImVec2(0, rowH));
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s  (host: %s)\nclick to %s",
                                          info.uid().c_str(), info.hostname().c_str(),
                                          csrc ? "focus" : "connect");
                    if (clicked) {
                        if (!csrc) connectStream(info);                        // connect
                        else       ImGui::SetWindowFocus(info.name().c_str());  // already on: focus its plot
                    }
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            // Drop any marker stream the user dismissed (all matching, in case a stale
            // duplicate outlet shares the source_id). Defer the join off the UI thread
            // (same as data streams): signal stop, move to closingMrk, reap when done.
            for (auto& p : markerSources)
                if (dismissed.count(streamKeyOf(*p))) { p->requestStop(); closingMrk.push_back(std::move(p)); }
            markerSources.erase(
                std::remove_if(markerSources.begin(), markerSources.end(),
                    [](const std::unique_ptr<MarkerSource>& p) { return !p; }),
                markerSources.end());
            std::erase_if(closingMrk, [](const std::unique_ptr<MarkerSource>& p) { return p->finished(); });

            // (Recording is pinned at the TOP of the rail — see above the stream list.)

            // ---- Performance (Debug menu) — lives at the bottom of the rail ----
            if (showPerf) {
                sectionHeader("Performance");
                const float avg = fps.avg();
                ImGui::Text("%.1f FPS  \xc2\xb7  %.2f ms avg / %.2f ms max",
                            avg > 0 ? 1000.0f / avg : 0.0f, avg, fps.maxv());
                ImGui::PlotLines("##ft", fps.ms, FrameStats::N, fps.idx, nullptr,
                                 0.0f, std::max(33.0f, fps.maxv() * 1.1f), ImVec2(-1, 50));
                ImGui::TextDisabled("ui+draw %.2f ms \xc2\xb7 gpu+vsync %.2f ms", uiMs, gpuMs);
                ImGui::Checkbox("VSync", &vsync);
                ImGui::SameLine();
                ImGui::TextDisabled("\xc2\xb7 gpu debug: %s", gpuDebug ? "on" : "off");
            }
            ImGui::End();
            // Keep Streams above the plot windows. There's no public always-on-top
            // flag, so use the internal display-front call — but skip it while a
            // popup/menu is open (public IsPopupOpen check) so menus aren't covered.
            if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
                ImGui::BringWindowToDisplayFront(streamsWin);

            // ---- One scrolling plot per connected stream ----------------
            const double frameDtSec = (double)frameMs / 1000.0;
            const bool   justPaused = paused && !pausedPrev;
            HfStreamSource* toRemove = nullptr;
            int winIdx = 0;
            std::unordered_map<std::string, int> nameSeen;  // disambiguate same-named streams
            for (auto& s : sources) {
                DisplayOpts& o = dispOpts[s.get()];   // per-stream settings (default on first use)
                // Consume a pending workspace restore for this stream (by source_id), once.
                if (!savedOpts.empty()) {
                    if (auto it = savedOpts.find(streamKeyOf(*s)); it != savedOpts.end()) {
                        o = std::move(it->second);
                        savedOpts.erase(it);
                    }
                }
                // Smooth right-edge: glide at wall-clock rate and ease toward the
                // (chunky) newest data time, so the scroll doesn't snap per chunk.
                double        edge       = 0.0;
                std::uint64_t headFreeze = 0;             // 0 = live; set while paused
                if (!paused && s->frozen()) s->unfreeze();   // pause ended -> back to the live ring
                if (s->anchored()) {
                    auto it = edgeMap.find(s.get());
                    const double target = s->newestTime() - 0.15;  // slightly behind newest
                    const bool   stale  = s->staleSeconds() > 0.5;
                    if (paused) {
                        // Freeze the edge AND snapshot the data so nothing scrolls or scrolls
                        // out of the (live, still-filling) ring while paused. freeze() also on a
                        // stream that connects mid-pause. Reads anchor on the snapshot's head.
                        edge = (it != edgeMap.end()) ? it->second : target;
                        if (!s->frozen()) s->freeze();
                        pauseHead[s.get()] = s->snapHead();
                        headFreeze = s->snapHead();
                    } else if (it == edgeMap.end()) {
                        edge = target;
                        edgeMap[s.get()] = edge;
                    } else {
                        // Constant, refresh-paced velocity keeps the scroll smooth.
                        edge = it->second + frameDtSec;
                        const double gap = target - edge;   // +: behind data; -: ahead
                        if (stale) {
                            // Let the edge run past the (frozen) newest data — that growing
                            // margin is the live dropout, painted red by drawDropoutRed — but
                            // cap it at one window so a dead stream keeps the last data on
                            // screen instead of scrolling into all-red.
                            if (edge - target > (double)o.history) edge = target + (double)o.history;
                        } else if (gap > (double)o.history || gap < -0.25) {
                            edge = target;          // resync after a stall / big drift
                        } else {
                            edge += 0.02 * gap;     // gentle alignment, ~constant velocity
                        }
                        edgeMap[s.get()] = edge;
                    }
                }
                ImGui::SetNextWindowDockID(dockCenter, ImGuiCond_FirstUseEver);  // time-series tab
                ImGui::SetNextWindowSize(ImVec2(680, 420), ImGuiCond_FirstUseEver);
                bool open = true;                       // window X disconnects the stream
                // Unique, STABLE window id even if two streams share a name (else ImGui
                // ID conflict + merged windows). The visible title carries a transient
                // "no data" marker while stalled — via "Display###id" so the changing
                // text doesn't alter the window id (which would reset pos/size/dock).
                const int dup = nameSeen[s->name()]++;
                const std::string id = dup ? s->name() + " #" + std::to_string(dup + 1)
                                           : s->name();
                std::string title = id;
                if (s->anchored()) {
                    const double st = s->staleSeconds();
                    if (st > 0.5) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "  -  no data %.1fs", st);
                        title += buf;
                    }
                }
                title += "###" + id;
                ImGui::Begin(title.c_str(), &open);
                drawStream(*s, o, edge, /*followX=*/(!paused || justPaused), headFreeze,
                           markerViews, scratch);
                ImGui::End();
                if (!open) toRemove = s.get();
                ++winIdx;
            }
            pausedPrev = paused;
            if (toRemove) {   // closing the window (X) = disconnect (deferred worker reap)
                spdlog::debug("disconnect '{}' (deferred reap)", toRemove->name());
                disconnectKey(streamKeyOf(*toRemove));
            }
            std::erase_if(closing, [](const std::unique_ptr<HfStreamSource>& p) { return p->finished(); });

            // ---- FFT / PSD (View menu) ----------------------------------
            if (showSpectrum) {
            ImGui::SetNextWindowSize(ImVec2(580, 440), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowDockID(dockBottom, ImGuiCond_FirstUseEver);
            if (focusSpectrum) { ImGui::SetNextWindowFocus(); focusSpectrum = false; }
            ImGui::Begin("Spectrum", &showSpectrum);
            if (sources.empty()) {
                ImGui::TextUnformatted("Connect a stream to view its spectrum.");
            } else {
                if (!fftWantSid.empty())   // workspace restore: bind once the stream appears
                    for (int i = 0; i < (int)sources.size(); ++i)
                        if (streamKeyOf(*sources[i]) == fftWantSid) {
                            fftStream = i; fftWantSid.clear(); fftRefit = true;
                            fftSel.assign(sources[i]->channels(), 0);   // apply saved channel selection
                            for (int c : fftWantSel) if (c >= 0 && c < (int)fftSel.size()) fftSel[c] = 1;
                            fftWantSel.clear();
                            break;
                        }
                if (fftStream >= (int)sources.size()) fftStream = 0;
                HfStreamSource* src = sources[fftStream].get();

                // ---- left config strip; the plot fills the right (like the time series) ----
                ImGui::BeginChild("cfg", ImVec2(200, 0), ImGuiChildFlags_Borders);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##stream", src->name().c_str())) {
                    for (int i = 0; i < (int)sources.size(); ++i)
                        if (ImGui::Selectable(sources[i]->name().c_str(), i == fftStream)) {
                            fftStream = i; fftRefit = true;
                        }
                    ImGui::EndCombo();
                }
                src = sources[fftStream].get();
                if (src->uid() != fftUid) {            // reset selection on stream change
                    fftUid = src->uid();
                    fftSel.assign(src->channels(), 0);
                    if (src->channels() > 0) fftSel[0] = 1;
                    fftRefit = true;
                }
                const auto& labels = src->labels();
                auto fpass = [&](int c) { return fftFilter.PassFilter(labels[c].c_str()); };

                ImGui::SetNextItemWidth(80);
                const char* szs[] = { "512", "1024", "2048", "4096" };
                int szi = (fftN == 512) ? 0 : (fftN == 1024) ? 1 : (fftN == 2048) ? 2 : 3;
                if (ImGui::Combo("N", &szi, szs, 4)) { fftN = 1 << (9 + szi); fftRefit = true; }
                ImGui::SameLine(); if (ImGui::Checkbox("dB", &fftDb)) fftRefit = true;
                if (ImGui::Checkbox("Filtered", &fftFiltered)) fftRefit = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("analyze the conditioned signal (whichever filter stages are enabled\n"
                                      "in the time-series window) instead of raw");
                if (ImGui::SmallButton("All"))  for (int c = 0; c < src->channels(); ++c) if (fpass(c)) fftSel[c] = 1;
                ImGui::SameLine();
                if (ImGui::SmallButton("None")) for (int c = 0; c < src->channels(); ++c) if (fpass(c)) fftSel[c] = 0;
                ImGui::SameLine();
                if (ImGui::SmallButton("Fit Hz")) fftFitHz = true;   // zoom X to where the energy is
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("zoom the frequency axis to the band that actually carries\n"
                                      "signal energy (handy at high sample rates, e.g. 48 kHz audio)");
                fftFilter.Draw("##f", -1.0f);                                           // channel name filter
                ImGui::BeginChild("chans", ImVec2(0, 0), ImGuiChildFlags_Borders);      // fills the rest of the strip
                for (int c = 0; c < src->channels(); ++c) {
                    if (!fpass(c)) continue;
                    bool on = fftSel[c] != 0;
                    if (ImGui::Checkbox(labels[c].c_str(), &on)) { fftSel[c] = on ? 1 : 0; fftRefit = true; }
                }
                ImGui::EndChild();   // chans
                ImGui::EndChild();   // cfg
                ImGui::SameLine();

                if (src->srate() <= 0.0) {
                    ImGui::TextUnformatted("irregular stream — no spectrum");
                } else {
                    if (fftN != psdN || (float)src->srate() != psdFs) {
                        psd.init(fftN, (float)src->srate());
                        psdN = fftN; psdFs = (float)src->srate();
                        psdFreq.resize(psd.bins());
                        for (int k = 0; k < psd.bins(); ++k) psdFreq[k] = psd.binHz(k);
                    }
                    if (ImPlot::BeginPlot("##psd", ImVec2(-1, -1), ImPlotFlags_NoMouseText)) {
                        // Fixed axes (re-fit only after a mode change, not every
                        // frame) so the spectrum doesn't jump; user can still zoom.
                        const ImPlotCond cond = fitCond(fftRefit);
                        ImPlot::SetupAxes("Hz", fftDb ? "dB" : "power",
                                          ImPlotAxisFlags_None,
                                          fftDb ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
                        // Legend bottom-right: spectra peak at low Hz (left) and the dB floor sits
                        // bottom-left, so the SE corner is the emptiest. User can still drag it.
                        ImPlot::SetupLegend(ImPlotLocation_SouthEast);
                        // X axis: full 0..Nyquist by default. "Fit Hz" instead snaps it (once) to
                        // the band carrying energy — ImPlot's own AutoFit can't, since every PSD
                        // bin has a value and would just span the whole range. Uses last frame's
                        // cached spectra (recomputed below); the highest bin within ~45 dB of the
                        // peak sets the upper edge, with a little headroom.
                        double xmax = src->srate() * 0.5;
                        ImPlotCond xcond = cond;
                        if (fftFitHz) {
                            fftFitHz = false;
                            const int b = psd.bins();
                            const int nc = src->channels();
                            if ((int)fftCache.size() == nc * b && (int)psdFreq.size() >= b && b > 1) {
                                float peak = -1e30f;
                                for (int c = 0; c < nc; ++c) if (fftSel[c])
                                    for (int k = 1; k < b; ++k) peak = std::max(peak, fftCache[(std::size_t)c * b + k]);
                                const bool haveSignal = fftDb ? (peak > -90.0f) : (peak > 0.0f);
                                if (haveSignal) {
                                    const float thresh = fftDb ? peak - 45.0f : peak * 3.2e-5f;  // ~ -45 dB
                                    int hi = 0;
                                    for (int c = 0; c < nc; ++c) if (fftSel[c])
                                        for (int k = 1; k < b; ++k)
                                            if (fftCache[(std::size_t)c * b + k] > thresh && k > hi) hi = k;
                                    if (hi > 0)
                                        xmax = std::min((double)psdFreq[hi] * 1.3, src->srate() * 0.5);
                                    xcond = ImPlotCond_Always;
                                }
                            }
                        }
                        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, xmax, xcond);
                        // Keep pan/zoom within the real frequency band (no negative Hz, no
                        // empty space past Nyquist).
                        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, src->srate() * 0.5);
                        if (fftDb)
                            ImPlot::SetupAxisLimits(ImAxis_Y1, -110.0, 30.0, cond);
                        std::size_t count = 0; std::uint64_t start = 0;
                        InterleavedRing& frg = fftFiltered ? src->ringHp() : src->ring();
                        // When paused, freeze on the same window the rest of the UI is frozen at
                        // (the producer keeps filling the ring, so the live edge would keep moving).
                        std::uint64_t endHead = 0;
                        if (paused) {
                            auto ph = pauseHead.find(src);
                            endHead = (ph != pauseHead.end()) ? ph->second : src->head();
                        }
                        const float* p = readWindow(frg, endHead, (std::size_t)fftN, count, start);
                        const int ch   = src->channels();
                        const int bins = psd.bins();
                        // Pause-entry latch — updated every frame (NOT inside the data guard
                        // below) so it can't desync when the plot isn't producing a frame.
                        const bool pauseEntered = paused && !fftPausedPrev;
                        fftPausedPrev = paused;
                        if ((int)count >= fftN) {
                            // Recompute the selected channels at most ~15 Hz (a fresh FFT
                            // every frame on a 128-ch "All" selection was N×60 scalar FFTs/s
                            // for no visible gain) — cache the result and plot from it. While
                            // paused the frozen window never changes, so recompute only ONCE on
                            // pause entry (to align the cache) and on a settings change.
                            const double now = ImGui::GetTime();
                            const bool recompute = fftRefit || pauseEntered ||
                                                   (!paused && (now - fftLastCompute) > (1.0 / 15.0));
                            if ((int)fftCache.size() != ch * bins) fftCache.assign((std::size_t)ch * bins, 0.0f);
                            if (recompute) {
                                fftLastCompute = now;
                                for (int c = 0; c < ch; ++c) {
                                    if (!fftSel[c]) continue;
                                    LSL_ZONE("psd");
                                    psd.compute(p + c, ch, psdOut);
                                    if (fftDb)
                                        for (auto& v : psdOut) v = 10.0f * std::log10(v + 1e-12f);
                                    std::copy(psdOut.begin(), psdOut.begin() + bins,
                                              fftCache.begin() + (std::size_t)c * bins);
                                }
                            }
                            for (int c = 0; c < ch; ++c)
                                if (fftSel[c])
                                    ImPlot::PlotLine(labels[c].c_str(), psdFreq.data(),
                                                     fftCache.data() + (std::size_t)c * bins, bins);
                        }
                        // Cursor: a vertical line at the mouse frequency + the Hz value as a
                        // floating label — more useful on a spectrum than the raw "x, y" readout.
                        // Drawn with the draw list (NOT ImPlot::TagX, which reserves axis padding
                        // for the tag and so jitters the plot size as the cursor enters/leaves).
                        if (ImPlot::IsPlotHovered()) {
                            const double fx = ImPlot::GetPlotMousePos().x;
                            const ImPlotRect lim = ImPlot::GetPlotLimits();
                            const ImVec2 top = ImPlot::PlotToPixels(fx, lim.Y.Max);
                            const ImVec2 bot = ImPlot::PlotToPixels(fx, lim.Y.Min);
                            ImDrawList* dl = ImPlot::GetPlotDrawList();
                            dl->AddLine(top, bot, IM_COL32(200, 200, 200, 130), 1.0f);
                            char hz[24]; std::snprintf(hz, sizeof hz, "%.1f Hz", fx);
                            const ImVec2 sz = ImGui::CalcTextSize(hz);
                            const float  pr = ImPlot::GetPlotPos().x + ImPlot::GetPlotSize().x;  // plot right edge (px)
                            // label just right of the line near the top, flipping left near the edge
                            const bool   left = top.x + 6.0f + sz.x > pr;
                            const ImVec2 p(left ? top.x - 6.0f - sz.x : top.x + 6.0f, top.y + 3.0f);
                            dl->AddRectFilled(ImVec2(p.x - 2, p.y - 1), ImVec2(p.x + sz.x + 2, p.y + sz.y + 1),
                                              IM_COL32(0, 0, 0, 150));
                            dl->AddText(p, IM_COL32(220, 220, 220, 255), hz);
                        }
                        fftRefit = false;
                        ImPlot::EndPlot();
                    }
                }
            }
            ImGui::End();
            }  // showSpectrum

            // ---- Spectrograms (multi-instance; View > New spectrogram) --
            for (auto& spp : spectros) {
                Spectro& spectro = *spp;
                LSL_ZONE("spectrogram win");
                ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowDockID(dockBottom, ImGuiCond_FirstUseEver);
                if (spectro.focus) { ImGui::SetNextWindowFocus(); spectro.focus = false; }
                char title[64];
                std::snprintf(title, sizeof(title), "Spectrogram %d", spectro.id);
                ImGui::Begin(title, &spectro.open);
                if (sources.empty()) {
                    ImGui::TextUnformatted("Connect a stream to view its spectrogram.");
                } else {
                    // Workspace restore: bind to the saved stream once it (re)appears.
                    if (!spectro.wantSid.empty())
                        for (int i = 0; i < (int)sources.size(); ++i)
                            if (streamKeyOf(*sources[i]) == spectro.wantSid) { spectro.streamIdx = i; spectro.wantSid.clear(); break; }
                    if (spectro.streamIdx >= (int)sources.size()) spectro.streamIdx = 0;
                    HfStreamSource* src = sources[spectro.streamIdx].get();
                    bool reset = (spectro.uid != src->uid());

                    // ---- left config strip; the plot + colorbar fill the right ----
                    ImGui::BeginChild("cfg", ImVec2(200, 0), ImGuiChildFlags_Borders);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##stream", src->name().c_str())) {
                        spectro.streamFilter.Draw("##sf", -1.0f);   // searchable
                        for (int i = 0; i < (int)sources.size(); ++i) {
                            if (!spectro.streamFilter.PassFilter(sources[i]->name().c_str())) continue;
                            if (ImGui::Selectable(sources[i]->name().c_str(), i == spectro.streamIdx)) {
                                spectro.streamIdx = i; reset = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    src = sources[spectro.streamIdx].get();
                    if (spectro.channel >= src->channels()) { spectro.channel = 0; reset = true; }
                    const auto& labels = src->labels();
                    const char* chn = (spectro.channel < (int)labels.size())
                                          ? labels[spectro.channel].c_str() : "ch";
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##channel", chn)) {
                        spectro.chanFilter.Draw("##chf", -1.0f);    // searchable (e.g. "Cz")
                        for (int c = 0; c < src->channels(); ++c) {
                            if (!spectro.chanFilter.PassFilter(labels[c].c_str())) continue;
                            if (ImGui::Selectable(labels[c].c_str(), c == spectro.channel)) {
                                spectro.channel = c; reset = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(90);
                    const char* nf[] = { "128", "256", "512", "1024" };
                    int ni = (spectro.nfft == 128) ? 0 : (spectro.nfft == 256) ? 1
                           : (spectro.nfft == 512) ? 2 : 3;
                    if (ImGui::Combo("NFFT", &ni, nf, 4)) { spectro.nfft = 1 << (7 + ni); reset = true; }
                    ImGui::SetNextItemWidth(110);
                    ImGui::SliderFloat("span (s)", &spectro.spanSec, 2.0f, 120.0f, "%.0f");
                    ImGui::SetNextItemWidth(140);
                    ImGui::DragFloatRange2("dB", &spectro.dbMin, &spectro.dbMax, 1.0f,
                                           -160.0f, 80.0f, "%.0f");
                    // Frequency range shown (Y-axis zoom). Vital when the sample rate is high but
                    // the signal is low-freq (48 kHz audio -> tones < 1 kHz); synced with mouse zoom.
                    const float ny = (float)(src->srate() * 0.5);
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::DragFloatRange2("Hz", &spectro.fMin, &spectro.fMax,
                                               std::max(0.5f, ny / 400.0f), 0.0f, ny, "%.0f"))
                        spectro.yDirty = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("frequency range shown — zoom the Y axis to the band of interest");
                    ImGui::SameLine();
                    // Fit Hz: snap the Y range to the highest frequency carrying energy across the
                    // stored columns (same idea as the spectrum's Fit Hz; rows are Nyquist..DC).
                    if (ImGui::SmallButton("Fit Hz") && spectro.freqBins > 1 && spectro.filled > 0) {
                        const int fb   = spectro.freqBins;
                        const int cols = std::min(spectro.filled, spectro.cols);
                        const auto at  = [&](int cc, int r) { return spectro.data[(std::size_t)cc * fb + r]; };
                        float peak = -1e30f;
                        for (int cc = 0; cc < cols; ++cc)
                            for (int r = 0; r < fb; ++r) peak = std::max(peak, at(cc, r));
                        const bool haveSignal = spectro.db ? (peak > -120.0f) : (peak > 0.0f);
                        if (haveSignal) {
                            const float thresh = spectro.db ? peak - 45.0f : peak * 3.2e-5f;
                            const int   bbBins = std::max(8, fb / 4);   // lit across >~25% of bins = "broadband"
                            // A brief broadband column is a transient (e.g. a chirp's sawtooth reset
                            // splattering the whole spectrum); exclude those so they don't inflate the
                            // fit. But if most columns are broadband the SIGNAL is broadband (EEG 1/f),
                            // so keep them.
                            int bbCols = 0;
                            for (int cc = 0; cc < cols; ++cc) {
                                int cnt = 0;
                                for (int r = 0; r < fb; ++r) if (at(cc, r) > thresh) ++cnt;
                                if (cnt > bbBins) ++bbCols;
                            }
                            const bool dropBroadband = bbCols * 5 < cols;   // broadband is the exception
                            int rHi = fb - 1;                               // row of the highest energetic freq
                            for (int cc = 0; cc < cols; ++cc) {
                                int cnt = 0, firstR = fb;
                                for (int r = 0; r < fb; ++r)
                                    if (at(cc, r) > thresh) { if (firstR == fb) firstR = r; ++cnt; }
                                if (firstR < fb && !(dropBroadband && cnt > bbBins)) rHi = std::min(rHi, firstR);
                            }
                            const float fhi = ny * (float)(fb - 1 - rHi) / (float)(fb - 1);
                            spectro.fMin = 0.0f;
                            spectro.fMax = std::min(ny, fhi * 1.2f);
                            spectro.yDirty = true;
                        }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("zoom the frequency axis to the band that actually carries energy");
                    if (ImGui::Checkbox("Filtered", &spectro.filtered)) reset = true;  // recompute columns
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("analyze the conditioned signal (whichever filter stages are enabled\n"
                                          "in the time-series window) instead of raw");
                    ImGui::EndChild();   // cfg
                    ImGui::SameLine();

                    if (src->srate() <= 0.0) {
                        ImGui::TextUnformatted("irregular stream — no spectrogram");
                    } else if (!src->anchored() || src->head() == 0) {
                        ImGui::TextUnformatted("waiting for data...");
                    } else {
                        if (reset) spectroReset(spectro, *src);
                        if (!paused) spectroUpdate(spectro, *src);  // freeze columns when paused

                        // Right edge of the newest cell (cells centered on their real times).
                        const double bMax = spectro.colNewestTime + 0.5 * spectro.hopSec;
                        // View edge. Normally ease toward the DATA edge (newest column)
                        // and never pass it, so sub-column scrolling stays smooth with no
                        // blank live-edge strip. While stalled, glide the edge PAST the
                        // data at wall-clock so the red gap scrolls in gradually (same
                        // pacing as the time series) instead of freezing then jumping.
                        // Steady scroll: advance at a constant, refresh-paced velocity (NOT
                        // an ease — that varies the speed). Identical scheme to the time-
                        // series edge: glide at frameDt, a weak pull keeps it aligned when
                        // live, and while stalled it runs ahead freely so the red gap scrolls
                        // in at the same steady rate.
                        const double dataEdge = bMax;
                        const bool   stalling = !paused && src->staleSeconds() > 0.4;
                        const double frameDt  = (double)ImGui::GetIO().DeltaTime;
                        if (!spectro.smoothInit) {
                            spectro.smoothNow = dataEdge; spectro.smoothInit = true;
                        } else if (paused) {
                            // frozen — leave smoothNow as-is
                        } else {
                            spectro.smoothNow += frameDt;                 // constant velocity
                            const double gap = dataEdge - spectro.smoothNow;
                            if (gap > spectro.spanSec) {
                                spectro.smoothNow = dataEdge;             // far behind → snap forward
                            } else if (!stalling) {
                                if (gap < -0.25) spectro.smoothNow = dataEdge;   // overshoot → snap
                                else             spectro.smoothNow += 0.02 * gap; // gentle align
                            }
                        }
                        const double viewNewest = stalling ? spectro.smoothNow
                                                           : std::min(spectro.smoothNow, dataEdge);
                        const double filledSec  = std::min(spectro.filled, spectro.cols) * spectro.hopSec;
                        const double viewSpan   = std::min((double)spectro.spanSec,
                                                           std::max(filledSec, spectro.hopSec));
                        const double viewX0 = viewNewest - viewSpan;
                        const double y1   = src->srate() * 0.5;

                        // De-rotate only the columns covering the visible span into a
                        // row-major scratch (was: hand the whole up-to-120 s ring to
                        // PlotHeatmap every frame -> ~770k cells, mostly off-screen).
                        int Ndraw = (int)std::ceil((bMax - viewX0) / spectro.hopSec) + 1;
                        Ndraw = std::clamp(Ndraw, 0, std::min(spectro.filled, spectro.cols));
                        const double bMaxDraw = bMax;
                        const double bMinDraw = bMax - (double)Ndraw * spectro.hopSec;
                        // Columns are immutable once written, so drawBuf only changes when
                        // the cursor or the visible count does — skip the transpose otherwise
                        // (e.g. paused, or scrolling between hops with the same column set).
                        if (Ndraw > 0 && (Ndraw != spectro.lastDrawNdraw ||
                                          spectro.writeCol != spectro.lastDrawWriteCol)) {
                            spectro.lastDrawNdraw = Ndraw; spectro.lastDrawWriteCol = spectro.writeCol;
                            const int fb = spectro.freqBins, C = spectro.cols;
                            spectro.drawBuf.resize((std::size_t)fb * Ndraw);
                            const int first = ((spectro.writeCol - Ndraw) % C + C) % C;  // oldest drawn
                            for (int k = 0; k < Ndraw; ++k) {
                                const float* csrc = &spectro.data[(std::size_t)((first + k) % C) * fb];
                                float* dst = spectro.drawBuf.data() + k;     // row-major, stride Ndraw
                                for (int r = 0; r < fb; ++r) dst[(std::size_t)r * Ndraw] = csrc[r];
                            }
                        }

                        ImPlot::PushColormap(ImPlotColormap_Viridis);
                        if (ImPlot::BeginPlot("##spectro", ImVec2(-70, -1))) {
                            ImPlot::SetupAxes("time (s)", "Hz");
                            if (!paused)   // live: follow the scroll; paused: free to pan/zoom the frozen view
                                ImPlot::SetupAxisLimits(ImAxis_X1, viewX0, viewNewest, ImPlotCond_Always);
                            // Y range is the user's freq window (control or mouse-zoom). Force it
                            // only when the control changed or on reset; otherwise leave it free so
                            // the mouse can zoom — then read the live limits back into the control.
                            ImPlot::SetupAxisLimits(ImAxis_Y1, spectro.fMin, spectro.fMax,
                                                    fitCond(reset || spectro.yDirty));
                            // Keep the Y (frequency) pan/zoom inside 0..Nyquist.
                            ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, src->srate() * 0.5);
                            spectro.yDirty = false;
                            if (Ndraw > 0)
                                ImPlot::PlotHeatmap("##h", spectro.drawBuf.data(), spectro.freqBins,
                                                    Ndraw, spectro.dbMin, spectro.dbMax, nullptr,
                                                    ImPlotPoint(bMinDraw, 0.0), ImPlotPoint(bMaxDraw, y1));
                            const ImPlotRect lim = ImPlot::GetPlotLimits();   // sync mouse-zoom -> control
                            spectro.fMin = std::clamp((float)lim.Y.Min, 0.0f, (float)y1);   // keep the Hz control sane
                            spectro.fMax = std::clamp((float)lim.Y.Max, spectro.fMin, (float)y1);
                            // Recorded gaps (extended by the STFT recovery latency, less
                            // half a column so the red ends at the last blank column) plus
                            // the live edge gap scrolling in — same opaque red as the time
                            // series; real data between dropouts is shown (not masked).
                            drawDropoutRed(*src,
                                (double)spectro.nfft * src->dt() - 0.5 * spectro.hopSec, 0.0);
                            ImPlot::EndPlot();
                        }
                        ImGui::SameLine();
                        ImPlot::ColormapScale("dB", spectro.dbMin, spectro.dbMax, ImVec2(60, -1));
                        ImPlot::PopColormap();
                    }
                }
                ImGui::End();
            }
            std::erase_if(spectros, [](const std::unique_ptr<Spectro>& s) { return !s->open; });

            // ---- ERPs (multi-instance; View > New ERP) ------------------
            for (auto& epp : erps) {
                Erp& erp = *epp;
                LSL_ZONE("erp win");
                ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowDockID(dockBottom, ImGuiCond_FirstUseEver);
                if (erp.focus) { ImGui::SetNextWindowFocus(); erp.focus = false; }
                char title[64];
                std::snprintf(title, sizeof(title), "ERP %d", erp.id);
                ImGui::Begin(title, &erp.open);
                if (sources.empty() || markerSources.empty()) {
                    ImGui::TextUnformatted("connect a data stream AND a marker stream");
                } else {
                    // Workspace restore: bind to the saved data + marker streams once present.
                    if (!erp.wantSid.empty())
                        for (int i = 0; i < (int)sources.size(); ++i)
                            if (streamKeyOf(*sources[i]) == erp.wantSid) { erp.streamIdx = i; erp.wantSid.clear(); break; }
                    if (!erp.wantMsid.empty())
                        for (int i = 0; i < (int)markerSources.size(); ++i)
                            if (streamKeyOf(*markerSources[i]) == erp.wantMsid) { erp.markerIdx = i; erp.wantMsid.clear(); break; }
                    if (erp.streamIdx >= (int)sources.size()) erp.streamIdx = 0;
                    if (erp.markerIdx >= (int)markerSources.size()) erp.markerIdx = 0;
                    HfStreamSource* src = sources[erp.streamIdx].get();
                    MarkerSource*   mk  = markerSources[erp.markerIdx].get();
                    bool reset = (erp.sUid != src->uid()) || (erp.mUid != mk->uid());

                    // ---- left config strip; the plot fills the right (matches the time
                    // series / spectrum / spectrogram windows) ----
                    ImGui::BeginChild("cfg", ImVec2(230, 0), ImGuiChildFlags_Borders);
                    // data stream
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::BeginCombo("stream", src->name().c_str())) {
                        erp.streamFilter.Draw("##sf", -1.0f);
                        for (int i = 0; i < (int)sources.size(); ++i) {
                            if (!erp.streamFilter.PassFilter(sources[i]->name().c_str())) continue;
                            if (ImGui::Selectable(sources[i]->name().c_str(), i == erp.streamIdx)) {
                                erp.streamIdx = i; reset = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    src = sources[erp.streamIdx].get();
                    if (erp.channel >= src->channels()) { erp.channel = 0; reset = true; }
                    const auto& labels = src->labels();
                    // single-channel picker (disabled when averaging all channels)
                    ImGui::BeginDisabled(erp.allCh);
                    ImGui::SetNextItemWidth(140);
                    const char* chn = (erp.channel < (int)labels.size()) ? labels[erp.channel].c_str() : "ch";
                    if (ImGui::BeginCombo("channel", chn)) {
                        erp.chanFilter.Draw("##chf", -1.0f);
                        for (int c = 0; c < src->channels(); ++c) {
                            if (!erp.chanFilter.PassFilter(labels[c].c_str())) continue;
                            if (ImGui::Selectable(labels[c].c_str(), c == erp.channel)) {
                                erp.channel = c; reset = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                    if (ImGui::Checkbox("all channels", &erp.allCh)) reset = true;  // average every channel
                    if (erp.allCh) {
                        ImGui::SetNextItemWidth(140);
                        if (ImGui::SliderInt("max ch", &erp.maxCh, 1, std::max(1, src->channels()))) reset = true;
                    }
                    // trigger marker stream
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::BeginCombo("trigger", mk->name().c_str())) {
                        erp.markerFilter.Draw("##mf", -1.0f);
                        for (int i = 0; i < (int)markerSources.size(); ++i) {
                            if (!erp.markerFilter.PassFilter(markerSources[i]->name().c_str())) continue;
                            if (ImGui::Selectable(markerSources[i]->name().c_str(), i == erp.markerIdx)) {
                                erp.markerIdx = i; reset = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    mk = markerSources[erp.markerIdx].get();
                    if (erp.labelFilter.Draw("match", 140.0f)) reset = true;  // only events matching fire
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("only trigger on events whose text matches (blank = every event)");

                    ImGui::SetNextItemWidth(140);
                    if (ImGui::SliderFloat("pre (ms)", &erp.preMs, 10.0f, 1000.0f, "%.0f")) reset = true;
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::SliderFloat("post (ms)", &erp.postMs, 50.0f, 2000.0f, "%.0f")) reset = true;
                    if (ImGui::Checkbox("baseline", &erp.baseline)) reset = true;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) erpClear(erp);
                    ImGui::Checkbox("raster", &erp.raster);   // channels x time heatmap, one row per channel
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("render the averaged ERP as a channels x time heatmap (color = µV),\none row per channel — the trigger-aligned twin of the time-series raster");
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d epoch%s", erp.count, erp.count == 1 ? "" : "s");
                    ImGui::EndChild();   // cfg
                    ImGui::SameLine();

                    if (src->srate() <= 0.0) {
                        ImGui::TextUnformatted("data stream has no regular rate");
                    } else {
                        if (reset) erpReset(erp, *src, mk->uid());
                        if (!paused) erpUpdate(erp, *src, *mk);

                        // Prominent t=0 onset marker: a white core over a dark halo so it
                        // reads on any colormap cell or line color behind it.
                        auto onsetLine = [](double y0, double y1) {
                            ImDrawList* dl = ImPlot::GetPlotDrawList();
                            const ImVec2 a = ImPlot::PlotToPixels(0.0, y0), b = ImPlot::PlotToPixels(0.0, y1);
                            dl->AddLine(a, b, IM_COL32(0, 0, 0, 170), 3.5f);
                            dl->AddLine(a, b, IM_COL32(255, 255, 255, 235), 1.5f);
                        };
                        // ERP is a static analysis view: fit the axes on a reset (stream / channel
                        // / pre-post change), then leave them FREE so the mouse can zoom/pan/
                        // double-click-fit into a latency window or amplitude range.
                        const ImPlotCond erpCond = fitCond(reset);

                        if (erp.raster) {
                            // channels x time heatmap (one row per channel) of the averaged ERP —
                            // the trigger-aligned twin of the time-series raster. erp.avg is already
                            // nchan x nbins row-major, so it feeds PlotHeatmap directly.
                            if (erp.nchan == 0 || erp.nbins == 0 || erp.count == 0) {
                                ImGui::TextDisabled("waiting for epochs...");
                            } else if (ImPlot::BeginPlot("##erpr", ImVec2(-70, -1), ImPlotFlags_NoLegend)) {
                                ImPlot::SetupAxes("time (ms)", nullptr,
                                                  ImPlotAxisFlags_None, ImPlotAxisFlags_NoGridLines);
                                ImPlot::SetupAxisLimits(ImAxis_X1, -erp.preMs, erp.postMs, erpCond);
                                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, (double)erp.nchan, erpCond);
                                // keep pan/zoom inside the epoch window / channel range
                                ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, -erp.preMs, erp.postMs);
                                ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, (double)erp.nchan);
                                // channel-name ticks at row centers (row 0 = top = first channel)
                                erp.tvals.resize(erp.nchan); erp.tlabs.resize(erp.nchan);
                                for (int j = 0; j < erp.nchan; ++j) {
                                    erp.tvals[j] = (double)erp.nchan - 0.5 - j;
                                    const int ch = erp.chans[j];
                                    erp.tlabs[j] = (ch < (int)labels.size()) ? labels[ch].c_str() : "";
                                }
                                ImPlot::SetupAxisTicks(ImAxis_Y1, erp.tvals.data(), erp.nchan, erp.tlabs.data());
                                // Diverging map, symmetric about 0, + = red (see divergingPosRed).
                                // Plain ascending scale so the heatmap and colorbar agree on sign.
                                ImPlot::PushColormap(divergingPosRed());
                                ImPlot::PlotHeatmap("##i", erp.avg.data(), erp.nchan, erp.nbins,
                                                    -erp.imgHalf, erp.imgHalf, nullptr,
                                                    ImPlotPoint(-erp.preMs, 0.0),
                                                    ImPlotPoint(erp.postMs, (double)erp.nchan));
                                onsetLine(0.0, (double)erp.nchan);
                                ImPlot::EndPlot();
                                ImGui::SameLine();
                                ImPlot::ColormapScale("uV", -erp.imgHalf, erp.imgHalf, ImVec2(60, -1), "%.0f");
                                ImPlot::PopColormap();
                            }
                        } else if (ImPlot::BeginPlot("##erp", ImVec2(-1, -1))) {
                            const bool single = (erp.nchan == 1);
                            ImPlot::SetupAxes("time (ms)",
                                single ? ((erp.chans[0] < (int)labels.size()) ? labels[erp.chans[0]].c_str() : "uV")
                                       : "uV");
                            ImPlot::SetupAxisLimits(ImAxis_X1, -erp.preMs, erp.postMs, erpCond);
                            ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, -erp.preMs, erp.postMs);  // stay in the epoch window
                            // Y auto-fits while the robust range (yHalf) is still evolving with
                            // accumulation, then is free to zoom once it settles (or after a reset).
                            const ImPlotCond yCond = fitCond(reset || erp.yHalf != erp.yHalfApplied);
                            ImPlot::SetupAxisLimits(ImAxis_Y1, -erp.yHalf, erp.yHalf, yCond);
                            erp.yHalfApplied = erp.yHalf;
                            if (erp.nbins > 0 && erp.count > 0) {
                                if (single) {
                                    // single channel: faint individual epochs (spaghetti) behind the average
                                    for (auto& ep : erp.epochs) {
                                        ImPlotSpec sp; sp.LineColor = ImVec4(0.6f, 0.6f, 0.7f, 0.18f);
                                        sp.LineWeight = 1.0f;
                                        ImPlot::PlotLine("##sp", erp.taxis.data(), ep.data(), erp.nbins, sp);
                                    }
                                    ImPlotSpec sp; sp.LineColor = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
                                    sp.LineWeight = 2.5f;
                                    ImPlot::PlotLine("average", erp.taxis.data(), erp.avg.data(), erp.nbins, sp);
                                } else {
                                    // multi channel: one average line per channel (ImPlot auto-colors; legend = names)
                                    for (int ci = 0; ci < erp.nchan; ++ci) {
                                        const int ch = erp.chans[ci];
                                        const char* nm = (ch < (int)labels.size()) ? labels[ch].c_str() : "ch";
                                        ImPlot::PlotLine(nm, erp.taxis.data(),
                                                         erp.avg.data() + (std::size_t)ci * erp.nbins, erp.nbins);
                                    }
                                }
                            }
                            const ImPlotRect lr = ImPlot::GetPlotLimits();
                            onsetLine(lr.Y.Min, lr.Y.Max);
                            ImPlot::EndPlot();
                        }
                    }
                }
                ImGui::End();
            }
            std::erase_if(erps, [](const std::unique_ptr<Erp>& e) { return !e->open; });

            if (showMetrics) {
                if (focusMetrics) { ImGui::SetNextWindowFocus(); focusMetrics = false; }
                ImGui::SetNextWindowDockID(dockBottom, ImGuiCond_FirstUseEver);
                ImGui::ShowMetricsWindow(&showMetrics);  // vertex/draw-call inspector
            }

#ifdef LSL_TESTS
            if (!runTests) ImGuiTestEngine_ShowTestEngineWindows(engine, nullptr);
#endif
            ImGui::Render();
        }
        uiMs = pcSeconds(tUi0, SDL_GetPerformanceCounter()) * 1000.0;

        // ---- Render (two-stage: prepare BEFORE the render pass) ---------
        const Uint64 tGpu0 = SDL_GetPerformanceCounter();
        {
            LSL_ZONE("gpu submit");
            ImDrawData* draw_data = ImGui::GetDrawData();
            const bool minimized =
                (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);

            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
            SDL_GPUTexture* swapchain = nullptr;
            Uint32 sw = 0, sh = 0;
            SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &sw, &sh);

            SDL_GPUTexture* renderTarget = swapchain;
#ifdef LSL_TESTS
            // Render into an offscreen texture so the frame stays readable for
            // screen capture; recreate it when the swapchain size changes.
            if (swapchain) {
                if (!offscreen || offW != (int)sw || offH != (int)sh) {
                    if (offscreen) SDL_ReleaseGPUTexture(gpu, offscreen);
                    SDL_GPUTextureCreateInfo ti = {};
                    ti.type   = SDL_GPU_TEXTURETYPE_2D;
                    ti.format = swapFmt;
                    ti.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    ti.width  = sw; ti.height = sh;
                    ti.layer_count_or_depth = 1; ti.num_levels = 1;
                    offscreen = SDL_CreateGPUTexture(gpu, &ti);
                    offW = (int)sw; offH = (int)sh;
                    capCtx.tex = offscreen; capCtx.w = (int)sw; capCtx.h = (int)sh;
                }
                renderTarget = offscreen;
            }
#endif

            if (swapchain && !minimized) {
                ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);  // MANDATORY, pre-pass

                SDL_GPUColorTargetInfo target = {};
                target.texture     = renderTarget;
                const ImVec4 bg    = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];  // match theme
                target.clear_color = SDL_FColor{bg.x, bg.y, bg.z, 1.0f};
                target.load_op     = SDL_GPU_LOADOP_CLEAR;
                target.store_op    = SDL_GPU_STOREOP_STORE;

                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
                ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
                SDL_EndGPURenderPass(pass);

#ifdef LSL_TESTS
                if (renderTarget != swapchain) {   // present the offscreen frame
                    SDL_GPUBlitInfo blit = {};
                    blit.source.texture      = offscreen;
                    blit.source.w            = sw; blit.source.h = sh;
                    blit.destination.texture = swapchain;
                    blit.destination.w       = sw; blit.destination.h = sh;
                    blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
                    blit.filter              = SDL_GPU_FILTER_NEAREST;
                    SDL_BlitGPUTexture(cmd, &blit);
                }
#endif
            }
#ifdef LSL_TESTS
            ImGuiTestEngine_PreSwap(engine);   // time measurement, before present
#endif
            SDL_SubmitGPUCommandBuffer(cmd);   // presents the acquired swapchain
#ifdef LSL_TESTS
            ImGuiTestEngine_PostSwap(engine);  // processes capture / queue timing
            // The engine loads settings (incl. empty VideoCapture*ToEncoder entries) on the
            // first frame, clobbering any pre-Start values; (re)apply the ffmpeg path + params
            // once after, so CaptureBeginVideo() can encode a .gif/.mp4.
            if (frameCounter == 2 && std::getenv("LSL_FFMPEG")) {
                std::strncpy(tio.VideoCaptureEncoderPath, std::getenv("LSL_FFMPEG"),
                             sizeof(tio.VideoCaptureEncoderPath) - 1);
                std::strncpy(tio.VideoCaptureEncoderParams, IMGUI_CAPTURE_DEFAULT_VIDEO_PARAMS_FOR_FFMPEG,
                             sizeof(tio.VideoCaptureEncoderParams) - 1);
                std::strncpy(tio.GifCaptureEncoderParams, IMGUI_CAPTURE_DEFAULT_GIF_PARAMS_FOR_FFMPEG,
                             sizeof(tio.GifCaptureEncoderParams) - 1);
            }
#endif
        }
        gpuMs = pcSeconds(tGpu0, SDL_GetPerformanceCounter()) * 1000.0;

        // Apply a deferred workspace load now the frame is fully rendered (a safe point to
        // mutate ImGui ini state): rebuilds analysis windows + restores the dock layout.
        if (!pendingWs.empty()) { wsApply(pendingWs); pendingWs.clear(); }

        LSL_FRAME_MARK();
        LSL_PLOT("frame ms", frameMs);

        if (bench) {
            benchAccum += frameMs / 1000.0;
            if (benchAccum >= 1.0) {
                const float a = fps.avg();
                std::printf("[bench] %.1f FPS | %.2f ms avg / %.2f ms max | "
                            "ui %.2f ms | gpu %.2f ms | streams %d\n",
                            a > 0 ? 1000.0f / a : 0.0f, a, fps.maxv(),
                            uiMs, gpuMs, (int)sources.size());
                std::fflush(stdout);
                benchAccum = 0.0;
            }
        }
        if (profile) {
            profAccum += frameMs / 1000.0;
            if (profAccum >= 3.0) { LSL_PROFILE_DUMP(profAccum); profAccum = 0.0; }
        }

#ifdef LSL_TESTS
        // Headless: exit once the queued tests have all drained (after a few
        // warm-up frames so the queue has actually started).
        if (runTests && frameCounter > 5 && ImGuiTestEngine_IsTestQueueEmpty(engine))
            done = true;
#endif
    }
    if (profile) LSL_PROFILE_DUMP(profAccum > 0 ? profAccum : 1.0);  // final partial window

#ifdef LSL_TESTS
    int exitCode = 0;
    if (runTests) {
        int tested = 0, ok = 0;
        ImGuiTestEngine_GetResult(engine, tested, ok);
        std::printf("\n[tests] %d/%d passed\n", ok, tested);
        std::fflush(stdout);
        exitCode = (tested > 0 && ok == tested) ? 0 : 1;
    }
    ImGuiTestEngine_Stop(engine);
#endif

    // ---- Shutdown -------------------------------------------------------
    SDL_WaitForGPUIdle(gpu);
#ifdef LSL_TESTS
    if (offscreen) SDL_ReleaseGPUTexture(gpu, offscreen);
#endif
    for (auto& s : sources)       s->requestStop();  // signal all so joins overlap
    for (auto& m : markerSources) m->requestStop();
    sources.clear();                            // joins worker threads via ~HfStreamSource
    markerSources.clear();

    if (io.IniFilename) ImGui::SaveIniSettingsToDisk(io.IniFilename);  // persist theme/path on exit
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

#ifdef LSL_TESTS
    // Must be after ImGui::DestroyContext() so test-engine .ini data can save.
    ImGuiTestEngine_DestroyContext(engine);
#endif

    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef LSL_TESTS
    return exitCode;
#else
    return 0;
#endif
}
