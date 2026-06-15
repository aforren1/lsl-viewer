# Building & running

## Prebuilt binaries

Every push builds binaries on CI ([.github/workflows/build.yml](../.github/workflows/build.yml)) —
grab them from the run's **Artifacts**:

- `lsl-viewer-linux` / `lsl-viewer-linux-aarch64` / `lsl-viewer-macos` / `lsl-viewer-windows`
  — self-contained (static) `lsl_viewer` + `xdf_record` for each OS/arch, plus `LICENSE`,
  `THIRD_PARTY_LICENSES`, and `portable.txt` (a [portable](#data-and-config-locations) build).
- `lsl-viewer-windows-installer` — **`lsl-viewer-setup.exe`**, an Inno Setup installer that
  drops the app into `Program Files` and uses the standard per-user locations.
- `lsl-viewer-macos-dmg` — **`LSL-Viewer.dmg`**, a drag-to-Applications disk image with an
  `LSL Viewer.app` bundle (and the `xdf_record` CLI alongside it).
- `lsl-viewer-appimage-x86_64` / `lsl-viewer-appimage-aarch64` — a portable
  **`LSL-Viewer-<arch>.AppImage`** built on an older glibc and with **both X11 and Wayland**
  backends, so it runs across desktops and distros (the host still provides the GPU driver /
  Vulkan loader, as always for a GPU app).
- `xdf-record-linux-musl` — the headless **`xdf_record`** recorder built **fully static against
  musl** (CLI only; `-DLSL_CLI_ONLY=ON`). It has **no glibc and no shared-library dependencies**
  (`ldd` says "not a dynamic executable"), so the single ~1.3 MB binary runs on **any** Linux —
  ancient glibc, Alpine/musl, minimal containers — with nothing to install. The GUI viewer can't
  be fully static (it needs the host's GPU driver + display libraries at runtime), which is why
  only the recorder ships this way.

The per-OS `lsl-viewer-linux` artifact is built on the latest Ubuntu (newer glibc); for
broad Linux portability prefer the AppImage (viewer) or the musl `xdf_record` (recorder).

None of the artifacts are code-signed, so first launch trips Gatekeeper on macOS
(right-click → Open, or `xattr -dr com.apple.quarantine "LSL Viewer.app"`) and SmartScreen
on Windows (More info → Run anyway).

## Build from source

All third-party libraries (SDL3, liblsl, Dear ImGui, ImPlot, spdlog, KissFFT, and
optionally Tracy / the ImGui Test Engine) are pulled in by CMake via `FetchContent` — so
besides a C++20 compiler and CMake ≥ 3.23, the only system packages are SDL3's display
backends: on **Linux** install the X11 + Wayland dev headers (`libx11-dev libxext-dev
libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxkbcommon-dev libwayland-dev
wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev` …; SDL build-errors if
`SDL_X11=ON` and the X11 headers are missing — `-DSDL_X11=OFF` drops that to Wayland-only).
**macOS** and **Windows** need nothing extra.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lsl_viewer          # on WSL: use ./run.sh (sets the WSLg Wayland socket)
```

## Test data

The viewer needs LSL streams to show. `tools/lsl_test_streams.py` publishes
synthetic ones (EEG with a 10‑20 montage, sine, chirp, an evoked-response demo
with markers, a 48 kHz "audio" stream, a flaky reconnecting stream, a sub‑Hz
drift stream, and more). It carries inline dependency metadata (PEP 723), so
[uv](https://docs.astral.sh/uv/) resolves `pylsl`/`numpy` automatically — or run
it with plain `python` in a venv that has them:

```bash
uv run tools/lsl_test_streams.py --streams eeg,sine,chirp,markers,evoked
uv run tools/lsl_test_streams.py --help          # all streams + tunables
```

By default the viewer waits for you to connect streams from the **Streams** rail;
set `LSL_AUTOCONNECT=1` to auto-connect everything it discovers.

## Data and config locations

User data and app state are kept separate:

- **Recordings** default to a visible user folder — `~/Documents/lsl-recordings` (falling back to
  your home directory); change it per session in the Recording panel.
- **Config and state** (`imgui.ini`, saved workspaces) go to the OS app-data directory:
  `%APPDATA%\lsl_viewer\` (Windows), `~/Library/Application Support/lsl_viewer/` (macOS), or
  `~/.local/share/lsl_viewer/` (Linux).

**Portable mode** instead keeps everything together in a `lsl_viewer_data/` folder beside the
executable — handy for a self-contained directory or a USB stick. Enable it with `LSL_PORTABLE=1`,
or by dropping an empty `portable.txt` next to the binary (or next to the `.AppImage`).

## Recording conformance test

`tests/compare_labrecorder.py` records the mock streams with both our headless recorder
(`xdf_record`, the same `Recorder`/`xdf_writer` as the viewer) and **LabRecorder**
(`LabRecorderCLI`) at the same time, then checks that the two XDFs hold identical sample
values and timestamps for every stream over a shared window. CI runs it on Linux (the
`recording-vs-labrecorder` job installs LabRecorderCLI + liblsl from the LSL releases).
Locally it needs `LabRecorderCLI` on `PATH` and a built `xdf_record`:

```bash
uv run tests/compare_labrecorder.py        # all mock streams, incl. 48 kHz audio
```

## Optional CMake flags

| Flag | Effect |
|------|--------|
| `-DLSL_TESTS=ON`  | build the UI test suite; run headless with `lsl_viewer --tests [query]` |
| `-DLSL_TRACY=ON`  | enable the [Tracy](https://github.com/wolfpld/tracy) frame profiler (connect the Tracy server to view) |
| `-DLSL_STATIC=ON` | static-link SDL3 + liblsl into one self-contained binary (see below) |
| `-DLSL_CLI_ONLY=ON` | build **only** the headless `xdf_record` (skip all GUI deps: SDL3/ImGui/ImPlot/KissFFT). With `-DLSL_STATIC=ON` + a musl toolchain + `-DCMAKE_EXE_LINKER_FLAGS=-static` → a fully-static recorder with no glibc/GPU deps |
| `-DSDL_X11=OFF`   | Linux/WSL: build the Wayland backend only |

There's also a lightweight built-in text profiler: run with `LSL_PROFILE=1` for a
per-zone timing table, or `LSL_BENCH=1` for an FPS / CPU-build / GPU-submit
readout (no Tracy needed).

## Static build (single-file distribution)

`-DLSL_STATIC=ON` folds SDL3 and liblsl into the executable so there's nothing to
ship alongside it:

```bash
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release -DLSL_STATIC=ON
cmake --build build-static
```

This is primarily a **Windows** convenience: the default (dynamic) build drops
`SDL3.dll`/`lsl.dll` next to the exe, whereas the static build needs none — and it
also switches the MSVC C runtime to static (`/MT`), so the target machine doesn't
need the Visual C++ redistributable. The UI font is embedded either way, so a
static Windows build is a genuinely standalone `lsl_viewer.exe`.

On Linux the GPU driver and glibc stay dynamic regardless (SDL loads the Vulkan
loader at runtime), so static linking buys less there — for distribution prefer the
**AppImage** (the CI `appimage` job wraps the static build with a desktop entry + icon
from [packaging/](../packaging/), built on an older glibc with both display backends).
The graphics driver is always a system component on both platforms.

**Link-time optimization** is enabled automatically for `Release` builds (but not
the test build or the default `RelWithDebInfo` dev build, so iterative links stay
fast). It mostly drops unreferenced ImGui/ImPlot code — measured **~45% smaller**
on the static binary (~10 MB → ~5.6 MB stripped). Runtime is unchanged (the app is
GPU-bound). No flag needed; it's on whenever `-DCMAKE_BUILD_TYPE=Release` and the
compiler supports IPO.

## Windows

The sources are portable (SDL_GPU uses the native D3D12 backend); only the build
artifacts are platform-specific. Copy the **source** (not `build*/` or `.venv/`)
to a native Windows path, install Visual Studio 2022 + CMake ≥ 3.23, then run the
same `cmake` configure/build (drop the Linux-only `-DSDL_X11=OFF`). `run.sh` and
the Wayland environment are not needed — just launch `lsl_viewer.exe`.

The remote-control TCP server uses Winsock on Windows; to verify that path at
runtime see [windows-remote-control-testing.md](windows-remote-control-testing.md).

## Repository layout

```
src/                     the viewer — one translation unit + header-only modules
  main.cpp                 app entry: UI, dock layout, render loop, recording UI
  hf_stream_source.hpp     LSL inlet -> ring buffers + filter chain on a worker thread
  magic_ring_buffer.hpp    mirrored (contiguous-wraparound) lock-free ring buffer
  minmax_summary.hpp       decimated min/max envelope for zoomed-out plots
  filter.hpp               DC-blocker high-pass + RBJ biquads (notch / low-pass)
  fft.hpp                  FFT + PSD (spectrum and spectrogram views)
  recorder.hpp             XDF recording driver (records connected streams)
  xdf_writer.hpp           XDF container writer
  remote_control.hpp       TCP remote-control server (start/stop/status)
  theme.hpp                UI theme + embedded font
  profiler.hpp             zone-profiling macros (text profiler / Tracy / no-op)

tools/                   standalone helpers (not linked into the viewer)
  lsl_test_streams.py      synthetic LSL sources for testing
  xdf_record.cpp           headless XDF recorder CLI (no GUI deps)

tests/                   Dear ImGui Test Engine UI tests + screenshot captures
  ui_tests.cpp             enabled with -DLSL_TESTS=ON; run via `lsl_viewer --tests`
  compare_labrecorder.py   records the mock streams with our recorder + LabRecorder and
                           checks the XDFs are identical (needs LabRecorderCLI; runs locally)

CMakeLists.txt           all dependencies are fetched at configure time (FetchContent)
run.sh                   WSLg launcher (points SDL at the Wayland runtime dir)
```

Build trees (`build*/`), the Python venv (`.venv/`), captured screenshots
(`output/`), recordings (`*.xdf`), and the persisted ImGui layout (`imgui.ini`)
are generated and git-ignored.

See [DESIGN.md](../DESIGN.md) for the architecture and rationale.
