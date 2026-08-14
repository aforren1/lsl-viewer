# Verifying the remote-control server on Windows

The recording remote-control server ([`src/remote_control.hpp`](../src/remote_control.hpp))
is a small TCP server. It was rewritten to be cross-platform: a thin socket layer
(`rc_socket_t` / `rc_close` / `RcWsaInit`) sits over **Winsock** on Windows and BSD
sockets elsewhere, so the accept/recv/dispatch logic is shared. The POSIX path is
verified end-to-end on Linux, and the `remote/roundtrip` test below passes on Windows,
which covers bind, accept, two concurrent clients, and teardown. This doc is the
checklist for the parts that only the live server shows.

What's Windows-specific (i.e. what this is really testing):
- `WSAStartup` / `WSACleanup` (done once via the function-local `RcWsaInit` static),
- `SOCKET` / `INVALID_SOCKET` / `closesocket` / `SD_BOTH` instead of the BSD spellings,
- linking `ws2_32` (wired up in `CMakeLists.txt` under `if(WIN32)`),
- the Windows Firewall prompt the first time the process binds (it listens on
  `INADDR_ANY`, port **22345** by default).

Build is the usual native build (no special flags): `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Release && cmake --build build`. See the README's Windows section.

---

## 1. Automated check (fastest — actually compiles & runs the Winsock code)

There's a self-contained test, `remote/roundtrip`, that starts the real server,
connects a loopback client built on the **same** `rc_socket_t` layer, and asserts the
replies + the resulting `RemoteState`. Building with tests on runs the genuine
Winsock path:

```powershell
cmake -S . -B build-test -DLSL_VIEWER_TESTS=ON
cmake --build build-test --config Release
.\build-test\Release\lsl_viewer.exe --tests remote
```

(The default Windows generator is Visual Studio, a multi-config generator: it ignores
`-DCMAKE_BUILD_TYPE` and picks the config at build time via `--config`, so the exe lands
in a per-config subfolder.)

Pass = `[tests] 1/1 passed`. (Drop the `remote` filter to run the whole suite.)
This needs no streams, no GUI interaction, and no external client — if it passes,
`WSAStartup`, bind/listen/accept, send/recv, and teardown all work on Windows.

> Note: the test binds port **22456** (the live server uses 22345), so the two don't
> collide. A Firewall prompt may still appear the first time — allow it (or it binds
> loopback-only, which is enough for the test).

---

## 2. Interactive check (the real server + a manual client)

Confirms the live server, discovery, and the connect/disconnect (`select`) behavior.

**Start the viewer with the server on** (auto-starts from the env var; default port
22345):

```powershell
$env:LSL_RC_PORT = "22345"
.\build\lsl_viewer.exe
```

(Equivalently: launch normally and tick **Remote control** in the Streams rail.)

The server binds **loopback (127.0.0.1) only by default** — there is no authentication, so
it is not exposed on the network unless you ask for it. To let a client elsewhere on the LAN
reach it, tick **Allow LAN access** in the Recording panel (it re-binds the running server),
or set `LSL_RC_BIND=all` before launching:

```powershell
$env:LSL_RC_PORT = "22345"; $env:LSL_RC_BIND = "all"
.\build\lsl_viewer.exe
```

Either way the first exposure triggers a **Windows Firewall** dialog — allow access on private
networks. For the loopback client below you do not need it.

Have at least one stream on the network (from a machine with Python + the venv):

```powershell
uv run tools\lsl_test_streams.py --streams eeg,sine
```

**Connect a client.** Any line-oriented TCP client works. Easiest options on Windows:

- **ncat** (ships with Nmap): `ncat 127.0.0.1 22345`
- **telnet** (enable "Telnet Client" Windows feature): `telnet 127.0.0.1 22345`
- **PowerShell only** (no extra tools) — paste this:

