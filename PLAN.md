# MICROSAR SIL: host-executable virtual ECU for Vector MICROSAR Classic projects

v0.2, 2026-10-31 (v0.1 draft 2026-10-17 was written before any code and before seeing the project).

## Status

The reference project is the **BR1 Cabin ECU** (`C:\br1_cabin_ecu`): MICROSAR Classic SIP
`SIP_2_0_0_HF1_R34_1.0.0` on NXP S32E288 (two Cortex-R52 cores), NXP RTD MCAL, MICROSAR OS 4.8.
Its inventory against the v0.1 checklist is in `docs/reference-project-br1-cabin.md`; the numbers
below come from it.

| Milestone | State 2026-10-31 |
|---|---|
| M0 skeleton, port layer, trace, runtime, CI | **done** (Windows MinGW build + tests green on this PC; Linux/MSVC CI defined, not yet run) |
| M1 SilOs single core, IRQ controller, ECUC generator, task tracing | **done** (103 semantics checks in `tests/unit/test_os.c`) |
| M2 multi-core (XSignal, spinlocks, barriers, IOC, idle barrier) | **done** for the mechanisms the reference project uses; `--lockstep-cores` and CPU pinning tests open |
| M3 virtual CAN, UDP backend, control server, pytest package | next |
| M4 SPI + peripheral models, SIL Kit, virtual LIN, remaining MCAL | after M3 |
| M5 profiling depth (Rte_Hook.c, flows, stats, ET model) | partly done: `siltrace.py stats`, Perfetto host-cost view, Rimac `.bin` export, `SilOs_ConsumeTime`; `gen_rte_hooks.py` open |
| M6 real SIP integration kit | `check_sip.py`, `gen_os_cfg.py`, `gen_mcal_cfg.py` verified on the Cabin ECU; host compile probe: 43/45 project files (all Vector BSW, full RTE, CDDs) syntax-check on the host with `sil/hal` + `sil/mcal/inc` headers; virtual MCAL implementations and `cmake/Sip.cmake` open |

### What the project review changed

- **Windows first.** This PC has no MSVC/clang/ninja; the build uses the MinGW-w64 GCC 12.2 that
  ships with the project's `Br1Tools` (preset `windows-mingw-br1tools`). Linux presets stay in CI.
- **Two cores from day one.** The RTE is split in `Rte_OsApplication_Trusted_Core0/1.c`; 240
  `GetSpinlock` sites, cross-core `SetEvent` (XSignal), two `Os_BarrierSynchronize` points and 8
  IOC channels are on the startup and 10 ms paths, so M1/M2 were built together.
- **No schedule tables, no alarm callbacks, no software counters** in the project: 101 alarms
  (100 SetEvent, 1 ActivateTask) on two hardware counters (1 ms tick, compare based). SilOs
  implements schedule tables anyway (tested) because other ECUs use them.
- **All 71 ISRs are Cat2 with `OsIsrSpecialFunctionName`** (driver handlers such as
  `CAN15_ORED_IRQHandler`, `Spi_Ip_SPI_2_IRQHandler`), `OsIsrEnableNesting=false` everywhere,
  priorities 15/16/32/33 (ARM GIC: lower = more urgent). The generator emits `Os_Isr_<fn>()`
  wrappers for them, so the virtual MCAL only has to provide the handler functions.
- **VFB tracing is not generated** by the project's RTE. The project instruments runnables itself
  (`Tests/Profiling/instrumentSrc.py`, `MEASURE_TIMESTAMP_RUNNABLE_CORE_<n>(id)`, 743 runnables);
  the SIL maps these macros onto its tracer and exports the project's own 64-bit profiling record
  format, so `ProfilingAnalyzeData.py` works unchanged. `Rte_Hook.c` generation stays for SIPs with
  VFB tracing.
- **Exclusive areas are interrupt locks**: 625 `SchM_Enter_*` expand to `SuspendAllInterrupts`;
  no `Rte_Enter_*`. SilOs interrupt locks are per context and cheap.
