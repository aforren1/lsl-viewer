// Tools > Walkthrough: a checklist panel that takes a new user through the main features
// on the demo streams.
//
// The design goal is that the walkthrough cannot go stale without a failure somewhere:
//   - A step is complete when the application is in the state the step asks for (Facts,
//     which main() fills from the real variables each frame), not when the user says so.
//     A step that the UI can no longer complete stays open, and the `ui/walkthrough` test
//     fails.
//   - The highlight comes from anchor() calls that sit beside the widgets themselves, so
//     it moves with the widget, and it goes with the widget if someone deletes it. The
//     same test checks that each step got an anchor.
// Only the step text is prose that nothing checks.
//
// The panel is not modal: it never takes input from the rest of the window, so the user
// does each step in the real UI.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"   // RenderCheckMark

#include <cmath>

namespace tour {

enum class Step : int { demo, filter, pause, resume, spectrum, record, stop, replay, count };

// The application state that the steps look at.
struct Facts {
    int  dataStreams = 0;      // connected data streams
    bool notch       = false;  // a connected stream has its notch filter on (off by default)
    bool paused      = false;  // View > Pause display
    int  spectra     = 0;      // open spectrum windows
    bool recording   = false;
    bool replaying   = false;  // a Playback replay is playing
};

struct StepDef {
    const char* title;
    const char* body;
    const char* hidden;                // shown in place of the highlight when no anchor is on screen
    bool (*done)(const Facts&);
};

inline constexpr StepDef kSteps[(int)Step::count] = {
    { "Show the demo streams",
      "Select Tools > Emit demo streams. The viewer publishes synthetic streams and connects to "
      "them. Each data stream opens as a plot tab. The Streams rail on the left lists them.",
      nullptr,
      [](const Facts& f) { return f.dataStreams > 0; } },
    { "Apply a filter",
      "In the control strip on the left side of a plot, open Display, then select Notch. "
      "The notch filter removes the mains frequency (50 Hz or 60 Hz). The filters change the "
      "display only: a recording holds the raw data.",
      "No plot shows its controls. Click a stream in the Streams rail to bring its plot to the front.",
      [](const Facts& f) { return f.notch; } },
    { "Pause the display",
      "Push the P key, or select View > Pause display. The plots stop, and you can examine them. "
      "The data continues to come in.",
      nullptr,
      [](const Facts& f) { return f.paused; } },
    { "Continue the display",
      "Push the P key again.",
      nullptr,
      [](const Facts& f) { return !f.paused; } },
    { "Open an analysis window",
      "Select View > New spectrum. In the new window, select a stream and its channels. "
      "The View menu also has the spectrogram and the marker average (ERP).",
      nullptr,
      [](const Facts& f) { return f.spectra > 0; } },
    { "Start a recording",
      "In the Recording section of the Streams rail, the line that starts with -> shows the file "
      "that the viewer will write. Select Record. The recording holds all connected streams.",
      "The Record button is not on the screen. Scroll the Streams rail down.",
      [](const Facts& f) { return f.recording; } },
    { "Stop the recording",
      "Let the recording run for some seconds. Then select Stop recording.",
      "The Stop recording button is not on the screen. Scroll the Streams rail down.",
      [](const Facts& f) { return !f.recording; } },
    { "Check the recording",
      "In the Playback section of the Streams rail, select Open XDF file..., then the file that "
      "you recorded. The viewer plays the file back as live streams.",
      "The Open XDF file... button is not on the screen. Stop or close the current replay first.",
      [](const Facts& f) { return f.replaying; } },
};

struct State {
    bool     open       = false;
    int      cur        = 0;        // index of the first step not yet complete; count = finished
    int      anchorRank = -1;       // best (lowest) rank registered this frame; -1 = none
    ImVec2   anchorMin, anchorMax;
    unsigned seen       = 0;        // bit per step: its rank 0 anchor was on screen while it was current (for the test)
};
inline State g;

inline void start() { g = State{}; g.open = true; }

// Call once a frame, before the UI is built: the anchors of this frame belong to the step
// that is current from here on.
inline void update(const Facts& f) {
    g.anchorRank = -1;
    if (!g.open) return;
    while (g.cur < (int)Step::count && kSteps[g.cur].done(f)) ++g.cur;
}

// Call right after the widget that does step `s`. A step can have several anchors: `rank`
// 0 is the widget itself, higher ranks lead to it (a menu that holds it, a header that
// hides it), and the lowest rank on screen gets the highlight.
inline void anchor(Step s, int rank = 0) {
    if (!g.open || (int)s != g.cur) return;
    if (!ImGui::IsItemVisible()) return;
    if (rank == 0) g.seen |= 1u << (int)s;
    if (g.anchorRank >= 0 && rank >= g.anchorRank) return;
    g.anchorRank = rank;
    g.anchorMin  = ImGui::GetItemRectMin();
    g.anchorMax  = ImGui::GetItemRectMax();
}

// Call once a frame, after all other windows: the panel, and the highlight on top of everything.
inline void draw() {
    if (!g.open) return;
    const int   n  = (int)Step::count;
    const float fs = ImGui::GetFontSize();

    if (g.cur < n && g.anchorRank >= 0) {
        // Amber reads on both themes and is not a color that the plots or the panel accents use.
        const float pulse = 0.75f + 0.25f * std::sin((float)ImGui::GetTime() * 5.0f);
        const float pad   = fs * 0.2f;
        ImGui::GetForegroundDrawList()->AddRect(
            ImVec2(g.anchorMin.x - pad, g.anchorMin.y - pad), ImVec2(g.anchorMax.x + pad, g.anchorMax.y + pad),
            ImGui::GetColorU32(ImVec4(1.0f, 0.69f, 0.13f, pulse)), fs * 0.25f, fs * 0.14f);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - fs, vp->WorkPos.y + fs * 3.0f),
                            ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(fs * 23.0f, 0.0f));   // fixed width for the wrapped text; the height fits
    // Not docked and not saved: a workspace or a layout reset must not hide it in a tab. It must
    // not take the focus when it appears either, or the P key of the pause step goes nowhere.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Walkthrough", &g.open, flags)) {
        const float indent = fs * 1.3f;
        if (g.cur < n) ImGui::TextDisabled("Step %d of %d", g.cur + 1, n);
        else           ImGui::TextDisabled("Complete");
        ImGui::Separator();
        for (int i = 0; i < n; ++i) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (i < g.cur)
                ImGui::RenderCheckMark(dl, ImVec2(p.x + fs * 0.15f, p.y + fs * 0.2f),
                                       ImGui::GetColorU32(ImGuiCol_TextDisabled), fs * 0.65f);
            else if (i == g.cur)
                dl->AddCircleFilled(ImVec2(p.x + fs * 0.45f, p.y + fs * 0.55f), fs * 0.2f, ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::Indent(indent);
            if (i == g.cur) {
                ImGui::TextUnformatted(kSteps[i].title);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(kSteps[i].body);
                if (g.anchorRank < 0 && kSteps[i].hidden != nullptr) ImGui::TextDisabled("%s", kSteps[i].hidden);
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
            } else {
                ImGui::TextDisabled("%s", kSteps[i].title);
            }
            ImGui::Unindent(indent);
        }
        ImGui::Separator();
        if (g.cur < n) {
            if (ImGui::Button("Skip step")) ++g.cur;
            ImGui::SameLine();
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("You used the main features. The documentation at "
                                   "github.com/aforren1/lsl-viewer has the guides for your own devices, "
                                   "the analysis windows, the file names, and the remote control.");
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Close")) g.open = false;
            ImGui::SameLine();
        }
        if (ImGui::Button("Start again")) { g.cur = 0; g.seen = 0; }
    }
    ImGui::End();
}

}  // namespace tour
