"""pytest fixtures: `sil_exe` (the ECU executable, from $MICROSARSIL_DEMO_EXE or a build tree) and
`sil_ecu` (a paused ECU with a connected control client, one per test)."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Iterator

import pytest

from .process import SilProcess, find_demo_exe


@pytest.fixture(scope="session")
def sil_exe() -> Path:
    exe = find_demo_exe()
    if exe is None:
        pytest.skip("no ECU executable: set MICROSARSIL_DEMO_EXE or build the demo")
    return exe


@pytest.fixture
def sil_ecu(sil_exe: Path) -> Iterator[SilProcess]:
    with SilProcess(sil_exe, paused=True) as ecu:
        yield ecu


@pytest.fixture
def sil_free_udp_port() -> int:
    """A port for a private UDP bus so that parallel test sessions do not hear each other."""
    return 50000 + (os.getpid() % 10000)