- **Vector OS extensions actually used**: `Os_GetTaskStackUsage` (stack monitor SWC),
  `Os_InitialEnableInterruptSources`, `Os_BarrierSynchronize`, `Os_Init/InitMemory/InitInterruptOnly`,
  interrupt source control, peripheral access, `Os_GetDetailedError`, `Os_PanicHook`. All are in
  SilOs (`Os_Api.h`, `SilOs_Compat.c`).
- **MCAL is NXP RTD, not Vector**: `Can_43_FLEXCAN` (6 controllers, FD, 129 HOHs), `Spi` (4 units,
  17 external devices: ADS7038 x10, BTS710336 x3, BTS722204 x2, DRV8910, L9305), `Icu` (34 APIs),
  `Adc`, `Pwm`, `Dio` (125 channels), `I2c`, `Uart`, NXP `Fee` on `MemAcc`/`Mem_43_*`,
  `Crypto_43_HSE`, safety modules. The Vector `Lin` driver (4 master channels) is the only Vector
  MCAL-level module. M4/M6 scope follows this list.
- **Host word size**: the project is 32-bit; the host build is 64-bit with cast warnings as errors;
  a 32-bit MinGW build is the fallback (unchanged).

## Context

A FreeRTOS project can be compiled for the POSIX/Win32 simulator port and run on a PC. There is no
equivalent for a Vector MICROSAR Classic project without Vector's vVIRTUALtarget (Windows only,
licensed; the SIP here even contains `IncludeVtt/` leftovers). The goal is a Linux-and-Windows SIL
runtime that:

1. Compiles and runs a MICROSAR Classic ECU project (BSW + RTE + SWCs) on the host before HW tests.
2. Profiles OS task and RTE runnable behaviour (activation, order, preemption, period/jitter, host CPU).
3. Connects CAN/LIN simulation tooling: Vector CANoe/CANalyzer, python-can/can-utils/Wireshark, and
   the user's own pytest scripts.
4. Runs a multi-core MICROSAR OS configuration with true parallelism.
5. Simulates interrupts faithfully, e.g. an SPI "message received" interrupt firing a Cat2 ISR.

No Vector or NXP code enters this repo. Everything is built and CI-tested against the stand-in demo
ECU (`demo/`); the real project is linked in through CMake options.

## Architecture

```
 +-------------------------------------------------------------------------+
 |  ECU project (unchanged): SWCs, RTE (GenData), BSW (Com, PduR, CanIf,  |
 |  CanTp, Dcm, Dem, NvM, EcuM, BswM, WdgM, Det ...)                        |
 +------------------+--------------------------+---------------------------+
                    | OSEK/AUTOSAR OS API      | MCAL API (RTD names: Can_43_FLEXCAN, Dio, Spi ...)
 +------------------v--------------+  +--------v-----------------------------+
 | SilOs (sil/os): N cores, tasks, |  | Virtual MCAL (sil/mcal, M3/M4/M6):  |
 | alarms, sched tables, resources,|  | Can, Lin, Spi, Mcu, Port, Dio, Gpt, |
 | events, spinlocks, XSignal, IOC,|  | Icu, Pwm, Adc, I2c, Uart, Wdg, Fee/ |
 | barriers, Cat2 ISRs, IRQ ctrl   |  | MemAcc (file backed), Crypto stand-in|
 +------------------+--------------+  +--------+------------------+----------+
                    |                           | bus backends     | peripheral models
 +------------------v--------------+  +--------v---------+  +-----v----------------+
 | Virtual clock (sil/os/Time):    |  | UDP virtual bus  |  | VirtualIo table,     |
 | virtual (event driven) |        |  | SIL Kit CAN+LIN  |  | SPI slave models,    |
 | realtime[:factor]               |  | SocketCAN (Linux)|  | virtual timers       |
 +------------------+--------------+  +------------------+  +----------------------+
                    |
 +------------------v------------------------------------------------------+
 | Tracer (sil/trace): per-core rings -> .siltrace / Perfetto / BTF /       |
 | Rimac .bin;  Control server (M3): newline JSON over TCP for pytest       |
 +-------------------------------------------------------------------------+
```

### Key decisions (v0.2; changes to v0.1 marked)

