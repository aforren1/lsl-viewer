# Building & running

## Prebuilt binaries

Every push builds binaries on CI ([.github/workflows/build.yml](../../.github/workflows/build.yml)) —
grab them from the run's **Artifacts**:

- `lsl-viewer-linux` / `lsl-viewer-linux-aarch64` / `lsl-viewer-macos` / `lsl-viewer-windows`
  — self-contained (static) `lsl_viewer` + `xdf_record` + `xdf_replay` for each OS/arch, plus `LICENSE`,
  `THIRD_PARTY_LICENSES`, and `portable.txt` (a [portable](../settings.md#portable-mode) build).
- `lsl-viewer-windows-installer` — **`lsl-viewer-setup.exe`**, an Inno Setup installer that
  drops the app into `Program Files` and uses the standard per-user locations.
- `lsl-viewer-macos-dmg` — **`LSL-Viewer.dmg`**, a drag-to-Applications disk image with an
  `LSL Viewer.app` bundle (and the `xdf_record` and `xdf_replay` CLIs alongside it).
- `lsl-viewer-appimage-x86_64` / `lsl-viewer-appimage-aarch64` — a portable
  **`LSL-Viewer-<arch>.AppImage`** built on an older glibc and with **both X11 and Wayland**
  backends, so it runs across desktops and distros (the host still provides the GPU driver /
  Vulkan loader, as always for a GPU app).
- `xdf-record-linux-musl` — the headless **`xdf_record`** recorder built **fully static against
  musl** (CLI only; `-DLSL_CLI_ONLY=ON`). It has **no glibc and no shared-library dependencies**
  (`ldd` says "not a dynamic executable"), so the single ~1.3 MB binary runs on **any** Linux —
  ancient glibc, Alpine/musl, minimal containers — with nothing to install. The GUI viewer can't
  be fully static (it needs the host's GPU driver + display libraries at runtime), which is why
  only the recorder ships this way. The artifact also holds a static `xdf_replay`.

The per-OS `lsl-viewer-linux` artifact and the AppImage are both built on Ubuntu 22.04, so
both need glibc 2.34 and `GLIBCXX_3.4.30` (GCC 12) or newer on the host: Ubuntu 22.04,
Debian 12, Fedora 35, RHEL 9, and later. For older hosts only the musl `xdf_record` runs.

None of the artifacts are code-signed, so first launch trips Gatekeeper on macOS
(right-click → Open, or `xattr -dr com.apple.quarantine "LSL Viewer.app"`) and SmartScreen
on Windows (More info → Run anyway).

## Build from source

All third-party libraries (SDL3, liblsl, Dear ImGui, ImPlot, spdlog, KissFFT, and
optionally Tracy / the ImGui Test Engine) are pulled in by CMake via `FetchContent` — so
besides a C++20 compiler and CMake ≥ 3.22, the only system packages are SDL3's display
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

To test with real data, `tools/xdf_replay.py` replays an XDF recording. Each
stream becomes a live outlet with the recorded metadata (including the full
`<desc>`) and timestamps, and all streams keep their relative timing. The tool
reads the file while it plays, so playback starts at once and memory use stays
small, also for multi-GB files. Use `--list` to show the streams, `--streams`
to replay only some of them, and `--start` to start at a time in the file:

```bash
uv run tools/xdf_replay.py rec.xdf --list
uv run tools/xdf_replay.py rec.xdf --suffix _replay --loop
uv run tools/xdf_replay.py rec.xdf --streams type:EEG,type:Markers --start 60 --duration 30
```

`xdf_replay` is the same tool as a compiled CLI. It is built with the viewer and in
an `-DLSL_CLI_ONLY=ON` configure, and it needs no Python. It takes the same options
and prints the same output. The viewer can also replay a file itself, in the
**Playback** section of the **Streams** rail, with the same engine. For how to use
the two, see [Record and replay](../record-and-replay.md).

At a speed other than 1, `--timestamps scaled` gives the behavior of
`tools/xdf_replay.py`: the spacing of the timestamps is divided by the speed. The
default of the compiled tool is `recording`.

The outlets are visible on the whole network, as for any LSL source. The liblsl
configuration cannot make the outlets local only: `ResolveScope = machine` alone
does not hide them from other hosts, and `ListenAddress = 127.0.0.1` hides them but
also stops a resolver on the same machine that uses liblsl 1.17 (such as this
viewer) from finding more than one of them. Use `--suffix` to keep the names apart
from the live devices.

For a scripted run, set `LSL_REPLAY` to the path of a file, and `LSL_AUTOCONNECT=1`
to connect all streams that the viewer finds. For the other startup variables, see
the [Reference](../reference.md#environment-variables).

## Data and config locations

See [Where the viewer keeps its files](../settings.md#where-the-viewer-keeps-its-files)
and [Portable mode](../settings.md#portable-mode). A `--tests` run always uses
portable mode, thus it does not change the settings, the workspaces, or the
recordings of the user.

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
| `-DLSL_VIEWER_TESTS=ON`  | build the UI test suite; run headless with `lsl_viewer --tests [query]`. Also builds `drift_map_test`, which needs no GPU: run it with `ctest --test-dir build-test -C Release` |
| `-DLSL_VIEWER_TRACY=ON`  | enable the [Tracy](https://github.com/wolfpld/tracy) frame profiler (connect the Tracy server to view) |
| `-DLSL_VIEWER_STATIC=ON` | static-link SDL3 + liblsl into one self-contained binary (see below) |
| `-DLSL_CLI_ONLY=ON` | build **only** the headless `xdf_record` and `xdf_replay` (skip all GUI deps: SDL3/ImGui/ImPlot/KissFFT). With `-DLSL_VIEWER_STATIC=ON` + a musl toolchain + `-DCMAKE_EXE_LINKER_FLAGS=-static` → a fully-static recorder with no glibc/GPU deps |
| `-DSDL_X11=OFF`   | Linux/WSL: build the Wayland backend only |

There's also a lightweight built-in text profiler: run with `LSL_PROFILE=1` for a
per-zone timing table, or `LSL_BENCH=1` for an FPS / CPU-build / GPU-submit
readout (no Tracy needed).

The default layout shows only 16 channels and no analysis windows. To profile the heavy
paths, use the `bench/soak` UI test in a `-DLSL_VIEWER_TESTS=ON` build. It opens two
spectra, two spectrograms, an ERP window, and the marker list, and then runs for the
time that you set:

```bash
uv run tools/lsl_test_streams.py --hd-channels 256 --hd-rate 8000
LSL_PROFILE=1 LSL_AUTOCONNECT=1 LSL_PERF_SOAK=30 LSL_PERF_STREAM=MockHighDensity \
    ./lsl_viewer --tests soak
```

Without `LSL_PERF_SOAK`, the test does nothing, so the normal suite is not slower. These
variables change what the soak does:

| Variable | Effect |
|----------|--------|
| `LSL_PERF_SOAK=<s>` | Run for this many seconds after the setup. |
| `LSL_PERF_STREAM=<name>` | Show all channels of the stream window with this name, and bind the spectra and spectrograms to it. |
| `LSL_PERF_RASTER=1` | Show that stream as a raster. |
| `LSL_PERF_OPEN=<list>` | Open only these windows, from `spectrum,spectrogram,erp,markers`. |
| `LSL_PERF_SPECTRUM_ALL=1` | Select all channels in the spectra. |
| `LSL_PERF_SPECTRO_SPAN=<s>` | Set the spectrogram span. |
| `LSL_PERF_SPEED=5x` | Set the replay speed, with `LSL_REPLAY=<file>`. |
| `LSL_PERF_CAPTURE=1` | Save a screenshot of the window at the end, in `output/captures/`. |
| `LSL_PERF_COMBINED=streams` or `combined` | For `--tests combined_vs_streams`: show 4 streams of 16 channels at 500 Hz as 4 stream plots, or as 1 combined plot on the same area. That test publishes the streams itself. |

These variables of the viewer itself help to profile on a given display or GPU:

| Variable | Effect |
|----------|--------|
| `LSL_WINDOW=WxH` | Set the initial window size. |
| `LSL_DISPLAY=<n>` | Open on display n (1 is the first). The log shows the display name. |
| `LSL_MAXIMIZE=1` | Maximize the window. |
| `LSL_FULLSCREEN=1` | Use borderless fullscreen. |
| `LSL_GPU_LOW_POWER=1` | Use the integrated GPU on a laptop with two GPUs. The log and the Debug menu show the GPU name. |
| `LSL_NOVSYNC=1` | Turn off VSync, to measure the full render cost. |
| `LSL_FPS_CAP=<n>` | Limit the frame rate to n frames a second (also in the Performance window). |
| `LSL_FRAMES_IN_FLIGHT=<n>` | Let the GPU queue 1 to 3 frames (default: 2). More frames give more input lag. Fewer frames make a late frame more likely to show as a hitch. |

To compare the time axis with and without the drift correction, set `LSL_DRIFT_CORR=0`.
The viewer then draws sample i of a regular stream at `t0 + i / nominal rate`, and a
device off its nominal rate drifts against the marker streams. The Info section of a
stream shows the correction in use.

## Static build (single-file distribution)

`-DLSL_VIEWER_STATIC=ON` folds SDL3 and liblsl into the executable so there's nothing to
ship alongside it:

```bash
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release -DLSL_VIEWER_STATIC=ON
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
from [packaging/](../../packaging/), built on an older glibc with both display backends).
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
to a native Windows path, install Visual Studio 2022 + CMake ≥ 3.22, then run the
same `cmake` configure/build (drop the Linux-only `-DSDL_X11=OFF`). `run.sh` and
the Wayland environment are not needed — just launch `lsl_viewer.exe`.

The remote-control TCP server uses Winsock on Windows; to verify that path at
runtime see [windows-remote-control-testing.md](windows-remote-control-testing.md).

## Repository layout

```
src/                     the viewer — one translation unit + header-only modules
  main.cpp                 app entry: UI, dock layout, render loop, recording UI
  hf_stream_source.hpp     LSL inlet -> ring buffers + filter chain on a worker thread
  drift_map.hpp            sample index -> display time: dropout gaps + drift correction
  magic_ring_buffer.hpp    mirrored (contiguous-wraparound) lock-free ring buffer
  minmax_summary.hpp       decimated min/max envelope for zoomed-out plots
  filter.hpp               DC-blocker high-pass + RBJ biquads (notch / low-pass)
  fft.hpp                  FFT + PSD (spectrum and spectrogram views)
  heatmap_image.hpp        heatmaps (raster, spectrogram, ERP raster) drawn as one texture
  recorder.hpp             XDF recording driver (records connected streams)
  xdf_writer.hpp           XDF container writer
  xdf_reader.hpp           XDF scan, chunk index, decoder, pyxdf clock sync and dejitter
  xdf_player.hpp           XDF replay engine (outlets, playback clock, pause/seek/speed/loop)
  remote_control.hpp       TCP remote-control server (start/stop/status)
  theme.hpp                UI theme + embedded font
  profiler.hpp             zone-profiling macros (text profiler / Tracy / no-op)

tools/                   standalone helpers (not linked into the viewer)
  lsl_test_streams.py      synthetic LSL sources for testing
  xdf_replay.py            replays an XDF recording as live LSL outlets (reference implementation)
  xdf_replay.cpp           the same as a compiled CLI, on src/xdf_player.hpp (no GUI deps)
  xdf_record.cpp           headless XDF recorder CLI (no GUI deps)

tests/                   Dear ImGui Test Engine UI tests + screenshot captures
  ui_tests.cpp             enabled with -DLSL_VIEWER_TESTS=ON; run via `lsl_viewer --tests`
  drift_map_test.cpp       headless drift-map tests on synthetic timestamps (`ctest`);
                           `drift_map_test --bench` times the per-frame time mapping
  compare_labrecorder.py   records the mock streams with our recorder + LabRecorder and
                           checks the XDFs are identical (needs LabRecorderCLI; runs locally)

CMakeLists.txt           all dependencies are fetched at configure time (FetchContent)
run.sh                   WSLg launcher (points SDL at the Wayland runtime dir)
```

Build trees (`build*/`), the Python venv (`.venv/`), captured screenshots
(`output/`), recordings (`*.xdf`), and the persisted app settings (`imgui.ini`)
are generated and git-ignored.

See [DESIGN.md](../../DESIGN.md) for the architecture and rationale.
