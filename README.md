# lsl_viewer

A real-time viewer for [Lab Streaming Layer](https://github.com/sccn/labstreaminglayer) (LSL) streams. It plots live data (EEG, MEG, fNIRS, accelerometers, markers), applies display filters, computes spectra and marker-averaged responses, and records to XDF.

![Live scrolling EEG montage](docs/images/live.gif)

## Quick start

To try all the views without an external source, launch the application and select **Tools > Emit demo streams**. The viewer then publishes a synthetic set: EEG with EOG, a 1 Hz to 40 Hz chirp, a 48 kHz stereo tone, and an evoked-response stream with markers.

## Features

### Live multi-stream time series

![Stacked montage of slow drift channels beside a 128-channel raster](docs/images/montage.png)

- A stacked montage that gives each channel its own named lane, or a shared-axis overlay. Each channel has a gain control, and there is an auto-fit.
- A raster/heatmap mode for high channel counts (32 to 256 or more), where single line traces become too thin to read.
- To find one channel quickly, hover over its name in the stacked montage or raster. The viewer highlights its lane. Click the name to keep the highlight. To remove it, click the name again or right-click a channel name.
- A pause control, to examine a frozen window.
- Dropouts show as gaps on the real timeline. The viewer does not join the data across the missing span.
- **Lock time axes** (Tools menu) applies one time window to all stream plots. The plots stay aligned, and they pan and zoom together. If a plot becomes difficult to read, **Reset view** in its panel puts it back to the default framing.
- Marker and event streams show as labeled event lines on the time series. The **Marker events** log (View menu) also lists them as a scrolling `time  value` feed. Thus you can see the events when no continuous stream is running.

### Signal conditioning

You can apply the filter stages in any combination. The spectrum, spectrogram, and zoomed views use the conditioned signal.

- High-pass (removes DC and drift), mains notch (50 Hz or 60 Hz), and low-pass.
- Re-referencing to the common average (CAR) or to one reference channel. CAR averages the EEG channels only. It excludes EOG, EMG, and trigger channels, which it identifies from the channel metadata.

### Frequency-domain analysis

| Per-channel FFT spectrum | Rolling spectrogram |
|---|---|
| ![FFT spectrum of two audio tones](docs/images/spectrum.png) | ![Spectrogram of a 1 Hz to 120 Hz chirp](docs/images/spectrogram.png) |

- A PSD (in dB or linear units) for each selected channel.
- A rolling STFT spectrogram with an adjustable frequency range. **Fit Hz** sets the range to the band that holds the signal energy. This helps when the sample rate is high compared to the signal of interest. Audio is an example, because the tones are much lower than the Nyquist frequency.
- Both views can read the raw signal or the conditioned signal.

### ERP and marker-aligned averaging

![ERP average with single-trial traces](docs/images/erp.png)

- Epochs around the events from a marker stream, averaged across trials. The viewer draws the single-trial traces below the average.
- Optional exact label matching. Give one label, such as `target`, or more than one label with a spaced pipe (` | `) between them, such as `start_a | start_b`.
- One channel or many channels, and an erpimage view (trials by time, or channels by time).

### Recording

- XDF recording of all connected streams. The files are compatible with LabRecorder, which was checked against LabRecorder output with `pyxdf`. The viewer keeps the raw timestamps and the clock-offset chunks, thus an importer can align the streams to a common clock.
- Filename templates, such as `sub-{subject}_task-{task}_run-{run}_eeg.xdf`.
- A headless CLI, `xdf_record`, that records without the GUI.
- **Replay XDF file** (Tools menu) plays a recording back as live streams in the viewer, at 0.5 to 20 times real time. Use it to check a recording right after you make it: all streams are present, there are no dropouts, and the markers are in the correct positions. The **Replay** panel has a position slider, pause, speed, loop, and stop. It also lists the warnings for a damaged file and the samples sent for each stream. A seek or a loop shows in the plots as a dashed blue line. The viewer does not show it as a dropout.
- A CLI, `xdf_replay`, that replays an XDF file as live LSL streams, also faster than real time. Use it at the rig to check a recording right after you make it, for example with `xdf_replay rec.xdf --speed 10` and the viewer. Run `xdf_replay --help` for the options.

### Other

- A docking layout. The Streams rail is on the left. The plots and the analysis windows are tabs that you arrange.
- Saved workspaces. A workspace holds the current view: the filters, channels, and gains for each stream, the open analysis windows, and the dock layout. When you load a workspace, the viewer reconnects the streams that the workspace refers to (matched on source ID and name) and lists the streams that are not on the network. It holds the recording until those streams connect or you dismiss the notice.
- Information for each stream: type, source ID, channels, sensor positions, and live counters for the measured rate, the clock offset, and the dropouts.
- TCP remote control of the recording, with client libraries for Python, MATLAB, and Octave. See [Remote control](#remote-control).
- A light theme and a dark theme. The viewer keeps the layout between sessions.

## Remote control

Enable a control port from the Recording panel, or with `LSL_RC_PORT=22345`. A client then controls the recording over TCP. The commands end with a newline, and the replies are lines of readable text.

The port binds to loopback (127.0.0.1) only. There is no authentication. To use the port from a different machine, turn on **Allow LAN access** in the Recording panel, or set `LSL_RC_BIND=all`. Use trusted networks only. Up to four clients can connect at the same time.

### Client libraries

Use a client library instead of a raw socket. The libraries read the replies correctly, give an error when the viewer refuses a command, and copy a recording to your machine.

- **Python:** [clients/python](clients/python/README.md). One file with no dependencies. Copy `lsl_viewer_rc.py` into your project, or install it:

  ```sh
  uv pip install "git+https://github.com/aforren1/lsl-viewer#subdirectory=clients/python"
  ```

- **MATLAB and GNU Octave:** [clients/matlab](clients/matlab/README.md). The same code runs in MATLAB R2019b or later and in Octave 6 or later. Add `clients/matlab` to the path.

A script finds the streams, selects the streams it wants, fills the BIDS fields, records, and copies the file:

```python
import lsl_viewer_rc as rc

with rc.Client("127.0.0.1", 22345) as viewer:
    keys = [s.key for s in viewer.streams() if s.type in ("EEG", "Markers")]
    viewer.select(keys)                 # An unknown key makes the viewer refuse all of them.
    viewer.set(subject="01", session="01", task="posner", run=1)
    viewer.start()                      # Returns when the file is open.
    # Present the stimuli, and push the markers through LSL.
    viewer.stop()
    viewer.get("data/")                 # Copy the .xdf file to this machine.
```

```matlab
rc = lslrc.Client('127.0.0.1', 22345);
st = rc.streams();
rc.select({st(strcmp({st.type}, 'EEG')).key});
rc.set('subject', '01');
rc.start();
% Present the stimuli here.
rc.stop();
rc.get(pwd);                        % A folder: the file keeps its name.
rc.close();
```

### Protocol

Read this section if you write a client for a different language. The libraries above follow these rules.

| Command | Reply |
|---|---|
| `streams` | Lists the streams that LSL can resolve. The header is `ok: <N> streams`. Then come N lines, one for each stream: `key \| name \| type \| <C>ch \| <rate>`. The rate is an integer or `irregular`. A connected stream has `  [rec]` at the end of its line. The first field is the key. |
| `selected` | Lists the keys that are connected. These streams are the ones that get recorded. The header is `ok: <N> selected`. Then come N lines, one key on each line. |
| `select all\|none\|<k1,k2,...>` | Selects the streams to connect and record. Each `key` is an identifier from `streams`. If a key is not a stream that the viewer can see, the viewer refuses the whole command. The command is also refused during a recording, because the set is locked until `stop`. |
| `set <subject\|session\|task\|run\|acq\|modality> <value>` | Fills one field of the filename template. The reply is `ok`. |
| `filename <path>` | Sets the output path or template directly. The reply is `ok`. |
| `start [path]` and `stop` | Start and stop the recording. |
| `get` | Sends the last completed recording to the client. The header is `ok: <bytes> <name>`. Then come exactly `<bytes>` of raw XDF. |
| `status` | Shows the recording state on one line: `ok: recording=<true\|false> seconds=<s> streams=<n> bytes=<n> file=<path>`. |
| `help` | Lists the commands. The header is `ok: <N> lines`. Then come N lines of text. |
| `quit` | Closes the connection. The reply is `bye`. |

The viewer uses protocol 2. When a client connects, the viewer sends one line: ``ok: lsl-viewer remote control, protocol 2. type `help`.`` Read the version from the text `protocol <N>`. If the line does not have this text, the viewer is too old for a protocol 2 client. If four clients are already connected, the viewer sends `error: too many control clients`, and then closes the connection.

These rules apply to the replies:

- The first line of a reply is `ok`, `ok: <text>`, or `error: <text>`. The only exception is `quit`.
- The `streams`, `selected`, and `help` replies have a header line `ok: <N> <noun>`, then exactly N lines. There is no end marker. If there are no streams, the reply is `ok: 0 streams` and nothing more.
- In the `status` reply, `file=` is always the last field. The path continues to the end of the line, and it can contain spaces. Split the text before `file=` on spaces, then take all the text after `file=` as the path.
- In the `get` header, the name continues to the end of the line, and it can contain spaces. If `get` fails, the reply is one `error:` line and no data.
- The `select`, `filename`, `set`, `start`, and `stop` replies are the same as in protocol 1, for compatibility with LabRecorder remote control clients.

The `select`, `start`, and `stop` commands must go through the viewer. They do not reply until the viewer has done the work, thus the reply gives the result: `ok: recording -> /path/file.xdf`, or `error: no streams connected`. A `status` that comes after such a reply always shows the new state. If the viewer does not answer in 2 seconds, the reply is `error: the viewer did not respond`, and the command is discarded. Thus set the read timeout of your client to more than 2 seconds. For `get`, use a longer timeout, because a large file takes more time.

### Discovery

The viewer also announces the control endpoint through LSL, with the type `ViewerControl`. The client libraries do this for you: `rc.discover()` in Python (needs `pylsl`) and `lslrc.discover()` in MATLAB or Octave (needs liblsl-Matlab). To do it yourself, resolve it to get the host and the port instead of writing `22345` in your code. The `source_id` has the form `lsl-viewer-rc:<host>:<pid>:<port>`, thus the port is the text after the last colon. The host and the process ID make the identifier different for each viewer, because LSL uses `source_id` as the identity of a stream.

```python
from pylsl import resolve_byprop, StreamInlet

for info in resolve_byprop("type", "ViewerControl", timeout=5.0):
    port = int(info.source_id().rsplit(":", 1)[1])
    desc = StreamInlet(info).info(timeout=3).desc()      # port, pid, bind, protocol, protocol_version
    if desc.child_value("protocol_version") != "2":
        continue                                         # A viewer that this client cannot talk to.
    host = "127.0.0.1" if desc.child_value("bind") == "loopback" else info.hostname()
```

The description also has `protocol_version`. Use it to skip a viewer with a different protocol before you connect to it.

Two things to know when you write a client:

- **The port can move.** If port 22345 is in use, and you did not set `LSL_RC_PORT`, the viewer takes a port from the operating system and announces that one. Thus a second viewer on the same machine also has a control port. If you do set `LSL_RC_PORT`, the viewer uses that port or none, because a script that asks for a port must not get a different one.
- **The host from `resolve` is not always the address to connect to.** A viewer that binds to loopback only is reachable at `127.0.0.1`, and only from the same machine, but LSL still reports the machine name. The `bind` field in the description says which: `loopback` or `all`.

To examine the endpoint by hand, use `nc localhost 22345`.

## System requirements

The viewer renders through **SDL_GPU**, the GPU abstraction of SDL3. Thus it needs a GPU and a driver that support one of the SDL_GPU backends, and a desktop display server. The other requirements are small: a few hundred MB of RAM, and any recent multi-core CPU.

- **Windows:** Windows 10 or later (64-bit), with a GPU and driver that support **Direct3D 12**. If Vulkan is available, SDL_GPU uses Vulkan instead.
- **macOS:** macOS 11 (Big Sur) or later, on a Mac with **Metal** support. This includes Apple Silicon Macs and Intel Macs with a Metal GPU. The `.app` and `.dmg` files are unsigned. For the first launch, right-click the app and select **Open**. On macOS 15 (Sequoia) and later, permit the local network when the system asks. If you refuse, the viewer finds no streams. See [docs/network.md](docs/network.md).
- **Linux:** a **Vulkan** loader and driver (`libvulkan` and an ICD for your GPU), and **Wayland or X11**. The AppImage is the easiest way to run the viewer on different distributions.

**Network:** LSL finds and reads the streams on the local network. It uses UDP broadcast and multicast to resolve the streams, and TCP to transfer the data. Thus the sources must be on the same subnet, and you can be required to permit the viewer through the firewall. The Windows installer adds the firewall rules for you. See [docs/network.md](docs/network.md) for the ports, for the portable build, and for what to do when the viewer finds no streams.

**Headless tools:** `xdf_record` and `xdf_replay` have no GPU or display requirements. They link only to liblsl, and the static musl builds for Linux run on all Linux distributions.

## Documentation

- [docs/building.md](docs/building.md): how to build, the CMake flags, the static and single-file builds, Windows, and the repository layout.
- [docs/network.md](docs/network.md): the LSL ports, the firewall rules for each platform, the macOS local network permission, and what to do when the viewer finds no streams.
- [DESIGN.md](DESIGN.md): the architecture and the reasons for it (the ring buffers, the threading, and the rendering path).

## Roadmap

- Scalp topography (topomap): an interpolated head map of the amplitude or the band power. The viewer already reads the sensor positions for each channel from the stream metadata, thus the map can apply to EEG, MEG, or fNIRS. The Info panel shows how many channels have a layout.
- Bipolar montages: named electrode chains, such as the longitudinal "double banana".
- Drag-and-drop of markers onto a plot, and more marker and event controls.
- Manual rejection of bad channels, which excludes them from the CAR and from the display.

## License

MIT. See [LICENSE](LICENSE). The viewer includes third-party components (SDL3, Dear ImGui, ImPlot, liblsl, KissFFT, spdlog, the Roboto font, and the components that they bundle), and it adapts the `xdfwriter` of LabRecorder. Their copyright notices and licenses are in [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## Acknowledgments

Built with [SDL3](https://github.com/libsdl-org/SDL) and SDL_GPU, [Dear ImGui](https://github.com/ocornut/imgui) (docking) with [ImPlot](https://github.com/epezent/implot), [liblsl](https://github.com/sccn/liblsl), and [KissFFT](https://github.com/mborgerding/kissfft). C++20.

Thanks to the developers of LSL, SDL, Dear ImGui, ImPlot, spdlog, KISS FFT, and Tracy for these useful and foundational tools.

I used AI (Claude Opus 4.8 and later models) to help me develop this tool.
