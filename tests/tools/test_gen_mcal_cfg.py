"""pytest for tools/gen_mcal_cfg.py (synthetic Dio/Spi/Mcu ECUC built with ecuc_build, plus the Cabin ECU if present)."""
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from ecuc_build import Cont, EcucModule  # noqa: E402
import gen_mcal_cfg  # noqa: E402

CABIN = Path(os.environ.get("SIL_CABIN_ECUC_DIR", r"C:\br1_cabin_ecu\BSw\DaVinciWorkSpace\Config\ECUC"))


def test_synthetic(tmp_path):
    dio = EcucModule("Dio", "/TS_T31D53M20I0R0/Dio")
    port = Cont("DioConfig", "DioConfig")
    p0 = Cont("DioPort", "DioPort_0", {"DioPortId": 0})
    p0.add(Cont("DioChannel", "LED_GREEN", {"DioChannelId": 17}))
    p0.add(Cont("DioChannel", "LED_RED", {"DioChannelId": 18}))
    port.add(p0)
    dio.add(port)
    dio.write(tmp_path / "Dio.arxml")
    spi = EcucModule("Spi", "/TS_T31D53M20I0R0/Spi")
    d = Cont("SpiDriver", "SpiDriver")
    d.add(Cont("SpiSequence", "Seq_Adc", {"SpiSequenceId": 3}))
    d.add(Cont("SpiJob", "Job_Adc", {"SpiJobId": 5}))
    spi.add(d)
    spi.write(tmp_path / "Spi.arxml")
    mcu = EcucModule("Mcu", "/TS_T31D53M20I0R0/Mcu")
    ms = Cont("McuModuleConfiguration", "McuModuleConfiguration")
    ms.add(Cont("McuModeSettingConf", "McuModeSettingConf_0", {"McuMode": 0}))
    ms.add(Cont("McuClockSettingConfig", "McuClockSettingConfig_0", {"McuClockSettingId": 0}))
    mcu.add(ms)
    mcu.write(tmp_path / "Mcu.arxml")
    out = tmp_path / "gen"
    assert gen_mcal_cfg.main(["-o", str(out), "--quiet", str(tmp_path / "Dio.arxml"), str(tmp_path / "Spi.arxml"), str(tmp_path / "Mcu.arxml")]) == 0
    dio_h = (out / "Dio_Cfg.h").read_text()
    assert "#define DioConf_DioChannel_LED_GREEN ((Dio_ChannelType)17U)" in dio_h
    assert "#define DioConf_DioPort_DioPort_0 ((Dio_PortType)0U)" in dio_h
    spi_h = (out / "Spi_Cfg.h").read_text()
    assert "#define SpiConf_SpiSequence_Seq_Adc ((Spi_SequenceType)3U)" in spi_h
    assert "#define SpiConf_SpiJob_Job_Adc ((Spi_JobType)5U)" in spi_h
    mcu_h = (out / "Mcu_Cfg.h").read_text()
    assert "#define McuConf_McuModeSettingConf_McuModeSettingConf_0 ((Mcu_ModeType)0U)" in mcu_h
    assert "#define McuConf_McuClockSettingConfig_McuClockSettingConfig_0 ((Mcu_ClockType)0U)" in mcu_h


@pytest.mark.skipif(not (CABIN / "Cabin_ECU_Dio_Dio_ecuc.arxml").exists(), reason="BR1 Cabin ECU configuration not available")
def test_cabin(tmp_path):
    files = [str(CABIN / f"Cabin_ECU_{m}_{m}_ecuc.arxml") for m in ("Dio", "Spi", "Mcu", "Can")]
    assert gen_mcal_cfg.main(["-o", str(tmp_path), "--quiet"] + files) == 0
    dio_h = (tmp_path / "Dio_Cfg.h").read_text()
    assert dio_h.count("DioConf_DioChannel_") == 84
    assert "McuConf_McuModeSettingConf_McuModeSettingConf_0" in (tmp_path / "Mcu_Cfg.h").read_text()
    can_h = (tmp_path / "Can_43_FLEXCAN_Cfg.h").read_text()
    assert can_h.count("CanConf_CanHardwareObject_") == 129 and can_h.count("CanConf_CanController_") == 6
