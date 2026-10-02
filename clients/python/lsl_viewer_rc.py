"""Client for the lsl-viewer TCP remote-control protocol (protocol 2).

One stdlib-only module: copy it into your project, or install it with pip.
A Client is not thread-safe. Use each Client from one thread only.
"""

from __future__ import annotations

import os
import re
import socket
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Iterable

__all__ = [
    "Client", "Status", "Stream", "Endpoint", "discover",
    "RcError", "RemoteError", "ProtocolError", "DEFAULT_PORT", "MIN_PROTOCOL",
]

DEFAULT_PORT = 22345
MIN_PROTOCOL = 2
_MAX_LINE = 64 * 1024  # same bound the server applies to command lines
_CHUNK = 1 << 16
_MULTILINE = ("streams", "selected", "help", "get")


class RcError(Exception):
    """Base class for the errors of this module."""


class RemoteError(RcError):
    """The server replied with ``error: ...``. The message is the full reply line."""


class ProtocolError(RcError):
    """The server sent a reply that does not follow the protocol, or closed early."""


@dataclass(frozen=True)
class Status:
    recording: bool
    seconds: float
    streams: int
    bytes: int
    file: str


@dataclass(frozen=True)
class Stream:
    key: str
    name: str
    type: str
    channels: int
    rate: float  # 0.0 means irregular
    recording: bool


