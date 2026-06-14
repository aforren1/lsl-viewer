# lsl-sdl

A real-time viewer for [Lab Streaming Layer](https://github.com/sccn/labstreaminglayer)
(LSL) streams. It plots live data (EEG, MEG, fNIRS, accelerometers, markers, …),
applies display filters, computes spectra and marker-averaged responses, and records
to XDF, in a single GPU-rendered window.

![Live scrolling EEG montage](docs/images/live.gif)

---

## Features

### Live multi-stream time series

![Stacked montage of slow drift channels beside a 128-channel raster](docs/images/montage.png)

- Stacked montage (one named lane per channel) or shared-axis overlay, with per-channel gain and an auto-fit.
- A raster/heatmap mode for high channel counts (32–256+), where individual line traces become too thin to read.
- Pause to inspect a frozen window.
- Dropouts are drawn as gaps on the real timeline rather than concatenating across the missing span.

### Signal conditioning

Independent filter stages applied in any combination; the spectrum, spectrogram, and
zoomed views use the conditioned signal.

- High-pass (DC/drift removal), mains notch (50 / 60 Hz), low-pass.
- Re-referencing: common-average (CAR) or a single reference channel. CAR averages over the EEG channels only; EOG/EMG/trigger channels are excluded based on the channel metadata.

### Frequency-domain analysis

| Per-channel FFT spectrum | Rolling spectrogram |
|---|---|
| ![FFT spectrum of two audio tones](docs/images/spectrum.png) | ![Spectrogram of a 1→120 Hz chirp](docs/images/spectrogram.png) |

- Per-channel PSD (dB or linear) for the selected channels.
- A rolling STFT spectrogram with an adjustable frequency range. "Fit Hz" sets the range to the band that carries signal energy, which helps when the sample rate is high relative to the signal of interest (e.g. audio, where the tones sit well below the Nyquist frequency).
- Both can read the raw or the conditioned signal.

### ERP / marker-aligned averaging

![ERP average with single-trial traces](docs/images/erp.png)

- Epoch around events from a marker stream (with optional label matching, e.g. `target`) and average over trials, with the single-trial traces drawn under the average.
- Single- or multi-channel, plus an erpimage (trials x time, or channels x time) view.

### Recording

- XDF recording of every connected stream, LabRecorder-compatible (checked against LabRecorder via `pyxdf`). Raw timestamps and clock-offset chunks are stored so importers can realign streams to a common clock.
- Filename templating (`sub-{subject}_task-{task}_run-{run}_eeg.xdf`).
- A headless `xdf_record` CLI (no GUI).

### Other

- Docking layout: a Streams rail on the left; plots and analysis windows are tabs you arrange.
- Saved workspaces: store the current view (per-stream filters/channels/gains, the open analysis windows, the dock layout) and reload it later. On load it reconnects the streams the workspace referenced (matched by `source_id`) and lists any that aren't on the network; recording is held until they connect or the notice is dismissed.
- Per-stream info: type, source id, channels, sensor positions, and live measured-rate / clock-offset / dropout counters.
- TCP remote control for recording (see below).
- Light / dark theme; layout persisted between sessions.


## Remote control

With a control port enabled (`LSL_RC_PORT=22345`, or from the Recording panel), a client
can drive recording over TCP with newline-terminated commands: `status`, `start [path]`,
`stop`, `help`. For example, starting and stopping a recording from the same script that
runs an experiment:

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

The port is also advertised over LSL (in the control stream's `source_id`), so a client
can discover it rather than hard-coding `22345`. `nc localhost 22345` works too.

## Quick start

Dependencies are fetched by CMake; you need a C++20 compiler and CMake ≥ 3.23 (on Linux,
also SDL3's display-backend headers; see [docs/building.md](docs/building.md)).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lsl_viewer          # on WSL: ./run.sh

# in another terminal, generate some synthetic streams to view:
uv run tools/lsl_test_streams.py --streams eeg,sine,chirp,markers,evoked
```

Connect streams from the Streams rail (or set `LSL_AUTOCONNECT=1`).

## Roadmap

- Scalp topography (topomap): interpolated head map of amplitude / band power. Per-channel sensor positions are already parsed from stream metadata (so it is modality-agnostic: EEG/MEG/fNIRS), and the Info panel reports how many channels carry a layout.
- Bipolar montages: named electrode chains (e.g. the longitudinal "double banana").
- Markers drag-onto-plot and richer marker/event handling.
- Per-channel bad-channel rejection (manual exclude from CAR / display).

## Documentation

- [docs/building.md](docs/building.md): building, CMake flags, static / single-file builds, Windows, repo layout.
- [DESIGN.md](DESIGN.md): architecture and rationale (ring buffers, threading, the rendering path).

Built with [SDL3](https://github.com/libsdl-org/SDL) + SDL_GPU, [Dear ImGui](https://github.com/ocornut/imgui) (docking) + [ImPlot](https://github.com/epezent/implot), [liblsl](https://github.com/sccn/liblsl), and [KissFFT](https://github.com/mborgerding/kissfft). C++20.
