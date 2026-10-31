"""SilProcess - start an ECU executable built on the microsarSIL runtime and talk to its control server."""
from __future__ import annotations

import os
import re
import subprocess
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Union

from .control import SilControl, SilControlError

_LISTEN_RE = re.compile(r"control: listening on ([\d.]+):(\d+)")


def find_demo_exe(start: Optional[Union[str, Path]] = None) -> Optional[Path]:
    """The demo ECU executable: $MICROSARSIL_DEMO_EXE or a build tree below the repository root."""
    env = os.environ.get("MICROSARSIL_DEMO_EXE")
    if env and Path(env).exists():
        return Path(env)
    root = Path(start or Path(__file__).resolve().parents[2])
    candidates: List[Path] = []
    for pattern in ("build/*/demo/demo_ecu.exe", "build/*/demo/demo_ecu", "build/*/demo/*/demo_ecu.exe", "build/demo/demo_ecu.exe",
                    "build/demo/demo_ecu"):
        candidates += list(root.glob(pattern))
    candidates = [c for c in candidates if c.is_file()]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


class SilProcess:
    """Runs an ECU executable. With control=True the process is started with --control (ephemeral port)
    and `self.control` is a connected SilControl. stderr lines are collected in `self.log`."""

    def __init__(self, exe: Union[str, Path], args: Sequence[str] = (), control: bool = True, paused: bool = False,
                 duration: Optional[float] = None, can: Optional[Dict[int, str]] = None, cwd: Optional[Union[str, Path]] = None,
                 env: Optional[Dict[str, str]] = None, startup_timeout: float = 30.0, quiet: bool = True) -> None:
        cmd: List[str] = [str(exe), *args]
        if control:
            cmd.append("--control=127.0.0.1:0")
        if paused:
            cmd.append("--paused")
        if duration is not None:
            cmd.append(f"--duration={duration}")
        if quiet and "--quiet" not in cmd:
            cmd.append("--quiet")
        for ctrl, spec in (can or {}).items():
            cmd.append(f"--can{ctrl}={spec}")
        self.cmd = cmd
        self.log: List[str] = []
        self._listen = threading.Event()
        self._host = "127.0.0.1"
        self._port = 0
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, cwd=cwd, env=env)
        self._reader = threading.Thread(target=self._read, name="sil-log", daemon=True)
        self._reader.start()
        self.control: Optional[SilControl] = None
        if control:
            if not self._listen.wait(startup_timeout):
                self.kill()
                raise SilControlError("the ECU did not start its control server:\n" + "\n".join(self.log[-20:]))
            self.control = SilControl(self._host, self._port)

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            self.log.append(line)
            m = _LISTEN_RE.search(line)
            if m:
                self._host, self._port = m.group(1), int(m.group(2))
                self._listen.set()
        self._listen.set()

    @property
    def port(self) -> int:
        return self._port

    def wait(self, timeout: Optional[float] = None) -> int:
        code = self.proc.wait(timeout)
        self._reader.join(5)
        return code

    def stop(self, timeout: float = 20.0) -> int:
        """Ask the ECU to shut down (ShutdownOS from outside) and wait for the process."""
        if self.control is not None:
            self.control.stop()
            self.control.close()
            self.control = None
        try:
            return self.wait(timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            return self.wait(5)

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        if self.control is not None:
            self.control.close()
            self.control = None

    @property
    def returncode(self) -> Optional[int]:
        return self.proc.poll()

    def __enter__(self) -> "SilProcess":
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()

    def wait_log(self, pattern: str, timeout: float = 10.0) -> Optional[str]:
        """Wait for a log line matching the regular expression."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            lines = self.log[seen:]
            for line in lines:
                if rx.search(line):
                    return line
            seen += len(lines)
            if self.proc.poll() is not None and seen >= len(self.log):
                return None
            time.sleep(0.02)
        return None
