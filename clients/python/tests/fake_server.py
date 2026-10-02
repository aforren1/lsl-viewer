"""A small in-process server that speaks protocol 2, for the offline tests."""

from __future__ import annotations

import socket
import threading
import time
from dataclasses import dataclass, field

BANNER_V2 = "ok: lsl-viewer remote control, protocol 2. type `help`."
BANNER_V1 = "lsl-viewer remote control. type `help`."
BUSY = "error: too many control clients"


@dataclass
class FakeStream:
    key: str
    name: str
    type: str
    channels: int
    rate: int  # 0 = irregular


@dataclass
class FakeViewer:
    """Viewer state, and the knobs the tests turn to make edge cases."""

    banner: str = BANNER_V2
    streams: list[FakeStream] = field(default_factory=list)
    selected: list[str] = field(default_factory=list)
    fields: dict[str, str] = field(default_factory=dict)
    file: str = ""
    recording: bool = False
    recording_name: str | None = None   # None: no completed recording yet
    recording_data: bytes = b""
    truncate_get_at: int | None = None  # send only this many body bytes, then close
    split: bool = False                 # send text replies in small delayed pieces
    received: list[str] = field(default_factory=list)


class FakeServer:
    def __init__(self, viewer: FakeViewer, port: int = 0) -> None:
        self.viewer = viewer
        self._lsock = socket.create_server(("127.0.0.1", port))
        self.port: int = self._lsock.getsockname()[1]
        self._thread = threading.Thread(target=self._accept, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._lsock.close()

    def _accept(self) -> None:
        while True:
            try:
                conn, _ = self._lsock.accept()
            except OSError:
                return
            threading.Thread(target=self._session, args=(conn,), daemon=True).start()

    def _send_text(self, conn: socket.socket, text: str) -> None:
        data = text.encode("utf-8")
        if not self.viewer.split:
            conn.sendall(data)
            return
        for i in range(0, len(data), 3):
            conn.sendall(data[i:i + 3])
            time.sleep(0.002)

    def _session(self, conn: socket.socket) -> None:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with conn, conn.makefile("rb") as rf:
            v = self.viewer
            self._send_text(conn, v.banner + "\n")
            if not v.banner.startswith("ok"):
                return
            for raw in rf:
                line = raw.decode("utf-8").rstrip("\r\n")
                if not line:
                    continue
                v.received.append(line)
                if not self._dispatch(conn, line):
                    return

    def _dispatch(self, conn: socket.socket, line: str) -> bool:
        v = self.viewer
        verb, _, arg = line.partition(" ")
        out: list[str]
        if verb == "status":
            n = len(v.selected) if v.recording else 0
            out = [f"ok: recording={'true' if v.recording else 'false'} seconds=1.5 "
                   f"streams={n} bytes=1234 file={v.file}"]
        elif verb == "streams":
            out = [f"ok: {len(v.streams)} streams"]
            for s in v.streams:
                rate = str(s.rate) if s.rate > 0 else "irregular"
                rec = "  [rec]" if s.key in v.selected else ""
                out.append(f"{s.key} | {s.name} | {s.type} | {s.channels}ch | {rate}{rec}")
        elif verb == "selected":
            out = [f"ok: {len(v.selected)} selected"] + v.selected
        elif verb == "help":
            out = ["ok: 2 lines", "commands: help status ...", "  more help"]
        elif verb == "select":
            known = {s.key for s in v.streams}
            keys = (sorted(known) if arg == "all" else [] if arg == "none"
                    else [k.strip() for k in arg.split(",") if k.strip()])
            bad = [k for k in keys if k not in known]
            if v.recording:
                out = ["error: cannot change the selection while recording"]
            elif bad:
                out = [f"error: unknown stream key(s): {','.join(bad)}"]
            else:
                v.selected = keys
                out = [f"ok: connected {len(keys)} stream(s)"]
        elif verb == "filename":
            v.file = arg
            out = ["ok"]
        elif verb == "set":
            k, _, val = arg.partition(" ")
            v.fields[k] = val
            out = ["ok"]
        elif verb == "start":
            if arg:
                v.file = arg
            if not v.selected:
                out = ["error: no streams connected"]
            else:
                v.recording = True
                out = [f"ok: recording -> {v.file}"]
        elif verb == "stop":
            v.recording = False
            out = [f"ok: stopped -> {v.file}"]
        elif verb == "get":
            return self._get(conn)
        elif verb in ("quit", "exit"):
            self._send_text(conn, "bye\n")
            return False
        else:
            out = ["error: unknown command (try `help`)"]
        self._send_text(conn, "".join(ln + "\n" for ln in out))
        return True

    def _get(self, conn: socket.socket) -> bool:
        v = self.viewer
        if v.recording_name is None:
            self._send_text(conn, "error: no completed recording yet\n")
            return True
        data = v.recording_data
        head = f"ok: {len(data)} {v.recording_name}\n".encode("utf-8")
        if v.truncate_get_at is not None:
            conn.sendall(head + data[:v.truncate_get_at])
            return False
        # One sendall: the header and the start of the body share a segment.
        conn.sendall(head + data)
        return True
