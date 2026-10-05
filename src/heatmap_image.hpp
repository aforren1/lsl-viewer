#pragma once
// Heatmaps drawn as one textured quad. ImPlot::PlotHeatmap emits a quad per cell: a 66-channel
// raster across a 3840 px plot is about 250k quads (1M vertices) a frame, and on a large or
// DisplayLink-driven display the GPU and present path then missed a third of the frames. Here the
// cells are colored on the CPU from the colormap's own table into an RGBA texture, which ImGui's
// renderer backend uploads before the render pass, and the plot draws it with nearest sampling
// so the cells keep their hard edges.
//
// Textures are registered with ImGui (RegisterUserTexture), so the backend creates, updates and
// destroys them. A retired texture is freed only after the backend has destroyed it, because a
// frame in flight may still sample it.

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture, ImTextureDataQueueUpload
#include "implot.h"
#include "implot_internal.h"  // the colormap tables

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace heat {

// Larger images fall back to PlotHeatmap. D3D12 allows 16384 texels a side; this keeps a margin
// for other backends and bounds the per-frame upload.
constexpr int kMaxDim = 8192;

inline std::vector<ImTextureData*> g_live;      // registered and in use
inline std::vector<ImTextureData*> g_retired;   // waiting for the backend to destroy them
inline std::vector<ImTextureData*> g_pool;      // frameTexture() slots (also in g_live)
inline int                         g_poolNext = 0;
inline bool                        g_closed   = false;   // after shutdown(): no ImGui calls

inline ImTextureData* create(int w, int h) {
    auto* t = new ImTextureData();
    t->Create(ImTextureFormat_RGBA32, w, h);
    ImGui::RegisterUserTexture(t);
    g_live.push_back(t);
    return t;
}

inline void retire(ImTextureData* t) {
    if (!t) return;
    if (g_closed) { delete t; return; }   // the backend shutdown already destroyed its GPU side
    std::erase(g_live, t);
    t->WantDestroyNextFrame = true;
    g_retired.push_back(t);
}

// Once per frame, after ImGui::NewFrame(): frees the retired textures the backend has destroyed,
// and hands frameTexture() slots out from the start again.
inline void beginFrame() {
    g_poolNext = 0;
    std::erase_if(g_retired, [](ImTextureData* t) {
        if (t->Status != ImTextureStatus_Destroyed) return false;
        ImGui::UnregisterUserTexture(t);
        delete t;
        return true;
    });
}

// After the renderer backend's Shutdown() (which destroys every registered texture) and before
// ImGui::DestroyContext(). Owned textures are deleted later by their owners.
inline void shutdown() {
    for (ImTextureData* t : g_retired) { ImGui::UnregisterUserTexture(t); delete t; }
    for (ImTextureData* t : g_live) ImGui::UnregisterUserTexture(t);
    for (ImTextureData* t : g_pool) delete t;
    g_retired.clear(); g_live.clear(); g_pool.clear();
    g_closed = true;
}

// A texture for an image rebuilt every frame. Slots are handed out in call order and reused the
// next frame, so a view that goes away needs no cleanup. The capacity is rounded up so that
// resizing a window does not reallocate on every pixel: draw with uv = (w / Width, h / Height).
inline ImTextureData* frameTexture(int w, int h) {
    if (g_poolNext == (int)g_pool.size()) g_pool.push_back(nullptr);
    ImTextureData*& t = g_pool[g_poolNext++];
    if (!t || t->Width < w || t->Height < h) {
        const int cw = std::min(kMaxDim, (std::max(w, t ? t->Width : 0) + 255) & ~255);
        const int ch = std::min(kMaxDim, (std::max(h, t ? t->Height : 0) + 15) & ~15);
        if (t) { std::erase(g_live, t); t->WantDestroyNextFrame = true; g_retired.push_back(t); }
        t = create(cw, ch);
    }
    return t;
}

inline ImU32* pixels(ImTextureData* t) { return (ImU32*)t->GetPixels(); }
inline void   upload(ImTextureData* t, int x, int y, int w, int h) { ImTextureDataQueueUpload(t, x, y, w, h); }

// An owned texture of a fixed size, for an image updated in place (the spectrogram's column ring).
class Texture {
public:
    Texture() = default;
    ~Texture() { retire(t_); }
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    // Returns true when the texture is new, so every pixel has to be written.
    bool ensure(int w, int h) {
        if (t_ && t_->Width == w && t_->Height == h) return false;
        retire(t_);
        t_ = create(w, h);
        return true;
    }
    ImTextureData* get() const { return t_; }
private:
    ImTextureData* t_ = nullptr;
};

// Maps a value to the current ImPlot colormap as PlotHeatmap does (the same table and rounding),
// so the image and its ColormapScale agree. A NaN takes the low end; in PlotHeatmap it indexed
// outside the table.
class ColorScale {
public:
    ColorScale(double scaleMin, double scaleMax) : lo_(scaleMin), span_(scaleMax - scaleMin) {
        ImPlotContext& gp = *GImPlot;
        const ImPlotColormap cmap = gp.Style.Colormap;
        table_ = gp.ColormapData.GetTable(cmap);
        n_     = gp.ColormapData.GetTableSize(cmap);
        qual_  = gp.ColormapData.IsQual(cmap);
    }
    ImU32 operator()(double v) const {
        float t = span_ != 0.0 ? (float)((v - lo_) / span_) : 0.0f;
        t = (t > 0.0f) ? std::min(t, 1.0f) : 0.0f;   // NaN fails the test and lands at 0
        const int idx = qual_ ? std::clamp((int)(n_ * t), 0, n_ - 1) : (int)((n_ - 1) * t + 0.5f);
        return table_[idx];
    }
private:
    double       lo_, span_;
    const ImU32* table_ = nullptr;
    int          n_ = 1;
    bool         qual_ = false;
};

// Draws the texture region uv0..uv1 over the plot rectangle bmin..bmax, row 0 at the top as in
// PlotHeatmap. Nearest sampling gives one texel per cell, with hard edges like the quad per cell.
inline void plot(const char* id, ImTextureData* t, const ImPlotPoint& bmin, const ImPlotPoint& bmax,
                 const ImVec2& uv0, const ImVec2& uv1) {
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    ImPlot::PlotImage(id, t->GetTexRef(), bmin, bmax, uv0, uv1);
    if (pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
}

// A whole row-major rows x cols image of values, as PlotHeatmap(values, rows, cols, ...) would
// draw it, through a frame texture; falls back to PlotHeatmap past kMaxDim.
template <class T>
inline void plotValues(const char* id, const T* values, int rows, int cols, double scaleMin, double scaleMax,
                       const ImPlotPoint& bmin, const ImPlotPoint& bmax) {
    if (rows <= 0 || cols <= 0) return;
    if (rows > kMaxDim || cols > kMaxDim) {
        ImPlot::PlotHeatmap(id, values, rows, cols, scaleMin, scaleMax, nullptr, bmin, bmax);
        return;
    }
    ImTextureData* t = frameTexture(cols, rows);
    const ColorScale cs(scaleMin, scaleMax);
    ImU32* px = pixels(t);
    for (int r = 0; r < rows; ++r) {
        const T* src = values + (std::size_t)r * cols;
        ImU32*   dst = px + (std::size_t)r * t->Width;
        for (int c = 0; c < cols; ++c) dst[c] = cs((double)src[c]);
    }
    upload(t, 0, 0, cols, rows);
    plot(id, t, bmin, bmax, ImVec2(0, 0), ImVec2((float)cols / t->Width, (float)rows / t->Height));
}

}  // namespace heat