| Topic | Decision | Why |
|---|---|---|
| Languages | C11 for everything the SIP links against (`sil/hal`, SilOs, virtual MCAL, trace); C++17 only behind `sil/port` (threads, sync, clocks); Python 3.10+ tools. | As v0.1; verified with MinGW GCC 12.2 (posix threads; no `<threads.h>`, hence no C11 threads). |
| OS threading model | One host thread per task and Cat2 ISR, one per core (init/idle), one run token per core, **one global OS lock**. Switch points: every OS service, ISR end, `SilOs_PreemptionPoint()`, `SilOs_ConsumeTime()`, spin and idle loops. Basic tasks restart via `setjmp/longjmp`. | Proven; threads are named (`os:Task_1ms_Core0`, `isr:Isr_CanRx`); sanitizer friendly. The global lock keeps cross-core state simple; contention is irrelevant because contexts spend their time waiting. *(changed: global lock instead of per-core locks)* |
| Interrupt model | Per-core virtual IRQ controller: `SilOs_RaiseIrq()` thread-safe; dispatch by urgency at the next switch point of the ISR's core (immediately if idle), honouring the running context's Disable/Suspend nesting, per-ISR enable, nesting flag and running-ISR priority. | Matches hardware semantics; the tests cover masking, nesting enabled/disabled, latency. |
| Multi-core | N schedulers on N host threads; cross-core `ActivateTask`/`SetEvent` through a per-core request queue serviced by the XSignal ISR, synchronous for the caller; a waiting core serves its own queue (no mutual-ISR deadlock); spinlocks apply `OsSpinlockLockMethod` before spinning; barriers; IOC generated per `OsIocCommunication`. `--lockstep-cores` and pinning: later. | As v0.1 with the deadlock rule added. |
| Time | 64-bit ns `VirtualClock`. Virtual mode: time jumps to the next event only when **no core can progress** (idle or spinning with an unsatisfied predicate, no deliverable ISR, no queued request, no reached consume target). Events: hardware counter expiries (alarms, schedule tables), one-shot virtual timers, consume targets, `--duration`. Realtime mode: driver thread + factor. Counters derive from the clock (`ticks = now / nsPerTick`). *(changed: "can progress" rule instead of "all idle")* | "All cores idle" let one core jump time while another was about to wake; the refined rule keeps traces exact (alarms expire at exactly the configured tick). |
| Execution-time model | `SilOs_ConsumeTime(ns)` consumes **net** execution time: preemptions shift the target; alarms/IRQs are delivered on the way. Table driven model (`et_model.yaml`) still M5. | Needed to see preemption and response times in zero-time emulation; verified (`Task_100ms` CET = 1000 us with preemptions, RT 1470 us). |
| Config source of truth | `tools/ecuc.py` + `tools/gen_os_cfg.py` read the ECUC arxml DaVinci writes; outputs `Os_Types_Lcfg.h`, `Os_Cfg.h` (event masks by greedy colouring), `Os_Hal_Cfg.h` (tick macros), `Os_Lcfg.c` tables, ISR wrappers, IOC API, `Os_Stack_Cfg.h`, `Os_Compiler_Cfg.h`. | Verified on the Cabin ECU: 18 tasks, 71 ISRs (63 wrappers, 4 `ISR()` bodies found in `Lin_Lcfg.c`), 101 alarms, 84 spinlocks, 2 barriers, 8 IOC. |
| Headers kept from SIP | Keep the project's `Compiler_Cfg.h` chain (ends in generated `Os_Compiler_Cfg.h`); replace `Compiler.h`, `Platform_Types.h`, `Std_Types.h`, `MemMap.h`, `Os_*_MemMap.h` (no-op), OS private headers via `sil/os/compat`. | Inventory shows only `Os.h`, `Os_Types.h`, `Os_Stack_Cfg.h`, `Os_Types_Lcfg.h`, `Os_Compiler_Cfg.h` are included by non-OS code (plus `Os_Hal_Compiler_GccNxpInt.h` in the excluded CDD_FETK). |
| Tracing formats | `.siltrace` (32-byte records), Perfetto JSON (virtual schedule + host cost processes), BTF 2.1.3, **Rimac profiling `.bin` + id XML** (compatible with `Tests/Profiling`). | The user's profiling pipeline stays usable. |
| Runnable instrumentation | `MEASURE_TIMESTAMP_RUNNABLE_CORE_<n>(id)` -> `SilOs_RunnableStart/Stop`; `Rte_Hook.c` generator for VFB-traced SIPs (M5). | The project RTE has no VFB hooks. |
| Bus backends | UDP virtual bus first (works on Windows, python-can plugin), SIL Kit second (CANoe), SocketCAN on Linux. | No SocketCAN on the development PC. |
| ECU reset | `Mcu_PerformReset` -> re-exec with `--resume` (NvM files persist). | Unchanged; `sil_process_reexec` exists in the port layer. |

