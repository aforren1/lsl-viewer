cmake_minimum_required(VERSION 3.22)
project(lsl_viewer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimized-with-symbols by default (single-config generators) so perf is meaningful;
# override with -DCMAKE_BUILD_TYPE=Debug when debugging.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(LSL_TRACY "Enable the Tracy frame profiler" OFF)
option(LSL_TESTS "Build with the Dear ImGui Test Engine and UI tests" OFF)
option(LSL_STATIC "Static-link SDL3 + liblsl (+ static MSVC CRT) into one self-contained binary" OFF)

# Static linking: fold SDL3 and liblsl into the executable so distribution is a
# single file (no SDL3.dll/lsl.dll alongside; on Windows also no VC++ redist).
# SDL aliases SDL3::SDL3 -> the static target when only static is built, and
# liblsl's `lsl` target keeps its name + propagates LIBLSL_STATIC (PUBLIC), so the
# link/compile lines below are unchanged. Mostly a Windows convenience; on Linux
# the GPU driver / glibc stay dynamic regardless, so prefer AppImage for shipping.
if(LSL_STATIC)
  set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
  set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
  set(LSL_BUILD_STATIC ON  CACHE BOOL "" FORCE)
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

include(FetchContent)

# --- SDL3 -------------------------------------------------------------------
# Pin GIT_TAG to the latest SDL3 stable release (3.2.x line).
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.4.10     # latest stable 3.4.x
  GIT_SHALLOW    TRUE
)
# Trim SDL to the subsystems the viewer actually uses: video + SDL_GPU (Vulkan/D3D12)
# for the window, events/timer (always built), and the file dialog (the recorder's
# folder picker, SDL_ShowOpenFolderDialog). Everything else is dead weight; disabling
# it shrinks the build and the binary. SDL_RENDER is the 2D SDL_Renderer API, which we
# never touch (Dear ImGui drives SDL_GPU directly) so it goes too.
set(SDL_AUDIO    OFF CACHE BOOL "" FORCE)
set(SDL_RENDER   OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA   OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC   OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI   OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR   OFF CACHE BOOL "" FORCE)
set(SDL_POWER    OFF CACHE BOOL "" FORCE)
# The viewer never synthesizes input or warps the mouse globally, so it doesn't need the
# X11 XTest extension. Turning it off drops the libXtst build dependency on Linux. (The
# other X11 extensions SDL checks are kept; they cover normal windowing.)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)

# --- liblsl -----------------------------------------------------------------
# Provides the `lsl` target. Pin to a current release tag.
FetchContent_Declare(liblsl
  GIT_REPOSITORY https://github.com/sccn/liblsl.git
  GIT_TAG        v1.17.7            # latest release
  GIT_SHALLOW    TRUE
)

# --- Dear ImGui (docking branch) --------------------------------------------
# imgui ships no CMakeLists by design; we build the sources we need below.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        docking
  GIT_SHALLOW    TRUE
)

# --- ImPlot -----------------------------------------------------------------
FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

# --- spdlog (logging) -------------------------------------------------------
set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.1
  GIT_SHALLOW    TRUE
)

# --- KissFFT (real FFT for the Spectrum/Spectrogram PSD) --------------------
# BSD-3-Clause (permissive, like the rest); float datatype, static, no tools/tests.
set(KISSFFT_DATATYPE float        CACHE STRING "" FORCE)
set(KISSFFT_STATIC    ON          CACHE BOOL   "" FORCE)
set(KISSFFT_TEST      OFF         CACHE BOOL   "" FORCE)
set(KISSFFT_TOOLS     OFF         CACHE BOOL   "" FORCE)
set(KISSFFT_PKGCONFIG OFF         CACHE BOOL   "" FORCE)
FetchContent_Declare(kissfft
  GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
  GIT_TAG        131.1.0
  GIT_SHALLOW    TRUE
)

# --- Tracy (optional frame profiler) ----------------------------------------
if(LSL_TRACY)
  # On-demand: collect only while a profiler server is connected, so an
  # unattended run doesn't pile profiling data into memory indefinitely.
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.13.1
    GIT_SHALLOW    TRUE
  )
endif()

# --- Dear ImGui Test Engine (optional UI testing + capture) -----------------
if(LSL_TESTS)
  FetchContent_Declare(imgui_test_engine
    GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
  )
endif()

FetchContent_MakeAvailable(SDL3 liblsl imgui implot spdlog kissfft)
if(LSL_TRACY)
  FetchContent_MakeAvailable(tracy)
endif()
if(LSL_TESTS)
  FetchContent_MakeAvailable(imgui_test_engine)
