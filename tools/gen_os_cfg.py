#!/usr/bin/env python3
"""gen_os_cfg.py - generate the SilOs configuration from a MICROSAR/AUTOSAR Os ECUC (.arxml).

Usage:
  gen_os_cfg.py -o <outdir> [options] <Os ecuc arxml> [more arxml, e.g. the EcuC module]

Generated files (all in <outdir>):
  Os_Types_Lcfg.h   object handle enums (TaskType values, ISRType, AlarmType, ...), counts
  Os_Cfg.h          OS_CFG_* switches, event masks, tick conversion macros (includes Os_Hal_Cfg.h)
  Os_Hal_Cfg.h      OS_MS2TICKS_<counter>() and friends
  Os_Lcfg.h         TASK/ISR prototypes
  Os_Lcfg.c         SilOs configuration tables (SilOs_Cfg) and ISR wrappers
  Os_Ioc_Lcfg.h/.c  IOC API (IocWrite_<ch>_<sender>, IocRead_<ch>, IocSend/IocReceive)
  Os_Stack_Cfg.h    OS_CFG_SIZE_<TASK>_STACK macros
  Os_Compiler_Cfg.h OS_<NAME>_CODE section macros (empty)
  Os_Hook_Cfg.h     hook switches

Options:
  --isr-prio-order lower-is-higher|higher-is-higher   meaning of OsIsrInterruptPriority (default lower-is-higher, ARM GIC)
  --isr-bodies NAME[,NAME...]   special function names for which the project defines ISR(NAME) itself (no wrapper)
  --isr-bodies-from DIR         scan DIR recursively for ISR(NAME) definitions to fill --isr-bodies
  --no-isr-wrappers             never generate Os_Isr_<special>() wrappers
  --ioc-init-values             initialise IOC channels from their OsIocInitValue symbols
  --tick-ns N                   override OsSecondsPerTick for all hardware counters (ns)
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ecuc import Container, Ecuc, Module  # noqa: E402

INVALID = "SILOS_INVALID_ID"


def cident(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def trailing_int(s: str, default: int = -1) -> int:
    m = re.search(r"(\d+)$", s)
    return int(m.group(1)) if m else default


class Model:
    def __init__(self, ecuc: Ecuc, os: Module, args: argparse.Namespace) -> None:
        self.ecuc = ecuc
        self.os = os
        self.args = args
        self.cores: Dict[str, int] = {}
        self.numCores = 1
        self.masterCore = 0
        self.appModes: List[str] = []
        self.apps: List[Container] = []
        self.appCore: Dict[str, int] = {}
        self.objApp: Dict[str, str] = {}  # object name -> app name
        self.tasks: List[Container] = []
        self.isrs: List[Container] = []
        self.counters: List[Container] = []
        self.alarms: List[Container] = []
        self.resources: List[Container] = []
        self.spinlocks: List[Container] = []
        self.barriers: List[Container] = []
        self.schedtbls: List[Container] = []
        self.events: List[Container] = []
        self.iocs: List[Container] = []
        self.eventMask: Dict[str, int] = {}
        self.taskIdx: Dict[str, int] = {}
        self.isrIdx: Dict[str, int] = {}
        self.counterIdx: Dict[str, int] = {}
        self.alarmIdx: Dict[str, int] = {}
        self.resIdx: Dict[str, int] = {}
        self.spinIdx: Dict[str, int] = {}
        self.barrierIdx: Dict[str, int] = {}
        self.stIdx: Dict[str, int] = {}
        self.appIdx: Dict[str, int] = {}
        self.modeIdx: Dict[str, int] = {}
        self.isrEntry: Dict[str, str] = {}
        self.isrKind: Dict[str, int] = {}
        self.isrWrappers: List[Tuple[str, str]] = []
        self.isrBodies: Set[str] = set(args.isr_bodies)
        self.build()

    # ------------------------------------------------------------------
    def build(self) -> None:
        os_ = self.os
        osos = os_.top("OsOS")
        self.osos = osos[0] if osos else None
        self.numCores = max(1, self.osos.getint("OsNumberOfCores", 1)) if self.osos else 1
        # cores: OsCore containers (OsCoreId) and EcucCoreDefinition (EcucCoreId) if present
        for c in os_.find("OsCore"):
            self.cores[c.name] = c.getint("OsCoreId", trailing_int(c.name, 0))
        ecucmod = self.ecuc.modules.get("EcuC")
        if ecucmod is not None:
            for c in ecucmod.find("EcucCoreDefinition"):
                self.cores[c.name] = c.getint("EcucCoreId", trailing_int(c.name, 0))
        if self.osos is not None:
            mc = self.osos.ref1("OsMasterCore")
            if mc:
                self.masterCore = self.core_of_ref(mc)
        # app modes
        modes = [c.name for c in os_.find("OsAppMode")]
        if "OSDEFAULTAPPMODE" in modes:
            modes.remove("OSDEFAULTAPPMODE")
            modes.insert(0, "OSDEFAULTAPPMODE")
        elif not modes:
            modes = ["OSDEFAULTAPPMODE"]
        self.appModes = modes
        self.modeIdx = {m: i for i, m in enumerate(modes)}
        # applications
        self.apps = os_.top("OsApplication")
        for i, a in enumerate(self.apps):
            self.appIdx[a.name] = i
            self.appCore[a.name] = self.core_of_ref(a.ref1("OsApplicationCoreRef") or "", default=0)
            for key in ("OsAppTaskRef", "OsAppIsrRef", "OsAppAlarmRef", "OsAppCounterRef", "OsAppScheduleTableRef",
                        "OsAppResourceRef"):
                for name in a.ref(key):
                    self.objApp[name] = a.name
        # objects in file order
        self.tasks = os_.top("OsTask")
        self.isrs = os_.top("OsIsr")
        self.counters = os_.top("OsCounter")
        self.alarms = os_.top("OsAlarm")
        self.resources = os_.top("OsResource")
        self.spinlocks = os_.top("OsSpinlock")
        self.barriers = os_.top("OsBarrier")
        self.schedtbls = os_.top("OsScheduleTable")
        self.events = os_.top("OsEvent")
        ioc = os_.top("OsIoc")
        self.iocs = ioc[0].sub("OsIocCommunication") if ioc else []
        self.taskIdx = {t.name: i for i, t in enumerate(self.tasks)}
        self.isrIdx = {t.name: i for i, t in enumerate(self.isrs)}
        self.counterIdx = {t.name: i for i, t in enumerate(self.counters)}
        self.alarmIdx = {t.name: i for i, t in enumerate(self.alarms)}
        self.resIdx = {t.name: i for i, t in enumerate(self.resources)}
        self.spinIdx = {t.name: i for i, t in enumerate(self.spinlocks)}
        self.barrierIdx = {t.name: i for i, t in enumerate(self.barriers)}
        self.stIdx = {t.name: i for i, t in enumerate(self.schedtbls)}
        self.assign_event_masks()
        self.classify_isrs()

    def core_of_ref(self, name: str, default: int = 0) -> int:
        if name in self.cores:
            return self.cores[name]
        t = trailing_int(name, -1)
        return t if t >= 0 else default

    def obj_core(self, name: str, default: int = 0) -> int:
        app = self.objApp.get(name)
        if app is not None:
            return self.appCore.get(app, default)
        return default

    def obj_app(self, name: str) -> str:
        app = self.objApp.get(name)
        return str(self.appIdx[app]) if app in self.appIdx else "0xFFFFu"

    # ------------------------------------------------------------------
    def assign_event_masks(self) -> None:
        """Explicit OsEventMask values are kept; AUTO masks get the lowest bit that is free in
        every task referencing the event (greedy colouring)."""
        taskEvents: Dict[str, List[str]] = {t.name: t.ref("OsTaskEventRef") for t in self.tasks}
        eventTasks: Dict[str, List[str]] = {}
        for tname, evs in taskEvents.items():
            for e in evs:
                eventTasks.setdefault(e, []).append(tname)
        explicit: Dict[str, int] = {}
        for e in self.events:
            m = e.get("OsEventMask", "AUTO")
            if m and m.upper() != "AUTO":
                try:
                    explicit[e.name] = int(m, 0)
                except ValueError:
                    pass
        order = [e.name for e in self.events]
        for tname, evs in taskEvents.items():
            for e in evs:
                if e not in order:
                    order.append(e)
        for name in order:
            if name in explicit:
                self.eventMask[name] = explicit[name]
        for name in order:
            if name in self.eventMask:
                continue
            used = 0
            for tname in eventTasks.get(name, []):
                for other in taskEvents[tname]:
                    if other != name and other in self.eventMask:
                        used |= self.eventMask[other]
            bit = 0
            while bit < 64 and (used >> bit) & 1:
                bit += 1
            if bit >= 64:
                raise SystemExit(f"event {name}: no free bit in a 64 bit mask")
            self.eventMask[name] = 1 << bit

    def classify_isrs(self) -> None:
        timerIsrs: Set[str] = set()
        for c in self.counters:
            drv = c.sub1("OsDriver")
            if drv is not None:
                for r in drv.ref("OsDriverIsrRef"):
                    timerIsrs.add(r)
        for isr in self.isrs:
            special = isr.get("OsIsrSpecialFunctionName", "")
            useSpecial = isr.getbool("OsIsrUseSpecialFunction", bool(special))
            if not useSpecial:
                special = ""
            kind = 0
            if isr.name in timerIsrs or special.startswith("Os_Timer"):
                kind = 1
            elif special.startswith("Os_XSig") or isr.name.startswith("XSignalIsr"):
                kind = 2
            self.isrKind[isr.name] = kind
            if kind != 0:
                self.isrEntry[isr.name] = "NULL"
            elif special:
                self.isrEntry[isr.name] = f"Os_Isr_{cident(special)}"
                if special not in self.isrBodies and not self.args.no_isr_wrappers:
                    self.isrWrappers.append((special, f"Os_Isr_{cident(special)}"))
            else:
                self.isrEntry[isr.name] = f"Os_Isr_{cident(isr.name)}"

    def isr_urgency(self, isr: Container) -> int:
        p = isr.getint("OsIsrInterruptPriority", 0)
        if self.args.isr_prio_order == "lower-is-higher":
            return 0xFFFF - p if p <= 0xFFFF else 0
        return p

    def task_is_idle(self, t: Container) -> bool:
        return t.getint("OsTaskPriority", 0) >= 0xFFFFFFFF or t.name.startswith("IdleTask")

    def task_extended(self, t: Container) -> bool:
        tt = (t.get("OsTaskType", "AUTO") or "AUTO").upper()
        if tt == "EXTENDED":
            return True
        if tt == "BASIC":
            return False
        return len(t.ref("OsTaskEventRef")) > 0

    def autostart_modes(self, refs: List[str]) -> str:
        bits = 0
        for m in refs:
            if m == "OS_APPMODE_ANY":
                bits = 0xFFFFFFFF
            elif m in self.modeIdx:
                bits |= 1 << self.modeIdx[m]
        return f"0x{bits:08X}u"

    def counter_core(self, c: Container) -> int:
        core = self.obj_core(c.name, -1)
        if core >= 0:
            return core
        drv = c.sub1("OsDriver")
        if drv is not None:
            for r in drv.ref("OsDriverIsrRef"):
                return self.obj_core(r, 0)
        return 0

    def counter_ns_per_tick(self, c: Container) -> int:
        if self.args.tick_ns:
            return int(self.args.tick_ns)
        spt = c.getfloat("OsSecondsPerTick", 0.001)
        return max(1, int(round(spt * 1e9)))


# ----------------------------------------------------------------------
# emitters
HEADER = "/* Generated by microsarSIL tools/gen_os_cfg.py from {src} - do not edit. */\n"


def emit_types_lcfg(m: Model, src: str) -> str:
    o: List[str] = [HEADER.format(src=src), "#ifndef OS_TYPES_LCFG_H", "#define OS_TYPES_LCFG_H", "", '#include "Os_Types.h"', ""]

    def enum(typename: str, names: List[str], countname: str, invalid: Optional[str] = None) -> None:
        o.append(f"typedef enum {{")
        for i, n in enumerate(names):
            o.append(f"  {cident(n)} = {i},")
        o.append(f"  {countname} = {len(names)}" + ("," if invalid else ""))
        if invalid:
            o.append(f"  {invalid} = {countname}")
        o.append(f"}} {typename};")
        for n in names:
            o.append(f"#define {cident(n)} {cident(n)}")
        o.append("")

    enum("Os_ApplicationIdEnum", [a.name for a in m.apps], "OS_APPID_COUNT")
    enum("Os_TaskIdEnum", [t.name for t in m.tasks], "OS_TASKID_COUNT", "INVALID_TASK")
    enum("Os_IsrIdEnum", [i.name for i in m.isrs], "OS_ISRID_COUNT", "INVALID_ISR")
    enum("Os_AlarmIdEnum", [a.name for a in m.alarms], "OS_ALARMID_COUNT")
    enum("Os_CounterIdEnum", [c.name for c in m.counters], "OS_COUNTERID_COUNT")
    enum("Os_ScheduleTableIdEnum", [s.name for s in m.schedtbls], "OS_SCHEDULETABLEID_COUNT")
    enum("Os_ResourceIdEnum", [r.name for r in m.resources], "OS_RESOURCEID_COUNT")
    enum("Os_SpinlockIdEnum", [s.name for s in m.spinlocks], "OS_SPINLOCKID_COUNT")
    enum("Os_BarrierIdEnum", [b.name for b in m.barriers], "OS_BARRIERID_COUNT")
    enum("Os_AppModeIdEnum", m.appModes, "OS_APPMODE_COUNT")
    o.append("typedef enum {")
    for c in range(m.numCores):
        o.append(f"  OS_CORE_ID_{c} = {c},")
    o.append(f"  OS_CORE_ID_COUNT = {m.numCores}")
    o.append("} Os_CoreIdEnum;")
    for c in range(m.numCores):
        o.append(f"#define OS_CORE_ID_{c} OS_CORE_ID_{c}")
    o.append("")
    o.append("typedef enum { OS_PERIPHERALID_COUNT = 0 } Os_PeripheralIdEnum;")
    o.append("typedef enum { OS_TRUSTEDFUNCTIONID_COUNT = 0 } TrustedFunctionIndexEnum;")
    o.append("typedef uint32 TimerISR1Type;")
    o.append("")
    o.append("#endif /* OS_TYPES_LCFG_H */")
    return "\n".join(o) + "\n"


def emit_cfg_h(m: Model, src: str) -> str:
    osos = m.osos
    hooks = osos.sub1("OsHooks") if osos is not None else None

    def hk(name: str) -> str:
        return "STD_ON" if (hooks is not None and hooks.getbool(name)) else "STD_OFF"

    status = (osos.get("OsStatus", "EXTENDED") if osos else "EXTENDED").upper()
    o = [HEADER.format(src=src), "#ifndef OS_CFG_H", "#define OS_CFG_H", "", '#include "Os_Types.h"', '#include "Os_Hal_Cfg.h"', ""]
    o += [
        "#define OS_CFG_MAJOR_VERSION (4u)",
        "#define OS_CFG_MINOR_VERSION (8u)",
        "#define OS_CFG_PLATFORM_SIL (STD_ON)",
        f"#define OS_CFG_COREPHYSICALID_COUNT ({m.numCores}u)",
        f"#define OS_CFG_NUM_CORES ({m.numCores}u)",
        f"#define OS_CFG_MASTER_CORE_ID ({m.masterCore}u)",
        f"#define OS_CFG_EXTENDED_STATUS ({'STD_ON' if status == 'EXTENDED' else 'STD_OFF'})",
        f"#define OS_CFG_STARTUPHOOK_SYSTEM ({hk('OsStartupHook')})",
        f"#define OS_CFG_SHUTDOWNHOOK_SYSTEM ({hk('OsShutdownHook')})",
        f"#define OS_CFG_ERRORHOOK_SYSTEM ({hk('OsErrorHook')})",
        f"#define OS_CFG_PROTECTIONHOOK_SYSTEM ({hk('OsProtectionHook')})",
        f"#define OS_CFG_PRETASKHOOK ({hk('OsPreTaskHook')})",
        f"#define OS_CFG_POSTTASKHOOK ({hk('OsPostTaskHook')})",
        f"#define OS_CFG_STARTUPHOOKS ({hk('OsStartupHook')})",
        f"#define OS_CFG_SHUTDOWNHOOKS ({hk('OsShutdownHook')})",
        f"#define OS_CFG_ERRORHOOKS ({hk('OsErrorHook')})",
        f"#define OS_CFG_PROTECTIONHOOK ({hk('OsProtectionHook')})",
        "#define OS_CFG_PANICHOOK (STD_OFF)",
        f"#define OS_CFG_ERR_GETSERVICEID ({'STD_ON' if (osos and osos.getbool('OsUseGetServiceId')) else 'STD_OFF'})",
        f"#define OS_CFG_ERR_PARAMETERACCESS ({'STD_ON' if (osos and osos.getbool('OsUseParameterAccess')) else 'STD_OFF'})",
        f"#define OS_CFG_STACKMONITORING ({'STD_ON' if (osos and osos.getbool('OsStackMonitoring')) else 'STD_OFF'})",
        f"#define OS_CFG_STACKMEASUREMENT ({'STD_ON' if (osos and osos.getbool('OsStackUsageMeasurement')) else 'STD_OFF'})",
        f"#define OS_CFG_EVENT ({'STD_ON' if m.events or any(m.task_extended(t) for t in m.tasks) else 'STD_OFF'})",
        f"#define OS_CFG_XSIGNAL ({'STD_ON' if m.numCores > 1 else 'STD_OFF'})",
        f"#define OS_CFG_IOC ({'STD_ON' if m.iocs else 'STD_OFF'})",
        f"#define OS_CFG_BARRIER ({'STD_ON' if m.barriers else 'STD_OFF'})",
        f"#define OS_CFG_NUM_SYSTEM_SPINLOCKS ({len(m.spinlocks)}u)",
        "#define OS_CFG_MEMORY_PROTECTION (STD_OFF)",
        "#define OS_CFG_TIMING_PROTECTION (STD_OFF)",
        "#define OS_CFG_SERVICE_PROTECTION (STD_OFF)",
        "#define OS_CFG_INTERRUPT_ONLY (STD_OFF)",
        "#define OS_CFG_ORTI (STD_OFF)",
        "#define OS_CFG_ARTI (STD_OFF)",
        "#define OS_CFG_OSTIMINGHOOKS (STD_OFF)",
        "",
        "/* event masks */",
    ]
    for name, mask in m.eventMask.items():
        o.append(f"#define {cident(name)} ((EventMaskType){mask}uLL)")
    o += ["", "#endif /* OS_CFG_H */"]
    return "\n".join(o) + "\n"


def emit_hal_cfg_h(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), "#ifndef OS_HAL_CFG_H", "#define OS_HAL_CFG_H", "", '#include "Os_Types.h"', "",
         "#define OS_CFG_HAL_MAJOR_VERSION (4u)", "#define OS_CFG_HAL_MINOR_VERSION (5u)", ""]
    for c in m.counters:
        n = cident(c.name)
        nspt = m.counter_ns_per_tick(c)
        o.append(f"/* {c.name}: {nspt} ns per tick */")
        o.append(f"#define OS_NS2TICKS_{n}(x) ((TickType)(((uint64)(x)) / {nspt}uLL))")
        o.append(f"#define OS_US2TICKS_{n}(x) ((TickType)((((uint64)(x)) * 1000uLL) / {nspt}uLL))")
        o.append(f"#define OS_MS2TICKS_{n}(x) ((TickType)((((uint64)(x)) * 1000000uLL) / {nspt}uLL))")
        o.append(f"#define OS_SEC2TICKS_{n}(x) ((TickType)((((uint64)(x)) * 1000000000uLL) / {nspt}uLL))")
        o.append(f"#define OS_TICKS2NS_{n}(x) ((uint64)(x) * {nspt}uLL)")
        o.append(f"#define OS_TICKS2US_{n}(x) (((uint64)(x) * {nspt}uLL) / 1000uLL)")
        o.append(f"#define OS_TICKS2MS_{n}(x) (((uint64)(x) * {nspt}uLL) / 1000000uLL)")
        o.append(f"#define OS_TICKS2SEC_{n}(x) (((uint64)(x) * {nspt}uLL) / 1000000000uLL)")
        o.append("")
    o += ["#endif /* OS_HAL_CFG_H */"]
    return "\n".join(o) + "\n"


def emit_lcfg_h(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), "#ifndef OS_LCFG_H", "#define OS_LCFG_H", "", '#include "Os_Types.h"', "",
         "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for t in m.tasks:
        if not m.task_is_idle(t):
            o.append(f"extern void Os_Task_{cident(t.name)}(void);")
    o.append("")
    seen: Set[str] = set()
    for isr in m.isrs:
        e = m.isrEntry[isr.name]
        if e != "NULL" and e not in seen:
            seen.add(e)
            o.append(f"extern void {e}(void);")
    o.append("")
    for a in m.alarms:
        cb = a.sub1("OsAlarmAction")
        cbn = cb.sub1("OsAlarmCallback") if cb else None
        if cbn is not None:
            o.append(f"extern void {cident(cbn.get('OsAlarmCallbackName', a.name + '_Callback') or '')}(void);")
    o += ["", "#ifdef __cplusplus", "}", "#endif", "", "#endif /* OS_LCFG_H */"]
    return "\n".join(o) + "\n"


def emit_lcfg_c(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), '#include "Os.h"', '#include "SilOs_Cfg.h"', ""]
    # ISR wrappers for driver handlers referenced as special functions
    if m.isrWrappers:
        o.append("/* ISR wrappers: the OS calls Os_Isr_<fn>(), the driver provides <fn>() */")
        for special, wrapper in m.isrWrappers:
            o.append(f"extern void {cident(special)}(void);")
            o.append(f"void {wrapper}(void) {{ {cident(special)}(); }}")
        o.append("")
    # app modes
    o.append("static const char *const osAppModeNames[] = {" + ", ".join(f'"{n}"' for n in m.appModes) + "};")
    o.append("")
    # apps
    o.append("static const SilOs_AppCfg osApps[] = {")
    for a in m.apps:
        o.append(f'  {{ "{a.name}", {m.appCore[a.name]}u, {1 if a.getbool("OsTrusted", True) else 0}u }},')
    if not m.apps:
        o.append('  { "NoApp", 0u, 1u },')
    o.append("};")
    o.append("")
    # tasks
    o.append("static const SilOs_TaskCfg osTasks[] = {")
    for t in m.tasks:
        idle = m.task_is_idle(t)
        prio = 0 if idle else t.getint("OsTaskPriority", 0)
        sched = "SILOS_SCHED_NON" if (t.get("OsTaskSchedule", "FULL") or "FULL").upper() == "NON" else "SILOS_SCHED_FULL"
        ext = 1 if m.task_extended(t) else 0
        act = max(1, t.getint("OsTaskActivation", 1))
        auto = t.sub1("OsTaskAutostart")
        modes = m.autostart_modes(auto.ref("OsTaskAppModeRef")) if auto is not None else "0x00000000u"
        ires = INVALID
        for r in t.ref("OsTaskResourceRef"):
            rc = m.os.byname(r)
            if rc is not None and (rc.get("OsResourceProperty", "STANDARD") or "").upper() == "INTERNAL" and r in m.resIdx:
                ires = f"{m.resIdx[r]}u"
        stack = t.getint("OsTaskStackSize", 1024)
        owned = 0
        for e in t.ref("OsTaskEventRef"):
            owned |= m.eventMask.get(e, 0)
        entry = "NULL" if idle else f"Os_Task_{cident(t.name)}"
        app = m.obj_app(t.name)
        core = m.obj_core(t.name, 0)
        o.append(f'  {{ "{t.name}", {entry}, {prio}u, {core}u, {app}, {sched}, {ext}u, {1 if idle else 0}u, {act}u, {modes}, {ires}, {stack}u, {owned}uLL }},')
    if not m.tasks:
        o.append('  { "none", NULL, 0u, 0u, 0xFFFFu, SILOS_SCHED_FULL, 0u, 1u, 1u, 0u, SILOS_INVALID_ID, 0u, 0uLL },')
    o.append("};")
    o.append("")
    # isrs
    o.append("static const SilOs_IsrCfg osIsrs[] = {")
    for isr in m.isrs:
        cat = 1 if (isr.get("OsIsrCategory", "CATEGORY_2") or "").upper().endswith("1") else 2
        nesting = 1 if isr.getbool("OsIsrEnableNesting", False) else 0
        ien = 1 if isr.getbool("OsIsrInitialEnableInterruptSource", True) else 0
        kind = m.isrKind[isr.name]
        if kind != 0:
            ien = 1
        src_ = isr.getint("OsIsrInterruptSource", 0)
        o.append(f'  {{ "{isr.name}", {m.isrEntry[isr.name]}, {m.obj_core(isr.name, 0)}u, {m.obj_app(isr.name)}, {m.isr_urgency(isr)}u, {cat}u, {nesting}u, {ien}u, {kind}u, {src_}u }},')
    if not m.isrs:
        o.append('  { "none", NULL, 0u, 0xFFFFu, 0u, 2u, 0u, 0u, 0u, 0u },')
    o.append("};")
    o.append("")
    # counters
    o.append("static const SilOs_CounterCfg osCounters[] = {")
    for c in m.counters:
        hw = 1 if (c.get("OsCounterType", "HARDWARE") or "").upper() == "HARDWARE" else 0
        drv = c.sub1("OsDriver")
        tisr = INVALID
        if drv is not None:
            r = drv.ref1("OsDriverIsrRef")
            if r in m.isrIdx:
                tisr = f"{m.isrIdx[r]}u"
        o.append(f'  {{ "{c.name}", {m.counter_core(c)}u, {m.obj_app(c.name)}, {c.getint("OsCounterMaxAllowedValue", 0xFFFF)}u, '
                 f'{c.getint("OsCounterTicksPerBase", 1)}u, {max(1, c.getint("OsCounterMinCycle", 1))}u, {m.counter_ns_per_tick(c)}uLL, {hw}u, {tisr} }},')
    if not m.counters:
        o.append('  { "none", 0u, 0xFFFFu, 0xFFFFu, 1u, 1u, 1000000uLL, 1u, SILOS_INVALID_ID },')
    o.append("};")
    o.append("")
    # alarms
    o.append("static const SilOs_AlarmCfg osAlarms[] = {")
    for a in m.alarms:
        cnt = a.ref1("OsAlarmCounterRef")
        cidx = m.counterIdx.get(cnt or "", 0)
        action = a.sub1("OsAlarmAction")
        act = "SILOS_ALARM_ACTIVATETASK"
        task = INVALID
        mask = "0uLL"
        inc = INVALID
        cb = "NULL"
        if action is not None:
            at = action.sub1("OsAlarmActivateTask")
            se = action.sub1("OsAlarmSetEvent")
            cbk = action.sub1("OsAlarmCallback")
            ic = action.sub1("OsAlarmIncrementCounter")
            if at is not None:
                task = f"{m.taskIdx.get(at.ref1('OsAlarmActivateTaskRef') or '', 0)}u"
            elif se is not None:
                act = "SILOS_ALARM_SETEVENT"
                task = f"{m.taskIdx.get(se.ref1('OsAlarmSetEventTaskRef') or '', 0)}u"
                mask = f"{m.eventMask.get(se.ref1('OsAlarmSetEventRef') or '', 0)}uLL"
            elif cbk is not None:
                act = "SILOS_ALARM_CALLBACK"
                cb = cident(cbk.get("OsAlarmCallbackName", a.name + "_Callback") or "")
            elif ic is not None:
                act = "SILOS_ALARM_INCREMENTCOUNTER"
                inc = f"{m.counterIdx.get(ic.ref1('OsAlarmIncrementCounterRef') or '', 0)}u"
        auto = a.sub1("OsAlarmAutostart")
        akind = "SILOS_AUTOSTART_NONE"
        amodes = "0x00000000u"
        start = 0
        cycle = 0
        if auto is not None:
            akind = "SILOS_AUTOSTART_ABSOLUTE" if (auto.get("OsAlarmAutostartType", "RELATIVE") or "").upper() == "ABSOLUTE" else "SILOS_AUTOSTART_RELATIVE"
            amodes = m.autostart_modes(auto.ref("OsAlarmAppModeRef"))
            start = auto.getint("OsAlarmAlarmTime", 0)
            cycle = auto.getint("OsAlarmCycleTime", 0)
        o.append(f'  {{ "{a.name}", {cidx}u, {act}, {akind}, {m.obj_app(a.name)}, {amodes}, {task}, {mask}, {inc}, {cb}, {start}u, {cycle}u }},')
    if not m.alarms:
        o.append('  { "none", 0u, SILOS_ALARM_ACTIVATETASK, SILOS_AUTOSTART_NONE, 0xFFFFu, 0u, SILOS_INVALID_ID, 0uLL, SILOS_INVALID_ID, NULL, 0u, 0u },')
    o.append("};")
    o.append("")
    # resources
    o.append("static const SilOs_ResourceCfg osResources[] = {")
    for r in m.resources:
        ceiling = 0
        core = 0
        for t in m.tasks:
            if r.name in t.ref("OsTaskResourceRef"):
                ceiling = max(ceiling, t.getint("OsTaskPriority", 0) if not m.task_is_idle(t) else 0)
                core = m.obj_core(t.name, 0)
        isrShared = 0
        for isr in m.isrs:
            if r.name in isr.ref("OsIsrResourceRef"):
                isrShared = 1
        internal = 1 if (r.get("OsResourceProperty", "STANDARD") or "").upper() == "INTERNAL" else 0
        if r.name == "RES_SCHEDULER":
            ceiling = 0xFFFFFFF0 - 2
        o.append(f'  {{ "{r.name}", {ceiling}u, {core}u, {isrShared}u, {internal}u }},')
    if not m.resources:
        o.append('  { "none", 0u, 0u, 0u, 0u },')
    o.append("};")
    o.append("")
    # spinlocks
    o.append("static const SilOs_SpinlockCfg osSpinlocks[] = {")
    methods = {"LOCK_ALL_INTERRUPTS": "SILOS_LOCK_ALL_INTERRUPTS", "LOCK_CAT2_INTERRUPTS": "SILOS_LOCK_CAT2_INTERRUPTS",
               "LOCK_WITH_RES_SCHEDULER": "SILOS_LOCK_WITH_RES_SCHEDULER", "LOCK_NOTHING": "SILOS_LOCK_NOTHING"}
    for s in m.spinlocks:
        meth = methods.get((s.get("OsSpinlockLockMethod", "LOCK_NOTHING") or "").upper(), "SILOS_LOCK_NOTHING")
        succ = s.ref1("OsSpinlockSuccessor")
        succ_s = f"{m.spinIdx[succ]}u" if succ in m.spinIdx else INVALID
        o.append(f'  {{ "{s.name}", {meth}, {succ_s} }},')
    if not m.spinlocks:
        o.append('  { "none", SILOS_LOCK_NOTHING, SILOS_INVALID_ID },')
    o.append("};")
    o.append("")
    # barriers
    o.append("static const SilOs_BarrierCfg osBarriers[] = {")
    for b in m.barriers:
        o.append(f'  {{ "{b.name}", {max(1, len(b.ref("OsBarrierTaskRef")))}u }},')
    if not m.barriers:
        o.append('  { "none", 1u },')
    o.append("};")
    o.append("")
    # schedule tables
    for s in m.schedtbls:
        n = cident(s.name)
        points = sorted(s.sub("OsScheduleTableExpiryPoint"), key=lambda p: p.getint("OsScheduleTblExpPointOffset", 0))
        for i, p in enumerate(points):
            tasks = [m.taskIdx[a.ref1("OsScheduleTableActivateTaskRef") or ""] for a in p.sub("OsScheduleTableTaskActivation")
                     if (a.ref1("OsScheduleTableActivateTaskRef") or "") in m.taskIdx]
            evs = p.sub("OsScheduleTableEventSetting")
            etasks = [m.taskIdx.get(e.ref1("OsScheduleTableSetEventTaskRef") or "", 0) for e in evs]
            emasks = [m.eventMask.get(e.ref1("OsScheduleTableSetEventRef") or "", 0) for e in evs]
            o.append(f"static const uint32 st_{n}_ep{i}_tasks[] = {{{', '.join(f'{t}u' for t in tasks) or '0u'}}};")
            o.append(f"static const uint32 st_{n}_ep{i}_etasks[] = {{{', '.join(f'{t}u' for t in etasks) or '0u'}}};")
            o.append(f"static const EventMaskType st_{n}_ep{i}_emasks[] = {{{', '.join(f'{t}uLL' for t in emasks) or '0uLL'}}};")
        o.append(f"static const SilOs_ExpiryPointCfg st_{n}_points[] = {{")
        for i, p in enumerate(points):
            nt = len([a for a in p.sub("OsScheduleTableTaskActivation")])
            ne = len(p.sub("OsScheduleTableEventSetting"))
            o.append(f"  {{ {p.getint('OsScheduleTblExpPointOffset', 0)}u, {nt}u, st_{n}_ep{i}_tasks, {ne}u, st_{n}_ep{i}_etasks, st_{n}_ep{i}_emasks }},")
        if not points:
            o.append("  { 0u, 0u, NULL, 0u, NULL, NULL },")
        o.append("};")
    o.append("static const SilOs_SchedTblCfg osSchedTbls[] = {")
    for s in m.schedtbls:
        n = cident(s.name)
        cnt = m.counterIdx.get(s.ref1("OsScheduleTableCounterRef") or "", 0)
        auto = s.sub1("OsScheduleTableAutostart")
        akind = "SILOS_AUTOSTART_NONE"
        aoff = 0
        amodes = "0x00000000u"
        if auto is not None:
            t_ = (auto.get("OsScheduleTableAutostartType", "RELATIVE") or "").upper()
            akind = {"ABSOLUTE": "SILOS_AUTOSTART_ABSOLUTE", "SYNCHRON": "SILOS_AUTOSTART_SYNCHRON"}.get(t_, "SILOS_AUTOSTART_RELATIVE")
            aoff = auto.getint("OsScheduleTableStartValue", 0)
            amodes = m.autostart_modes(auto.ref("OsScheduleTableAppModeRef"))
        npts = len(s.sub("OsScheduleTableExpiryPoint"))
        o.append(f'  {{ "{s.name}", {cnt}u, {s.getint("OsScheduleTableDuration", 1)}u, {1 if s.getbool("OsScheduleTableRepeating") else 0}u, '
                 f'{akind}, {m.obj_app(s.name)}, {aoff}u, {amodes}, {npts}u, st_{n}_points }},')
    if not m.schedtbls:
        o.append('  { "none", 0u, 1u, 0u, SILOS_AUTOSTART_NONE, 0xFFFFu, 0u, 0u, 0u, NULL },')
    o.append("};")
    o.append("")
    # hooks
    osos = m.osos
    hooks = osos.sub1("OsHooks") if osos is not None else None

    def hk(name: str, fn: str) -> str:
        return fn if (hooks is not None and hooks.getbool(name)) else "NULL"

    o.append("const SilOs_Config SilOs_Cfg = {")
    o.append(f"  {m.numCores}u, {m.masterCore}u, {1 if (osos is None or (osos.get('OsStatus', 'EXTENDED') or '').upper() == 'EXTENDED') else 0}u, {len(m.appModes)}u, osAppModeNames,")
    o.append(f"  {len(m.tasks)}u, osTasks,")
    o.append(f"  {len(m.isrs)}u, osIsrs,")
    o.append(f"  {len(m.counters)}u, osCounters,")
    o.append(f"  {len(m.alarms)}u, osAlarms,")
    o.append(f"  {len(m.resources)}u, osResources,")
    o.append(f"  {len(m.spinlocks)}u, osSpinlocks,")
    o.append(f"  {len(m.barriers)}u, osBarriers,")
    o.append(f"  {max(1, len(m.apps)) if m.apps else 0}u, osApps,")
    o.append(f"  {len(m.schedtbls)}u, osSchedTbls,")
    o.append(f"  {{ {hk('OsStartupHook', 'StartupHook')}, {hk('OsShutdownHook', 'ShutdownHook')}, {hk('OsErrorHook', 'ErrorHook')}, "
             f"{hk('OsPreTaskHook', 'PreTaskHook')}, {hk('OsPostTaskHook', 'PostTaskHook')}, {hk('OsProtectionHook', 'ProtectionHook')}, NULL }}")
    o.append("};")
    return "\n".join(o) + "\n"


def ioc_type(ch: Container) -> Tuple[str, bool]:
    dp = ch.sub("OsIocDataProperties")
    if not dp:
        return "uint8", True
    d = dp[0]
    t = d.get("OsIocDataType") or d.ref1("OsIocDataTypeRef") or "uint8"
    return cident(t), d.getbool("OsIocDataTypeIsAtomic", True)


def emit_ioc_h(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), "#ifndef OS_IOC_LCFG_H", "#define OS_IOC_LCFG_H", "", '#include "Os_Types.h"', ""]
    incl = None
    ioc = m.os.top("OsIoc")
    if ioc:
        incl = ioc[0].get("OsIocIncludeHeader")
    if incl:
        o.append(f'#include "{incl}"')
    o += ["", "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for ch in m.iocs:
        t, _ = ioc_type(ch)
        n = cident(ch.name)
        queued = ch.getint("OsIocBufferLength", 0) > 0
        senders = ch.sub("OsIocSenderProperties")
        for s in senders:
            sid = s.getint("OsIocSenderId", 0)
            if queued:
                o.append(f"Std_ReturnType IocSend_{n}_{sid}({t} data);")
            else:
                o.append(f"Std_ReturnType IocWrite_{n}_{sid}({t} data);")
        if not senders:
            o.append(f"Std_ReturnType {'IocSend' if queued else 'IocWrite'}_{n}({t} data);")
        if queued:
            o.append(f"Std_ReturnType IocReceive_{n}({t} *data);")
            o.append(f"Std_ReturnType IocEmptyQueue_{n}(void);")
        else:
            o.append(f"Std_ReturnType IocRead_{n}({t} *data);")
        o.append(f"void Os_IocInit_{n}(void);")
    o += ["", "#ifdef __cplusplus", "}", "#endif", "", "#endif /* OS_IOC_LCFG_H */"]
    return "\n".join(o) + "\n"


def emit_ioc_c(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), '#include "Os.h"', '#include "Os_Ioc_Lcfg.h"', '#include "SilOs_Ioc.h"', ""]
    for ch in m.iocs:
        t, _ = ioc_type(ch)
        n = cident(ch.name)
        qlen = ch.getint("OsIocBufferLength", 0)
        slots = max(1, qlen)
        o.append(f"static {t} ioc_{n}_buf[{slots}];")
        o.append(f"static SilOs_IocChannel ioc_{n} = {{ ioc_{n}_buf, sizeof({t}), {slots}u, 0u, 0u, 0u, 0u }};")
        senders = ch.sub("OsIocSenderProperties")
        for s in senders:
            sid = s.getint("OsIocSenderId", 0)
            if qlen > 0:
                o.append(f"Std_ReturnType IocSend_{n}_{sid}({t} data) {{ return SilOs_IocSendQueued(&ioc_{n}, &data); }}")
            else:
                o.append(f"Std_ReturnType IocWrite_{n}_{sid}({t} data) {{ return SilOs_IocWriteUnqueued(&ioc_{n}, &data); }}")
        if not senders:
            if qlen > 0:
                o.append(f"Std_ReturnType IocSend_{n}({t} data) {{ return SilOs_IocSendQueued(&ioc_{n}, &data); }}")
            else:
                o.append(f"Std_ReturnType IocWrite_{n}({t} data) {{ return SilOs_IocWriteUnqueued(&ioc_{n}, &data); }}")
        if qlen > 0:
            o.append(f"Std_ReturnType IocReceive_{n}({t} *data) {{ return SilOs_IocReceiveQueued(&ioc_{n}, data); }}")
            o.append(f"Std_ReturnType IocEmptyQueue_{n}(void) {{ return SilOs_IocEmptyQueue(&ioc_{n}); }}")
        else:
            o.append(f"Std_ReturnType IocRead_{n}({t} *data) {{ return SilOs_IocReadUnqueued(&ioc_{n}, data); }}")
        dp = ch.sub1("OsIocDataProperties")
        init = dp.get("OsIocInitValue") if dp is not None else None
        if m.args.ioc_init_values and init:
            o.append(f"void Os_IocInit_{n}(void) {{ {t} v = {init}; SilOs_IocInit(&ioc_{n}, &v); }}")
        else:
            o.append(f"void Os_IocInit_{n}(void) {{ SilOs_IocInit(&ioc_{n}, NULL); }}")
        o.append("")
    return "\n".join(o) + "\n"


def emit_stack_cfg_h(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), "#ifndef OS_STACK_CFG_H", "#define OS_STACK_CFG_H", "", '#include "Std_Types.h"', "",
         "#define OS_CFG_STACKMONITORING (STD_ON)", "#define OS_CFG_STACKMEASUREMENT (STD_ON)", ""]
    for t in m.tasks:
        o.append(f"#define OS_CFG_SIZE_{cident(t.name).upper()}_STACK ({t.getint('OsTaskStackSize', 1024)}u)")
    for isr in m.isrs:
        o.append(f"#define OS_CFG_SIZE_{cident(isr.name).upper()}_STACK ({isr.getint('OsIsrStackSize', 1024)}u)")
    for c in range(m.numCores):
        for kind in ("INIT", "ISR_CORE", "KERNEL", "SHUTDOWN"):
            o.append(f"#define OS_CFG_SIZE_OSCORE{c}_{kind}_STACK (1024u)")
    o += ["", "#endif /* OS_STACK_CFG_H */"]
    return "\n".join(o) + "\n"


def emit_compiler_cfg_h(m: Model, src: str) -> str:
    o = [HEADER.format(src=src), "#ifndef OS_COMPILER_CFG_H", "#define OS_COMPILER_CFG_H", ""]
    names: List[str] = []
    for t in m.tasks:
        names.append(cident(t.name).upper())
    for isr in m.isrs:
        names.append(cident(isr.name).upper())
        sp = isr.get("OsIsrSpecialFunctionName", "")
        if sp:
            names.append(cident(sp).upper())
    for a in m.apps:
        names.append(cident(a.name).upper())
    for n in sorted(set(names)):
        o.append(f"#define OS_{n}_CODE")
    o += ["", "#endif /* OS_COMPILER_CFG_H */"]
    return "\n".join(o) + "\n"


def emit_hook_cfg_h(m: Model, src: str) -> str:
    return HEADER.format(src=src) + '#ifndef OS_HOOK_CFG_H\n#define OS_HOOK_CFG_H\n#include "Os_Cfg.h"\n#endif\n'


def scan_isr_bodies(root: str) -> Set[str]:
    found: Set[str] = set()
    rx = re.compile(r"\bISR\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
    for dp, _, files in os.walk(root):
        for f in files:
            if f.endswith((".c", ".h")):
                try:
                    txt = Path(dp, f).read_text(encoding="utf-8", errors="ignore")
                except OSError:
                    continue
                for mm in rx.finditer(txt):
                    found.add(mm.group(1))
    return found


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("arxml", nargs="+")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("--isr-prio-order", choices=["lower-is-higher", "higher-is-higher"], default="lower-is-higher")
    ap.add_argument("--isr-bodies", default="")
    ap.add_argument("--isr-bodies-from", default=None)
    ap.add_argument("--no-isr-wrappers", action="store_true")
    ap.add_argument("--ioc-init-values", action="store_true")
    ap.add_argument("--tick-ns", type=int, default=0)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    args.isr_bodies = [s for s in args.isr_bodies.split(",") if s]
    if args.isr_bodies_from:
        args.isr_bodies += sorted(scan_isr_bodies(args.isr_bodies_from))

    ecuc = Ecuc.load(args.arxml)
    if "Os" not in ecuc.modules:
        raise SystemExit(f"no Os module in {args.arxml}; modules: {sorted(ecuc.modules)}")
    m = Model(ecuc, ecuc.module("Os"), args)
    src = Path(args.arxml[0]).name
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    files = {
        "Os_Types_Lcfg.h": emit_types_lcfg(m, src),
        "Os_Cfg.h": emit_cfg_h(m, src),
        "Os_Hal_Cfg.h": emit_hal_cfg_h(m, src),
        "Os_Lcfg.h": emit_lcfg_h(m, src),
        "Os_Lcfg.c": emit_lcfg_c(m, src),
        "Os_Ioc_Lcfg.h": emit_ioc_h(m, src),
        "Os_Ioc_Lcfg.c": emit_ioc_c(m, src),
        "Os_Stack_Cfg.h": emit_stack_cfg_h(m, src),
        "Os_Compiler_Cfg.h": emit_compiler_cfg_h(m, src),
        "Os_Hook_Cfg.h": emit_hook_cfg_h(m, src),
    }
    changed = 0
    for name, content in files.items():
        p = out / name
        if not p.exists() or p.read_text(encoding="utf-8") != content:
            p.write_text(content, encoding="utf-8", newline="\n")
            changed += 1
    if not args.quiet:
        print(f"gen_os_cfg: {len(m.tasks)} tasks, {len(m.isrs)} ISRs ({len(m.isrWrappers)} wrappers), {len(m.alarms)} alarms, "
              f"{len(m.counters)} counters, {len(m.resources)} resources, {len(m.spinlocks)} spinlocks, {len(m.barriers)} barriers, "
              f"{len(m.schedtbls)} schedule tables, {len(m.iocs)} IOC channels, {m.numCores} cores -> {out} ({changed} files updated)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