## Interrupt simulation

Unchanged from v0.1 in substance; implemented: ISR objects from ECUC (`OsIsr` with category,
priority, core via `OsApplication`, special function), virtual IRQ controller per core, raise from
any thread or from virtual timers (callbacks run outside the OS lock), delivery rules, trace
events `IRQ_RAISE`/`ISR_START`/`ISR_END`. Peripheral models with latency use
`SilOs_TimerSchedule(delay, fn, arg)` (demo: SPI transfer done after 64 us raises `Isr_SpiDone`;
CAN frame every 7 ms raises `Isr_CanRx`). Slave model plug-ins, SIL Kit RPC slaves, control-server
fault injection and `irq_map.yaml` remain M3/M4.

## Repository layout (actual)

```
microsarSIL/
├── CMakeLists.txt  CMakePresets.json  .github/workflows/ci.yml  README.md  PLAN.md
├── cmake/            Warnings.cmake Sanitizers.cmake SilOsEcu.cmake (silos_add_ecu)
├── sil/port/         sil_port.h + impl/port.cpp (threads, semaphores, mutex/cond, atomics, clocks, re-exec)
├── sil/hal/          Compiler.h Compiler_Cfg.h Platform_Types.h Std_Types.h MemMap.h
├── sil/os/           Os_Api.h Os.h Os_Types.h SilOs.h SilOs_Cfg.h SilOs_Internal.h SilOs_Core.c SilOs_Task.c
│                     SilOs_Time.c SilOs_Compat.c SilOs_Ioc.{h,c} compat/ (Os_Common.h, Os_*_MemMap.h, ...)
├── sil/trace/        SilTrace.{h,c} SilTrace_Writers.c (siltrace, perfetto, btf, rimac)
├── sil/runtime/      sil_runtime.{h,c} (CLI: --time --duration --trace --perfetto --btf --rimac)
├── tools/            ecuc.py ecuc_build.py gen_os_cfg.py check_sip.py siltrace.py
├── demo/             ecuc/make_demo_ecuc.py -> DemoEcu.arxml, app/demo_ecu.c
├── tests/            unit/test_os.c, tools/test_gen_os_cfg.py, CMakeLists.txt (ctest)
└── docs/             reference-project-br1-cabin.md architecture.md integrating-microsar.md
```

## Milestones (re-estimated after the project review)

### M3: virtual CAN, UDP backend, control server, pytest package (next)
- `sil/mcal/Can`: RTD API names (`Can_43_FLEXCAN_Init/SetControllerMode/Write/MainFunction_*`,
  `Can_GetControllerErrorState`, error counters, `SetBaudrate`, `DisableControllerInterrupts`) plus the
  `CAN<n>_ORED*/EnhancedData_IRQHandler` entry points; controllers/HOHs/filters generated from the
  `Can` ECUC (`gen_mcal_cfg.py`), RX/TX confirmation in ISR context, bus-off model.
- `sil/bus`: `can_udp` (multicast framing, Linux+Windows), `can_null`; per-controller backend in
  `runtime.json`; SocketCAN on Linux.
- `sil/ctrl`: newline JSON over TCP: `step`, `run`, `pause`, `time`, `io.set/get`, `irq.raise`,
  `trace.flush`, `shutdown`.