endif()

# Build imgui core + SDL3/SDL_GPU backends + implot as one static lib.
add_library(ui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(ui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${implot_SOURCE_DIR}
)
target_link_libraries(ui PUBLIC SDL3::SDL3)

# Test Engine hooks must be compiled INTO imgui (and seen by every consumer, or
# the ImGuiContext layout diverges) — so the define is PUBLIC on `ui`.
if(LSL_TESTS)
  target_compile_definitions(ui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
  set(TE_DIR ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine)
  add_library(imgui_te STATIC
    ${TE_DIR}/imgui_te_context.cpp
    ${TE_DIR}/imgui_te_coroutine.cpp
    ${TE_DIR}/imgui_te_engine.cpp
    ${TE_DIR}/imgui_te_exporters.cpp
    ${TE_DIR}/imgui_te_perftool.cpp
    ${TE_DIR}/imgui_te_ui.cpp
    ${TE_DIR}/imgui_te_utils.cpp
    ${TE_DIR}/imgui_capture_tool.cpp
  )
  target_include_directories(imgui_te PUBLIC
    ${imgui_test_engine_SOURCE_DIR}
    ${TE_DIR}/thirdparty/Str
  )
  target_link_libraries(imgui_te PUBLIC ui)
  # imgui_te_imconfig.h (which would default these) is not auto-included, so set
  # them explicitly: std::thread coroutine backend (else IO.CoroutineFuncs is
  # null -> crash in ImGuiTestEngine_Start) and screen capture (else captures are
  # skipped at runtime). stb_image_write is bundled in imgui_capture_tool.cpp.
  target_compile_definitions(imgui_te PUBLIC
    IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
    IMGUI_TEST_ENGINE_ENABLE_CAPTURE=1)
  target_compile_options(imgui_te PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/Z7,-g>)  # debug symbols (MSVC: /Z7 not -g)
endif()

add_executable(lsl_viewer src/main.cpp)
target_include_directories(lsl_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsl_viewer PRIVATE ui SDL3::SDL3 lsl spdlog::spdlog kissfft::kissfft)
if(WIN32)
  # VirtualAlloc2 / MapViewOfFile3 (magic_ring_buffer.hpp) live in onecore.lib;
  # ws2_32 = Winsock for the remote-control TCP server (remote_control.hpp).
  target_link_libraries(lsl_viewer PRIVATE onecore ws2_32)
  # SDL3.dll / lsl.dll are built in _deps subdirs; copy them next to the exe.
  # (No-op when LSL_STATIC: there are no runtime DLLs to copy.)
  if(NOT LSL_STATIC)
    add_custom_command(TARGET lsl_viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:lsl_viewer> $<TARGET_FILE_DIR:lsl_viewer>
      COMMAND_EXPAND_LISTS)
  endif()
endif()

# Headless XDF recorder CLI (no GUI deps) — same Recorder/xdf_writer as the viewer.
add_executable(xdf_record tools/xdf_record.cpp)
target_include_directories(xdf_record PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(xdf_record PRIVATE lsl spdlog::spdlog)
if(WIN32 AND NOT LSL_STATIC)
  add_custom_command(TARGET xdf_record POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:xdf_record> $<TARGET_FILE_DIR:xdf_record>
    COMMAND_EXPAND_LISTS)
endif()

if(LSL_TRACY)
  target_compile_definitions(lsl_viewer PRIVATE LSL_TRACY)
  target_link_libraries(lsl_viewer PRIVATE Tracy::TracyClient)
endif()

if(LSL_TESTS)
  target_sources(lsl_viewer PRIVATE tests/ui_tests.cpp)
  target_compile_definitions(lsl_viewer PRIVATE LSL_TESTS)
  target_link_libraries(lsl_viewer PRIVATE imgui_te)
  target_compile_options(lsl_viewer PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/Z7,-g>)
endif()

# Link-time optimization for shipping builds — trims unreferenced library code
# (measured ~45% smaller binary, mostly LTO dropping unused ImGui/ImPlot incl.
# imgui_demo). Negligible runtime gain (the app is GPU-bound and the hot CPU code
# is already header-only/single-TU). Enabled for Release, but NOT for the test
# build (LSL_TESTS = the iteration loop) nor the default RelWithDebInfo dev build,
# so iterative links stay fast.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT LSL_TESTS)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg LANGUAGES CXX)  # project is CXX-only
  if(_ipo_ok)
    set_property(TARGET lsl_viewer ui PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(STATUS "IPO/LTO not supported: ${_ipo_msg}")
  endif()
endif()