class Client:
    """A connection to one viewer. Use it as a context manager."""

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT,
                 timeout: float = 5.0) -> None:
        # The default must exceed the server's 2 s apply wait for select/start/stop.
        self._timeout = timeout
        self._buf = bytearray()
        self._sock: socket.socket | None = socket.create_connection((host, port), timeout=timeout)
        try:
            banner = self._readline()
            if banner.startswith("error:"):
                raise RemoteError(banner)
            m = re.search(r"protocol (\d+)", banner)
            if not banner.startswith("ok") or m is None:
                raise ProtocolError(f"server too old or not an lsl-viewer (banner: {banner!r})")
            self._protocol = int(m.group(1))
            if self._protocol < MIN_PROTOCOL:
                raise ProtocolError(f"server speaks protocol {self._protocol}, need {MIN_PROTOCOL}")
        except BaseException:
            self.close()
            raise

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    @property
    def protocol(self) -> int:
        return self._protocol

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def command(self, line: str) -> str:
        """Send one command and return the first reply line verbatim.

        Only for commands with a one-line reply. ``streams``, ``selected``,
        ``help`` and ``get`` raise ValueError: their unread lines would be
        taken as the replies to later commands. Use their methods instead.
        """
        verb = line.split()[0].lower() if line.strip() else ""
        if verb in _MULTILINE:
            raise ValueError(f"{line.split()[0]!r} has a multi-line reply; use its method")
        return self._send(line)

    def _send(self, line: str) -> str:
        if "\n" in line or "\r" in line or not line.strip():
            raise ValueError(f"not a single non-empty command line: {line!r}")
        try:
            self._live().sendall(line.encode("utf-8") + b"\n")
            return self._readline()
        except (OSError, ProtocolError):
            self.close()  # the reply stream position is unknown now
            raise

    def status(self) -> Status:
        return _parse_status(self._ok("status"))

    def streams(self) -> list[Stream]:
        return [_parse_stream(ln) for ln in self._counted("streams")]

    def selected(self) -> list[str]:
        return self._counted("selected")

    def help(self) -> list[str]:
        return self._counted("help")

    def select(self, keys: str | Iterable[str]) -> str:
        """Select ``"all"``, ``"none"``, one key, or an iterable of keys.

        An empty iterable selects none.
        """
        if isinstance(keys, str):
            if keys in ("all", "none"):
                return self._ok(f"select {keys}")
            keys = [keys]
        keys = list(keys)
        for k in keys:
            # The server splits on "," and trims spaces, so such a key cannot round-trip.
            if "," in k or not k.strip() or k != k.strip():
                raise ValueError(f"key cannot be sent to the server: {k!r}")
        return self._ok("select " + (",".join(keys) if keys else "none"))

    def set(self, field: str | None = None, value: object = None, **fields: object) -> str:
        """Set filename template fields: ``set("subject", "01")`` or ``set(subject="01", run=1)``.

        Returns the reply text of the last field sent.
        """
        pairs = list(fields.items())
        if field is not None:
            pairs.insert(0, (field, value))
        if not pairs:
            raise ValueError("set() needs at least one field")
        reply = ""
        for k, v in pairs:
            if not k or " " in k:
                raise ValueError(f"bad field name: {k!r}")
            reply = self._ok(f"set {k} {'' if v is None else v}")
        return reply

    def filename(self, path: str | os.PathLike[str]) -> str:
        return self._ok(f"filename {os.fspath(path)}")

    def start(self, path: str | os.PathLike[str] | None = None) -> str:
        return self._ok("start" if path is None else f"start {os.fspath(path)}")

    def stop(self) -> str:
        return self._ok("stop")

    def get(self, dest: str | os.PathLike[str] = ".", timeout: float = 120.0) -> Path:
        """Download the last completed recording and return its local path.

        ``dest`` is a directory (the file keeps the server's name) or a file path.
        A ``dest`` that ends with a slash is always a directory.
        ``timeout`` is the longest wait for each chunk, not for the whole file.
        An existing file at the target is replaced.
        """
        # A trailing slash means a directory, even a missing one, so that a typo
        # fails instead of writing a file named like the directory.
        into_dir = os.fspath(dest).endswith(("/", os.sep)) or Path(dest).is_dir()
        dest = Path(dest)
        folder = dest if into_dir else dest.parent
        # Make the temp file before the request, so a bad dest fails while the
        # connection is still in a known state.
        fd, tmp = tempfile.mkstemp(dir=str(folder), prefix=".lsl_viewer_rc.", suffix=".part")
        try:
            with os.fdopen(fd, "wb") as f:
                name = self._fetch(f, timeout)
            target = folder / name if into_dir else dest
            os.replace(tmp, target)
            return target
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise

    # -- internals --

    def _live(self) -> socket.socket:
        if self._sock is None:
            raise ConnectionError("client is closed")
        return self._sock

    def _readline(self) -> str:
        sock = self._live()
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                break
            if len(self._buf) > _MAX_LINE:
                raise ProtocolError("reply line too long")
            data = sock.recv(_CHUNK)
            if not data:
                raise ProtocolError("connection closed by the server")
            self._buf += data
        if i > _MAX_LINE:
            raise ProtocolError("reply line too long")
        line = bytes(self._buf[:i])
        del self._buf[:i + 1]
        return line.rstrip(b"\r").decode("utf-8", errors="replace")

    def _ok(self, line: str) -> str:
        """Send a command and return its reply text minus ``ok: ``."""
        reply = self._send(line)
        if reply == "ok":
            return ""
        if reply.startswith("ok:"):
            return reply[3:].lstrip(" ")
        if reply.startswith("error:"):
            raise RemoteError(reply)
        self.close()
        raise ProtocolError(f"unexpected reply to {line.split()[0]!r}: {reply!r}")

    def _counted(self, line: str) -> list[str]:
        head = self._ok(line)
        m = re.match(r"(\d+)(?: |$)", head)
        if m is None:
            self.close()
            raise ProtocolError(f"expected '<N> <noun>' header, got {head!r}")
        try:
            return [self._readline() for _ in range(int(m.group(1)))]
        except (OSError, ProtocolError):
            self.close()
            raise

    def _fetch(self, f: BinaryIO, timeout: float) -> str:
        sock = self._live()
        sock.settimeout(timeout)
        try:
            m = re.fullmatch(r"(\d+) (.*)", self._ok("get"))
            if m is None:
                raise ProtocolError("malformed get header")
            self._copy_body(f, int(m.group(1)))
            return _base_name(m.group(2))
        except RemoteError:
            raise  # no body follows an error, so the connection is still usable
        except BaseException:
            self.close()
            raise
        finally:
            if self._sock is not None:
                self._sock.settimeout(self._timeout)

    def _copy_body(self, f: BinaryIO, size: int) -> None:
        # The header recv can already hold the start of the body.
        head = self._buf[:size]
        f.write(head)
        del self._buf[:len(head)]
        remaining = size - len(head)
        chunk = memoryview(bytearray(_CHUNK))  # reused, so the loop does not allocate
        sock = self._live()
        while remaining:
            n = sock.recv_into(chunk, min(_CHUNK, remaining))
            if n == 0:
                raise ProtocolError(f"connection closed after {size - remaining} of {size} bytes")
            f.write(chunk[:n])
            remaining -= n


