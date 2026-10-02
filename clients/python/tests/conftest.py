from __future__ import annotations

import os
import sys
from typing import Iterator

import pytest

sys.path.insert(0, os.path.dirname(__file__))  # so the tests can import fake_server

from fake_server import FakeServer, FakeStream, FakeViewer  # noqa: E402


@pytest.fixture
def viewer() -> FakeViewer:
    return FakeViewer(
        streams=[
            FakeStream("host1|MockEEG", "MockEEG", "EEG", 32, 500),
            FakeStream("host1|MockMarkers", "MockMarkers", "Markers", 1, 0),
            FakeStream("uid-1234", "Audio in", "Audio", 2, 48000),
        ],
        file="C:/data/my study/sub 01.xdf",
    )


@pytest.fixture
def server(viewer: FakeViewer) -> Iterator[FakeServer]:
    srv = FakeServer(viewer)
    yield srv
    srv.close()
