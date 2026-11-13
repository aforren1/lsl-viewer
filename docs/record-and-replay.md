# Record and replay

This guide shows how to record streams to an XDF file, how to name the files, and how to replay a recording to check it. It also shows the two command-line tools, `xdf_record` and `xdf_replay`.

## Record in the viewer

The **Recording** section is in the **Streams** rail, below the stream list.

1. Connect the streams that you want to record. The viewer records all connected streams, and only those. There is no separate selection.
2. Set the folder. Type it in the first field, or select **Browse**. The default is `Documents/lsl-recordings`.
3. Set the file name. See [Name the files](#name-the-files).
4. Check the line that starts with `->`. It shows the full path of the file that the viewer will write. Hover over it if the path is too long for the rail.
5. Select **Record**.
6. To stop, select **Stop recording**.

During a recording:

- The **Recording** section shows the time, the number of streams, and the file size. The right side of the menu bar shows `[REC ...]`, which stays visible in all layouts.
- You cannot connect or disconnect streams.
- The filters, the display pause, and the **Clear** buttons of the marker log have no effect on the file. The recorder reads the streams through its own connections, and it writes the raw data.

**Caution:** if a file with the same path exists, the viewer replaces it and does not ask. Change `{run}` for each recording, or put `{datetime}` in the template.

### When the Record button is not available

| Message | Cause | Solution |
|---|---|---|
| `Connect a stream to record.` | No stream is connected. | Connect a stream. |
| `recording held: ... workspace stream(s) missing` | You loaded a workspace, and some of its streams are not on the network. The viewer holds the recording, so that you do not record a session with a stream missing. | Start the missing sources, or select **Dismiss** in the notice at the top of the rail. |

### What the file holds

The file is XDF, the format that LabRecorder writes. The viewer was checked against LabRecorder output with `pyxdf`. The file holds the raw timestamps and the clock offset measurements of each stream. Thus an importer such as `pyxdf` can align the streams to a common clock.

## Name the files

The second field of the **Recording** section is the file name template. The viewer replaces each field in braces with a value. The default template follows the BIDS layout:

```text
sub-{subject}/ses-{session}/{modality}/sub-{subject}_ses-{session}_task-{task}_run-{run}_{modality}.xdf
```

To set the values, open **filename fields** below the template.

| Field | Default value |
|---|---|
| `{subject}` | `P001` |
| `{session}` | `S001` |
| `{task}` | `Default` |
| `{run}` | `001` |
| `{modality}` | `eeg` |
| `{acq}` | Empty. The default template does not use it. |

The viewer fills these fields itself:

| Field | Value | Example |
|---|---|---|
| `{datetime}` | The local date and time when the recording starts. | `20261113_142501` |
| `{date}` | The local date. | `20261113` |
| `{time}` | The local time. | `142501` |

These rules apply:

- The template can contain folders. The viewer creates them when the recording starts.
- If the template is an absolute path, the viewer ignores the folder field.
- A field that the viewer does not know stays in the name as it is. Thus a typing error such as `{subjct}` is visible in the file name.
- To get the default template back, right-click the template field and select **Reset to default template**.

The viewer keeps the folder and the template between sessions. It does not keep the values of the fields.

Examples:

```text
sub-{subject}_task-{task}_run-{run}_eeg.xdf
pilot/{date}/{subject}_{time}.xdf
```

## Replay a recording in the viewer

Use **Playback** to check a recording right after you make it: all streams are present, there are no dropouts, and the markers are in the correct positions.

1. In the **Playback** section at the bottom of the rail, select **Open XDF file...**.
2. Select the file. The dialog opens in the recording folder.

The viewer creates the streams of the file, connects to them, and starts the replay. The time axis of the plots shows the seconds of the recording.

| Control | Effect |
|---|---|
| Position slider | Drag or click to go to a different time. To type the seconds, Ctrl+click the slider. |
| **Pause replay** / **Resume replay** | Stops and continues the data. |
| Speed list | 0.5 to 20 times real time. The viewer keeps the speed between sessions. |
| **Loop** | Starts the file again when it ends. |
| **Stop** | Closes the streams of the replay and removes their plots. |
| **Replay again** | After a stop, plays the same file from the start. |
| **×** | After a stop, removes the replay from the section. |

**Pause replay** and **View > Pause display** are different. **Pause replay** stops the source of the data. **Pause display** (the **P** key) stops only the plots, and the replay continues.

### What to look at

- **The stream rows.** In the stream list, each replayed stream has a play mark and two counts: the samples sent, and the samples in the file. Hover over a row to see the type, the channels, the format, and the rate.
- **Not in the viewer.** Below the controls, the section lists the streams of the file that have no row: `closed` if you disconnected the stream, and `not shown` if it did not connect.
- **Dropouts.** A dropout in the recording shows as a gap in the plot. A seek or the start of a new loop shows as a dashed blue line. The viewer does not show these as dropouts, because no data is missing from the file.
- **Warnings.** If the file is damaged, a button shows the number of warnings. Select it to read the list. A damaged file still plays.
- **late chunks.** The number of chunks that the viewer read from the file too late and then sent at once. Some late chunks after a seek are normal. If the number increases continuously, the disk or the CPU is too slow for the speed that you set.

The replayed streams are visible to all LSL software on the network, as for any LSL source. Each replayed stream has `replay-` before its source ID. Thus a recorder or a workspace that identifies a device by its source ID does not use the replay as the live device.

To open a file at launch, set the environment variable `LSL_REPLAY` to its path.

## Record without the viewer: `xdf_record`

`xdf_record` records from the command line. It has no window, and it needs no GPU. Use it on a machine with no display, or in a script.

```sh
xdf_record <out.xdf> [--seconds N] [--streams name1,name2] [--resolve-wait S]
```

| Option | Effect |
|---|---|
| `<out.xdf>` | The output file. |
| `--seconds N` | Records for N seconds. Without this option, the tool records until you push Ctrl+C. |
| `--streams a,b` | Records only the streams with these names. The names must be exact. Without this option, the tool records all streams that it finds. |
| `--resolve-wait S` | The time in seconds to look for streams before the recording starts. The default is 2. |

Examples:

```sh
xdf_record session.xdf
xdf_record session.xdf --seconds 300 --streams MyAmp,MyMarkers
```

The tool looks for the streams one time, at the start. A stream that comes after that is not in the file.

## Replay from the command line: `xdf_replay`

`xdf_replay` plays an XDF file as live LSL streams, for the viewer or for other LSL software. It uses the same replay engine as **Playback**.

```sh
xdf_replay rec.xdf --list                 # Show the streams. No replay.
xdf_replay rec.xdf                        # Replay all streams one time.
xdf_replay rec.xdf --speed 10             # Check a recording quickly.
xdf_replay rec.xdf --loop --suffix _replay
xdf_replay rec.xdf --streams 2,type:Markers --start 60 --duration 30
```

Push Ctrl+C to stop. For all options, see the [Reference](reference.md#xdf_replay) or run `xdf_replay --help`.

### Select the streams

`--streams` takes a list with commas between the items. Each item is one of these:

- A stream ID, as `--list` shows it.
- `type:<pattern>`, such as `type:EEG`.
- A name pattern, such as `"Mock*"`. The wildcards are `*` and `?`.

### Keep the replay apart from the live devices

Other machines on the network also see the replayed streams. A recorder that selects streams by name and host, such as LabRecorder with `RequiredStreams`, can select a replay as the device. To prevent this, add a suffix to the names:

```sh
xdf_replay rec.xdf --suffix _replay
```

### Timestamps at a speed other than 1

`--timestamps` sets how the timestamps follow the speed.

| Value | Effect |
|---|---|
| `recording` (default) | The timestamps keep the spacing of the recording. The viewer then shows the seconds of the recording on its time axis, the dropouts with their recorded width, and the markers on the correct samples. |
| `scaled` | The spacing is divided by the speed. The timestamps then follow the local clock. In the viewer, the dropouts become narrower, and the markers move away from their samples. |

Use `recording` with the viewer. Use `scaled` for software that compares the timestamps with the local clock.

### Limits

- The tool does not read compressed files (`.xdfz`). Decompress the file first.
- If the structure of a damaged file is broken, the data up to the next boundary chunk is lost. This is about 10 s in a file from LabRecorder or from the viewer. A file that was cut off plays up to the cut.