```powershell
$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 22345)
$ns = $c.GetStream()
function rc($cmd) {
  if ($cmd) { $b=[Text.Encoding]::ASCII.GetBytes("$cmd`n"); $ns.Write($b,0,$b.Length) }
  Start-Sleep -Milliseconds 400
  $out=''; while ($ns.DataAvailable) { $out += [char]$ns.ReadByte() }
  $out
}
rc $null                 # banner
rc 'help'
rc 'streams'             # one line per discovered stream; '[rec]' = connected
rc 'select mock-eeg'     # connect just EEG (use a key from `streams`)
rc 'streams'             # MockEEG should now show  [rec]
rc 'selected'            # -> the connected keys
rc 'status'              # recording=false ... (until you `start`)
rc 'select all'          # connect everything
rc 'select none'         # disconnect everything
rc 'start'               # begin recording the connected set
rc 'status'              # recording=true ... bytes climbing
rc 'stop'
rc 'quit'
$c.Close()
```

### What to expect / pass criteria

- [ ] Connecting prints the banner `lsl-viewer remote control. type `help`.`
- [ ] `streams` lists each discovered stream as `key | name | type | Nch | rate`
- [ ] `select <key>` replies `ok: connected 1 stream(s)` and the stream gains `[rec]` in
      `streams` **and** appears as a plot in the viewer window (connection IS the
      record selector now — recording captures all connected streams)
- [ ] `select all` / `select none` connect / disconnect everything
- [ ] a bogus key (`select nope`) is refused whole: `error: unknown stream(s): nope`,
      and `selected` is unchanged
- [ ] `start` replies `ok: recording -> <path>` (not an optimistic ok) and `start`
      with nothing connected replies `error: no streams connected`
- [ ] a **second** client can connect and run `status` while the first stays open
- [ ] `start` then `stop` produces a `.xdf` (default is the BIDS template
      `sub-{subject}/ses-{session}/{modality}/..._{modality}.xdf` under
      `~/Documents/lsl-recordings`; `status` shows the resolved path); `status` shows
      `recording=true` with growing `bytes` while active
- [ ] the written file opens in pyxdf (`python -c "import pyxdf; print(len(pyxdf.load_xdf('...')[0]))"`)
- [ ] **Stop is instant** — the "Stop recording" button (or `stop`) returns without a
      visible UI freeze (the worker joins happen on a background closer thread)
- [ ] closing the viewer flushes the file (no truncation — pyxdf still reads it)

### Discovery (optional)

While listening, the viewer also publishes an LSL outlet named `LSLViewerControl`
(type `ViewerControl`); the TCP port is the last field of its `source_id`,
`lsl-viewer-rc:<host>:<pid>:<port>`. A client resolves type `ViewerControl` to find the
endpoint without knowing it in advance. Verified on this machine with `pylsl`:

```
resolved 1
  name=LSLViewerControl host=MWPF4RADY3 source_id=lsl-viewer-rc:MWPF4RADY3:16956:22345
  desc: port=22345 pid=16956 bind=loopback
  connect 127.0.0.1:22345 -> 'lsl-viewer remote control. type `help`.'
```

Note that `info.hostname()` is the machine name even when the server is bound to
loopback — connect to `127.0.0.1` unless the beacon's `bind` field says `all`. Reading
`bind`/`port`/`pid` needs an inlet on the beacon (`StreamInlet(info).info().desc()`);
`source_id` alone is enough for the port.

### Second instance (optional)

Start a second viewer **without** `LSL_RC_PORT` and tick **Remote control**: 22345 is
taken, so it binds an ephemeral port and announces that one (the label next to the
checkbox shows it, and two beacons now resolve). With `LSL_RC_PORT` set, the second
viewer instead logs `remote control unavailable: bind() failed (port in use?)` — a
pinned port is never silently exchanged for another. The `remote/second_instance` test
covers both paths, including the Windows-specific bind semantics.

---

## If something fails

- **Build/link error on `ws2_32`** — confirm the `if(WIN32)` block in `CMakeLists.txt`
  links `ws2_32` (and `onecore`); reconfigure from scratch if the cache is stale.
- **`socket() failed` / nothing listens** — `WSAStartup` didn't run; check that
  `RcWsaInit` (the function-local static in `start()`) isn't being optimized away.
- **Client can't connect from another machine** — Windows Firewall is blocking the
  bind; allow `lsl_viewer.exe` on private networks. Loopback (`127.0.0.1`) is unaffected.
- **`bind() failed (port in use?)`** — another process holds 22345; set
  `LSL_RC_PORT` to a free port.

Report back which boxes pass; the only thing that can't be exercised from Linux is the
literal Winsock syscalls, so a green run here closes the loop.
