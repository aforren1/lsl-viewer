# Remote control protocol

Read this page if you write a remote control client for a language that has no client library. The [Python client](../../clients/python/README.md) and the [MATLAB and Octave client](../../clients/matlab/README.md) follow these rules. For how to use the remote control, see [Control the recording from a script](../remote-control.md).

The protocol is text lines on TCP. Each command ends with a newline, and the replies are lines of readable text. The default port is 22345.

## Connection

The viewer uses protocol 2. When a client connects, the viewer sends one line:

```text
ok: lsl-viewer remote control, protocol 2. type `help`.
```

Read the version from the text `protocol <N>`. If the line does not have this text, the viewer is too old for a protocol 2 client.

Up to four clients can connect at the same time. If four clients are already connected, the viewer sends `error: too many control clients`, and then closes the connection.

The port binds to loopback (127.0.0.1) only, unless the user permits LAN access. There is no authentication.

## Commands

| Command | Reply |
|---|---|
| `streams` | Lists the streams that LSL can resolve. The header is `ok: <N> streams`. Then come N lines, one for each stream: `key \| name \| type \| <C>ch \| <rate>`. The rate is an integer or `irregular`. A connected stream has `  [rec]` at the end of its line. The first field is the key. |
| `selected` | Lists the keys that are connected. These streams are the ones that get recorded. The header is `ok: <N> selected`. Then come N lines, one key on each line. |
| `select all\|none\|<k1,k2,...>` | Selects the streams to connect and record. Each `key` is an identifier from `streams`. If a key is not a stream that the viewer can see, the viewer refuses the whole command. The command is also refused during a recording, because the set is locked until `stop`. |
| `set <subject\|session\|task\|run\|acq\|modality> <value>` | Fills one field of the filename template. The reply is `ok`. |
| `filename <path>` | Sets the output path or template directly. The reply is `ok`. |
| `start [path]` and `stop` | Start and stop the recording. |
| `get` | Sends the last completed recording to the client. The header is `ok: <bytes> <name>`. Then come exactly `<bytes>` of raw XDF. |
| `status` | Shows the recording state on one line: `ok: recording=<true\|false> seconds=<s> streams=<n> bytes=<n> file=<path>`. |
| `help` | Lists the commands. The header is `ok: <N> lines`. Then come N lines of text. |
| `quit` | Closes the connection. The reply is `bye`. |

## Reply rules

- The first line of a reply is `ok`, `ok: <text>`, or `error: <text>`. The only exception is `quit`.
- The `streams`, `selected`, and `help` replies have a header line `ok: <N> <noun>`, then exactly N lines. There is no end marker. If there are no streams, the reply is `ok: 0 streams` and nothing more.
- In the `status` reply, `file=` is always the last field. The path continues to the end of the line, and it can contain spaces. Split the text before `file=` on spaces, then take all the text after `file=` as the path.
- In the `get` header, the name continues to the end of the line, and it can contain spaces. If `get` fails, the reply is one `error:` line and no data.
- The `select`, `filename`, `set`, `start`, and `stop` replies are the same as in protocol 1, for compatibility with LabRecorder remote control clients.

## Timing

The `select`, `start`, and `stop` commands must go through the viewer. They do not reply until the viewer has done the work, thus the reply gives the result: `ok: recording -> /path/file.xdf`, or `error: no streams connected`. A `status` that comes after such a reply always shows the new state.

If the viewer does not answer in 2 seconds, the reply is `error: the viewer did not respond`, and the command is discarded. Thus set the read timeout of your client to more than 2 seconds. For `get`, use a longer timeout, because a large file takes more time.

## Discovery

The viewer also announces the control endpoint through LSL, with the type `ViewerControl` and the name `LSLViewerControl`. Resolve it to get the host and the port instead of writing `22345` in your code.

The `source_id` has the form `lsl-viewer-rc:<host>:<pid>:<port>`, thus the port is the text after the last colon. The host and the process ID make the identifier different for each viewer, because LSL uses `source_id` as the identity of a stream.

```python
from pylsl import resolve_byprop, StreamInlet

for info in resolve_byprop("type", "ViewerControl", timeout=5.0):
    port = int(info.source_id().rsplit(":", 1)[1])
    desc = StreamInlet(info).info(timeout=3).desc()      # port, pid, bind, protocol, protocol_version
    if desc.child_value("protocol_version") != "2":
        continue                                         # A viewer that this client cannot talk to.
    host = "127.0.0.1" if desc.child_value("bind") == "loopback" else info.hostname()
```

The description also has `protocol_version`. Use it to skip a viewer with a different protocol before you connect to it.

Two things to know when you write a client:

- **The port can move.** If port 22345 is in use, and the user did not set `LSL_RC_PORT`, the viewer takes a port from the operating system and announces that one. Thus a second viewer on the same machine also has a control port. If the user does set `LSL_RC_PORT`, the viewer uses that port or none, because a script that asks for a port must not get a different one.
- **The host from `resolve` is not always the address to connect to.** A viewer that binds to loopback only is reachable at `127.0.0.1`, and only from the same machine, but LSL still reports the machine name. The `bind` field in the description says which: `loopback` or `all`.

A recorder must not record the control endpoint, because it never sends a sample. `xdf_record` and the viewer skip a stream that has the type `ViewerControl` or a `source_id` that starts with `lsl-viewer-rc`.

## Examine the endpoint by hand

Use `nc localhost 22345`, or `ncat 127.0.0.1 22345` on Windows. For a PowerShell client with no extra tools, see [windows-remote-control-testing.md](windows-remote-control-testing.md).
