"""gen_can_cfg.py on the demo Can ECUC and, when present, on the BR1 Cabin ECU Can ECUC."""
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import gen_can_cfg  # noqa: E402

DEMO_OS = ROOT / "demo" / "ecuc" / "DemoEcu.arxml"
DEMO_CAN = ROOT / "demo" / "ecuc" / "DemoEcu_Can.arxml"
CABIN_CAN = Path("C:/br1_cabin_ecu/BSw/DaVinciWorkSpace/Config/ECUC/Cabin_ECU_Can_Can_ecuc.arxml")


def test_demo_config(tmp_path):
    assert gen_can_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO_OS), str(DEMO_CAN)]) == 0
    src = (tmp_path / "SilCan_Cfg.c").read_text()
    assert "SilCan_Cfg = { 2u, silcan_controllers, 6u, silcan_hohs }" in src
    assert '"CT_CAN0", 0u, 0u, 500000u, 0u, 0u, 0u, 0u, "CAN0_ORED_0_31_MB_IRQHandler"' in src
    assert '"Can0_Rx_Cmd", 1u, 0u, 0u, 0u, 0u, 1u, 1u, hoh1_filters' in src
    assert "{ 0x200u, 0x700u, 0u }" in src
    assert '"Can1_Rx_Status", 4u, 0u, 1u, 0u, 1u' in src            # FULL
    assert "const Can_43_FLEXCAN_ConfigType Can_43_FLEXCAN_Config" in src
    for ch in (0, 1):
        assert f"void CAN{ch}_ORED_0_31_MB_IRQHandler(void) {{ SilCan_Isr({ch}u, SILCAN_ISR_MB); }}" in src
        assert f"void CAN{ch}_ORED_IRQHandler(void) {{ SilCan_Isr({ch}u, SILCAN_ISR_BUSOFF); }}" in src


def test_empty_config_and_extra_channels(tmp_path):
    with pytest.raises(SystemExit):
        gen_can_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO_OS)])
    assert gen_can_cfg.main(["-o", str(tmp_path), "--quiet", "--allow-empty", "--channels", "13", str(DEMO_OS)]) == 0
    src = (tmp_path / "SilCan_Cfg.c").read_text()
    assert "SilCan_Cfg = { 0u, silcan_controllers, 0u, silcan_hohs }" in src
    assert "void CAN13_ORED_IRQHandler(void) { SilCan_Isr(255u, SILCAN_ISR_BUSOFF); }" in src


def test_idempotent(tmp_path):
    gen_can_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO_CAN)])
    p = tmp_path / "SilCan_Cfg.c"
    m1 = p.stat().st_mtime_ns
    gen_can_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO_CAN)])
    assert p.stat().st_mtime_ns == m1


@pytest.mark.skipif(not CABIN_CAN.exists(), reason="BR1 Cabin ECU not available")
def test_cabin_ecu(tmp_path):
    assert gen_can_cfg.main(["-o", str(tmp_path), "--quiet", "--channels", "13", str(CABIN_CAN)]) == 0
    src = (tmp_path / "SilCan_Cfg.c").read_text()
    assert "SilCan_Cfg = { 6u, silcan_controllers, 129u, silcan_hohs }" in src
    channels = sorted(int(m) for m in set(re.findall(r"void CAN(\d+)_ORED_IRQHandler\(void\);", src)))
    assert channels == [0, 4, 9, 11, 13, 14, 15]
    assert src.count("_filters[] = {") == 123
    assert ', 2000000u, ' in src  # FD data phase baud rate on three controllers
