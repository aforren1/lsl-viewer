#!/usr/bin/env python3
"""Fake lsl-viewer remote-control server (protocol 2) for the MATLAB/Octave client tests.

Commands that start with "__" exist only here. They let a test script the next reply
without a second process or a second port.
"""
import argparse
import os
import socket
import threading
import time

BANNERS = {
    "v2": b"ok: lsl-viewer remote control, protocol 2. type `help`.\n",
    "v3": b"ok: lsl-viewer remote control, protocol 3. type `help`.\n",
    "v1": b"ok: lsl-viewer remote control. type `help`.\n",
    "busy": b"error: too many control clients\n",
}

# Keys contain "|" but never " | ", and one name contains " | ", as the spec allows.
CATALOG = [
    ("host-a|MockEEG", "MockEEG", "EEG", 32, "500"),
    ("host-a|MockMarkers", "Mock Markers", "Markers", 1, "irregular"),
    ("{0b4f-uid}", "Audio | Left", "Audio", 2, "48000"),
    ("host-b|Gaze", "Gaze", "Gaze", 3, "120"),
]
FIELDS = ("subject", "session", "task", "run", "acq", "modality")


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.next_banner = "v2"
        self.nstreams = 3
        self.selected = []
        self.recording = False
        self.file = "C:/data dir/sub 01/rec file.xdf"
        self.get_size = 1000
        self.get_name = "rec file.xdf"
        self.truncate = False
        self.clients = 0
        self.stop = threading.Event()


def body(n):
    # A period of 251 (prime) makes most shifts or lost chunks visible.
    block = bytes(range(251))
    return (block * (n // 251 + 1))[:n]


def handle(st, line):
    """Return (reply bytes, close_after, truncate_at or None)."""
    verb, _, arg = line.partition(" ")
    arg = arg.strip()
    with st.lock:
        streams = CATALOG[: st.nstreams]
        keys = [s[0] for s in streams]
        if verb == "help":
            return b"ok: 2 lines\ncommands: help status streams\nselect/start/stop wait.\n", False, None
        if verb == "status":
            txt = "ok: recording=%s seconds=%.1f streams=%d bytes=%d file=%s\n" % (
                "true" if st.recording else "false", 12.5 if st.recording else 0.0,
                len(st.selected), 123456789012, st.file)
            return txt.encode(), False, None
        if verb == "streams":
            out = ["ok: %d streams" % len(streams)]
            for k, name, typ, ch, rate in streams:
                rec = "  [rec]" if k in st.selected else ""
                out.append("%s | %s | %s | %dch | %s%s" % (k, name, typ, ch, rate, rec))
            return ("\n".join(out) + "\n").encode(), False, None
        if verb == "selected":
            return ("\n".join(["ok: %d selected" % len(st.selected)] + st.selected) + "\n").encode(), False, None
        if verb == "select":
            if st.recording:
                return b"error: recording -- stop the recording before changing streams\n", False, None
            if arg == "all":
                want = list(keys)
            elif arg == "none":
                want = []
            else:
                want = [k for k in arg.split(",") if k]
                bad = [k for k in want if k not in keys]
                if bad:
                    return ("error: unknown stream(s): %s (see `streams`)\n" % " ".join(bad)).encode(), False, None
            st.selected = want
            return ("ok: connected %d stream(s)\n" % len(want)).encode(), False, None
        if verb == "set":
            field, _, value = arg.partition(" ")
            if field not in FIELDS:
                return ("error: unknown field `%s`\n" % field).encode(), False, None
            return ("ok: %s = %s\n" % (field, value)).encode(), False, None
        if verb == "filename":
            st.file = arg
            return ("ok: filename -> %s\n" % arg).encode(), False, None
        if verb == "start":
            if not st.selected:
                return b"error: no streams connected\n", False, None
            if arg:
                st.file = arg
            st.recording = True
            return ("ok: recording -> %s\n" % st.file).encode(), False, None
        if verb == "stop":
            st.recording = False
            return b"ok: stopped\n", False, None
        if verb == "get":
            if st.recording:
                return b"error: stop the recording before `get`\n", False, None
            # Header and data go out in one send, thus they share a segment.
            data = ("ok: %d %s\n" % (st.get_size, st.get_name)).encode() + body(st.get_size)
            cut = None
            if st.truncate:
                st.truncate = False
                cut = len(data) - st.get_size // 2 - 1
            return data, False, cut
        if verb in ("quit", "exit"):
            return b"bye\n", True, None
        # Test-only controls.
        if verb == "__next":
            st.next_banner = arg
            return b"ok\n", False, None
        if verb == "__streams":
            st.nstreams = int(arg)
            st.selected = [k for k in st.selected if k in [s[0] for s in CATALOG[: st.nstreams]]]
            return b"ok\n", False, None
        if verb == "__file":
            st.file = arg
            return b"ok\n", False, None
        if verb == "__getsize":
            st.get_size = int(arg)
            return b"ok\n", False, None
        if verb == "__getname":
            st.get_name = arg
            return b"ok\n", False, None
        if verb == "__truncate":
            st.truncate = True
            return b"ok\n", False, None
        if verb == "__clients":
            return ("ok: %d\n" % st.clients).encode(), False, None
        if verb == "__shutdown":
            st.stop.set()
            return b"bye\n", True, None
        return ("error: unknown command `%s` (try `help`)\n" % verb).encode(), False, None


def session(st, conn):
    with st.lock:
        st.clients += 1
    try:
        serve(st, conn)
    finally:
        with st.lock:
            st.clients -= 1


def serve(st, conn):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    with conn:
        with st.lock:
            mode, st.next_banner = st.next_banner, "v2"
        conn.sendall(BANNERS.get(mode, BANNERS["v2"]))
        if mode == "busy":
            return
        buf = b""
        while True:
            try:
                chunk = conn.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                raw, _, buf = buf.partition(b"\n")
                line = raw.decode("utf-8", "replace").rstrip("\r").strip()
                if not line:
                    continue
                data, close, cut = handle(st, line)
                try:
                    conn.sendall(data if cut is None else data[:cut])
                except OSError:
                    return
                if close or cut is not None:
                    return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--portfile", required=True)
    ap.add_argument("--lifetime", type=float, default=600.0,
                    help="exit after this many seconds, so a crashed test leaves no server")
    a = ap.parse_args()
    st = State()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(8)
    srv.settimeout(0.2)
    # Rename, so the reader never sees a half-written file.
    tmp = a.portfile + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(srv.getsockname()[1]))
    os.replace(tmp, a.portfile)
    deadline = time.monotonic() + a.lifetime
    while not st.stop.is_set() and time.monotonic() < deadline:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        conn.settimeout(None)
        threading.Thread(target=session, args=(st, conn), daemon=True).start()
    srv.close()


if __name__ == "__main__":
    main()
