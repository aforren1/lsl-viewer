"""Tests against a real viewer. They run only when LSL_VIEWER_EXE is set."""

from __future__ import annotations

import importlib.util
import os
import socket
import subprocess
import time
from pathlib import Path
from typing import Iterator

import pytest

import lsl_viewer_rc as rc

EXE = os.environ.get("LSL_VIEWER_EXE")
PORT = 22421

pytestmark = pytest.mark.skipif(not EXE, reason="set LSL_VIEWER_EXE to run against a real viewer")


def _port_open(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


@pytest.fixture(scope="module")
def viewer_proc(tmp_path_factory: pytest.TempPathFactory) -> Iterator[subprocess.Popen[bytes]]:
    # LSL_RC_PORT never falls back to another port, so an open port is a different
    # viewer, and the tests would drive it instead of ours.
    if _port_open(PORT):
        pytest.fail(f"port {PORT} is already in use")
    log = tmp_path_factory.mktemp("viewer") / "viewer.log"
    # Portable mode keeps imgui.ini beside the exe, so the temp recording path the test
    # sets does not become the default of the user's own viewer.
    env = dict(os.environ, LSL_RC_PORT=str(PORT), LSL_DEMO="1", LSL_PORTABLE="1")
    with open(log, "wb") as out:
        proc = subprocess.Popen([str(EXE)], env=env, stdout=out, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 30
        while not _port_open(PORT):
            if proc.poll() is not None:
                pytest.fail(f"viewer exited with {proc.returncode}; see {log}")
            if time.monotonic() > deadline:
                pytest.fail(f"port {PORT} did not open; see {log}")
            time.sleep(0.2)
        yield proc
    finally:
        proc.kill()
        proc.wait(timeout=10)


def _wait_for_streams(c: rc.Client) -> list[rc.Stream]:
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        found = c.streams()
        if found:
            return found
        time.sleep(0.5)
    pytest.fail("the viewer found no demo streams")


def test_record_and_fetch(viewer_proc: subprocess.Popen[bytes], tmp_path: Path) -> None:
    with rc.Client(port=PORT) as c:
        assert c.protocol >= 2
        found = _wait_for_streams(c)
        # Other viewers with LSL_DEMO=1 add their streams too, so pick from the list.
        key = found[0].key
        c.select([key])
        assert key in c.selected()
        c.set(subject="01", task="rctest")
        c.set("run", 1)
        reply = c.start(tmp_path / "rec.xdf")
        assert reply.startswith("recording")
        time.sleep(2.0)
        assert c.status().recording is True
        c.stop()
        st = c.status()
        assert st.recording is False
        fetched = c.get(tmp_path / "fetched.xdf")
        assert fetched.read_bytes()[:4] == b"XDF:"
        c.select("none")


@pytest.mark.skipif(importlib.util.find_spec("pylsl") is None, reason="needs pylsl")
def test_discover(viewer_proc: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        mine = [ep for ep in rc.discover(timeout=2.0) if ep.pid == viewer_proc.pid]
        if mine:
            break
    else:
        pytest.fail("discover() did not find the viewer that the test started")
    ep = mine[0]
    assert (ep.port, ep.bind, ep.host, ep.protocol_version) == (PORT, "loopback", "127.0.0.1", 2)
    with ep.connect() as c:
        assert c.protocol == 2
