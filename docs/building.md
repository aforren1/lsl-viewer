# Building & running

## Prebuilt binaries

Every push builds binaries on CI ([.github/workflows/build.yml](../.github/workflows/build.yml)) —
grab them from the run's **Artifacts**:

- `lsl-viewer-linux` / `lsl-viewer-linux-aarch64` / `lsl-viewer-macos` / `lsl-viewer-windows`
  — self-contained (static) `lsl_viewer` + `xdf_record` + `xdf_replay` for each OS/arch, plus `LICENSE`,
  `THIRD_PARTY_LICENSES`, and `portable.txt` (a [portable](#data-and-config-locations) build).
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
and prints the same output. Use it at the rig to check a recording right after you
make it, for example at 10 times real time with the viewer open:

```bash
xdf_replay rec.xdf --list
xdf_replay rec.xdf --speed 10
```

At a speed other than 1, `--timestamps` sets how the timestamps follow the speed:

- `recording` (default): the timestamps keep the recorded spacing. The viewer then
  shows recording seconds on its time axis, dropouts with their recorded width, and
  markers on the correct samples. The timestamps run ahead of `local_clock()`.
- `scaled`: the spacing is divided by the speed, so the timestamps track
  `local_clock()`. This is what `tools/xdf_replay.py` does. In the viewer, the
  dropouts become narrower by the speed factor and the markers move away from
  their samples.

Each source_id gets the prefix `replay-` (`--source-id-prefix`). Thus a recorder
or a viewer workspace that matches on the source_id does not take the replay for
the live device.

The outlets are visible on the whole network, as for any LSL source. A recorder
that selects streams by name and host (LabRecorder's `RequiredStreams`) also
selects a replay that runs on the host of the device. To prevent this, add a
suffix to the names with `--suffix _replay`. The liblsl configuration cannot
make the outlets local only: `ResolveScope = machine` alone does not hide them
from other hosts, and `ListenAddress = 127.0.0.1` hides them but also stops a
resolver on the same machine that uses liblsl 1.17 (such as this viewer) from
finding more than one of them.

At 10 times real time, the viewer's plot edge moves at the speed of the data, so
the plots show the full time window while the replay runs.

The viewer can also replay a file itself, with no terminal. In the **Playback**
section at the bottom of the **Streams** rail, select **Open XDF file...** and
then the file. The viewer creates the streams, shows them, and then starts the
replay. It uses the same engine as `xdf_replay`, with `recording` timestamps.
The **Playback** section has these controls:

- The position slider. Release it to seek, or Ctrl+click it to type the seconds.
  A seek shows in the plots as a dashed blue line. The viewer does not show a
  seek as a dropout, because no data is missing from the file.
- **Pause replay** stops the data until you select **Resume replay**. **Pause
  display** (View menu, or the P key) only freezes the plots, and the replay
  continues.
- The speed (0.5 to 20 times real time) and **Loop**. A loop wrap also shows as a
  dashed blue line.
- **Stop** closes the streams and removes their plots. After a stop, **Replay
  again** plays the same file from the start, and the close button (**×**)
  removes the replay from the section.
- The warnings for a damaged file. Select the warnings button to see the list.

In the stream list, the rows of the replayed streams have a play mark and show
two counts: the samples sent and the samples in the file. Hover over a row to
see the type, the channels, the format, and the rate. Below the controls,
**Playback** lists the file streams that are not in the viewer: `closed` if you
disconnected the stream, or `not shown` if it did not connect.

To open a file from a script, set `LSL_REPLAY` to its path. The viewer then opens
the file at launch, as if you selected it in the dialog.

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
| `-DLSL_VIEWER_TESTS=ON`  | build the UI test suite; run headless with `lsl_viewer --tests [query]` |
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
| `LSL_FRAMES_IN_FLIGHT=<n>` | Let the GPU queue 1 to 3 frames (default: 1). More frames give more input lag. |

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
  compare_labrecorder.py   records the mock streams with our recorder + LabRecorder and
                           checks the XDFs are identical (needs LabRecorderCLI; runs locally)

CMakeLists.txt           all dependencies are fetched at configure time (FetchContent)
run.sh                   WSLg launcher (points SDL at the Wayland runtime dir)
```

Build trees (`build*/`), the Python venv (`.venv/`), captured screenshots
(`output/`), recordings (`*.xdf`), and the persisted app settings (`imgui.ini`)
are generated and git-ignored.

See [DESIGN.md](../DESIGN.md) for the architecture and rationale.
