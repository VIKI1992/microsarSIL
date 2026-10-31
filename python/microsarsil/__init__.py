"""microsarsil - test automation for ECUs running on the microsarSIL runtime.

    from microsarsil import SilProcess
    with SilProcess("demo_ecu.exe", paused=True) as ecu:
        ecu.control.run_until(0.5)             # virtual seconds
        ecu.control.can_send(0, 0x201, b"\\x01\\x02")
        ecu.control.step(0.002)
        frames = ecu.control.can_recv()

The UDP virtual bus (udp:<group>:<port>) is reachable through SilUdpSocket (no dependency) or the
python-can interface "silcan" (SilCanBus).
"""
from .control import SilControl, SilControlError
from .process import SilProcess, find_demo_exe
from .udpbus import Frame, SilUdpSocket

__all__ = ["SilControl", "SilControlError", "SilProcess", "find_demo_exe", "Frame", "SilUdpSocket"]
__version__ = "0.3.0"
