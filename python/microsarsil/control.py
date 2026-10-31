"""Client of the SIL control server (newline delimited JSON over TCP, see sil/ctrl/sil_ctrl.h)."""
from __future__ import annotations

import json
import socket
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Union

NS = 1_000_000_000


class SilControlError(RuntimeError):
    pass


def _ns(value: Union[int, float]) -> int:
    """Seconds (float) or nanoseconds (int) -> nanoseconds."""
    if isinstance(value, float):
        return int(round(value * NS))
    return int(value)


@dataclass
class CanFrame:
    t_ns: int
    ctrl: int
    dir: str          # "tx" (sent by the ECU) or "rx" (received by the ECU)
    id: int
    data: bytes
    ext: bool = False
    fd: bool = False

    @property
    def t(self) -> float:
        return self.t_ns / NS


class SilControl:
    def __init__(self, host: str = "127.0.0.1", port: int = 0, timeout: float = 60.0, connect_timeout: float = 10.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        deadline = time.monotonic() + connect_timeout
        last: Optional[Exception] = None
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=timeout)
                break
            except OSError as e:  # the server thread may still be starting
                last = e
                if time.monotonic() > deadline:
                    raise SilControlError(f"cannot connect to {host}:{port}: {e}") from e
                time.sleep(0.05)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._rx = self.sock.makefile("rb")
        self._seq = 0
        del last

    def close(self) -> None:
        try:
            self._rx.close()
            self.sock.close()
        except OSError:
            pass

    def __enter__(self) -> "SilControl":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ------------------------------------------------------------------
    def call(self, cmd: str, **kw: Any) -> Dict[str, Any]:
        self._seq += 1
        req = {"cmd": cmd, "id": self._seq}
        req.update({k: v for k, v in kw.items() if v is not None})
        self.sock.sendall((json.dumps(req) + "\n").encode())
        line = self._rx.readline()
        if not line:
            raise SilControlError(f"connection closed while waiting for the reply to {cmd}")
        rep = json.loads(line)
        if not rep.get("ok"):
            raise SilControlError(rep.get("error", "unknown error"))
        return rep

    # ---- time ----------------------------------------------------------
    def ping(self) -> Dict[str, Any]:
        return self.call("ping")

    def time(self) -> Dict[str, Any]:
        return self.call("time")

    @property
    def now_ns(self) -> int:
        return int(self.call("time")["t_ns"])

    @property
    def now(self) -> float:
        return self.now_ns / NS

    def run_until(self, t: Union[int, float], timeout_ms: int = 60000) -> Dict[str, Any]:
        """Let virtual time run up to t (float seconds or int ns) and wait until the ECU is quiescent there."""
        rep = self.call("run_until", t_ns=_ns(t), timeout_ms=timeout_ms)
        if not rep.get("quiescent"):
            raise SilControlError(f"ECU not quiescent at {_ns(t)} ns within {timeout_ms} ms (t_ns={rep.get('t_ns')})")
        return rep

    def step(self, dt: Union[int, float], timeout_ms: int = 60000) -> Dict[str, Any]:
        rep = self.call("step", ns=_ns(dt), timeout_ms=timeout_ms)
        if not rep.get("quiescent"):
            raise SilControlError(f"ECU not quiescent after a step of {_ns(dt)} ns within {timeout_ms} ms")
        return rep

    def sync(self, timeout_ms: int = 60000) -> Dict[str, Any]:
        """Wait until the OS started and, when paused, until everything at the paused time ran."""
        return self.call("sync", timeout_ms=timeout_ms)

    def pause(self, timeout_ms: int = 60000) -> Dict[str, Any]:
        return self.call("pause", timeout_ms=timeout_ms)

    def resume(self) -> Dict[str, Any]:
        return self.call("resume")

    def stop(self, code: int = 0) -> None:
        try:
            self.call("stop", code=code)
        except SilControlError:
            pass

    # ---- observation ------------------------------------------------------
    def stats(self) -> Dict[str, Any]:
        return self.call("stats")

    def task_activations(self, name: str) -> int:
        return int(self.stats()["tasks"][name])

    def isr_runs(self, name: str) -> int:
        return int(self.stats()["isrs"][name])

    def can_stats(self, ctrl: int = 0) -> Dict[str, Any]:
        return self.stats()["can"][ctrl]

    def user_event(self, id: int, aux: int = 0) -> None:
        self.call("user", id=id, aux=aux)

    def write_traces(self) -> None:
        self.call("write_traces")

    def irq(self, isr: str) -> int:
        return int(self.call("irq", isr=isr)["isr"])

    # ---- CAN -------------------------------------------------------------
    def can_send(self, ctrl: int, id: int, data: Union[bytes, bytearray, Sequence[int]] = b"", ext: Optional[bool] = None,
                 fd: Optional[bool] = None, brs: bool = False, rtr: bool = False) -> int:
        """Deliver a frame to controller ctrl as if it came from the bus. Returns the virtual time."""
        rep = self.call("can_send", ctrl=ctrl, id=id, data=bytes(data).hex(), ext=ext, fd=fd, brs=brs or None, rtr=rtr or None)
        return int(rep["t_ns"])

    def can_recv(self, max: int = 256) -> List[CanFrame]:
        """Frames the ECU sent (dir tx) or received (dir rx) since the previous call."""
        rep = self.call("can_recv", max=max)
        return [CanFrame(int(f["t_ns"]), int(f["ctrl"]), str(f["dir"]), int(f["id"]), bytes.fromhex(f["data"]), bool(f["ext"]), bool(f["fd"]))
                for f in rep["frames"]]

    def can_busoff(self, ctrl: int) -> None:
        self.call("can_busoff", ctrl=ctrl)

    def can_bus(self, ctrl: int, spec: str) -> None:
        """Attach a bus backend: null | loop:<name> | udp[:<group>:<port>]. ctrl -1 = all controllers."""
        self.call("can_bus", ctrl=ctrl, spec=spec)

    # ---- virtual I/O ----------------------------------------------------------
    def io_set(self, key: str, value: float) -> float:
        """Set a virtual input: "dio.5", "adc.1", "icu.0.freq", "DioConf_DioChannel_BUTTON", ... Returns the stored value."""
        return float(self.call("io_set", key=key, value=value)["value"])

    def io_get(self, key: str) -> float:
        return float(self.call("io_get", key=key)["value"])

    def io_list(self) -> Dict[str, float]:
        """Every I/O field that has been set, as {key: value} (symbolic names where known)."""
        out: Dict[str, float] = {}
        for e in self.call("io_list")["entries"]:
            out[e["key"]] = float(e["value"])
            if e.get("symbol"):
                out[e["symbol"] + e["key"][e["key"].rindex("."):]] = float(e["value"])
        return out

    # ---- peripheral traffic -----------------------------------------------------
    def periph_recv(self, max: int = 256) -> List["PeriphEvent"]:
        """SPI transfers, I2C transactions, UART bytes and LIN frames seen since the previous call."""
        rep = self.call("periph_recv", max=max)
        return [PeriphEvent(int(e["t_ns"]), str(e["kind"]), int(e["channel"]), int(e["dir"]), int(e["id"]), bytes.fromhex(e["tx"]),
                            bytes.fromhex(e["rx"])) for e in rep["events"]]

    def reg_write(self, address: int, reg: int, data: Union[bytes, Sequence[int]], bus: int = 1) -> None:
        """Preload a register file device (bus 1 = I2C slave address, bus 0 = SPI device index)."""
        self.call("reg_write", bus=bus, device=address, reg=reg, data=bytes(data).hex())

    def reg_read(self, address: int, reg: int, length: int = 1, bus: int = 1) -> bytes:
        return bytes.fromhex(self.call("reg_read", bus=bus, device=address, reg=reg, len=length)["data"])

    def uart_send(self, channel: int, data: Union[bytes, str]) -> None:
        if isinstance(data, str):
            self.call("uart_send", channel=channel, text=data)
        else:
            self.call("uart_send", channel=channel, data=bytes(data).hex())

    def uart_recv(self, channel: int = 0) -> bytes:
        """Bytes the ECU sent on the channel since the previous call."""
        return bytes.fromhex(self.call("uart_recv", channel=channel)["data"])

    def lin_response(self, channel: int, pid: int, data: Union[bytes, Sequence[int]]) -> None:
        """Slave response the virtual LIN bus answers to a header with this PID (empty data removes it)."""
        self.call("lin_response", channel=channel, pid=pid, data=bytes(data).hex())

    def spi_model(self, device: int, model: str) -> None:
        """Slave model of an SPI external device: echo | zero | ones | regfile."""
        self.call("spi_model", device=device, model=model)


@dataclass
class PeriphEvent:
    t_ns: int
    kind: str        # spi | i2c | uart | lin
    channel: int     # spi: device index
    dir: int         # 0 ecu -> world, 1 world -> ecu, 2 both (spi)
    id: int          # i2c address, lin pid, spi sequence
    tx: bytes
    rx: bytes