- `python/microsarsil`: `SilProcess`, `SilControl`, python-can plugin for the UDP bus, fixtures;
  `tests/sil` pytest on the demo (periodic TX, RX -> ISR -> task, time stepping determinism).
- Demo gets a `CanIfLite` (frame <-> signal) so the demo exercises the real driver API.
- Verify: `demo_ecu --can0=udp --time=realtime` visible in python-can; `pytest tests/sil` green on
  Windows; deterministic trace hash in virtual mode single-core.

### M4: SPI + peripheral models, SIL Kit, virtual LIN, remaining MCAL
- Virtual `Spi` with `ISpiSlave` models for the Cabin devices (ADS7038, BTS7103/7222 supervisors,
  DRV8910, L9305) and the SPI unit ISRs; `Dio/Port/Pwm/Icu/Adc/Gpt/I2c/Uart` on `VirtualIo`;
  `Mcu/Platform/OsIf/Pit_Ip/Mcl` stubs with the lifetime timer on the virtual clock; file-backed
  `Fee` replacing NXP Fee + MemAcc + Mem_43_*; Crypto stand-in; safety module stubs.
- SIL Kit loader + CAN/LIN participants + time sync (`--time=silkit`); virtual `Lin` (4 master
  channels, LDF driven responses, Python slaves).
- `gen_mcal_cfg.py` for the symbolic names (`DioConf_DioChannel_*`, `SpiConf_SpiSequence_*`, ...).

### M5: profiling depth
- `gen_rte_hooks.py` (`Rte_Hook.c`), flow events (activation -> start, CAN RX -> runnable -> TX),
  table-driven execution-time model (`et_model.yaml`), `siltrace.py` CSV already exists; add
  response-time histograms and the Gliwa CET/GET/RT/DT vocabulary of the user's OS note.

### M6: real SIP integration kit
- `cmake/Sip.cmake`: glob the Vector modules the project enables (`sip_config.cmake` options),
  `Generated_BSW/*.c` minus `Os_*`, `BSw/StartUpCode` minus hardware files; include path order from
  `docs/integrating-microsar.md`; `sil_ecu_entry` replacing `main`.
- Compile-probe order: `Det`, `Crc`, `VStdLib`, `EcuM`, `BswM`, `Com`, `PduR`, `CanIf`, `CanSM`,
  `CanNm`, `Nm`, `ComM`, `CanTp`, `Dcm`, `Dem`, `NvM`, `MemIf`, `WdgM/WdgIf`, `Csm/CryIf`, `E2E*`,
  `Xcp/CanXcp`, RTE, SWCs, CDDs. Fix 64-bit pitfalls with `-Werror=pointer-to-int-cast`.
- Verify on the Cabin ECU: reaches `EcuM` RUN, Com periodic PDUs on the UDP bus, UDS `0x22` answered.

## Risks and unknowns (updated)
- Vector licence for running the SIP on a host/in CI: unchanged; keep the repo clean.
- The RTE's cross-core spinlock traffic (240 sites, zero execution time in the SIL) will show no
  contention unless the execution-time model is used; document.
- NXP `Fee` + `MemAcc` + `Mem_43_*` emulation and the NvM block layout (110 blocks) need the Fee
  ECUC; `Cdd_Mram`/`KeyProvider` also use MemAcc directly.
- `CDD_FETK` (ETAS probe) and `thirdParty/ddr` must be excluded from the host build.
- SPI slave models: 17 devices; start with register-echo models and refine per CDD.
- 64-bit host: unchanged mitigation; `-m32` MinGW fallback if the SIP resists.
- Determinism across cores: unchanged (`--lockstep-cores` later).

## Verification summary (current)
- `ctest`: `os_semantics` (103 checks, two cores, deterministic over repeated runs),
  `demo_runs_2s` (virtual time with Perfetto/BTF output), `demo_realtime_1s` (realtime x4),
  `tools_pytest` (ECUC parser, generator on demo and Cabin ECU configs, idempotence).
- Performance: 10 s of virtual time of the demo (two cores, ~40k trace events/s) runs in 0.9 s wall.
