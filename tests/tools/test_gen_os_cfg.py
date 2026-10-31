"""pytest for tools/ecuc.py and tools/gen_os_cfg.py (demo ECUC and, if present, the BR1 Cabin ECU)."""
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from ecuc import Ecuc  # noqa: E402
import gen_os_cfg  # noqa: E402

DEMO = ROOT / "demo" / "ecuc" / "DemoEcu.arxml"
CABIN_OS = Path(os.environ.get("SIL_CABIN_ECUC", r"C:\br1_cabin_ecu\BSw\DaVinciWorkSpace\Config\ECUC\Cabin_ECU_Os_Os_ecuc.arxml"))
CABIN_ECUC = CABIN_OS.with_name("Cabin_ECU_EcuC_EcuC_ecuc.arxml")


def test_demo_parses():
    e = Ecuc.load([DEMO])
    os_ = e.module("Os")
    assert len(os_.top("OsTask")) == 12
    assert len(os_.find("OsIsr")) == 7
    t = os_.byname("Task_1ms_Core0")
    assert t is not None and t.getint("OsTaskPriority") == 30
    assert t.ref("OsTaskEventRef") == ["Ev_1ms", "Ev_CanRx", "Ev_SpiDone"]


def test_demo_generate(tmp_path):
    assert gen_os_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO)]) == 0
    types = (tmp_path / "Os_Types_Lcfg.h").read_text()
    cfg = (tmp_path / "Os_Cfg.h").read_text()
    lcfg = (tmp_path / "Os_Lcfg.c").read_text()
    assert "OS_TASKID_COUNT = 12" in types
    assert "INVALID_ISR" in types and "OS_CORE_ID_1" in types
    # events of one task get distinct bits; events of different tasks may share
    masks = {line.split()[1]: line.split()[2] for line in cfg.splitlines() if line.startswith("#define Ev_")}
    assert masks["Ev_1ms"] != masks["Ev_CanRx"] != masks["Ev_SpiDone"]
    assert '"IdleTask_OsCore1", NULL_PTR, 0u, 1u' in lcfg  # idle task: no entry, core 1
    assert "SILOS_SCHED_NON" in lcfg and "SILOS_LOCK_CAT2_INTERRUPTS" in lcfg
    assert "IocWrite_Ioc_Speed_0" in (tmp_path / "Os_Ioc_Lcfg.h").read_text()
    assert "IocSend_Ioc_Samples_0" in (tmp_path / "Os_Ioc_Lcfg.c").read_text()
    assert "OS_MS2TICKS_SystemTimer_Core0" in (tmp_path / "Os_Hal_Cfg.h").read_text()


def test_demo_generate_is_idempotent(tmp_path):
    gen_os_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO)])
    first = {p.name: p.read_text() for p in tmp_path.iterdir()}
    gen_os_cfg.main(["-o", str(tmp_path), "--quiet", str(DEMO)])
    second = {p.name: p.read_text() for p in tmp_path.iterdir()}
    assert first == second


@pytest.mark.skipif(not CABIN_OS.exists(), reason="BR1 Cabin ECU configuration not available")
def test_cabin_ecu_generate(tmp_path):
    args = ["-o", str(tmp_path), "--quiet", "--no-isr-wrappers", str(CABIN_OS)]
    if CABIN_ECUC.exists():
        args.append(str(CABIN_ECUC))
    assert gen_os_cfg.main(args) == 0
    types = (tmp_path / "Os_Types_Lcfg.h").read_text()
    lcfg = (tmp_path / "Os_Lcfg.c").read_text()
    cfg = (tmp_path / "Os_Cfg.h").read_text()
    assert "OS_TASKID_COUNT = 18" in types and "OS_ISRID_COUNT = 71" in types and "OS_ALARMID_COUNT = 101" in types
    assert '"ApplicationRunTask_0_10ms_Core1", Os_Task_ApplicationRunTask_0_10ms_Core1, 12u, 1u' in lcfg
    assert "#define Rte_Ev_WP_ApplicationRunTask_50_5000ms_Core1 ((EventMaskType)2uLL)" in cfg
    assert lcfg.count("SILOS_LOCK_CAT2_INTERRUPTS") == 72 and lcfg.count("SILOS_LOCK_NOTHING") == 12
    assert '"OsResource", 30u' in lcfg
    assert "IocWrite_Rte_CtIcICALBMS_BMS_Multiplexor_BMS_Multiplexor_0(uint8 data)" in (tmp_path / "Os_Ioc_Lcfg.h").read_text()


def test_cli_help():
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_os_cfg.py"), "--help"], capture_output=True, text=True)
    assert r.returncode == 0 and "Os_Types_Lcfg.h" in r.stdout
