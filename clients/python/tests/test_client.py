from __future__ import annotations

import builtins
import os
from pathlib import Path

import pytest

import lsl_viewer_rc as rc
from fake_server import BANNER_V1, BUSY, FakeServer, FakeViewer


def connect(server: FakeServer) -> rc.Client:
    return rc.Client(port=server.port, timeout=5.0)


def test_banner_and_protocol(server: FakeServer) -> None:
    with connect(server) as c:
        assert c.protocol == 2


def test_v1_banner_is_rejected(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.banner = BANNER_V1
    with pytest.raises(rc.ProtocolError, match="too old"):
        connect(server)


def test_unknown_banner_is_rejected(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.banner = "SSH-2.0-OpenSSH_9.6"
    with pytest.raises(rc.ProtocolError):
        connect(server)


def test_busy_server(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.banner = BUSY
    with pytest.raises(rc.RemoteError, match="too many control clients"):
        connect(server)


def test_errors_share_a_base() -> None:
    assert issubclass(rc.RemoteError, rc.RcError)
    assert issubclass(rc.ProtocolError, rc.RcError)


def test_streams(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.selected = ["host1|MockEEG"]
    with connect(server) as c:
        s = c.streams()
    assert [x.key for x in s] == ["host1|MockEEG", "host1|MockMarkers", "uid-1234"]
    assert s[0] == rc.Stream("host1|MockEEG", "MockEEG", "EEG", 32, 500.0, True)
    assert s[1].rate == 0.0 and not s[1].recording
    assert s[2].name == "Audio in"


def test_zero_streams_keeps_the_connection_in_sync(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.streams = []
    with connect(server) as c:
        assert c.streams() == []
        assert c.selected() == []
        assert c.status().recording is False


def test_status_file_with_spaces(server: FakeServer) -> None:
    with connect(server) as c:
        st = c.status()
    assert st == rc.Status(False, 1.5, 0, 1234, "C:/data/my study/sub 01.xdf")


def test_status_empty_file(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.file = ""
    with connect(server) as c:
        assert c.status().file == ""


def test_select_set_start_stop(server: FakeServer, viewer: FakeViewer) -> None:
    with connect(server) as c:
        assert c.select(["host1|MockEEG", "uid-1234"]) == "connected 2 stream(s)"
        assert c.selected() == ["host1|MockEEG", "uid-1234"]
        assert c.set("subject", "01") == ""
        c.set(session="02", run=3)
        assert c.filename(Path("out dir") / "x.xdf") == ""
        assert c.start() == f"recording -> {viewer.file}"
        assert c.status().recording is True
        assert c.stop().startswith("stopped -> ")
        assert c.select("all") == "connected 3 stream(s)"
        assert c.select("none") == "connected 0 stream(s)"
        assert c.select([]) == "connected 0 stream(s)"
        assert c.select("uid-1234") == "connected 1 stream(s)"
    assert viewer.fields == {"subject": "01", "session": "02", "run": "3"}
    assert "select host1|MockEEG,uid-1234" in viewer.received


def test_start_with_path(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.selected = ["uid-1234"]
    with connect(server) as c:
        assert c.start("D:/a b/c.xdf") == "recording -> D:/a b/c.xdf"
    assert viewer.received[-1] == "start D:/a b/c.xdf"


def test_error_reply_raises_and_connection_survives(server: FakeServer) -> None:
    with connect(server) as c:
        with pytest.raises(rc.RemoteError) as ei:
            c.start()
        assert str(ei.value) == "error: no streams connected"
        with pytest.raises(rc.RemoteError, match="unknown stream"):
            c.select(["nope"])
        assert c.status().recording is False


def test_select_rejects_bad_keys_client_side(server: FakeServer, viewer: FakeViewer) -> None:
    with connect(server) as c:
        for bad in (["a,b"], [""], [" padded"], ["x\ny"]):
            with pytest.raises(ValueError):
                c.select(bad)
        assert c.status().recording is False
    assert not any(r.startswith("select") for r in viewer.received)


def test_command_is_raw_and_rejects_newlines(server: FakeServer) -> None:
    with connect(server) as c:
        with pytest.raises(ValueError):
            c.command("status\nstop")
        with pytest.raises(ValueError):
            c.command("   ")
        assert c.command("status").startswith("ok: recording=")
        assert c.command("bogus").startswith("error: unknown command")
        for verb in ("streams", "HELP", "get", "selected"):
            with pytest.raises(ValueError):
                c.command(verb)
        assert c.status().recording is False   # nothing was sent, so still in sync


def test_help(server: FakeServer) -> None:
    with connect(server) as c:
        assert len(c.help()) == 2
        assert c.status().bytes == 1234


def test_lines_split_across_segments(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.split = True
    with connect(server) as c:
        assert len(c.streams()) == 3
        assert c.status().file == "C:/data/my study/sub 01.xdf"


def test_overlong_line_is_rejected(server: FakeServer, viewer: FakeViewer) -> None:
    viewer.banner = "ok: protocol 2 " + "x" * (rc._MAX_LINE + 10)
    with pytest.raises(rc.ProtocolError, match="too long"):
        connect(server)


def test_get_header_and_body_in_one_segment(
        server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "sub 01.xdf", b"XDF:" + bytes(range(256)) * 4
    with connect(server) as c:
        p = c.get(tmp_path)
        assert p == tmp_path / "sub 01.xdf"
        assert p.read_bytes() == viewer.recording_data
        assert c.status().bytes == 1234  # the body did not eat into the next reply


def test_get_large_file_to_file_path(server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "r.xdf", os.urandom(5 * 1024 * 1024 + 17)
    dest = tmp_path / "copy.xdf"
    with connect(server) as c:
        assert c.get(dest) == dest
        assert c.status().recording is False
    assert dest.read_bytes() == viewer.recording_data
    assert os.listdir(tmp_path) == ["copy.xdf"]


def test_get_empty_file(server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "e.xdf", b""
    with connect(server) as c:
        assert c.get(tmp_path).read_bytes() == b""


def test_get_trailing_slash_means_directory(
        server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "r.xdf", b"XDF:"
    with connect(server) as c:
        with pytest.raises(OSError):
            c.get(str(tmp_path / "missing") + "/")
        assert c.get(str(tmp_path) + "/") == tmp_path / "r.xdf"
    assert os.listdir(tmp_path) == ["r.xdf"]


def test_get_never_writes_outside_dest(server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "../../evil.xdf", b"XDF:"
    with connect(server) as c:
        assert c.get(tmp_path) == tmp_path / "evil.xdf"


def test_get_error_leaves_no_file(server: FakeServer, tmp_path: Path) -> None:
    with connect(server) as c:
        with pytest.raises(rc.RemoteError, match="no completed recording"):
            c.get(tmp_path)
        assert c.status().recording is False
    assert os.listdir(tmp_path) == []


def test_get_truncated_body(server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "t.xdf", os.urandom(2 * 1024 * 1024)
    viewer.truncate_get_at = 300_000
    dest = tmp_path / "t.xdf"
    with connect(server) as c:
        with pytest.raises(rc.ProtocolError, match="closed after"):
            c.get(dest)
        with pytest.raises(ConnectionError):
            c.status()  # the client closed itself: the stream is out of sync
    assert os.listdir(tmp_path) == []


def test_get_into_missing_dir_fails_before_request(
        server: FakeServer, viewer: FakeViewer, tmp_path: Path) -> None:
    viewer.recording_name, viewer.recording_data = "r.xdf", b"XDF:"
    with connect(server) as c:
        with pytest.raises(OSError):
            c.get(tmp_path / "missing" / "r.xdf")
        assert c.status().recording is False
    assert "get" not in viewer.received


def test_discover_without_pylsl(monkeypatch: pytest.MonkeyPatch) -> None:
    real_import = builtins.__import__

    def fake_import(name: str, *args: object, **kwargs: object) -> object:
        if name == "pylsl":
            raise ImportError("no pylsl")
        return real_import(name, *args, **kwargs)  # type: ignore[arg-type]

    monkeypatch.setattr(builtins, "__import__", fake_import)
    with pytest.raises(ImportError, match="needs pylsl"):
        rc.discover()
