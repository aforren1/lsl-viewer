# lsl-sdl — live LSL stream viewer

A real-time, EEG-style viewer for [Lab Streaming Layer](https://github.com/sccn/labstreaminglayer)
streams. SDL3 + SDL_GPU + Dear ImGui (docking) + ImPlot + liblsl, C++20.

See [DESIGN.md](DESIGN.md) for architecture and rationale.

## Repository layout

```
src/                     the viewer — one translation unit + header-only modules
  main.cpp                 app entry: UI, dock layout, render loop, recording UI
  hf_stream_source.hpp     LSL inlet -> ring buffer on a worker thread
  magic_ring_buffer.hpp    mirrored (contiguous-wraparound) lock-free ring buffer
  minmax_summary.hpp       decimated min/max envelope for zoomed-out plots
  filter.hpp               DC-blocker / running high-pass
  fft.hpp                  FFT + PSD (spectrum and spectrogram views)
  recorder.hpp             XDF recording driver (records connected streams)
  xdf_writer.hpp           XDF container writer
  remote_control.hpp       TCP remote-control server (start/stop/status)
  profiler.hpp             zone-profiling macros (no-ops unless -DLSL_TRACY)

tools/                   standalone helpers (not linked into the viewer)
  lsl_test_streams.py      synthetic LSL sources for testing (EEG, sine, chirp, markers)
  xdf_record.cpp           headless XDF recorder CLI (no GUI deps)

tests/                   Dear ImGui Test Engine UI tests + screenshot captures
  ui_tests.cpp             enabled with -DLSL_TESTS=ON; run via `lsl_viewer --tests`

CMakeLists.txt           all dependencies are fetched at configure time (FetchContent)
run.sh                   WSLg launcher (points SDL at the Wayland runtime dir)
```

Build trees (`build/`, `build-tracy/`), the Python venv (`.venv/`), captured
screenshots (`output/`), recordings (`*.xdf`), and the persisted ImGui layout
(`imgui.ini`) are generated and git-ignored.

## Build & run

All third-party libraries (SDL3, liblsl, Dear ImGui, ImPlot, spdlog, and
optionally Tracy / the ImGui Test Engine) are pulled in by CMake — no system
packages to install beyond a C++20 compiler and CMake ≥ 3.23.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lsl_viewer          # on WSL: use ./run.sh (sets the WSLg Wayland socket)
```

Feed it test data — the script carries inline metadata (PEP 723), so
[uv](https://docs.astral.sh/uv/) resolves `pylsl`/`numpy` automatically
(or run it with plain `python` in a venv that has them):

```bash
uv run tools/lsl_test_streams.py --streams eeg,sine,chirp,markers
```

### Optional CMake flags

| Flag | Effect |
|------|--------|
| `-DLSL_TESTS=ON`  | build the UI test suite; run headless with `lsl_viewer --tests` |
| `-DLSL_TRACY=ON`  | enable the Tracy frame profiler (connect the Tracy server to view) |

### Windows

The sources are portable (SDL_GPU uses the native D3D12 backend); only the build
artifacts are platform-specific. Copy the **source** (not `build*/` or `.venv/`)
to a native Windows path, install Visual Studio 2022 + CMake ≥ 3.23, then run the
same `cmake` configure/build (drop the Linux-only `-DSDL_X11=OFF`). `run.sh` and
the Wayland environment are not needed — just launch `lsl_viewer.exe`.
