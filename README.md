# lsl-sdl

**A fast, real-time viewer for [Lab Streaming Layer](https://github.com/sccn/labstreaminglayer) streams.**

Plot live LSL streams (e.g. EEG, MEG, fNIRS, accelerometers, markers) with
filtering, spectral views, evoked-response averaging, and recording all in one
GPU-accelerated window.

![Live scrolling EEG montage](docs/images/live.gif)

---

## Features

### Live multi-stream time series

![Stacked montage of slow drift channels beside a 128-channel raster](docs/images/montage.png)

- **Stacked montage** (EEG-style, one named lane per channel) or shared-axis **overlay**, with per-channel gains and an Auto-fit.
- Smooth GPU scrolling that stays fluid at high channel counts and sample rates.
- A single-quad **raster/heatmap** mode for dense montages (32–256+ channels) where line traces are too short to read — spot regional activity across the whole array at a glance.
- **Pause** to inspect a frozen window.
- **Dropouts shown honestly** — a stream that drops out leaves a red gap on the real timeline, not a silent jump.

### Signal conditioning

Independent, stackable filter stages you apply in any combination — and *every*
view (time series, spectrum, spectrogram, zoom) reflects them:

- **High-pass** (DC/drift removal) · **mains notch** (50 / 60 Hz) · **low-pass** (anti-EMG)
- **Re-referencing**: common-average (CAR) or a single reference channel. CAR averages the **EEG channels only** — EOG/EMG/trigger channels are excluded automatically from the metadata.

### Frequency domain analysis

| Per-channel FFT spectrum | Rolling spectrogram |
|---|---|
| ![FFT spectrum of two audio tones](docs/images/spectrum.png) | ![Spectrogram of a 1→120 Hz chirp](docs/images/spectrogram.png) |

- Overlaid **PSD** for any selected channels (dB or linear).
- A rolling **STFT spectrogram** with a **zoomable frequency axis**, plus a one-click **Fit Hz** that snaps it to the band actually carrying energy — essential when the sample rate is high but the signal of interest is low (e.g. 48 kHz audio, tones < 1 kHz).
- Both can analyze the raw or the conditioned signal.

### ERP / marker-aligned averaging

![ERP average with single-trial spaghetti](docs/images/erp.png)

- Trigger on a marker stream (with optional label matching, e.g. `target`), cut epochs around each event, and watch the **evoked response** build up — average in bold over the faint single-trial "spaghetti".
- Multi-channel and an **erpimage** (trials × time, or channels × time) raster view.

### Recording

- One-click **XDF recording** of every connected stream — LabRecorder-compatible (verified against the real LabRecorder via `pyxdf`), with raw timestamps + clock-offset chunks so importers realign streams to a common clock.
- BIDS-ish **filename templating** (`sub-{subject}_task-{task}_run-{run}_eeg.xdf`).
- A headless **`xdf_record`** CLI for unattended capture (no GUI).

### And more

- **IDE-like docking** layout: a Streams rail on the left, plots and analysis as tabs you arrange to taste.
- **Saved workspaces** — name and save the whole view (per-stream filters/channels/gains, the open analysis windows, and the dock layout); loading one re-applies it, re-binding to each stream by `source_id` as it reconnects. Set your rig up once.
- **Stream Info & health** — type, source id, channels, sensor positions, plus live measured-rate / clock-offset / dropout counters per stream.
- **Remote control** over TCP — drive recording from your experiment script (see below).
- Light / dark theme, persisted layout.

---

## Remote control

Start the viewer with a control port (`LSL_RC_PORT=22345`, or enable it from the
Recording panel) and any client can drive recording over TCP with newline-terminated
commands — `status`, `start [path]`, `stop`, `help`. Handy for starting/stopping a
recording from the same script that runs your experiment:

```python
import socket

with socket.create_connection(("localhost", 22345)) as rc:
    def cmd(c):
        rc.sendall((c + "\n").encode())
        return rc.recv(4096).decode().strip()

    cmd("start sub-01_task-oddball_run-1_eeg.xdf")
    # ... present stimuli, push markers via LSL ...
    cmd("stop")
    print(cmd("status"))
```

The port is also advertised over LSL (in the control stream's `source_id`), so a
client can discover it instead of hard-coding `22345`. Or just `nc localhost 22345`.

---

## Quick start

All dependencies are fetched by CMake — you just need a **C++20 compiler** and
**CMake ≥ 3.23**.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lsl_viewer          # on WSL: ./run.sh

# in another terminal, feed it synthetic streams to play with:
uv run tools/lsl_test_streams.py --streams eeg,sine,chirp,markers,evoked
```

Then connect streams from the **Streams** rail (or set `LSL_AUTOCONNECT=1`).

➡️ **[docs/building.md](docs/building.md)** — build options, single-file/static builds, Windows, test data, repo layout.

---

## Roadmap

- **Scalp topography (topomap)** — interpolated head map of amplitude / band power. Groundwork is in place: per-channel sensor positions are already parsed from stream metadata (so it's modality-agnostic — EEG/MEG/fNIRS), and the Info panel reports how many channels carry a layout.
- **Bipolar montages** — named electrode chains (e.g. the longitudinal "double banana").
- **Markers drag-onto-plot** and richer marker/event handling.
- Per-channel **bad-channel rejection** (manual exclude from CAR / display).

---

## Documentation

- **[docs/building.md](docs/building.md)** — building, CMake flags, static / single-file builds, Windows, repo layout
- **[DESIGN.md](DESIGN.md)** — architecture & rationale (ring buffers, threading, the rendering path)

Built with [SDL3](https://github.com/libsdl-org/SDL) + SDL_GPU, [Dear ImGui](https://github.com/ocornut/imgui) (docking) + [ImPlot](https://github.com/epezent/implot), [liblsl](https://github.com/sccn/liblsl), and [KissFFT](https://github.com/mborgerding/kissfft). C++20.
