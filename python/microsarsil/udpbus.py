"""The UDP multicast virtual CAN bus of microsarSIL (sil/bus/sil_bus.h wire format).

Datagram (little endian): "SILCAN1\\0" | u32 node id | u32 can id | u8 flags | u8 len | u16 0 | u64 t_ns | data[len]
flags: bit0 extended id, bit1 CAN FD, bit2 RTR, bit3 BRS. Receivers drop their own node id.

SilUdpSocket needs nothing but the standard library; SilCanBus is a python-can interface
("silcan", channel "<group>:<port>") when python-can is installed.
"""
from __future__ import annotations

import os
import socket
import struct
import time
from dataclasses import dataclass, field
from typing import Optional, Tuple

MAGIC = b"SILCAN1\x00"
HEADER = struct.Struct("<8sIIBBHQ")  # 28 bytes
FLAG_EXT, FLAG_FD, FLAG_RTR, FLAG_BRS = 0x01, 0x02, 0x04, 0x08
DEFAULT_GROUP = "239.0.0.1"
DEFAULT_PORT = 52000


@dataclass
class Frame:
    id: int
    data: bytes = b""
    ext: bool = False
    fd: bool = False
    rtr: bool = False
    brs: bool = False
    t_ns: int = 0
    node: int = 0
    host_time: float = field(default_factory=time.time)

    @property
    def flags(self) -> int:
        return (FLAG_EXT if self.ext else 0) | (FLAG_FD if self.fd else 0) | (FLAG_RTR if self.rtr else 0) | (FLAG_BRS if self.brs else 0)


def encode(frame: Frame, node: int) -> bytes:
    return HEADER.pack(MAGIC, node & 0xFFFFFFFF, frame.id & 0x1FFFFFFF, frame.flags, len(frame.data), 0, frame.t_ns) + bytes(frame.data)


def decode(buf: bytes) -> Optional[Frame]:
    if len(buf) < HEADER.size:
        return None
    magic, node, cid, flags, length, _, t_ns = HEADER.unpack_from(buf)
    if magic != MAGIC or len(buf) < HEADER.size + length:
        return None
    return Frame(cid, bytes(buf[HEADER.size:HEADER.size + length]), bool(flags & FLAG_EXT), bool(flags & FLAG_FD), bool(flags & FLAG_RTR),
                 bool(flags & FLAG_BRS), t_ns, node)


def parse_spec(spec: str) -> Tuple[str, int]:
    """'udp:239.0.0.1:52000' | '239.0.0.1:52000' | 'udp' | '52000' -> (group, port)"""
    s = spec
    for prefix in ("udp://", "udp:", "udp"):
        if s.startswith(prefix):
            s = s[len(prefix):]
            break
    if not s:
        return DEFAULT_GROUP, DEFAULT_PORT
    if ":" in s:
        group, port = s.rsplit(":", 1)
        return group or DEFAULT_GROUP, int(port)
    if "." in s:
        return s, DEFAULT_PORT
    return DEFAULT_GROUP, int(s)


class SilUdpSocket:
    """One participant on the UDP virtual bus."""

    def __init__(self, spec: str = "udp", node: Optional[int] = None) -> None:
        self.group, self.port = parse_spec(spec)
        self.node = node if node is not None else (int.from_bytes(os.urandom(4), "little") or 1)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            try:
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except OSError:
                pass
        self.sock.bind(("", self.port))
        mreq = socket.inet_aton(self.group) + socket.inet_aton("0.0.0.0")
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)

    def send(self, id: int, data: bytes = b"", ext: Optional[bool] = None, fd: bool = False, rtr: bool = False, brs: bool = False,
             t_ns: int = 0) -> None:
        if ext is None:
            ext = id > 0x7FF
        self.sock.sendto(encode(Frame(id, bytes(data), ext, fd, rtr, brs, t_ns), self.node), (self.group, self.port))

    def send_frame(self, frame: Frame) -> None:
        self.sock.sendto(encode(frame, self.node), (self.group, self.port))

    def recv(self, timeout: Optional[float] = 1.0) -> Optional[Frame]:
        """Next frame from another participant, None on timeout."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            remaining = None
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    return None
            self.sock.settimeout(remaining)
            try:
                buf, _ = self.sock.recvfrom(4096)
            except socket.timeout:
                return None
            f = decode(buf)
            if f is None or f.node == self.node:
                continue
            return f

    def close(self) -> None:
        try:
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_DROP_MEMBERSHIP, socket.inet_aton(self.group) + socket.inet_aton("0.0.0.0"))
        except OSError:
            pass
        self.sock.close()

    def __enter__(self) -> "SilUdpSocket":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


try:  # python-can interface, optional
    import can as _can

    class SilCanBus(_can.BusABC):
        """python-can interface for the microsarSIL UDP bus: can.Bus(interface="silcan", channel="239.0.0.1:52000")."""

        def __init__(self, channel: str = "udp", can_filters=None, **kwargs) -> None:  # type: ignore[no-untyped-def]
            self._sil = SilUdpSocket(channel or "udp")
            self.channel_info = f"microsarSIL udp bus {self._sil.group}:{self._sil.port}"
            super().__init__(channel=channel, can_filters=can_filters, **kwargs)

        def _recv_internal(self, timeout):  # type: ignore[no-untyped-def]
            f = self._sil.recv(timeout)
            if f is None:
                return None, False
            msg = _can.Message(timestamp=f.host_time, arbitration_id=f.id, is_extended_id=f.ext, is_remote_frame=f.rtr, is_fd=f.fd,
                               bitrate_switch=f.brs, dlc=len(f.data), data=f.data, channel=self.channel_info)
            return msg, False

        def send(self, msg, timeout=None):  # type: ignore[no-untyped-def]
            self._sil.send(msg.arbitration_id, bytes(msg.data), msg.is_extended_id, msg.is_fd, msg.is_remote_frame, msg.bitrate_switch)

        def shutdown(self) -> None:
            super().shutdown()
            self._sil.close()

except ImportError:  # pragma: no cover
    SilCanBus = None  # type: ignore[assignment,misc]
