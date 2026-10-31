#!/usr/bin/env python3
"""probe_sip_compile.py - M6 feasibility probe: syntax-check real MICROSAR project sources on the host.

Takes the include directories of a reference compile command from the project's
compile_commands.json, drops the OS and MCAL plugin directories, prepends the SIL headers (hal,
SilOs, generated OS configuration) and runs `gcc -fsyntax-only` on a list of project files.
Reports per file the number of errors and the first distinct error messages, so that the
remaining integration gaps (missing MCAL headers, OS private types, pointer casts) are visible
without building the virtual MCAL first.

Usage:
  probe_sip_compile.py <project root> --gen <dir with generated Os_*.h> [--cc gcc] [--files f1 f2 ...]
                       [--extra-inc dir ...] [--out report.md]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, OrderedDict
from pathlib import Path
from typing import Dict, List

SIL_ROOT = Path(__file__).resolve().parents[1]

DEFAULT_FILES = [
    "Sip/Components/Det/Implementation/Det.c",
    "Sip/Components/Crc/Implementation/Crc.c",
    "Sip/Components/VStdLib/Implementation/vstdlib.c",
    "Sip/Components/EcuM/Implementation/EcuM.c",
    "Sip/Components/BswM/Implementation/BswM.c",
    "Sip/Components/Com/Implementation/Com.c",
    "Sip/Components/PduR/Implementation/PduR.c",
    "Sip/Components/CanIf/Implementation/CanIf.c",
    "Sip/Components/CanSM/Implementation/CanSM.c",
    "Sip/Components/CanNm/Implementation/CanNm.c",
    "Sip/Components/Nm/Implementation/Nm.c",
    "Sip/Components/ComM/Implementation/ComM.c",
    "Sip/Components/CanTp/Implementation/CanTp.c",
    "Sip/Components/Dcm/Implementation/Dcm.c",
    "Sip/Components/Dem/Implementation/Dem.c",
    "Sip/Components/NvM/Implementation/NvM.c",
    "Sip/Components/MemIf/Implementation/MemIf.c",
    "Sip/Components/WdgM/Implementation/WdgM.c",
    "Sip/Components/WdgIf/Implementation/WdgIf.c",
    "Sip/Components/LinIf/Implementation/LinIf.c",
    "Sip/Components/LinSM/Implementation/LinSM.c",
    "Sip/Components/IpduM/Implementation/IpduM.c",
    "Sip/Components/Csm/Implementation/Csm.c",
    "Sip/Components/CryIf/Implementation/CryIf.c",
    "Sip/Components/E2E/Implementation/E2E.c",
    "Sip/Components/E2EXf/Implementation/E2EXf.c",
    "Sip/Components/FiM/Implementation/FiM.c",
    "Sip/Components/Xcp/Implementation/Xcp.c",
    "Sip/Components/CanXcp/Implementation/CanXcp.c",
    "Sip/Components/IoHwAb/Implementation/IoHwAb.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/Rte.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/Rte_OsApplication_Trusted_Core1.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/Rte_OsApplication_Trusted_Core0.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/BswM_Lcfg_OsApplication_Trusted_Core0.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/EcuM_Cfg.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/CanIf_Lcfg.c",
    "BSw/DaVinciWorkSpace/Generated_BSW/Com_Lcfg.c",
    "BSw/StartUpCode/src/EcuM_Callout_Stubs.c",
    "BSw/StartUpCode/src/BswM_Callout_Stubs.c",
    "BSw/StartUpCode/src/Os_Callout_Stubs.c",
    "BSw/StartUpCode/src/Det_Externals.c",
    "ASw/SWC/SWC_stackMonitor/src/CtApStackMonitor.c",
    "ASw/CDD/CDD_I2C_Wrapper/src/cdd_i2c_wrapper.c",
    "ASw/CDD/CDD_AIN/src/cdd_ain.c",
    "ASw/CDD/CDD_BTS7222/src/cdd_bts7222.c",
    "ASw/CDD/CDD_Lifetimer/src/CDD_Lifetimer.c",
]

DROP_DIR_PATTERNS = [
    r"[/\\]Components[/\\]Os[/\\]",
    r"[/\\]ThirdParty[/\\]Mcal_",
    r"[/\\]Components[/\\]Mcal_S32SZExx",
    r"[/\\]Generated_BSW[/\\]include$",
    r"[/\\]thirdParty[/\\]ddr",
    r"[/\\]IncludeVtt",
]


def find_ref_command(cc: list, root: Path) -> dict:
    for entry in cc:
        f = entry.get("file", "").replace("\\", "/")
        if f.endswith("/EcuM.c") or f.endswith("Generated_BSW/Rte.c"):
            return entry
    return cc[0]


def include_dirs(entry: dict) -> List[str]:
    cmd = entry.get("command") or " ".join(entry.get("arguments", []))
    return re.findall(r"-I\s*(\S+)", cmd)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--gen", required=True, help="directory with the generated Os_*.h for this project")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--files", nargs="*", default=None)
    ap.add_argument("--extra-inc", nargs="*", default=[])
    ap.add_argument("--defines", nargs="*", default=[], help="extra -D options, e.g. SIL_CPU_TYPE=32")
    ap.add_argument("--out", default=None)
    ap.add_argument("--max-errors", type=int, default=25)
    args = ap.parse_args(argv)
    root = Path(args.root)
    ccjson = root / "build" / "compile_commands.json"
    if not ccjson.exists():
        raise SystemExit(f"{ccjson} not found: build the project once (or pass include dirs via --extra-inc)")
    cc = json.loads(ccjson.read_text(encoding="utf-8"))
    ref = find_ref_command(cc, root)
    incs = [d for d in include_dirs(ref) if not any(re.search(p, d) for p in DROP_DIR_PATTERNS)]
    sil_incs = [str(args.gen), str(SIL_ROOT / "sil/hal"), str(SIL_ROOT / "sil/os"), str(SIL_ROOT / "sil/os/compat"),
                str(SIL_ROOT / "sil/trace"), str(SIL_ROOT / "sil/port")] + list(args.extra_inc)
    defines = ["-DBRS_COMP_GCCNXP", "-DBRS_PLATFORM_ARM", "-DECUNAME=0u", "-DSIL_HOST_BUILD=1"] + [f"-D{d}" for d in args.defines]
    files = args.files or DEFAULT_FILES
    report: List[str] = [f"# SIP compile probe for {root}\n", f"compiler: {args.cc}; {len(incs)} project include dirs + {len(sil_incs)} SIL dirs\n"]
    summary: Dict[str, int] = OrderedDict()
    categories: Counter = Counter()
    for rel in files:
        src = root / rel
        if not src.exists():
            report.append(f"- {rel}: MISSING")
            summary[rel] = -1
            continue
        cmd = [args.cc, "-fsyntax-only", "-std=gnu11", "-fmax-errors=200", "-w", "-Werror=pointer-to-int-cast", "-Werror=int-to-pointer-cast"]
        cmd += defines
        for d in sil_incs:
            cmd += ["-I", d]
        for d in incs:
            cmd += ["-I", d]
        cmd.append(str(src))
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        errs = [ln for ln in r.stderr.splitlines() if re.search(r": (fatal )?error: ", ln)]
        summary[rel] = len(errs)
        distinct = OrderedDict()
        for ln in errs:
            m = re.search(r": (?:fatal )?error: (.*)$", ln)
            msg = m.group(1) if m else ln
            msg = re.sub(r"'[^']*'", lambda mm: mm.group(0), msg)
            key = re.sub(r"\d+", "N", msg)[:120]
            distinct.setdefault(key, ln)
            if "No such file" in msg:
                categories["missing header: " + msg.split(":")[0].strip()] += 1
            elif "unknown type name" in msg:
                categories["unknown type: " + msg.split("'")[1] if "'" in msg else msg] += 1
            elif "implicit declaration" in msg:
                categories["implicit function: " + (msg.split("'")[1] if "'" in msg else msg)] += 1
            else:
                categories[key] += 1
        status = "OK" if r.returncode == 0 else f"{len(errs)} errors"
        report.append(f"\n## {rel}: {status}")
        for i, (k, ln) in enumerate(distinct.items()):
            if i >= args.max_errors:
                report.append(f"  ... {len(distinct) - i} more distinct")
                break
            report.append("  " + ln.strip()[:220])
    report.append("\n## Summary (file: errors, -1 = missing)")
    for k, v in summary.items():
        report.append(f"- {k}: {v}")
    report.append("\n## Most frequent error categories")
    for k, v in categories.most_common(40):
        report.append(f"- {v:4d}  {k}")
    text = "\n".join(report) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    print(text[:20000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
