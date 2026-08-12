#pragma once
// UI theme + branding, kept out of main.cpp: the light/dark color palette
// (mostly-grayscale chrome with electric-blue + purple accents) and the embedded
// Roboto font. applyTheme() is safe to call at runtime to toggle light/dark.

#include "imgui.h"
#include "implot.h"
#include "roboto_font.h"   // embedded Roboto-Regular (Apache-2.0)

#include <SDL3/SDL.h>
#include <cmath>

// User-facing UI scale (persisted in imgui.ini as `scale=`). Defaults to 1.0 on every platform —
// like most cross-platform apps we let the OS handle DPI rather than baking in a per-OS scale, and
// retina/DPI crispness is automatic (ImGui 1.92's dynamic atlas rasterizes at the backend-provided
// framebuffer density, no manual factor). Applied in applyTheme() via style.FontScaleMain (fonts) +
// ScaleAllSizes (paddings/borders); a loaded ini value overrides it, and the user can adjust it from
// the App menu.
inline float g_uiScale = 1.0f;

// Scale a hard-coded pixel size by the UI scale. ScaleAllSizes (paddings/borders) and FontScaleMain
// (fonts) cover the style + text automatically, but raw pixel literals passed to layout calls —
// panel/rail widths, initial window sizes, fixed item widths, plot label-reserve margins — do not.
// Wrap those in uiScaled(). Do NOT wrap values already derived from text metrics (CalcTextSize) or
// style fields (ItemSpacing): those are scaled once already and would compound.
inline float uiScaled(float px) { return px * g_uiScale; }

