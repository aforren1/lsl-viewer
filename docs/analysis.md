# Analyze signals

This guide shows how to use the three analysis windows: the spectrum, the spectrogram, and the marker-aligned average (ERP). Open them from the **View** menu. You can open more than one window of each type.

Each window has a control strip on the left and a plot on the right. A window needs a connected data stream. See [View streams](view-streams.md) for how to connect one.

## Raw or filtered signal

The spectrum and the spectrogram have a **Filtered** checkbox.

- Selected: the window reads the signal after the filters that are on in the plot of the stream.
- Cleared: the window reads the raw signal.

## Spectrum

**View > New spectrum** shows the power spectral density (PSD) of one or more channels.

1. In the **stream** list, select the stream.
2. Select the channels in the list at the bottom of the strip. Use the filter field above the list to find a channel. **All** and **None** apply to the channels that the filter shows.

| Control | Effect |
|---|---|
| **N** | The FFT size: 512, 1024, 2048, or 4096 samples. A larger size gives a finer frequency resolution and a slower response. |
| **dB** | Shows the power in dB. When it is cleared, the scale is linear. |
| **Fit Hz** | Sets the frequency axis to the band that holds the signal energy. |

**Fit Hz** helps when the sample rate is much higher than the frequencies of interest. Audio at 48 kHz is an example, because the tones are far below the Nyquist frequency.

## Spectrogram

**View > New spectrogram** shows how the spectrum of one channel changes with time.

1. In the **stream** list, select the stream.
2. In the **channel** list, select the channel.

| Control | Effect |
|---|---|
| **NFFT** | The FFT size: 128, 256, 512, or 1024 samples. |
| **span (s)** | The length of time that the window shows, from 2 s to 60 s. |
| **dB** | The range of the color scale. Drag the two values. |
| **Hz** | The frequency range that the window shows. Drag the two values, or zoom the Y axis with the mouse. |
| **Fit Hz** | Sets the frequency range to the band that holds the signal energy. |

The spectrogram needs a stream with a regular rate.

## ERP (marker-aligned average)

**View > New ERP (marker average)** cuts epochs around the events of a marker stream, and shows the average of the epochs. The single-trial traces show below the average.

The window needs one connected data stream with a regular rate and one connected marker stream.

1. In the **stream** list, select the data stream.
2. In the **channel** list, select the channel. To average many channels, select **all channels**, and set the number of channels with **max ch**.
3. In the **trigger** list, select the marker stream.
4. Set the epoch with **pre (ms)** and **post (ms)**. These are the times before and after each event.

| Control | Effect |
|---|---|
| **match** | Uses only the events with the labels that you give. See below. |
| **baseline** | Subtracts the mean of the time before the event from each epoch. |
| **raster** | Shows the average as a heatmap of channels by time, with one row for each channel. |
| **Clear** | Removes the collected epochs and starts the average again. |

The strip shows the number of epochs in the average. A change of the stream, the channel, the trigger, the labels, or the epoch times also starts the average again.

### Select the events by label

When **match** is empty, the window uses all events of the marker stream.

To use only some events, type the label. The label must be exactly the same as the text of the event. To give more than one label, put a pipe with a space on each side between them:

```text
target
start_a | start_b
```

A label can contain a pipe that has no spaces around it.

### Epochs that the average leaves out

If an epoch includes a dropout, the average does not use it, because the samples on the two sides of the gap are not adjacent in time. The same applies to an epoch that includes a seek of a replay. The strip shows the number of these epochs as `(N left out)`.
