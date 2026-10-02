# lslrc: MATLAB and Octave client for the lsl-viewer remote control

`lslrc` controls the recording of a running lsl-viewer over TCP. It uses protocol 2 of the remote control. The same code runs in MATLAB and in GNU Octave.

## Requirements

- **MATLAB** R2019b or later. MATLAB R2023a is tested. You do not need a toolbox.
- **GNU Octave** 6 or later. Octave 6.4, 9.4, 10.1, and 11.3 are tested. Octave needs one of these two items:
  - A Java runtime that Octave can load. To examine this, type `usejava('jvm')`. The result must be `1`.
  - The `instrument-control` package. The Octave installer for Windows includes it. On Linux, install it with your package manager (for example, `sudo apt install octave-instrument-control`) or with `pkg install -forge instrument-control`. `lslrc` loads the package for you.
- **lsl-viewer** with remote control protocol 2 or later. An older viewer causes the error `lslrc:protocol`.
- **liblsl-Matlab** is necessary only for `lslrc.discover`.

## Quick start

1. Start lsl-viewer with a control port. For example, set `LSL_RC_PORT=22345`, or enable the port in the Recording panel.
2. Add the folder that contains `+lslrc` to the path:

   ```matlab
   addpath('/path/to/lsl_viewer/clients/matlab');
   ```

3. Connect, select the streams, record, and get the file:

   ```matlab
   rc = lslrc.Client('127.0.0.1', 22345);

   st = rc.streams();                  % struct array, one element for each stream
   disp({st.key});
   rc.select({st(1).key, st(2).key});  % or rc.select('all')

   rc.set('subject', '01');
   rc.set('task', 'posner');
   rc.set('run', 1);

   rc.start();                         % returns when the file is open
   % Present the stimuli here.
   rc.stop();

   s = rc.status();                    % s.recording, s.seconds, s.bytes, s.file
   p = rc.get(tempdir);                % saves the file, returns its path
   rc.close();
   ```

If the viewer refuses a command, the method gives an error with the identifier `lslrc:remote`. Thus a typing error in a stream key stops the script before the experiment starts.

## API reference

### Constructor

```matlab
rc = lslrc.Client(host, port, Name, Value, ...)
```

