# Troubleshooting

## The viewer finds no streams

First, find out if the problem is the viewer or the network. Select **Tools > Emit demo streams**. If the demo streams show in the rail, the viewer operates correctly, and the problem is between the machines.

LSL finds streams with UDP broadcast and multicast, and transfers the data with TCP. Thus these conditions are necessary:

- The source and the viewer are on the same subnet.
- The firewall of each machine permits the LSL traffic.

Do the checks for your system.

**Windows**

- If you selected **Cancel** in the Windows Firewall prompt at the first launch, Windows made a rule that blocks the viewer. Install the viewer again with the installer, which repairs the rules, or correct the rules by hand.
- Windows often gives the **Public** class to a lab network that has no internet connection. The usual firewall rules do not apply to public networks. Set the network to **Private**.
- If the machine has two network adapters, such as a lab network and Wi-Fi, LSL can send its queries through the incorrect adapter.

**macOS**

- On macOS 15 (Sequoia) and later, go to **System Settings > Privacy & Security > Local Network** and make sure that the switch for **LSL Viewer** is on.
- After an update of the viewer, the switch can show as on while the permission does not apply. Set the switch off and then on again.
- For `xdf_record` and `xdf_replay`, the permission belongs to your terminal application, not to the viewer.

**Linux**

- Fedora, RHEL, and openSUSE block inbound traffic by default. Open the LSL ports in `firewalld`.

The commands for each of these checks are in [technical/network.md](technical/network.md). If you do not administer the machine, give that page to the person who does.

## The streams show in the list but do not connect

The firewall permits the discovery traffic (UDP) but not the data (TCP). Permit the viewer as a program, not as a list of ports. See [technical/network.md](technical/network.md).

## A stream shows `[no data]`

The stream is connected, but no data came in the last second. The source stopped, or the network connection has a problem. The viewer continues to wait, and the plot shows a gap for the missing time when the data comes back.

## The Record button is not available

See [When the Record button is not available](record-and-replay.md#when-the-record-button-is-not-available).

## A stream shows as events, but it is data (or the opposite)

Right-click the row of the connected stream, and select **Treat as data stream** or **Treat as marker (show as events)**. See [Change how the viewer treats a stream](view-streams.md#change-how-the-viewer-treats-a-stream).

## A window is gone or out of reach

Select **View > Reset window layout**.

## The text is too small or too large

Change **UI scale** in the **App** menu.

## The viewer does not start, or the window is empty

The viewer needs a GPU and a driver that support Direct3D 12 or Vulkan (Windows), Metal (macOS), or Vulkan (Linux). See the [system requirements](../README.md#system-requirements).

- On Linux, install the Vulkan driver for your GPU. On Debian and Ubuntu, the package for Intel and AMD GPUs is `mesa-vulkan-drivers`.
- On a laptop with two GPUs, set `LSL_GPU_LOW_POWER=1` to use the integrated GPU.

To see which GPU the viewer uses, open the **Debug** menu.

## The display is not smooth

- Open **Debug > Performance** to see the frame times, and to set VSync or a frame rate limit.
- For a stream with many channels, select **Raster** in the **Display** section of the plot. One heatmap is much less work to draw than many line traces.
- Close the analysis windows that you do not use.

## The remote control does not connect

See [Problems](remote-control.md#problems) in the remote control guide.

## Report a problem

Select **App > About**, then **Copy version info**, and include the text in your report at <https://github.com/aforren1/lsl-viewer/issues>.
