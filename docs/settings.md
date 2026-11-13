# Workspaces and settings

This guide shows how to arrange the windows, how to save a view as a workspace, how to change the appearance, and where the viewer keeps its files.

## Arrange the windows

The **Streams** rail has a fixed position on the left. It holds the stream list, and below it the **Recording** and **Playback** sections.

The plots and the analysis windows are tabs in a docking layout:

- To move a window, drag its tab. Drop it on the side of a different window to split the area, or on the center to make it a tab there.
- To put all windows back to the default layout, select **View > Reset window layout**. Use this also when a window is in a position that you cannot reach.

Each session starts with the default layout. To use a layout again in a different session, save it in a workspace.

## Workspaces

A workspace is a saved view with a name. Use one workspace for each setup or study.

| A workspace holds | A workspace does not hold |
|---|---|
| The connected streams | The theme |
| The filters, the channels, and the gains of each stream | The UI scale |
| The open analysis windows and combined plots | The recording folder and the file name template |
| The window layout | |

Thus, when you load a workspace, the appearance and the recording settings do not change.

### Save a workspace

1. Set up the streams, the plots, and the windows.
2. Open the **Workspaces** menu.
3. Type a name, and select **Save**.

To update a workspace, save it again with the same name.

### Load a workspace

Open the **Workspaces** menu and click the name.

The viewer connects the streams of the workspace again. It identifies each stream by its source ID and its name. Thus the settings apply to the same device in a later session.

If a stream of the workspace is not on the network, the top of the rail shows a notice with the names of the missing streams. The viewer holds the recording until one of these occurs:

- The missing streams connect.
- You select **Dismiss** in the notice.

This prevents a recording that has a stream missing without your knowledge.

### Delete a workspace

Open the **Workspaces** menu and select **×** beside the name.

## Change the appearance

The controls are in the **App** menu.

- **Light theme** changes between the light theme and the dark theme.
- **UI scale** changes the size of the text and the controls, from 0.5 to 2.0. Type a value and push Enter, or use the **-** and **+** buttons.

## What the viewer keeps between sessions

- The theme and the UI scale.
- The recording folder and the file name template.
- The folder of the last replayed file, and the replay speed.
- Your choice to show a stream as markers or as data.

The viewer does not keep the window layout, the connected streams, or the values of the filename fields. Use a workspace for the layout and the streams.

## Where the viewer keeps its files

| Files | Location |
|---|---|
| Recordings | `Documents/lsl-recordings`. If there is no Documents folder, the viewer uses your home folder. You can change the folder in the **Recording** section. |
| Settings and workspaces on Windows | `%APPDATA%\lsl_viewer\` |
| Settings and workspaces on macOS | `~/Library/Application Support/lsl_viewer/` |
| Settings and workspaces on Linux | `~/.local/share/lsl_viewer/` |

The settings are in `imgui.ini`. The workspaces are in the `workspaces` folder, with one `.lslws` file for each workspace. To move a workspace to a different machine, copy its file.

To set all settings back to the defaults, close the viewer and delete `imgui.ini`.

## Portable mode

In portable mode, the viewer keeps all of its files in one folder, `lsl_viewer_data`, beside the executable. The settings, the workspaces, and the default recording folder (`lsl_viewer_data/recordings`) are there. Use portable mode for a USB drive, or to keep a setup apart from your usual settings.

To turn on portable mode, do one of these:

- Put a file with the name `portable.txt` beside the executable, or beside the `.AppImage` file. The file can be empty.
- Set the environment variable `LSL_PORTABLE=1`.

The portable downloads for Windows, macOS, and Linux include `portable.txt`. To use the usual locations with these downloads, delete the file.

## Start the viewer with options

Environment variables set some options at launch. For example, this command starts the viewer with the demo streams and the control port:

```sh
LSL_DEMO=1 LSL_RC_PORT=22345 lsl_viewer
```

In PowerShell:

```powershell
$env:LSL_DEMO = "1"; $env:LSL_RC_PORT = "22345"; .\lsl_viewer.exe
```

For the list of variables, see the [Reference](reference.md#environment-variables).
