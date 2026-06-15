// Minimal SDL3 + SDL_GPU + Dear ImGui app to diagnose the "text input cursor is missing"
// issue (reported on Windows and WSLg/Wayland). It uses the SAME platform backend as the
// viewer (imgui_impl_sdl3), which is what manages OS mouse cursors, so it reproduces the
// real behavior in ~150 lines with no LSL/recording/plot machinery in the way.
//
// Build:  cmake --build build --target cursor_test
// Run:    ./build/cursor_test     (WSL: XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir SDL_VIDEODRIVER=wayland ./build/cursor_test)
//
// Hover the text box: the OS cursor should become an I-beam. The on-screen panel reports what
// ImGui is requesting and what SDL is doing, so a missing/blank cursor can be pinned to a
// specific layer (ImGui asks for the wrong cursor / SDL can't create it / SDL hides it).

#include <SDL3/SDL.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

static const char* CursorName(ImGuiMouseCursor c) {
    switch (c) {
        case ImGuiMouseCursor_None:       return "None (cursor hidden)";
        case ImGuiMouseCursor_Arrow:      return "Arrow";
        case ImGuiMouseCursor_TextInput:  return "TextInput (I-beam)";
        case ImGuiMouseCursor_ResizeAll:  return "ResizeAll";
        case ImGuiMouseCursor_ResizeNS:   return "ResizeNS";
        case ImGuiMouseCursor_ResizeEW:   return "ResizeEW";
        case ImGuiMouseCursor_ResizeNESW: return "ResizeNESW";
        case ImGuiMouseCursor_ResizeNWSE: return "ResizeNWSE";
        case ImGuiMouseCursor_Hand:       return "Hand";
        default:                          return "?";
    }
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }

    // Up-front: can SDL even create the system cursors on this platform/theme? A null here for
    // SDL_SYSTEM_CURSOR_TEXT is the simplest explanation for a missing I-beam.
    SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    SDL_Cursor* text  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    std::printf("SDL_CreateSystemCursor(DEFAULT) = %p\n", (void*)arrow);
    std::printf("SDL_CreateSystemCursor(TEXT)    = %p   <- null/blank here would explain it\n", (void*)text);
    std::printf("SDL video driver = %s\n", SDL_GetCurrentVideoDriver());
    std::fflush(stdout);

    SDL_Window* window = SDL_CreateWindow("ImGui cursor test", 720, 480, SDL_WINDOW_RESIZABLE);
    if (!window) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }

    SDL_GPUDevice* gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
        SDL_GPU_SHADERFORMAT_MSL  | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
    if (!gpu || !SDL_ClaimWindowForGPUDevice(gpu, window)) {
        SDL_Log("GPU init: %s", SDL_GetError()); return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init = {};
    init.Device            = gpu;
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    init.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);

    char buf[128] = "hover me — the pointer should turn into an I-beam";
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(10); continue; }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(680, 420), ImGuiCond_FirstUseEver);
        ImGui::Begin("text cursor diagnostics");

        ImGui::TextUnformatted("Text inputs (hover each — pointer should become an I-beam):");
        ImGui::InputText("single line", buf, sizeof buf);
        static char multi[256] = "multi-line input";
        ImGui::InputTextMultiline("multi line", multi, sizeof multi, ImVec2(-1, 60));
        ImGui::Separator();

        // What ImGui asks for vs what the SDL3 backend should do with it. The backend hides the
        // OS cursor only when MouseDrawCursor is set OR the requested cursor is None; otherwise
        // it SDL_SetCursor()s the matching system cursor (falling back to Arrow if null).
        ImGui::Text("ImGui::GetMouseCursor() = %d (%s)",
                    ImGui::GetMouseCursor(), CursorName(ImGui::GetMouseCursor()));
        ImGui::Text("io.WantCaptureMouse = %d   io.WantTextInput = %d",
                    io.WantCaptureMouse, io.WantTextInput);
        ImGui::Text("io.MouseDrawCursor = %d   (ImGui draws the cursor itself when set)",
                    io.MouseDrawCursor);
        ImGui::Text("SDL_CursorVisible() = %d", SDL_CursorVisible());
        ImGui::Text("SDL TEXT system cursor created: %s", text ? "yes (non-null)" : "NO (null!)");
        // The active OS cursor. When hovering a text box this SHOULD be the TEXT cursor — if it
        // is (and CursorVisible=1) but you still see no I-beam, SDL set a cursor the OS won't
        // render; if it's NOT the TEXT cursor, the imgui_impl_sdl3 backend never applied it.
        SDL_Cursor* active = SDL_GetCursor();
        ImGui::Text("SDL_GetCursor() = %p   (== TEXT? %s   == Arrow? %s)", (void*)active,
                    active == text ? "YES" : "no", active == arrow ? "YES" : "no");
        ImGui::Text("SDL_GetDefaultCursor() = %p", (void*)SDL_GetDefaultCursor());
        ImGui::Text("SDL_GetCurrentVideoDriver() = %s", SDL_GetCurrentVideoDriver());
        ImGui::Separator();

        ImGui::Checkbox("io.MouseDrawCursor (software cursor — works everywhere, ~1 frame lag)",
                        &io.MouseDrawCursor);
        if (ImGui::Button("Force-SDL_SetCursor(TEXT) now") && text) SDL_SetCursor(text);
        ImGui::SameLine();
        if (ImGui::Button("Reset to Arrow") && arrow) SDL_SetCursor(arrow);
        ImGui::TextDisabled("If 'Force-SDL_SetCursor(TEXT)' shows nothing, the OS/theme has no\n"
                            "I-beam cursor and the software cursor (checkbox above) is the fix.");
        ImGui::End();

        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
        SDL_GPUTexture* swap = nullptr; Uint32 w = 0, h = 0;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h);
        if (swap && dd->DisplaySize.x > 0 && dd->DisplaySize.y > 0) {
            ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
            SDL_GPUColorTargetInfo target = {};
            target.texture     = swap;
            target.clear_color = SDL_FColor{0.10f, 0.10f, 0.12f, 1.0f};
            target.load_op     = SDL_GPU_LOADOP_CLEAR;
            target.store_op    = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    SDL_WaitForGPUIdle(gpu);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
