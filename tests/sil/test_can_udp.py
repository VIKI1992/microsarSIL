"""The UDP virtual bus: frames between the ECU's CAN0 and a python participant (SilUdpSocket / python-can)."""
import socket
import time

import pytest

from microsarsil import SilProcess, SilUdpSocket


def _bus_ok(port: int) -> bool:
    try:
        with SilUdpSocket(f"udp:239.0.0.1:{port}") as s:
            s.send(0x7FF, b"\x00")
    except OSError:
        return False
    return True


@pytest.fixture
def udp_spec(sil_free_udp_port):
    spec = f"udp:239.0.0.1:{sil_free_udp_port}"
    if not _bus_ok(sil_free_udp_port):
        pytest.skip("no multicast capable interface on this host")
    return spec


def test_udp_rx_and_reply_in_virtual_time(sil_exe, udp_spec):
    with SilProcess(sil_exe, paused=True, can={0: udp_spec}) as ecu, SilUdpSocket(udp_spec) as bus:
        c = ecu.control
        assert c.can_stats(0)["bus"] == udp_spec
        c.run_until(0.02)
        # a frame from the python side reaches the paused ECU through the UDP rx thread
        deadline = time.monotonic() + 5.0
        while c.can_stats(0)["rx"] == 0 and time.monotonic() < deadline:
            bus.send(0x201, b"\xaa\xbb")
            time.sleep(0.05)
        assert c.can_stats(0)["rx"] >= 1
        c.step(0.002)                                   # the ECU answers with 0x301 ...
        got = None
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            f = bus.recv(0.5)
            if f is not None and f.id == 0x301:
                got = f
                break
        assert got is not None and got.data == b"\xaa\xbb"
        assert not got.ext and not got.fd


def test_udp_status_frames_in_realtime(sil_exe, udp_spec):
    # free running real time; the ECU is stopped through the control server once enough frames arrived
    with SilUdpSocket(udp_spec) as bus, SilProcess(sil_exe, args=["--time=realtime"], can={0: udp_spec}) as ecu:
        frames = []
        deadline = time.monotonic() + 20.0
        while len(frames) < 5 and time.monotonic() < deadline:
            f = bus.recv(1.0)
            if f is not None and f.id == 0x100:
                frames.append(f)
        assert len(frames) >= 5
        counters = [int.from_bytes(f.data[0:4], "little") for f in frames]
        assert counters == sorted(counters) and counters[-1] > counters[0]
        assert ecu.control.time()["mode"] == "realtime"
        assert ecu.stop() == 0


def test_python_can_interface(sil_exe, udp_spec):
    can = pytest.importorskip("can")
    from microsarsil.udpbus import SilCanBus

    with SilProcess(sil_exe, paused=True, can={0: udp_spec}) as ecu:
        bus = SilCanBus(channel=udp_spec)
        try:
            c = ecu.control
            c.run_until(0.01)
            deadline = time.monotonic() + 5.0
            while c.can_stats(0)["rx"] == 0 and time.monotonic() < deadline:
                bus.send(can.Message(arbitration_id=0x201, data=[1, 2, 3, 4], is_extended_id=False))
                time.sleep(0.05)
            assert c.can_stats(0)["rx"] >= 1
            c.step(0.002)
            msg = None
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                m = bus.recv(0.5)
                if m is not None and m.arbitration_id == 0x301:
                    msg = m
                    break
            assert msg is not None and bytes(msg.data) == b"\x01\x02\x03\x04"
        finally:
            bus.shutdown()


def test_wire_format_roundtrip():
    from microsarsil.udpbus import Frame, decode, encode

    f = Frame(0x1ABCDEF0, bytes(range(12)), ext=True, fd=True, brs=True, t_ns=123456789)
    buf = encode(f, 0xDEADBEEF)
    assert len(buf) == 28 + 12 and buf[:8] == b"SILCAN1\x00"
    g = decode(buf)
    assert g is not None and g.id == f.id and g.data == f.data and g.ext and g.fd and g.brs and not g.rtr
    assert g.t_ns == 123456789 and g.node == 0xDEADBEEF
    assert decode(b"junk") is None
    assert socket  # keep the import used on platforms without multicast
