# lsl_viewer documentation

The pages in this folder are for people who use the viewer. The pages in [technical/](technical/) are for people who build the viewer, administer a lab network, or write a remote control client.

## Tutorial

- [Get started](getting-started.md): install the viewer, and run the built-in walkthrough.

## How-to guides

- [View streams](view-streams.md): connect streams, set up the plots, select channels, apply filters, and show markers.
- [Analyze signals](analysis.md): the spectrum, the spectrogram, and the marker-aligned average (ERP).
- [Record and replay](record-and-replay.md): record to XDF, name the files, replay a recording, and use the command-line tools.
- [Control the recording from a script](remote-control.md): start and stop the recording from Python, MATLAB, or Octave.
- [Workspaces and settings](settings.md): save a view, change the appearance, and find the files that the viewer writes.
- [Troubleshooting](troubleshooting.md): what to do when the viewer finds no streams, and other problems.

## Reference

- [Reference](reference.md): the menus, the keyboard shortcuts, the filename template fields, the environment variables, and the options of the command-line tools.
- [Python client](../clients/python/README.md) and [MATLAB and Octave client](../clients/matlab/README.md): the API of the remote control libraries.

## Technical documentation

- [technical/network.md](technical/network.md): the LSL ports, the firewall rules for each platform, and the macOS local network permission.
- [technical/remote-control-protocol.md](technical/remote-control-protocol.md): the TCP protocol of the remote control, for a client in a different language.
- [technical/building.md](technical/building.md): how to build from source, the CMake flags, the test data, the profiling variables, and the repository layout.
- [technical/windows-remote-control-testing.md](technical/windows-remote-control-testing.md): a checklist to verify the remote control server on Windows.
- [DESIGN.md](../DESIGN.md): the architecture and the reasons for it.
