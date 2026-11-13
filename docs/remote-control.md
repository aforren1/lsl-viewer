# Control the recording from a script

The viewer has a TCP control port. Through this port, an experiment script selects the streams, sets the file name, starts and stops the recording, and copies the file to its own machine.

Client libraries are available for Python, MATLAB, and GNU Octave. To write a client in a different language, see [technical/remote-control-protocol.md](technical/remote-control-protocol.md).

## Turn on the control port

Do one of these:

- In the viewer, select **App > Remote control**.
- Set the environment variable `LSL_RC_PORT` before you start the viewer:

  ```sh
  LSL_RC_PORT=22345 lsl_viewer
  ```

While the port is on, the right side of the menu bar shows its address and the number of connected clients. Hover over the text to see the addresses of the clients.

The default port is 22345. If that port is in use and you did not set `LSL_RC_PORT`, the viewer uses a different port, and the menu bar shows it. If you set `LSL_RC_PORT` and the port is in use, the viewer does not open a control port.

Up to four clients can connect at the same time.

## Permit access from a different machine

By default, the port accepts connections from the same machine only (127.0.0.1).

To permit connections from other machines, select **App > Allow LAN access (no auth)**, or set `LSL_RC_BIND=all`.

**Caution:** the port has no authentication. All machines that can reach the port can start and stop the recording, and can download the last recording. Permit LAN access on trusted networks only. Look at the client count in the menu bar to see who is connected.

On Windows, the firewall can ask for permission when you turn on LAN access. Permit the viewer on private networks.

## Install a client library

**Python.** The library is one file with no dependencies. It needs Python 3.9 or later. Install it:

```sh
uv pip install "git+https://github.com/aforren1/lsl-viewer#subdirectory=clients/python"
```

As an alternative, copy `clients/python/lsl_viewer_rc.py` into your project.

**MATLAB and GNU Octave.** The same code runs in MATLAB R2019b or later and in Octave 6 or later. Add the folder to the path:

```matlab
addpath('/path/to/lsl_viewer/clients/matlab');
```

For the full API, see the [Python client](../clients/python/README.md) and the [MATLAB and Octave client](../clients/matlab/README.md).

## Record one run

The script lists the streams, selects the streams that it wants, sets the fields of the file name, records, and copies the file.

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

These rules apply:

- `select` connects the streams that you give and disconnects the others, as if you clicked them in the rail. The viewer records the connected streams.
- You cannot change the selection during a recording.
- `start` and `stop` return after the viewer has done the work. Thus, when `start` returns, the file is open.
- `set` fills the same fields as **filename fields** in the **Recording** section. See [Name the files](record-and-replay.md#name-the-files).
- `get` copies the last completed recording. Stop the recording before you call it. The destination folder must exist.

## Record all streams

To record all streams that the viewer can see, select `all`:

```python
viewer.select("all")
```

```matlab
rc.select('all');
```

## Record several runs

Change the `run` field before each recording. Each run then goes to its own file.

```python
import lsl_viewer_rc as rc

with rc.Client() as viewer:
    viewer.select("all")
    viewer.set(subject="01", task="posner")
    for run in range(1, 4):
        viewer.set(run=run)
        print(viewer.start())           # recording -> /path/sub-01_..._run-1_eeg.xdf
        run_block(run)                  # Your experiment code.
        viewer.stop()
        viewer.get("data/")
```

If two recordings get the same path, the second one replaces the first one. Make sure that a field of the template changes between the runs.

## Set the file name directly

To use your own file name in place of the fields, give a path or a template:

```python
viewer.filename("pilot/{date}/{subject}_{time}.xdf")
viewer.start()
```

As an alternative, give the path to `start`:

```python
viewer.start("C:/data/pilot_01.xdf")
```

The path is a path on the machine that runs the viewer.

## Stop the script when something is wrong

The libraries give an error when the viewer refuses a command. Thus a typing error in a stream key stops the script before the experiment starts.

In Python, a refused command raises `RemoteError`:

```python
try:
    viewer.select(["no-such-stream"])
except rc.RemoteError as err:
    print(err)                          # error: unknown stream(s): no-such-stream (see `streams`)
    raise
```

In MATLAB and Octave, the error has the identifier `lslrc:remote`:

```matlab
try
  rc.select({'no-such-stream'});
catch err
  if strcmp(err.identifier, 'lslrc:remote'), disp(err.message); end
  rethrow(err);
end
```

To make sure that the streams of a study are present before the first trial, compare the stream list with the list that you expect:

```python
need = {"MyAmp", "MyMarkers"}
have = {s.name for s in viewer.streams()}
if not need <= have:
    raise RuntimeError(f"missing streams: {need - have}")
```

## Check the recording state

`status` gives the state of the recording: if it is on, the time in seconds, the number of streams, the file size, and the path.

```python
st = viewer.status()
print(st.recording, st.seconds, st.streams, st.bytes, st.file)
```

```matlab
s = rc.status();                    % s.recording, s.seconds, s.streams, s.bytes, s.file
```

To make sure that data comes in, read `bytes` two times with some seconds between, and check that the value increases.

## Find the viewer on the network

The viewer announces its control port through LSL. Thus a script does not need to know the host or the port.

```python
import lsl_viewer_rc as rc

for ep in rc.discover():                # Needs pylsl.
    print(ep.hostname, ep.port)
    with ep.connect() as viewer:
        print(viewer.status())
```

```matlab
v = lslrc.discover('Timeout', 2);   % Needs liblsl-Matlab.
rc = lslrc.Client(v(1).host, v(1).port);
```

`discover` finds a viewer on a different machine only if that viewer permits LAN access.

## Try the port by hand

The commands are lines of text, thus you can type them. Connect with `nc` (or `ncat` on Windows), and type `help`:

```text
$ nc localhost 22345
ok: lsl-viewer remote control, protocol 2. type `help`.
status
ok: recording=false seconds=0.0 streams=0 bytes=0 file=
```

For the commands and the replies, see [technical/remote-control-protocol.md](technical/remote-control-protocol.md).

## Problems

| Problem | Solution |
|---|---|
| The client cannot connect from the same machine. | Make sure that the port is on, and that the port in the script is the port in the menu bar. |
| The client cannot connect from a different machine. | Turn on **App > Allow LAN access (no auth)**. Then permit the viewer in the firewall. See [technical/network.md](technical/network.md). |
| `error: too many control clients` | Four clients are connected. Close a client that you do not use. |
| `error: no streams connected` | `start` came before a `select`, or the selection is empty. |
| `error: the viewer did not respond` | The viewer did not do the command in 2 seconds. Send the command again. |
| The client reports a protocol error at the connection. | The viewer is older than protocol 2. Update the viewer. |