| Argument | Default | Description |
|---|---|---|
| `host` | `'127.0.0.1'` | The address of the viewer. |
| `port` | `22345` | The control port of the viewer. |
| `'Timeout'` | `5` | The maximum time in seconds to wait for a reply. It must be more than 2 s, because `select`, `start`, and `stop` can wait 2 s for the viewer. |
| `'GetTimeout'` | `60` | The maximum time in seconds without data during `get`. |
| `'Transport'` | `'auto'` | `'java'`, `'tcpclient'`, or `'auto'`. See [Transports](#transports). |

The constructor reads the banner of the server. It gives an error if the protocol is older than 2, or if the viewer has too many clients.

### Properties (read-only)

| Property | Description |
|---|---|
| `protocol` | The protocol version from the banner, for example `2`. |
| `banner` | The first line that the server sent. |
| `transport` | `'java'` or `'tcpclient'`. |
| `host`, `port`, `Timeout`, `GetTimeout` | The values from the constructor. |

### Methods

| Method | Returns | Description |
|---|---|---|
| `rc.status()` | struct | The fields are `recording` (logical), `seconds`, `streams`, `bytes` (double), and `file` (char). `file` can be empty, and it can contain spaces. |
| `rc.streams()` | struct array | The streams that the viewer can see. The fields are `key`, `name`, `type`, `channels`, `rate`, and `recording`. `rate` is `0` for an irregular stream. `recording` is `true` for a selected stream. If there are no streams, the result is a 0x0 struct with these fields. |
| `rc.selected()` | cellstr | The keys of the selected streams, as a column. |
| `rc.select(keys)` | char | Selects the streams to record. `keys` is `'all'`, `'none'`, or a cellstr of keys from `streams()`. The viewer refuses the full list if one key is unknown. |
| `rc.set(field, value)` | char | Sets one field of the file name template. `field` is `subject`, `session`, `task`, `run`, `acq`, or `modality`. `value` is char or a number. |
| `rc.filename(path)` | char | Sets the output path or the template. |
| `rc.start()`, `rc.start(path)` | char | Starts the recording. The reply contains the path of the file. |
| `rc.stop()` | char | Stops the recording. |
| `rc.get(dest)` | char | Saves the last completed recording, and returns its path. If `dest` is a folder, the file keeps the name from the viewer. Otherwise, `dest` is the file path. The default is the current folder. |
| `rc.command(line)` | char | Sends one command line, and returns the full reply. This method does not give an error for an `error:` reply. A multi-line reply has its lines joined with newline characters. |
| `rc.close()` | | Closes the connection. `delete(rc)` and `clear rc` also close it. |

`select`, `set`, `filename`, `start`, and `stop` return the reply line from the viewer, for example `'ok: recording -> C:\data\sub-01_eeg.xdf'`.

`get` writes the data to `<path>.part`, examines the size, and then moves the file to `<path>`. If the transfer fails, `get` deletes the partial file and closes the connection.

### Discovery

```matlab
v = lslrc.discover('Timeout', 2, 'Minimum', 1);
rc = lslrc.Client(v(1).host, v(1).port);
```

`lslrc.discover` finds the control ports that the viewers announce through LSL. It returns when it finds `Minimum` viewers, or after `Timeout` seconds. To find all viewers, set `Minimum` to a large number. The struct fields are `host`, `port`, `pid`, `bind`, `protocol`, `hostname`, and `source_id`. If the viewer binds to loopback only, `host` is `127.0.0.1`.

This function needs [liblsl-Matlab](https://github.com/labstreaminglayer/liblsl-Matlab) on the path. If `lsl_loadlib` is not on the path, the error is `lslrc:nolsl`.

### Errors

| Identifier | Cause |
|---|---|
| `lslrc:connect` | The client cannot connect to the host and the port. |
| `lslrc:protocol` | The server is older than protocol 2, or a reply has an unexpected format. |
| `lslrc:busy` | The viewer has too many control clients. |
| `lslrc:remote` | The viewer replied `error: ...`. The message is the reply. |
| `lslrc:timeout` | No reply came in the time limit. The connection closes. |
| `lslrc:closed` | The connection closed, for example during `get`. |
| `lslrc:file` | `get` cannot write or move the file. |
| `lslrc:transport` | No TCP transport is available. See [Requirements](#requirements). |
| `lslrc:arg` | An argument is not correct, for example a value with a newline. |
| `lslrc:nolsl` | `lslrc.discover` cannot find liblsl-Matlab. |

To catch one type of error, compare the identifier:

```matlab
try
  rc.select({'no-such-stream'});
catch err
  if strcmp(err.identifier, 'lslrc:remote'), disp(err.message); end
end
```

## Transports

`lslrc` has two transports. They have the same behavior.

- **`java`** uses `java.net.Socket`. MATLAB always has Java, unless you start it with `-nojvm`. Octave has Java if it can load a Java runtime. This transport sees a closed connection immediately.
- **`tcpclient`** uses `tcpclient`. MATLAB has it without a toolbox. Octave gets it from the `instrument-control` package. In MATLAB, this transport cannot see a closed connection. It reports a timeout when the time limit ends.

With `'auto'`, `lslrc` uses Java if it is available. Otherwise, it uses `tcpclient`.

These are the speeds of a `get` of 20 MB on loopback, from a test on one Windows 11 computer. The speeds include the write to disk.

| Platform | `java` | `tcpclient` |
|---|---|---|
| MATLAB R2023a | 78 to 154 MB/s | 110 to 226 MB/s |
| Octave 10.1 | 107 to 134 MB/s | 60 to 136 MB/s |

## Run the tests

The tests use a fake server, `tests/fake_server.py`. It needs Python 3 and the standard library only. If `python` (Windows) or `python3` (other systems) is not the correct command, set `LSLRC_PYTHON`.

In MATLAB:

```text
cd clients/matlab/tests
matlab -batch "run_tests"
```

In Octave:

```text
cd clients/matlab/tests
octave-cli --eval "run_tests"
```

`run_tests` tests each transport that is available. To test one transport, type `run_tests({'java'})`. If a test fails, `run_tests` gives an error. Thus the exit code is not zero.

The integration test starts a real viewer. It runs only if `LSL_VIEWER_EXE` contains the path of the viewer executable. The test starts the viewer with `LSL_RC_PORT=22422` and `LSL_DEMO=1`, records up to two streams, and fetches the file. Then it stops only that viewer process.
