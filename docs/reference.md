# Reference

## Menus

### App

| Item | Effect |
|---|---|
| **Light theme** | Changes between the light theme and the dark theme. |
| **UI scale** | Sets the size of the text and the controls, from 0.5 to 2.0. |
| **Remote control** | Turns the TCP control port on and off. See [Control the recording from a script](remote-control.md). |
| **Allow LAN access (no auth)** | Lets other machines connect to the control port. |
| **About** | Shows the version and the licenses. |
| **Quit** | Closes the viewer. |

### Tools

| Item | Effect |
|---|---|
| **Walkthrough** | Opens a guided tour of the main features. See [Get started](getting-started.md#2-run-the-walkthrough). |
| **Emit demo streams** | Publishes synthetic streams and connects to them. |

### View

| Item | Effect |
|---|---|
| **Marker events** | Opens the log of the marker events. |
| **New combined plot** | Opens a plot for channels of several streams. |
| **New spectrum** | Opens a spectrum window. |
| **New spectrogram** | Opens a spectrogram window. |
| **New ERP (marker average)** | Opens a marker-aligned average window. |
| **Pause display** | Stops the plots. The data and a recording continue. |
| **Lock time axes** | Gives one time window to all time series plots. |
| **Reset window layout** | Puts all windows back to the default layout. |

### Workspaces

Saves, loads, and deletes workspaces. See [Workspaces](settings.md#workspaces).

### Debug

| Item | Effect |
|---|---|
| **Performance** | Shows the frame times, and has the VSync and frame rate controls. |
| **Metrics (ImGui)** | Shows the internal data of the UI library. |

The menu also shows the GPU backend and the name of the GPU.

## Keyboard and mouse

| Input | Effect |
|---|---|
| **P** | Pause or continue the display. |
| **Ctrl+Q** | Close the viewer. |
| **Page Down**, **Page Up**, with the pointer on a plot | Go to the next or the previous page of channels. |
| Click a stream row | Connect the stream. For a connected stream, bring its plot to the front. |
| Right-click a connected stream row | Open the menu to disconnect the stream, or to change how the viewer treats it. |
| Click a channel name in a plot | Keep the highlight on that lane. Click again to remove it. |
| Drag a channel name or a row of the **Channels** list | Add the channel to a combined plot. |
| Right-click the file name template | Reset the template to the default. |
| Ctrl+click a slider | Type a value. |

## Filename template fields

| Field | Source |
|---|---|
| `{subject}`, `{session}`, `{task}`, `{run}`, `{modality}`, `{acq}` | **filename fields** in the **Recording** section, or the `set` command of the remote control. |
| `{datetime}` | The local date and time at the start of the recording, as `YYYYMMDD_HHMMSS`. |
| `{date}` | The local date, as `YYYYMMDD`. |
| `{time}` | The local time, as `HHMMSS`. |

The default template is:

```text
sub-{subject}/ses-{session}/{modality}/sub-{subject}_ses-{session}_task-{task}_run-{run}_{modality}.xdf
```

See [Name the files](record-and-replay.md#name-the-files) for the rules.

## Environment variables

Set these variables before you start the viewer. For the on/off variables, the viewer examines only if the variable is set: all values turn the option on, also `0`. To turn the option off, remove the variable.

### Startup

| Variable | Effect |
|---|---|
| `LSL_AUTOCONNECT=1` | Connects all streams that the viewer finds. |
| `LSL_DEMO=1` | Starts the demo streams at launch. |
| `LSL_REPLAY=<file>` | Opens an XDF file in **Playback** at launch. |
| `LSL_PORTABLE=1` | Uses [portable mode](settings.md#portable-mode). |
| `LSL_LIGHT=1` | Uses the light theme, if there is no saved theme setting. |

### Remote control

| Variable | Effect |
|---|---|
| `LSL_RC_PORT=<port>` | Turns on the control port at this port. If the port is in use, the viewer does not open a control port. |
| `LSL_RC_BIND=all` | Lets other machines connect to the control port. |

### Window and display

| Variable | Effect |
|---|---|
| `LSL_WINDOW=<W>x<H>` | Sets the initial window size, for example `1600x900`. |
| `LSL_DISPLAY=<n>` | Opens the window on display n. The first display is 1. |
| `LSL_MAXIMIZE=1` | Maximizes the window. |
| `LSL_FULLSCREEN=1` | Uses a borderless full-screen window. |
| `LSL_GPU_LOW_POWER=1` | Uses the integrated GPU on a laptop with two GPUs. |
| `LSL_FPS_CAP=<n>` | Limits the frame rate to n frames a second. |

### Log

| Variable | Effect |
|---|---|
| `SPDLOG_LEVEL=<level>` | Sets the detail of the log on the terminal: `debug`, `info`, or `warn`. |

More variables are available for profiling and tests. See [technical/building.md](technical/building.md).

LSL itself reads its configuration from `lsl_api.cfg`. See the [LSL documentation](https://labstreaminglayer.readthedocs.io/info/lslapicfg.html).

## `xdf_record`

```text
xdf_record <out.xdf> [--seconds N] [--streams a,b] [--resolve-wait S]
```

| Option | Default | Effect |
|---|---|---|
| `<out.xdf>` | | The output file. |
| `--seconds N` | Until Ctrl+C | The length of the recording in seconds. |
| `--streams a,b` | All streams | The exact names of the streams to record, with commas between them. |
| `--resolve-wait S` | `2` | The time in seconds to look for streams before the recording starts. |

| Exit code | Meaning |
|---|---|
| `0` | The recording is complete. |
| `1` | The tool found no streams that match, or could not open the file. |
| `2` | The arguments are not correct. |

## `xdf_replay`

```text
xdf_replay [-h] [--list] [--streams STREAMS] [--prefix PREFIX] [--suffix SUFFIX]
           [--start START] [--duration DURATION] [--speed SPEED] [--loop]
           [--delay DELAY] [--chunk-ms CHUNK_MS] [--lookahead LOOKAHEAD]
           [--sync | --no-sync] [--dejitter | --no-dejitter]
           [--timestamps {recording,scaled}] [--source-id-prefix PREFIX]
           file
```

| Option | Default | Effect |
|---|---|---|
| `file` | | The XDF file (`.xdf`). |
| `--list` | | Prints the streams in the file, and stops. |
| `--streams STREAMS` | All streams | A list of stream IDs, name patterns, or `type:<pattern>` items, with commas between them. |
| `--prefix PREFIX` | | Text to put before each stream name. |
| `--suffix SUFFIX` | | Text to put after each stream name. |
| `--start START` | `0` | The start of the replay, in seconds after the first sample in the file. |
| `--duration DURATION` | To the end | The length of the replay in seconds. |
| `--speed SPEED` | `1` | The speed factor. |
| `--loop` | | Repeats the replay. The timestamps continue to increase. |
| `--delay DELAY` | `0.5` | The time in seconds between the creation of the streams and the first sample. This gives the receivers time to connect. |
| `--chunk-ms CHUNK_MS` | `20` | The interval in ms at which the tool sends the data of a regular stream. |
| `--lookahead LOOKAHEAD` | `2` | The time in seconds that the tool reads ahead of the replay. |
| `--sync`, `--no-sync` | On | Applies the clock synchronization of `pyxdf`. |
| `--dejitter`, `--no-dejitter` | Off | Applies the timestamp dejitter of `pyxdf`. When it is off, the replay keeps the recorded jitter. |
| `--timestamps {recording,scaled}` | `recording` | Sets how the timestamps follow a speed other than 1. See [Timestamps at a speed other than 1](record-and-replay.md#timestamps-at-a-speed-other-than-1). |
| `--source-id-prefix PREFIX` | `replay-` | Text to put before each source ID. Give `""` to keep the recorded source IDs. |