// Apply a light or dark theme to both ImGui and ImPlot, plus shared style polish.
inline void applyTheme(bool light) {
    if (light) { ImGui::StyleColorsLight(); ImPlot::StyleColorsLight(); }
    else       { ImGui::StyleColorsDark();  ImPlot::StyleColorsDark(); }
    ImGuiStyle& s = ImGui::GetStyle();

    // Mostly-grayscale neutrals; blue + purple are the ONLY accents (checkmark,
    // hover, active/pressed, selected-tab overline, text selection) so the chrome
    // stays quiet and the highlights read. Blue = electric/royal hue (the palette's,
    // not imgui's azure); purple marks the strongest active/selected states.
    auto rgb = [](int r, int g, int b, float a = 1.0f) {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
    };
    auto A = [](ImVec4 v, float a) { v.w = a; return v; };
    ImVec4* c = s.Colors;
    if (light) {
        const ImVec4 blue = rgb(0x1B, 0x33, 0xE6), purp = rgb(0x7B, 0x22, 0xA8);  // electric blue + purple (palette)
        c[ImGuiCol_Text]                 = rgb(0x1E, 0x1E, 0x22);   // near-black ink
        c[ImGuiCol_TextDisabled]         = rgb(0x77, 0x77, 0x7F);
        c[ImGuiCol_WindowBg]             = rgb(0xF4, 0xF4, 0xF6);
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = rgb(0xFF, 0xFF, 0xFF, 0.98f);
        c[ImGuiCol_Border]               = rgb(0x1E, 0x1E, 0x22, 0.55f);   // bold window outlines / dividers
        c[ImGuiCol_FrameBg]              = rgb(0xE7, 0xE7, 0xEA);
        c[ImGuiCol_FrameBgHovered]       = rgb(0xDB, 0xDB, 0xDF);
        c[ImGuiCol_FrameBgActive]        = rgb(0xCF, 0xCF, 0xD4);
        c[ImGuiCol_TitleBg]              = rgb(0xDA, 0xDA, 0xDF);
        c[ImGuiCol_TitleBgActive]        = rgb(0xD6, 0xD6, 0xDB);   // title strip stays neutral; only the active TAB accents
        c[ImGuiCol_MenuBarBg]            = rgb(0xEC, 0xEC, 0xEF);
        c[ImGuiCol_Header]               = A(blue, 0.18f);
        c[ImGuiCol_HeaderHovered]        = A(blue, 0.32f);
        c[ImGuiCol_HeaderActive]         = A(purp, 0.42f);
        c[ImGuiCol_Button]               = rgb(0xDD, 0xDD, 0xE2);
        c[ImGuiCol_ButtonHovered]        = A(blue, 0.32f);
        c[ImGuiCol_ButtonActive]         = A(purp, 0.55f);
        c[ImGuiCol_CheckMark]            = blue;
        c[ImGuiCol_SliderGrab]           = rgb(0xA6, 0xA6, 0xAE);
        c[ImGuiCol_SliderGrabActive]     = purp;
        c[ImGuiCol_Tab]                  = rgb(0xDD, 0xDD, 0xE2);
        c[ImGuiCol_TabHovered]           = A(blue, 0.32f);
        c[ImGuiCol_TabSelected]          = rgb(0xC2, 0xCE, 0xF6);   // focused window's tab = blue accent tint
        c[ImGuiCol_TabSelectedOverline]  = purp;
        c[ImGuiCol_TabDimmed]            = rgb(0xE6, 0xE6, 0xE9);
        c[ImGuiCol_TabDimmedSelected]    = rgb(0xD4, 0xD4, 0xD9);
        c[ImGuiCol_Separator]            = rgb(0x1E, 0x1E, 0x22, 0.55f);   // dock splitters between windows
        c[ImGuiCol_SeparatorHovered]     = A(blue, 0.55f);
        c[ImGuiCol_ResizeGrip]           = rgb(0x1E, 0x1E, 0x22, 0.12f);
        c[ImGuiCol_ResizeGripHovered]    = A(blue, 0.45f);
        c[ImGuiCol_ResizeGripActive]     = A(purp, 0.80f);
        c[ImGuiCol_ScrollbarBg]          = rgb(0xEC, 0xEC, 0xEF);
        c[ImGuiCol_ScrollbarGrab]        = rgb(0xC4, 0xC4, 0xCA);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0xB2, 0xB2, 0xBA);
        c[ImGuiCol_ScrollbarGrabActive]  = A(blue, 0.70f);
        c[ImGuiCol_TextSelectedBg]       = A(blue, 0.25f);
        c[ImGuiCol_NavCursor]            = blue;
        c[ImGuiCol_DockingPreview]       = A(blue, 0.45f);
        c[ImGuiCol_PlotLines]            = blue;
        c[ImGuiCol_PlotHistogram]        = purp;
    } else {
        // Palette electric-blue + purple, lifted in lightness for dark-gray contrast but
        // kept at the palette's royal/violet HUE (not imgui's lighter azure).
        const ImVec4 blue = rgb(0x3D, 0x50, 0xEE), purp = rgb(0x9E, 0x3A, 0xD0);
        c[ImGuiCol_Text]                 = rgb(0xE8, 0xE8, 0xEC);
        c[ImGuiCol_TextDisabled]         = rgb(0x86, 0x86, 0x8E);
        c[ImGuiCol_WindowBg]             = rgb(0x18, 0x18, 0x1B);   // neutral dark gray
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg]              = rgb(0x20, 0x20, 0x24, 0.98f);
        c[ImGuiCol_Border]               = rgb(0x6A, 0x6A, 0x7C, 0.95f);   // bold window outlines / dividers
        c[ImGuiCol_FrameBg]              = rgb(0x28, 0x28, 0x2D);
        c[ImGuiCol_FrameBgHovered]       = rgb(0x33, 0x33, 0x39);
        c[ImGuiCol_FrameBgActive]        = rgb(0x3D, 0x3D, 0x44);
        c[ImGuiCol_TitleBg]              = rgb(0x1C, 0x1C, 0x20);
        c[ImGuiCol_TitleBgActive]        = rgb(0x2A, 0x2A, 0x30);   // title strip stays neutral; only the active TAB accents
        c[ImGuiCol_MenuBarBg]            = rgb(0x16, 0x16, 0x19);
        c[ImGuiCol_Header]               = A(blue, 0.26f);
        c[ImGuiCol_HeaderHovered]        = A(blue, 0.45f);
        c[ImGuiCol_HeaderActive]         = A(purp, 0.55f);
        c[ImGuiCol_Button]               = rgb(0x34, 0x34, 0x3A);
        c[ImGuiCol_ButtonHovered]        = A(blue, 0.45f);
        c[ImGuiCol_ButtonActive]         = A(purp, 0.80f);
        c[ImGuiCol_CheckMark]            = blue;
        c[ImGuiCol_SliderGrab]           = rgb(0x57, 0x57, 0x60);
        c[ImGuiCol_SliderGrabActive]     = purp;
        c[ImGuiCol_Tab]                  = rgb(0x22, 0x22, 0x27);
        c[ImGuiCol_TabHovered]           = A(blue, 0.45f);
        c[ImGuiCol_TabSelected]          = rgb(0x33, 0x40, 0x7E);   // focused window's tab = blue accent tint
        c[ImGuiCol_TabSelectedOverline]  = purp;
        c[ImGuiCol_TabDimmed]            = rgb(0x18, 0x18, 0x1B);
        c[ImGuiCol_TabDimmedSelected]    = rgb(0x28, 0x28, 0x2E);
        c[ImGuiCol_Separator]            = rgb(0x6A, 0x6A, 0x7C, 0.95f);   // dock splitters between windows
        c[ImGuiCol_SeparatorHovered]     = A(blue, 0.60f);
        c[ImGuiCol_ResizeGrip]           = rgb(0xFF, 0xFF, 0xFF, 0.10f);
        c[ImGuiCol_ResizeGripHovered]    = A(blue, 0.55f);
        c[ImGuiCol_ResizeGripActive]     = A(purp, 0.85f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = rgb(0x3A, 0x3A, 0x42);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x4A, 0x4A, 0x54);
        c[ImGuiCol_ScrollbarGrabActive]  = A(blue, 0.75f);
        c[ImGuiCol_TextSelectedBg]       = A(blue, 0.35f);
        c[ImGuiCol_NavCursor]            = blue;
        c[ImGuiCol_DockingPreview]       = A(blue, 0.60f);
        c[ImGuiCol_PlotLines]            = blue;
        c[ImGuiCol_PlotHistogram]        = purp;
    }

    // ImGui hardcodes DockingEmptyBg to a dark gray in BOTH styles, so the empty
    // dockspace (visible when no plots are open) stayed dark in the light theme.
    // Match it to the window background so it follows the theme.
    s.Colors[ImGuiCol_DockingEmptyBg] = s.Colors[ImGuiCol_WindowBg];
    s.WindowRounding    = 5.0f; s.ChildRounding = 4.0f; s.FrameRounding = 4.0f;
    s.PopupRounding     = 4.0f; s.GrabRounding  = 3.0f; s.TabRounding   = 4.0f;
    s.ScrollbarRounding = 9.0f;
    s.WindowBorderSize  = 2.0f; s.FrameBorderSize = 0.0f;   // bold outline around each (docked) window
    s.ChildBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(8, 8);
    s.FramePadding      = ImVec2(7, 4);
    s.ItemSpacing       = ImVec2(8, 5);
    s.AntiAliasedLinesUseTex = true;          // texture AA = fewer line vertices

    // Apply the UI scale: FontScaleMain scales every font (1.92 re-rasterizes crisply at the new
    // size), ScaleAllSizes grows/shrinks paddings, rounding, borders, spacing. Both run last on the
    // freshly-reset base values above, so re-calling applyTheme() after a scale change is idempotent
    // rather than compounding. (DPI density stays automatic — it's orthogonal to FontScaleMain.)
    s.FontScaleMain = g_uiScale;
    s.ScaleAllSizes(g_uiScale);
}

// Embedded Roboto-Regular (the face Tracy uses), loaded at its natural logical size. ImGui 1.92's
// dynamic font atlas rasterizes on demand at the display's framebuffer density (the SDL3 backend
// feeds it via DisplayFramebufferScale / Platform_GetWindowFramebufferScale), so retina stays crisp
// automatically — no manual DPI pre-scaling. Global UI scale rides on style.FontScaleMain (see
// applyTheme). Embedding (vs a system font path) keeps text identical everywhere and works in a
// static single-file build with no assets.
inline void loadEmbeddedFont(ImGuiIO& io) {
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 1;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoRegular_compressed_data, RobotoRegular_compressed_size, 15.0f, &cfg);
}