def _base_name(name: str) -> str:
    # Never let the server pick a directory on our side.
    base = re.split(r"[\\/]", name)[-1].strip()
    return base if base not in ("", ".", "..") else "recording.xdf"


def _parse_status(text: str) -> Status:
    if text.startswith("file="):
        head, file = "", text[5:]
    else:
        i = text.find(" file=")
        if i < 0:
            raise ProtocolError(f"status has no file= field: {text!r}")
        head, file = text[:i], text[i + 6:]
    kv = dict(tok.split("=", 1) for tok in head.split() if "=" in tok)
    try:
        if kv["recording"] not in ("true", "false"):
            raise ValueError(kv["recording"])
        return Status(recording=kv["recording"] == "true", seconds=float(kv["seconds"]),
                      streams=int(kv["streams"]), bytes=int(kv["bytes"]), file=file)
    except (KeyError, ValueError) as e:
        raise ProtocolError(f"malformed status: {text!r}") from e


def _parse_stream(line: str) -> Stream:
    f = [p.strip() for p in line.split(" | ")]
    if len(f) < 5:
        raise ProtocolError(f"malformed stream line: {line!r}")
    rate, rec = f[-1], False
    if rate.endswith("[rec]"):
        rate, rec = rate[:-5].strip(), True
    try:
        if not f[-2].endswith("ch"):
            raise ValueError(f[-2])
        channels = int(f[-2][:-2])
        hz = 0.0 if rate == "irregular" else float(rate)
    except ValueError as e:
        raise ProtocolError(f"malformed stream line: {line!r}") from e
    # A name that contains " | " ends up split; join it back rather than fail.
    return Stream(key=f[0], name=" | ".join(f[1:-3]), type=f[-3], channels=channels,
                  rate=hz, recording=rec)


@dataclass(frozen=True)
class Endpoint:
    host: str
    port: int
    pid: int
    bind: str
    hostname: str
    source_id: str
    protocol_version: int

    def connect(self, **kw: Any) -> Client:
        return Client(self.host, self.port, **kw)


def discover(timeout: float = 2.0) -> list[Endpoint]:
    """Find the viewers on the network through their LSL beacon. Needs pylsl."""
    try:
        import pylsl
    except ImportError as e:
        raise ImportError("discover() needs pylsl: pip install pylsl "
                          "(or install lsl-viewer-rc[discover])") from e
    out = []
    for info in pylsl.resolve_byprop("type", "ViewerControl", timeout=timeout):
        sid = info.source_id()
        # The resolve result has no desc(); only the full info from an inlet has it.
        try:
            inlet = pylsl.StreamInlet(info)
            desc = inlet.info(timeout=timeout).desc()
            meta = {k: desc.child_value(k) for k in ("port", "pid", "bind", "protocol_version")}
            inlet.close_stream()
        except Exception:
            meta = None
        # source_id is "lsl-viewer-rc:<host>:<pid>:<port>"; fall back to it without a desc.
        parts = sid.rsplit(":", 2)
        port = _int((meta or {}).get("port")) or _int(parts[-1])
        if not port:
            continue  # not a viewer beacon we can reach
        bind = (meta or {}).get("bind", "")
        hostname = info.hostname()
        if meta is None:
            version = 0  # unknown
        else:
            version = _int(meta["protocol_version"]) or 1  # protocol 1 beacons lack the field
        out.append(Endpoint(
            host="127.0.0.1" if bind == "loopback" else hostname,
            port=port,
            pid=_int((meta or {}).get("pid")) or (_int(parts[-2]) if len(parts) == 3 else 0),
            bind=bind,
            hostname=hostname,
            source_id=sid,
            protocol_version=version,
        ))
    return out


def _int(text: str | None) -> int:
    return int(text) if text and text.isdigit() else 0
