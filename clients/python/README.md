# lsl-viewer-rc

A Python client for the remote control port of lsl-viewer. It speaks protocol 2.

The library is one file, `lsl_viewer_rc.py`, with no dependencies outside the standard library. It needs Python 3.9 or later. The `discover()` function also needs `pylsl`.

## Install

Install from the repository:

```sh
uv pip install "git+https://github.com/aforren1/lsl-viewer#subdirectory=clients/python"
```

To use `discover()`, install the `discover` extra:

```sh
uv pip install "lsl-viewer-rc[discover] @ git+https://github.com/aforren1/lsl-viewer#subdirectory=clients/python"
```

Or copy `lsl_viewer_rc.py` into your project.

## Quick start

1. Start the viewer with the control port open:

   ```sh
   LSL_RC_PORT=22345 lsl_viewer
   ```

2. Connect, select the streams, and record:

   ```python
   import time
   import lsl_viewer_rc as rc

   with rc.Client("127.0.0.1", 22345) as viewer:
       for s in viewer.streams():
           print(s.key, s.type, s.channels, s.rate)

       # Select by key. An unknown key makes the viewer refuse the whole command.
       eeg = [s.key for s in viewer.streams() if s.type in ("EEG", "Markers")]
       viewer.select(eeg)

       viewer.set(subject="01", session="01", task="posner", run=1)
       print(viewer.start())    # recording -> /path/sub-01_..._eeg.xdf
       time.sleep(10)           # Run the experiment here.
       viewer.stop()

       print(viewer.status())
       path = viewer.get("data/")   # Copy the file to this machine.
       print("saved", path)
   ```

3. If you do not know the host or the port, find the viewers through LSL:

   ```python
   for ep in rc.discover():
       print(ep.hostname, ep.port, ep.protocol_version)
       with ep.connect() as viewer:
           print(viewer.status())
   ```

## API reference

### `Client(host="127.0.0.1", port=22345, timeout=5.0)`

Opens a connection and reads the banner. Use it as a context manager, or call `close()`.

- Raises `ProtocolError` if the server is not protocol 2 or later.
- Raises `RemoteError` if the server has too many clients.
- `timeout` is in seconds. Keep it above 2 s, because `select`, `start`, and `stop` can wait 2 s for the viewer.

A `Client` is not thread-safe. Use one `Client` from one thread.

| Member | Returns | Description |
|---|---|---|
| `protocol` | `int` | The protocol version from the banner. |
| `status()` | `Status` | The recording state. |
| `streams()` | `list[Stream]` | The streams that the viewer can see. |
| `selected()` | `list[str]` | The keys of the selected streams. |
| `help()` | `list[str]` | The help text of the server. |
| `select(keys)` | `str` | Selects `"all"`, `"none"`, one key, or an iterable of keys. An empty iterable selects none. A key that contains `,` or starts or ends with a space raises `ValueError`. |
| `set(field, value)` or `set(**fields)` | `str` | Sets fields of the filename template: `subject`, `session`, `task`, `run`, `acq`, `modality`. |
| `filename(path)` | `str` | Sets the output path or template. |
| `start(path=None)` | `str` | Starts the recording. |
| `stop()` | `str` | Stops the recording. |
| `get(dest=".", timeout=120.0)` | `Path` | Downloads the last completed recording. See below. |
| `command(line)` | `str` | Sends one raw command and returns the first reply line as it is. It refuses `streams`, `selected`, `help`, and `get` with `ValueError`, because their extra lines would break the next reply. Use their methods. |
| `close()` | `None` | Closes the connection. |

The commands that return `str` return the reply text without the `ok: ` prefix. If the server replies `error: ...`, they raise `RemoteError`, and the connection stays open.

`get(dest)` writes to a temporary file in the target directory and renames it when all the bytes are in. If `dest` is a directory, or ends with `/`, the file keeps the name from the server. Otherwise, `dest` is the file path. The directory must exist. An existing file is replaced. `timeout` is the longest wait for each chunk, not for the full file. If the transfer stops early, `get` raises `ProtocolError` and deletes the temporary file.

### `Status`

A dataclass with `recording: bool`, `seconds: float`, `streams: int`, `bytes: int`, and `file: str`.

### `Stream`

A dataclass with `key: str`, `name: str`, `type: str`, `channels: int`, `rate: float`, and `recording: bool`. A `rate` of `0.0` means an irregular rate. `recording` is true when the stream is selected.

### `discover(timeout=2.0)`

Finds the viewers through their LSL beacon and returns `list[Endpoint]`. Needs `pylsl`. If `pylsl` is missing, raises `ImportError`.

### `Endpoint`

A dataclass with `host`, `port`, `pid`, `bind`, `hostname`, `source_id`, and `protocol_version`. When `bind` is `loopback`, `host` is `127.0.0.1`. `protocol_version` is `0` when the viewer did not send its description. `connect(**kw)` returns a `Client`.

### Errors

| Exception | Cause |
|---|---|
| `RcError` | The base class of the two errors below. |
| `RemoteError` | The server replied `error: ...`. The message is the full reply line. |
| `ProtocolError` | The reply does not follow the protocol, or the server closed the connection during a reply. The client closes itself, because it cannot find the start of the next reply. |

Socket errors and timeouts raise `OSError`. After an `OSError`, the client is closed.

## Run the tests

```sh
cd clients/python
uv run --with pytest pytest
```

The tests use a fake server, thus they do not need a viewer. To also test against a real viewer, set `LSL_VIEWER_EXE` to the path of the viewer executable. The test starts the viewer with `LSL_RC_PORT=22421 LSL_DEMO=1 LSL_PORTABLE=1`, and stops it at the end. Portable mode keeps the viewer settings in `lsl_viewer_data/` next to the executable, thus the test does not change the settings of your usual viewer.
