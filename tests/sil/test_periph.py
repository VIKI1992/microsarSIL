"""Virtual MCAL on the demo: I/O registry, ADC -> PWM, ICU, GPT, SPI, I2C, UART, LIN, Fee persistence."""
import re

import pytest

from microsarsil import SilProcess


def _stat(c, timeout_s=0.5):
    """Drop old UART output, run until the next status line and parse the latest one into a dict."""
    c.uart_recv(0)
    deadline = c.now + timeout_s + 0.11
    while c.now < deadline:
        c.step(0.01)
        text = c.uart_recv(0).decode(errors="replace")
        lines = re.findall(r"STAT t=([\d.]+) ([^\n]*)", text)
        if lines:
            return {k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)", lines[-1][1])}
    raise AssertionError("no UART status line")


def test_io_registry_and_dio(sil_ecu):
    c = sil_ecu.control
    c.run_until(0.05)
    assert c.io_get("DioConf_DioChannel_LED_GREEN") == 0.0
    c.io_set("DioConf_DioChannel_BUTTON", 1)
    c.step(0.02)
    assert c.io_get("dio.1") == 1.0                       # LED_GREEN follows the button
    c.io_set("dio.5", 0)
    c.step(0.02)
    assert c.io_get("DioConf_DioChannel_LED_GREEN") == 0.0
    entries = c.io_list()
    assert entries["DioConf_DioChannel_LED_GREEN.value"] == 0.0
    assert "pwm.0.value" in entries


def test_adc_drives_pwm_and_uart_status(sil_ecu):
    c = sil_ecu.control
    c.io_set("AdcConf_AdcChannel_AdcChannel_Vbat", 2500)
    c.io_set("adc.1", 1000)                                 # temperature counts -> fan duty 8000
    c.run_until(0.2)
    assert c.io_get("PwmConf_PwmChannel_PwmChannel_Fan") == 8000.0
    st = _stat(c)
    assert st["vbat"] == 2500 and st["temp"] == 1000 and st["fan"] == 8000


def test_icu_measurement_counting_and_edges(sil_ecu):
    c = sil_ecu.control
    c.io_set("icu.0.freq", 250)                             # 4 ms period at 1 MHz ticks
    c.io_set("icu.2.freq", 1000)                            # both edges counted
    c.run_until(0.3)
    st = _stat(c)
    assert st["speed"] == 4000
    assert 500 <= st["pulses"] <= 700
    # static hall input: two rising edges from outside
    c.io_set("icu.1.value", 1)
    c.step(0.001)
    c.io_set("icu.1.value", 0)
    c.step(0.001)
    c.io_set("icu.1.value", 1)
    c.step(0.001)
    st = _stat(c)
    assert st["hall"] == 2
    assert c.isr_runs("SilOs_SoftIsr_OsCore0") >= 2


def test_gpt_timeout(sil_ecu):
    c = sil_ecu.control
    c.run_until(0.45)
    assert c.io_get("DioConf_DioChannel_LED_RED") == 0.0
    c.run_until(0.52)
    assert c.io_get("DioConf_DioChannel_LED_RED") == 1.0
    st = _stat(c)
    assert st["gpt"] == 1


def test_spi_echo_and_models(sil_ecu):
    c = sil_ecu.control
    c.run_until(0.1)
    c.periph_recv()
    c.step(0.02)
    spi = [e for e in c.periph_recv() if e.kind == "spi"]
    assert len(spi) >= 3
    assert all(e.rx == e.tx for e in spi)                   # echo model
    assert c.isr_runs("SpiIsr_0") >= 3
    c.spi_model(0, "zero")
    c.step(0.02)
    spi = [e for e in c.periph_recv() if e.kind == "spi"]
    assert spi and all(e.rx == bytes(len(e.rx)) for e in spi)
    c.spi_model(0, "regfile")
    c.reg_write(0, 0x00, b"\x11\x22\x33", bus=0)
    c.step(0.02)
    spi = [e for e in c.periph_recv() if e.kind == "spi"]
    assert spi and spi[-1].rx[1:4] == b"\x11\x22\x33"      # read of register 0 (tx[0] = 0x80)


def test_i2c_register_device(sil_ecu):
    c = sil_ecu.control
    c.reg_write(0x48, 0, b"\x01\x90")                       # temperature register of the sensor model
    c.run_until(0.15)
    st = _stat(c)
    assert st["i2c"] == 0x0190
    i2c = [e for e in c.periph_recv() if e.kind == "i2c"]
    assert any(e.dir == 1 and e.rx == b"\x01\x90" for e in i2c)


def test_lin_master_frames(sil_ecu):
    c = sil_ecu.control
    c.lin_response(0, 0x10, b"\xaa\xbb\xcc\xdd\x00\x00\x00\x00")
    c.run_until(0.2)
    lin = [e for e in c.periph_recv() if e.kind == "lin"]
    assert any(e.channel == 1 and e.dir == 0 and e.id == 0x11 for e in lin)
    assert any(e.channel == 0 and e.dir == 1 and e.id == 0x10 and e.rx.startswith(b"\xaa\xbb\xcc\xdd") for e in lin)
    st = _stat(c)
    assert st["lin"] >= 1


def test_fee_persists_across_runs(sil_exe, tmp_path):
    nv = str(tmp_path)
    with SilProcess(sil_exe, paused=True, args=[f"--nv={nv}"]) as ecu:
        c = ecu.control
        c.run_until(2.15)
        st = _stat(c)
        assert st["odo"] == 2 and st["boot"] == 1
    assert (tmp_path / "fee.bin").exists() and (tmp_path / "memacc0.bin").exists()
    with SilProcess(sil_exe, paused=True, args=[f"--nv={nv}"]) as ecu:
        c = ecu.control
        c.run_until(1.15)
        st = _stat(c)
        assert st["odo"] == 3 and st["boot"] == 2


def test_io_init_option(sil_exe):
    with SilProcess(sil_exe, paused=True, args=["--io=dio.5=1", "--io=AdcConf_AdcChannel_AdcChannel_Vbat=777"]) as ecu:
        c = ecu.control
        c.run_until(0.05)
        assert c.io_get("DioConf_DioChannel_LED_GREEN") == 1.0
        assert c.io_get("adc.0") == 777.0
        with pytest.raises(Exception):
            c.io_get("nothing.here")
