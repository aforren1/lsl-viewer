# View streams

This guide shows how to connect streams and how to set up the time series plots.

## Connect and disconnect streams

The **Streams** rail on the left lists the streams that LSL finds on the network. Each row shows the name, the type, the number of channels, and the rate. Hover over a row to see the stream identifier and the host.

- To connect a stream, click its row. A data stream opens a plot tab. A marker stream has no plot of its own: its events show on the plots of the data streams.
- To bring the plot of a connected data stream to the front, click its row. For a connected marker stream, the click opens the **Marker events** log.
- To disconnect a stream, right-click its row and select **Disconnect**.

A connected stream has a mark before its name:

| Mark | Meaning |
|---|---|
| `[on]` | The stream is connected and sends data. |
| `[no data]` | The stream is connected, but no data came in the last second. |

You cannot connect or disconnect streams during a recording. Stop the recording first.

To connect all streams automatically at launch, set the environment variable `LSL_AUTOCONNECT=1`.

### Change how the viewer treats a stream

The viewer shows a stream of type `Markers`, or a stream of text, as events. It shows other streams as data. You can change this for some streams:

- A data stream with an irregular rate, such as a trigger channel of integers: right-click the row and select **Treat as marker (show as events)**.
- A marker stream of numbers: right-click the row and select **Treat as data stream**. A marker stream of text cannot become a data stream.

The viewer resamples a data stream with an irregular rate to a regular rate when it connects. The **Info** section of the plot shows that rate.

## Set up a plot

Each plot has a control strip on its left side. To give the plot the full width, select **< hide**. To show the strip again, select **>**.

**Reset view** puts the time window, the gains, and the spacing back to the defaults. It does not change the filters or the selected channels.

### Select the display mode

Open **Display** in the control strip.

| Control | Effect |
|---|---|
| **History (s)** | The length of the time window, from 1 s to 60 s. |
| **Stacked montage** | On: each channel has its own lane with its name. Off: all channels share one Y axis (overlay). |
| **Raster** | Draws the stacked montage as one heatmap, where the color shows the amplitude. Use it for 32 channels or more, where line traces become too thin to read. |
| **Gain/lane** | The amplitude that one lane height shows, in the unit of the stream. Stacked montage only. |
| **Spacing** | The vertical distance between the channels. Overlay only. |
| **Line width** | The width of the traces. |

In overlay mode, zoom and pan the Y axis with the mouse. Double-click the plot to fit the axis to the data again.

### Select the channels

Open **Channels** in the control strip.

- Select or clear the checkbox of a channel to show or hide it.
- Type in the **filter** field to make the list shorter. Examples: `Cz` shows the channels that have `Cz` in their name, `Cz,Pz` shows the channels that match one of the two, and `-EOG` hides the channels that have `EOG` in their name.
- **All** and **None** apply to the channels that the filter shows. Thus `Cz` and then **All** selects only the `Cz` channels.

### Go through the pages

A stream connects with all of its channels enabled. The plot shows them in pages of 16. A stream with 16 channels or fewer fits on one page.

To go to a different page, do one of these:

- Use the arrows in the **Channels** section.
- Click the page label in the corner of the plot.
- Put the pointer on the plot and push **Page Down** or **Page Up**.

To change the page size, change **per page** in the **Channels** section. To show all enabled channels at once, clear **Paginate**.

### Find one channel

Hover over a channel name in the stacked montage or the raster. The viewer highlights its lane. Click the name to keep the highlight. To remove the highlight, click the name again, or right-click a channel name.

### Set the gain of one channel

**Channel gains** shows when the plot is a stacked montage and **Raster** is off. Each visible channel has a gain that multiplies its trace.

- **Auto** sets each gain from the measured amplitude of the channel. Use it when the channels have very different amplitudes.
- **Reset** sets the gains back to 1.

## Apply filters

The filters are in the **Display** section. You can use them in any combination. When all of them are off, the plot shows the raw signal.

| Control | Effect |
|---|---|
| **High-pass** | Removes DC and slow drift. The cutoff is 0.1 Hz to 5 Hz. |
| **Notch** | Removes the mains frequency. The button beside the slider changes between 50 Hz and 60 Hz. |
| **Low-pass** | Removes the content above the cutoff, such as muscle activity. The cutoff is 5 Hz to 120 Hz. |
| **Reference** | **Avg (CAR)** subtracts the common average. **Channel** subtracts one channel that you select. |

The viewer applies the stages in this sequence: reference, high-pass, notch, low-pass.

The common average uses the EEG channels only. The viewer reads the channel types from the stream metadata, and excludes the EOG, EMG, and trigger channels from the average.

The filters change the display only. A recording always holds the raw data.

## Pause and compare

- **View > Pause display**, or the **P** key, stops all plots. The data continues to come in, and a recording continues.
- **View > Lock time axes** gives one time window to all time series plots. The plots stay aligned, and a pan or zoom of one plot moves all of them. **History (s)** then sets the shared window.

A dropout shows as a gap at its real position in time. The viewer does not join the data across the gap.

## Show channels of several streams in one plot

A combined plot shows channels of different streams on one time axis.

1. Select **View > New combined plot**.
2. Select **Add...**, then the stream and the channels. As an alternative, drag a channel from a stream plot to the combined plot. You can drag a row of the **Channels** list or a lane name.

The **Traces** section lists the traces. Drag a row to change the sequence. **fit** scales one lane to its visible range. **x** removes the trace.

In the **Display** section, **Stacked lanes** gives each trace its own lane. When it is off, the traces overlay each other, with one Y axis for each unit.

Each trace shows what its stream plot shows: the filtered signal if a filter is on, and the raw signal if not. The stream plot keeps the connection and the filters. If you close the stream plot, the lane stays in the combined plot, and it shows data again when the stream connects again.

[Workspaces](settings.md#workspaces) save the combined plots.

## Work with markers

A connected marker stream shows its events as labeled lines on all time series plots.

To select which marker streams a plot shows, open **Markers** in the control strip of that plot. Clear **Overlay markers** to hide all events, or clear the checkbox of one stream. The section shows only when a marker stream is connected.

### The Marker events log

**View > Marker events** opens a log of the events of each marker stream. You can use the log when no data stream is connected.

| Control | Effect |
|---|---|
| **Relative time** | Shows the time as seconds after the first event of all marker streams. When it is off, the log shows the LSL clock. |
| **Lock scroll to time** | When you scroll one log, the other logs scroll to the nearest time. |
| **Depth** | The number of recent events that the log keeps for each stream. |
| **Clear all** | Removes the events from the logs and from the plots. |
| **Copy CSV** | Copies the events of one stream to the clipboard. The columns are the time, the time after the event before, the time after the last event with the same label, and the label. |

**Clear** and **Clear all** change the display only. They do not change a recording.

## Read the stream information

Open **Info** in the control strip of a plot.

| Line | Meaning |
|---|---|
| **type**, **source id**, **channels**, **unit**, **ch types** | The metadata of the stream. |
| **positions** | The number of channels that have a sensor position in the metadata. |
| **nominal** | The rate that the stream declares. |
| **measured** | The rate from the timestamps of the last 5 s. A device that is off its declared rate, or that loses samples, shows here. |
| **clock offset** | The LSL time correction between the source and this machine, with its uncertainty. The uncertainty is small on one computer, and larger on a busy or wireless network. |
| **drift correction** | The correction that the viewer applies to keep the time axis aligned with the timestamps of the stream. |
| **dropouts** | The number of dropouts since the stream connected. |
