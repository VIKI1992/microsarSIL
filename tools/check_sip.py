#!/usr/bin/env python3
"""check_sip.py - inventory a MICROSAR Classic project for SIL integration.

Reports what the SIL has to provide: OS API and Vector OS extensions used outside the OS,
OS private headers included by non-OS code, MCAL driver APIs called by BSW/application code,
ISR definitions, CanIf driver binding, and the OS configuration summary.

Usage: check_sip.py <project root> [--sip <sip root>] [--gen <Generated_BSW dir>] [--json out.json]

Defaults match the BR1 layout: <root>/Sip, <root>/BSw/DaVinciWorkSpace/Generated_BSW,
<root>/ASw, <root>/BSw/StartUpCode.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Set

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ecuc import Ecuc  # noqa: E402

OSEK_API = [
    "ActivateTask", "TerminateTask", "ChainTask", "Schedule", "GetTaskID", "GetTaskState", "SetEvent", "ClearEvent",
    "GetEvent", "WaitEvent", "GetResource", "ReleaseResource", "SetRelAlarm", "SetAbsAlarm", "CancelAlarm", "GetAlarm",
    "GetAlarmBase", "GetCounterValue", "GetElapsedValue", "IncrementCounter", "StartScheduleTableRel",
    "StartScheduleTableAbs", "StopScheduleTable", "NextScheduleTable", "GetScheduleTableStatus", "StartOS",
    "ShutdownOS", "ShutdownAllCores", "GetActiveApplicationMode", "GetApplicationID", "GetCurrentApplicationID",
    "GetISRID", "GetCoreID", "GetNumberOfActivatedCores", "StartCore", "StartNonAutosarCore", "GetSpinlock",
    "ReleaseSpinlock", "TryToGetSpinlock", "DisableAllInterrupts", "EnableAllInterrupts", "SuspendAllInterrupts",
    "ResumeAllInterrupts", "SuspendOSInterrupts", "ResumeOSInterrupts", "CallTrustedFunction", "CheckISRMemoryAccess",
    "CheckTaskMemoryAccess", "CheckObjectAccess", "CheckObjectOwnership", "AllowAccess", "TerminateApplication",
    "GetApplicationState", "ControlIdle", "EnableInterruptSource", "DisableInterruptSource", "ClearPendingInterrupt",
]
MCAL_PREFIXES = [
    "Can_43_FLEXCAN", "Can_43_CANEXCEL", "Dio", "Port", "Spi", "Adc", "Gpt", "Icu", "Pwm", "Mcu", "I2c", "I3c", "Uart",
    "MemAcc", "Mem_43_INFLS", "Mem_43_EXFLS", "Crypto_43_HSE", "Platform", "IntCtrl_Ip", "Sbc_fs8x", "OsIf", "Lin",
    "Fee", "Fls", "Wdg", "eMcem", "sBoot", "sCheck", "Bist", "Mcl", "Hse_Ip", "Power_Ip", "Clock_Ip", "Pit_Ip", "Stm_Ip",
    "Siul2_Dio_Ip", "Siul2_Port_Ip", "Siul2_Icu_Ip", "Lpspi_Ip", "Lpi2c_Ip", "Lpuart_Uart_Ip", "Linflexd_Uart_Ip",
    "Emios_Icu_Ip", "Emios_Pwm_Ip", "Emios_Gpt_Ip", "Emios_Mcl_Ip", "Dma_Ip", "Cache_Ip", "FlexCAN_Ip", "C40_Ip",
    "Qspi_Ip", "Eth_43_GMAC", "Eth_43_NETC", "EthSwt_43_NETC", "Fr", "Ocu", "Sent", "Psi5",
]
VECTOR_SIP_MODULE_DIRS_IGNORED = {"Os", "Mcal_S32SZExx", "RteAnalyzer", "_Common", "MSR_Vector_SLP4", "MSR4_Product"}
C_EXT = (".c", ".h")

RX_CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
RX_OS_INCLUDE = re.compile(r'#\s*include\s+"(Os[A-Za-z0-9_]*\.h)"')
RX_ISR_DEF = re.compile(r"\bISR\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
RX_TASK_DEF = re.compile(r"\bTASK\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")


def iter_files(root: Path, exclude_dirs: Iterable[str] = ()) -> Iterable[Path]:
    ex = set(exclude_dirs)
    for dp, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ex]
        for f in files:
            if f.endswith(C_EXT):
                yield Path(dp, f)


def read(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--sip", default=None)
    ap.add_argument("--gen", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args(argv)
    root = Path(args.root)
    sip = Path(args.sip) if args.sip else root / "Sip"
    gen = Path(args.gen) if args.gen else root / "BSw" / "DaVinciWorkSpace" / "Generated_BSW"
    client_dirs = [root / "ASw", root / "BSw" / "StartUpCode", gen]
    vector_bsw = sip / "Components"
    mcal_dir = sip / "ThirdParty"

    osek = Counter()
    vector_os = Counter()
    os_includes = Counter()
    os_include_files: Dict[str, Set[str]] = defaultdict(set)
    mcal = Counter()
    mcal_callers: Dict[str, Set[str]] = defaultdict(set)
    isr_defs: Dict[str, str] = {}
    task_defs: Dict[str, str] = {}
    scanned = 0

    def scan(p: Path, is_vector_bsw: bool) -> None:
        nonlocal scanned
        txt = read(p)
        if not txt:
            return
        scanned += 1
        rel = str(p.relative_to(root)) if root in p.parents else str(p)
        for m in RX_CALL.finditer(txt):
            name = m.group(1)
            if name in OSEK_API:
                osek[name] += 1
            elif name.startswith("Os_") and not name.startswith(("Os_Hal", "Os_Task_", "Os_Isr_", "Os_Trace")):
                vector_os[name] += 1
            elif name.startswith(("IocWrite_", "IocRead_", "IocSend_", "IocReceive_")):
                osek["Ioc*"] += 1
            else:
                for pre in MCAL_PREFIXES:
                    if name.startswith(pre + "_") and name[len(pre) + 1:len(pre) + 2].isupper():
                        if not re.match(r".*_(Ipw|Ip)_", name) or pre.endswith("_Ip"):
                            mcal[name] += 1
                            mcal_callers[pre].add(rel.split(os.sep)[0] if not is_vector_bsw else "VectorBSW")
                        break
        if not p.name.startswith("Os_"):
            for m in RX_OS_INCLUDE.finditer(txt):
                h = m.group(1)
                if "MemMap" in h:
                    continue
                os_includes[h] += 1
                os_include_files[h].add(rel)
        for m in RX_ISR_DEF.finditer(txt):
            isr_defs[m.group(1)] = rel
        for m in RX_TASK_DEF.finditer(txt):
            task_defs[m.group(1)] = rel

    for d in client_dirs:
        if d.exists():
            for p in iter_files(d):
                if p.name.startswith("Os_") and d == gen:
                    continue
                scan(p, False)
    if vector_bsw.exists():
        for mod in vector_bsw.iterdir():
            if mod.name in VECTOR_SIP_MODULE_DIRS_IGNORED or mod.name.startswith(("Lin", "Fee_", "Fls_", "Wdg_30", "Sbc_30", "CanTrcv", "LinTrcv", "EthTrcv")):
                continue
            for p in iter_files(mod):
                scan(p, True)

    # OS configuration summary
    os_cfg: Dict[str, object] = {}
    ecuc_dir = root / "BSw" / "DaVinciWorkSpace" / "Config" / "ECUC"
    os_arxml = sorted(ecuc_dir.glob("*_Os_Os_ecuc.arxml")) if ecuc_dir.exists() else []
    if os_arxml:
        e = Ecuc.load([os_arxml[0]])
        osm = e.module("Os")
        osos = osm.top("OsOS")
        os_cfg = {
            "arxml": str(os_arxml[0]),
            "cores": osos[0].getint("OsNumberOfCores", 1) if osos else 1,
            "status": osos[0].get("OsStatus") if osos else None,
            "tasks": len(osm.top("OsTask")),
            "isrs": len(osm.top("OsIsr")),
            "alarms": len(osm.top("OsAlarm")),
            "counters": len(osm.top("OsCounter")),
            "scheduleTables": len(osm.top("OsScheduleTable")),
            "resources": len(osm.top("OsResource")),
            "spinlocks": len(osm.top("OsSpinlock")),
            "barriers": len(osm.top("OsBarrier")),
            "ioc": len(osm.find("OsIocCommunication")),
            "applications": len(osm.top("OsApplication")),
            "isrSpecialFunctions": sorted({i.get("OsIsrSpecialFunctionName", "") for i in osm.top("OsIsr") if i.get("OsIsrSpecialFunctionName")}),
        }

    # CanIf binding
    canif_binding = {}
    cfg = gen / "CanIf_Cfg.h"
    if cfg.exists():
        for m in re.finditer(r"#\s*define\s+(Can_[A-Za-z]+)\s+(Can_[A-Za-z0-9_]+)", read(cfg)):
            canif_binding[m.group(1)] = m.group(2)

    report = {
        "root": str(root),
        "filesScanned": scanned,
        "osConfig": os_cfg,
        "osekApiUsed": dict(osek.most_common()),
        "vectorOsExtensionsUsed": dict(vector_os.most_common()),
        "osHeadersIncludedByNonOsCode": {h: sorted(f)[:10] for h, f in os_include_files.items()},
        "mcalApiUsed": dict(mcal.most_common()),
        "mcalCallers": {k: sorted(v) for k, v in mcal_callers.items()},
        "isrBodiesDefined": isr_defs,
        "taskBodiesDefined": task_defs,
        "canIfDriverBinding": canif_binding,
    }
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"check_sip: scanned {scanned} files under {root}")
    if os_cfg:
        print(f"OS: {os_cfg['cores']} cores, {os_cfg['status']} status, {os_cfg['tasks']} tasks, {os_cfg['isrs']} ISRs, "
              f"{os_cfg['alarms']} alarms, {os_cfg['scheduleTables']} schedule tables, {os_cfg['resources']} resources, "
              f"{os_cfg['spinlocks']} spinlocks, {os_cfg['barriers']} barriers, {os_cfg['ioc']} IOC channels")
    print("\nOSEK/AUTOSAR OS API used (call sites):")
    print("  " + ", ".join(f"{k} {v}" for k, v in osek.most_common()))
    print("\nVector OS extensions used:")
    print("  " + ", ".join(f"{k} {v}" for k, v in vector_os.most_common(args.top)))
    print("\nOS private headers included by non-OS code:")
    for h, n in os_includes.most_common():
        print(f"  {h} ({n}): {', '.join(sorted(os_include_files[h])[:3])}")
    print("\nMCAL API used per module (call sites):")
    bymod: Dict[str, List[str]] = defaultdict(list)
    for name, n in mcal.most_common():
        for pre in MCAL_PREFIXES:
            if name.startswith(pre + "_"):
                bymod[pre].append(f"{name[len(pre) + 1:]}({n})")
                break
    for pre in sorted(bymod, key=lambda k: -len(bymod[k])):
        print(f"  [{pre}] {len(bymod[pre])} APIs: {' '.join(bymod[pre][:args.top])}")
    print(f"\nISR bodies defined in project code: {len(isr_defs)}; TASK bodies: {len(task_defs)}")
    if canif_binding:
        print("CanIf driver binding: " + ", ".join(f"{k}->{v}" for k, v in canif_binding.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
