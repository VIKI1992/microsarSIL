# MICROSAR SIL: host-executable virtual ECU for Vector MICROSAR Classic projects

Draft v0.1, 2026-10-17.

## Status and next step

This is a first-pass architecture and implementation plan written before any code exists and
before seeing the target project. **A full AUTOSAR (MICROSAR Classic) project will be provided
next; this plan will be refined against it.** Items to verify once the project is available:

- OS: number of cores, OS applications and their core assignment, counters/tick period, schedule
  tables vs alarms, spinlocks and IOC usage, hooks enabled, STANDARD vs EXTENDED status, every
  Vector-specific `Os_*` API the BSW/RTE/EcuM calls, and which OS-private headers are included.
- RTE: whether VFB tracing is already enabled, the exact `Rte_Hook.h` prototypes, ARTI availability,
  exclusive-area implementation (interrupt lock, resource, spinlock), multi-core RTE/IOC usage.
- Communication: CanIf driver infix and HOH layout, CAN FD, number of controllers; LIN master/slave
  roles, LDF/schedule tables; any Ethernet/FlexRay.
- MCAL inventory: which drivers the project uses (Spi, Icu, Gpt, Pwm, Adc, Wdg, Fls vs vMem/vMemAccM,
  Eep), which notifications/ISRs they are wired to, the OsIsr names per MCAL unit.
- Startup: `main`, `EcuM_Init`, driver init lists, `Mcu` reset/PLL handling, BswM/EcuM state flow.
- Toolchain: target compiler and the SIP's `Compiler.h`/`Platform_Types.h`/`MemMap` conventions,
  32-bit assumptions, any compiler extensions in portable code.
- Test intent: which signals/frames/diagnostics the first SIL tests must cover, which CANoe
  configuration (if any) should connect.

The milestones below are sized for a generic project and will be re-estimated after that review.

## Context

A FreeRTOS project can be compiled for the POSIX/Win32 simulator port and run on a PC. There is no
equivalent for a Vector MICROSAR Classic project without Vector's vVIRTUALtarget (Windows only,
licensed). The goal is a Linux-and-Windows SIL runtime that:

1. Compiles and runs a MICROSAR Classic ECU project (BSW + RTE + SWCs) on the host before HW tests.
2. Profiles OS task and RTE runnable behaviour (activation, order, preemption, period/jitter, host CPU).
3. Connects CAN/LIN simulation tooling: Vector CANoe/CANalyzer, python-can/can-utils/Wireshark, and
   the user's own pytest scripts.
4. Runs a multi-core MICROSAR OS configuration with true parallelism.
5. Simulates interrupts faithfully, e.g. an SPI "message received" interrupt firing a Cat2 ISR.

The repo is empty and the Vector SIP is not yet available here. Everything is built and CI-tested
against a small stand-in demo stack; integration with the real SIP is a documented, CMake-driven
step. No Vector code enters this repo.

Verified facts that shape the design (2026-10):
- Vector SIL Kit (MIT, C++17, prebuilt for Ubuntu 22.04/24.04 and Windows, v5.x) has a stable C API:
  `Can.h`, `Lin.h`, `Orchestration.h` (lifecycle + time sync), `DataPubSub.h`, `Rpc.h`. CANoe 16+
  connects natively to a SIL Kit simulation (registry over TCP), so a Linux SIL can talk to a Windows
  CANoe without a Windows port. `sil-kit-adapters-vcan` mirrors a SIL Kit CAN network onto SocketCAN.
- Mainline Linux (incl. 7.x) has no LIN subsystem; LIN must be simulated in user space.
- python-can: `socketcan` (Linux), `udp_multicast` (Unix only), `vector` XL virtual channels (Windows only).
- Trace formats consumed by timing tools: BTF (Eclipse Trace Compass, Vector TA Tool Suite, APP4MC)
  and Perfetto/Chrome JSON. Both are trivial to emit.
- MICROSAR RTE "VFB Tracing" generates `Rte_Hook.h` with `Rte_Runnable_<SWC>_<RE>_Start/_Return`,
  `SchM_Schedulable_*`, `Rte_Task_*` hooks that the integrator implements. Newer SIPs also offer ARTI.
- Host: gcc 15.2 and gdb 17.1 present; cmake, ninja, clang, can-utils, python-can not installed.

## Architecture

```
 +-------------------------------------------------------------------------+
 |  ECU project (unchanged): SWCs, RTE (GenData), BSW (Com, PduR, CanIf,  |
 |  CanTp, Dcm, Dem, NvM, Fee, EcuM, BswM, WdgM, Det ...)                   |
 +------------------+--------------------------+---------------------------+
                    | OSEK/AUTOSAR OS API      | MCAL API (Can/Lin/Spi/Dio/...)
 +------------------v--------------+  +--------v-----------------------------+
 | SilOs: emulated AUTOSAR OS      |  | Virtual MCAL: Can, Lin, Spi, Mcu,   |
 | N virtual cores, tasks, alarms, |  | Port, Dio, Gpt, Icu, Pwm, Adc, Wdg, |
 | sched tables, resources, events,|  | Fls/Eep (file-backed)               |
 | spinlocks, IOC, Cat2 ISRs,      |  +--------+------------------+----------+
 | virtual IRQ controller          |           | bus backends     | peripheral models
 +------------------+--------------+  +--------v---------+  +-----v----------------+
                    |                 | SocketCAN (Linux)|  | VirtualIo table,     |
 +------------------v--------------+  | SIL Kit CAN+LIN  |  | SPI slave models,    |
 | Virtual clock / time driver:    |  | UDP virtual bus  |  | timers, fault inject |
 | virtual | realtime | silkit-sync|  +------------------+  +----------------------+
 +------------------+--------------+
                    |
 +------------------v------------------------------------------------------+
 | Tracer (per-core ring buffers) -> .siltrace -> BTF / Perfetto / stats   |
 | Control server (newline JSON over TCP) for pytest: step, io, irq, faults|
 +-------------------------------------------------------------------------+
```

### Key decisions

| Topic | Decision | Why |
|---|---|---|
| Languages | **C11** for everything the SIP links against or includes: `sil/hal`, SilOs core + API, virtual MCAL, `Rte_Hook.c`, trace recorder. **C++17** behind C headers for `sil/port` (threads/sync/time/sockets on `std::thread`, `std::condition_variable`, `std::chrono`, Winsock/BSD shim), bus backends, trace writers, control server. Python 3.10+ for generators/analysis (stdlib XML). | BSW-facing code stays plain C and debuggable next to the BSW. C++17 std primitives make Linux/Windows parity almost free; MSVC C11 threads/atomics are still weak. |
| OS threading model | **One host thread per task and per Cat2 ISR; one "run token" per virtual core.** Exactly one thread per core runs at a time; a context switch hands the token via per-thread semaphores. Switch points: every OS API call, `SilOs_PreemptionPoint()` (called from RTE hooks and `SilOs_ConsumeTime`), `ResumeAllInterrupts`, ISR end, idle loop. No signals, no `SuspendThread` in the default build. | Proven FreeRTOS-port design; identical on both OSes; gdb/VS show `os:Task_10ms`, `isr:SpiRx` as named threads; ASan/TSan/UBSan work unchanged (TSan finds real cross-core races in SWC code). Fibers/ucontext would break MSVC parity and sanitizers. Async preemption buys nothing in zero-execution-time mode and is kept as an optional later mode behind the same scheduler interface. |
| Interrupt model | Virtual IRQ controller per core: `SilOs_RaiseIrq(IsrId)` is thread-safe and callable from any host thread or task. Pending IRQs are dispatched by `OsIsrInterruptPriority` at the next switch point of the ISR's core (immediately if the core is idle), honouring `Disable/Suspend*Interrupts` nesting. The ISR thread then runs the project's `ISR(name)` body. See "Interrupt simulation" below. | Matches hardware semantics (ISR preempts tasks, nests by priority, cannot block) while staying deterministic. |
| Multi-core | N virtual cores = N schedulers running concurrently on host threads. Cross-core `ActivateTask`/`SetEvent`/alarm and schedule-table actions go through a per-core request queue serviced as a top-priority ISR (emulates XSIGNAL). `StartCore`, `StartNonAutosarCore`, synchronized `StartOS` barrier, `GetCoreID`, `ShutdownAllCores`, spinlocks on `std::atomic` with `OsSpinlockSuccessor` order checking, generated IOC API. `--lockstep-cores` serializes cores round-robin for reproducible debugging; optional host CPU pinning. | True parallelism as required, with a deterministic per-core tick model. |
| Time | Global `VirtualClock` (ns); OS tick period from the system `OsCounter` (`OsSecondsPerTick`). Virtual time moves only (1) when **all cores are idle**, jumping to the next timer/IRQ event (event-driven, as fast as possible), or (2) inside `SilOs_ConsumeTime(ns)` (execution-time model). Modes: `virtual` (default, test-driven stepping via control server or free-running), `realtime[:factor]` (timer thread posts ticks, late ticks counted), `silkit` (SIL Kit time-sync participant; sim step = tick). | Reproducible traces and tests by default; interactive bench mode; co-simulation mode. |
| Execution-time model (M5) | Table (`runnable/task/isr -> const | uniform(min,max,seed) | host*scale`). `Rte_Runnable_*_Return` (or `_Start`, `--et-apply`) calls `SilOs_ConsumeTime(d)`, which walks virtual time toward `now+d`, firing alarms/IRQs on the way; if a higher-priority task/ISR becomes ready the remaining budget is stored in the TCB and the task is preempted, resuming later. | Enables preemption and response-time studies despite zero host execution time. Documented limitation: a timing model, not a memory-interleaving model. |
| SIL Kit integration | Bind the SIL Kit C API via `dlopen`/`LoadLibrary` (optional at runtime, no ABI coupling). Time: `SetSimulationStepHandlerAsync`; handler posts `target = now + step` to the clock, cores advance to `target`, and when all cores are idle at `target` the runtime calls `CompleteSimulationStep`. RX frames carry SIL Kit timestamps; TX uses the thread-safe `SilKit_CanController_SendFrame`. | Never blocks the SIL Kit I/O thread; OS tick and SIL Kit step are one quantum, so CANoe and the OS agree on time. |
| Bus RX into OS context | Backend threads push frames into a per-controller SPSC queue and raise the controller's RX IRQ. The virtual Can/Lin ISR drains the queue and calls `CanIf_RxIndication` etc. in ISR context (`CanRxProcessing=POLLING` -> drained in `Can_MainFunction_Read`). TX confirmation and bus-off likewise via IRQs. | Faithful to interrupt-driven MICROSAR drivers; BSW is never called from foreign threads. |
| Host word size | 64-bit default on both OSes. SIP compiled with `-Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Wconversion` (MSVC `C4311/C4312` as errors). Optional `linux-gcc-m32` preset (without SIL Kit). | SIL Kit prebuilt binaries and sanitizers are 64-bit; MICROSAR uses fixed-width types, so the few pointer/int sins are caught statically. |
| Config source of truth | Python generators parse the project's ECUC `.arxml` (standard AUTOSAR ECUC XML, matched by `DEFINITION-REF`) and emit `Os_Types_Lcfg.h`/`Os_Lcfg.c`, `Can_Cfg.*`, `Lin_Cfg.*`, `Spi_Cfg.*`, `Dio_Cfg.*` ... for SilOs and the virtual MCAL. | DaVinci stays the single source of truth; no hand-maintained duplicate. |
| ECU reset | `Mcu_PerformReset`/`ShutdownOS` -> re-exec the process with `--resume` (NvM/Fls files persist). | MICROSAR relies on `.bss` zero-init; in-process restart is unsafe. |
| Headers kept from SIP | Keep the SIP's `Compiler_Cfg.h` and `vstdlib` portable variant; replace only `Compiler.h`, `Platform_Types.h`, `Std_Types.h`, `MemMap.h`/`*_MemMap.h` (single no-op shim). | Minimal surface, fewer surprises. |

## Interrupt simulation (how an SPI receive interrupt works in the SIL)

On hardware: the SPI peripheral finishes a transfer, raises its IRQ line, the CPU vectors into the
MICROSAR OS Cat2 wrapper for `ISR(SpiRxIsr)`, the MCAL Spi driver's handler copies the RX FIFO into the
job's IB/EB buffers and calls the configured `SpiJobEndNotification`/`SpiSeqEndNotification`, which the
project routes to e.g. an SBC/external-ADC driver that then sets an event or activates a task.

In the SIL every one of those pieces has a virtual counterpart:

1. **ISR objects come from the project's ECUC.** Each `/MICROSAR/Os/OsIsr` (category, priority, core via
   `OsApplication`) becomes a SilOs ISR thread with priority above all tasks. `ISR(x)` expands to
   `void Os_Isr_x(void)`; the generated table binds name -> entry -> core -> priority.
2. **A virtual IRQ controller per core.** `SilOs_RaiseIrq(IsrId)` sets the pending bit (thread-safe, any
   host thread). Dispatch rules: highest pending priority first; blocked while the core is in
   `DisableAllInterrupts`/`SuspendAllInterrupts`/`SuspendOSInterrupts`; delivered at the core's next switch
   point, or immediately if the core is idle; nested ISRs preempt lower ISRs at their switch points.
   Trace records `IRQ_RAISE`, `ISR_START`, `ISR_END`, so raise-to-start latency (interrupt latency caused by
   non-preemptable sections or masked interrupts) is visible in Perfetto/BTF.
3. **Peripheral models raise IRQs with virtual-time latency.** The virtual Spi driver (`Spi_Init`,
   `Spi_WriteIB/ReadIB`, `Spi_SetupEB`, `Spi_AsyncTransmit`, `Spi_SyncTransmit`, `Spi_Cancel`,
   `Spi_GetStatus/JobResult/SequenceResult`, `Spi_MainFunction_Handling`, channels/jobs/sequences from
   `SpiDriver` ECUC) hands the MOSI bytes of each job to a **slave model** behind `ISpiSlave`:
   `transfer(cs, mosi, len) -> (miso, latency_ns)`. The driver schedules a one-shot virtual timer at
   `now + latency` (bytes x baud from `SpiBaudrate`, or model-provided); when it expires the driver
   raises the ISR mapped to that SPI unit (`SilOs_RaiseIrq(ISR_SPI0_RX)`). The ISR body runs the driver's
   completion path: write RX buffers, update job/sequence results, call the end notifications in ISR
   context, exactly like the real driver. `SpiLevelDelivered=2` async in `SPI_POLLING_MODE` is honoured by
   completing inside `Spi_MainFunction_Handling` instead.
4. **Slave models are pluggable.** (a) in-process C model (register map / echo / scripted responses,
   e.g. an SBC-like device answering status registers); (b) external Python model over the control
   socket (`periph.spi.request` -> response), the step blocks until the model answers so virtual time
   stays deterministic; (c) SIL Kit RPC server, so a CANoe CAPL/.NET node can act as the SPI slave.
5. **The same mechanism serves every interrupt source**: OS tick (per-core system counter), Gpt
   channel notifications, Icu edge notifications (raised when `io.set` or a SIL Kit DataSubscriber
   changes a Dio/Icu input), Pwm period notifications, Adc group conversion complete, CAN/LIN RX/TX/
   bus-off/wakeup from bus backends, cross-core XSIGNAL, and Wdg.
6. **Test control and fault injection.** Control-server commands `irq.raise {isr}`, `irq.storm {isr,
   period}`, `periph.spi.latency {unit, ns}`, `periph.spi.fail {unit, mode}`; pytest fixtures wrap them.
7. **Mapping MCAL unit -> OsIsr** is generated by name heuristics from the OsIsr list (e.g. `SpiIsr_0`,
   `CanIsr_0_Rx`) and overridable in a small YAML (`irq_map.yaml`) because Vector MCAL ISR names vary
   per derivative.
8. **Not emulated**: Cat1 ISRs (MCAL-internal on real targets; our drivers do not need them), timing
   protection, MPU, stack monitoring. Documented in `docs/architecture.md`.

## Repository layout

```
microsarSIL/
├── CMakeLists.txt  CMakePresets.json  .github/workflows/ci.yml  README.md  LICENSE  PLAN.md
├── cmake/            SilKit.cmake, Warnings.cmake, Sanitizers.cmake, Sip.cmake
├── scripts/          setup-ubuntu.sh, setup-windows.ps1, vcan-up.sh
├── sil/port/         sil_port.h (C API) + impl/*.cpp (std::thread, sem, clock, sockets, process re-exec)
├── sil/hal/          Compiler.h Platform_Types.h Std_Types.h MemMap.h (no-op shim) Det glue
├── sil/os/           Os.h Os_Types.h SilOs_Sched.c SilOs_Core.c SilOs_Irq.c SilOs_Clock.c SilOs_Alarm.c
│                     SilOs_Counter.c SilOs_SchedTbl.c SilOs_Resource.c SilOs_Event.c SilOs_Spinlock.c
│                     SilOs_Ioc.c SilOs_Hooks.c SilOs_Et.c SilOs_Compat.c (Vector extension no-ops)
├── sil/trace/        SilTrace.h SilTrace.c (per-core ring, lossy counters) writers/{siltrace,btf,perfetto}.cpp
├── sil/mcal/         Can/ Lin/ Spi/ Mcu/ Port/ Dio/ Gpt/ Icu/ Pwm/ Adc/ Wdg/ Fls/ Eep/ (Xxx.c + Xxx_Cfg templates)
├── sil/periph/       VirtualIo.c (named values), ISpiSlave.h + models/, VirtualTimer.c, FaultInject.c
├── sil/bus/          sil_bus.h can_socketcan.cpp can_silkit.cpp can_udp.cpp lin_silkit.cpp lin_udp.cpp
│                     can_null.cpp silkit_loader.cpp
├── sil/ctrl/         ctrl_server.cpp ctrl_proto.md
├── sil/runtime/      main.c cli.c runtime_config.c (JSON) time_driver.c sil_runtime.h
├── tools/            ecuc.py gen_os_cfg.py gen_mcal_cfg.py gen_rte_hooks.py gen_irq_map.py
│                     siltrace.py (to-btf, to-perfetto, stats) silctl.py check_sip.py
├── python/microsarsil/  SilProcess, SilControl, python-can plugin ("microsarsil" UDP bus), LinMaster/
│                     LinSlave, SpiSlaveServer, pytest fixtures; pyproject.toml
├── demo/             ecuc/DemoEcu.arxml (Os 2 cores, Can, Lin, Spi, Dio, Icu), app/ (Rte-lite, SWCs,
│                     CanIfLite, EcuMLite, Rte_Hook.h sample), config/{runtime.json, et_model.yaml, irq_map.yaml}
├── tests/unit/       OSEK-semantics tests (C), trace writer tests, pytest for tools/
├── tests/sil/        pytest black-box on demo: bus traffic, time stepping, IRQ/SPI, NvM persistence, trace
└── docs/             architecture.md interrupts.md integrating-microsar.md tracing.md canoe-silkit.md windows.md
```

## Milestones

Every milestone leaves `demo_ecu` runnable on Linux and Windows CI. Sizes are rough and will be
re-estimated once the real project has been reviewed.

### M0: skeleton, toolchains, CI, port layer
- `scripts/setup-ubuntu.sh` (apt: cmake ninja-build clang can-utils; pip: python-can pytest pyyaml).
- CMake presets: `linux-gcc`, `linux-clang-asan`, `linux-gcc-tsan`, `linux-gcc-m32` (optional),
  `windows-msvc`, `windows-mingw`. Options `SIL_WITH_SILKIT`, `SIL_WITH_SOCKETCAN`, `SIL_SIP_DIR`,
  `SIL_GENDATA_DIR`, `SIL_CAN_INFIX`.
- `sil/port` C API on C++17 std (threads with names, semaphores, monotonic + thread-CPU clocks,
  sleep-until, TCP/UDP shim, re-exec). `sil/hal` headers (`CPU_TYPE_64`, byte order from host).
- `sil/trace` ring + `.siltrace` writer + Perfetto writer; `sil/runtime` CLI; demo "hello" (no OS yet).
- CI: ubuntu-24.04 (gcc, clang+ASan, TSan) + windows-2022 (MSVC, msys2 MinGW-w64); `ctest` wired.
- Verify: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc`
  green on both OSes; `demo_ecu --trace out.json` opens in ui.perfetto.dev.

### M1: SilOs single core, virtual IRQ controller, ECUC generator, task-level tracing
- Scheduler with run token; task states; priority FIFO queues; basic tasks with multiple activations;
  extended tasks/events; `FULL`/`NON` preemption; internal and ceiling resources; `ChainTask`,
  `Schedule`; alarms (rel/abs/cyclic; ActivateTask/SetEvent/IncrementCounter/Callback); software and
  hardware counters; schedule tables (rel/abs start, stop, next, expiry points, repeating); app modes;
  hooks (Startup/Shutdown/Error with `OSErrorGetServiceId` + parameter access/PreTask/PostTask);
  STANDARD vs EXTENDED status; `Disable/Enable/Suspend/ResumeAllInterrupts`, `Suspend/ResumeOSInterrupts`.
- Virtual IRQ controller + ISR threads (`SilOs_RaiseIrq`, priority dispatch, masking), one-shot
  `VirtualTimer` service for peripherals.
- `VirtualClock` modes `virtual` (event-driven jump when idle) and `realtime`; tick ISR per core.
- Vector extension no-ops in `SilOs_Compat.c` (`Os_InitMemory`, `Os_Init`, `Os_InitInterruptOnly`,
  `Os_EnterPreStartTask`, `Os_GetVersionInfo`, stack-usage queries).
- Hand-written `demo/gen/Os_*` first; then `tools/ecuc.py` + `gen_os_cfg.py` must reproduce them
  byte-identically from `DemoEcu.arxml` (containers `OsOS, OsTask, OsIsr, OsAlarm, OsCounter, OsEvent,
  OsResource, OsScheduleTable, OsAppMode, OsApplication, OsSpinlock, OsIoc, OsHooks`).
- Trace: task activate/start/preempt/resume/terminate/wait/release, IRQ raise, ISR start/end, alarm
  expiry, resource get/release. `siltrace.py to-btf|to-perfetto`.
- Unit tests (~40): priority preemption, ceiling, non-preemptive, activation limits (`E_OS_LIMIT`),
  event wait/set, alarm cycles, schedule-table repeat, ErrorHook codes, IRQ masking/nesting/latency.
- Verify: demo for 10 virtual s finishes in <1 s wall; `siltrace.py stats` shows 1000/100/10
  activations; `gdb -ex 'info threads'` lists `os:Task_10ms`, `isr:Tick0`; BTF opens in Trace Compass.

### M2: multi-core
- Core objects from `OsApplication` core assignment; `StartCore`, `StartNonAutosarCore`,
  `GetNumberOfActivatedCores`, synchronized `StartOS` barrier, `ShutdownAllCores`; every core runs the
  project entry via `sil_ecu_entry(coreId)` (the SIP's `main` is mapped onto it).
- Per-core cross-core request queue (XSIGNAL ISR) for ActivateTask/SetEvent/alarm/schedule-table
  actions; spinlocks with successor-order checking; generated IOC API (queued/unqueued, groups).
- Global idle barrier for virtual-time advance; `--lockstep-cores`; optional CPU pinning.
- Trace: per-core buffers, spinlock wait/acquire/release, cross-core activation flow events.
- TSan CI job runs the 2-core demo.
- Verify: core-0 alarm activates a core-1 task; trace shows cross-core latency; spinlock/IOC unit
  tests; TSan clean.

### M3: virtual CAN, SocketCAN + UDP backends, control server, pytest
- `sil/mcal/Can`: `Can_Init/SetControllerMode/Write/MainFunction_*`, HOH tables from `gen_mcal_cfg.py`
  (`CanController`, `CanHardwareObject`, `CanHwFilter`), API infix via `CAN_API(name)` from
  `SIL_CAN_INFIX` (read from `CanIfCtrlDrvCfg`), RX/TX/bus-off ISRs or polling.
- `sil/bus`: `can_socketcan` (raw socket, CAN FD, own-message echo = TX confirmation), `can_udp`
  (own multicast framing, Linux+Windows), `can_null`. Per-controller backend choice in `runtime.json`.
- `sil/ctrl`: newline JSON over TCP: `step`, `run`, `pause`, `time`, `io.set/get`, `irq.raise`,
  `fault`, `stats`, `trace.flush`, `reset`, `shutdown`.
- `python/microsarsil`: `SilProcess`, `SilControl`, python-can plugin for the UDP bus, fixtures.
- Demo: `CanIfLite` echo + periodic TX runnables; Dio-driven CAN signal; Icu edge -> ISR -> event task.
- Verify: `scripts/vcan-up.sh`; `demo_ecu --can0=socketcan:vcan0 --time=realtime`; `candump vcan0`
  shows ID 0x100 every 100 ms; `cansend vcan0 200#01` answered by 0x201; `pytest tests/sil` on Linux
  (SocketCAN) and Windows CI (UDP backend); Wireshark decodes SocketCAN.

### M4: SPI + peripheral models, SIL Kit (CAN + LIN + time sync), virtual LIN, remaining MCAL
- `sil/mcal/Spi` + `sil/periph/ISpiSlave` with C echo/register-map model, Python
  `SpiSlaveServer` over the control socket, SIL Kit RPC slave; latency model; `gen_irq_map.py`.
- `sil/bus/silkit_loader` + `can_silkit` + `lin_silkit` (master/slave, frame-response config from
  `LinChannel`/LDF), lifecycle + async step handler (`--time=silkit`).
- `sil/mcal/Lin`: `Lin_Init/SendFrame/GetStatus/GoToSleep/Wakeup/CheckWakeup`; `lin_udp` virtual bus
  + Python `LinMaster/LinSlave`.
- Fls/Eep file-backed from `FlsSector`/`EepInitConfiguration` (Fee/Ea/NvM persist); Mcu/Port/Gpt/
  Icu/Pwm/Adc/Wdg backed by `VirtualIo` (control-server settable, optional SIL Kit Data pub/sub so CANoe
  panels drive inputs); virtual Wdg reports "watchdog reset" to tests.
- `docs/canoe-silkit.md`, `docs/interrupts.md`.
- Verify: SPI test: `Spi_AsyncTransmit` -> model latency 64 us -> `ISR_SPI0_RX` at `t+64us` in trace ->
  `SpiSeqEndNotification` -> task activated; SIL Kit: `sil-kit-registry` + `sil-kit-adapters-vcan`,
  frames visible on `vcan0` and in CANoe, trace virtual time equals SIL Kit time; LIN round-trip with a
  Python slave; NvM data survives `reset` (re-exec).

### M5: profiling depth
- `tools/gen_rte_hooks.py`: parse `Rte_Hook.h` prototypes, emit `Rte_Hook.c` forwarding
  `Rte_Runnable_*_Start/_Return`, `SchM_Schedulable_*`, `Rte_Task_*`, optional `Rte_ComHook_*` to the
  tracer (string-table IDs) and `SilOs_ConsumeTime` on return. `Arti.h` shim for ARTI-capable SIPs.
- Trace: runnable slices nested in task slices, Perfetto flow events (cross-core activation, CAN RX ->
  runnable -> CAN TX, IRQ raise -> ISR start), bus TX/RX instants; two Perfetto "processes": virtual
  schedule and host cost.
- `siltrace.py stats`: per runnable count, host CPU min/avg/max/p99, virtual period and jitter; per task
  activations, response time, preemption count, per-core load; ISR latency; spinlock wait; end-to-end
  CAN RX -> TX latency. Table + CSV.
- Execution-time model (`SilOs_Et.c`, `et_model.yaml`, `--et-apply=start|return`).
- `docs/tracing.md`: Perfetto/Trace Compass walkthroughs; `perf record`, callgrind, gcov recipes.
- Verify: with `Task_10ms` ET = 3 ms the 1 ms task preempts it in Perfetto and `Task_100ms` response
  time grows in the stats CSV (asserted in pytest).

### M6: real SIP integration kit, Windows polish
- `cmake/Sip.cmake`: glob `BSW/*/*.c` + `GenData/*.c`, exclude `^(Os|Can|Lin|Spi|Mcu|Port|Dio|Gpt|Icu|
  Pwm|Adc|Wdg|Fls|Eep|vBrs|vLinkGen)(_|\.)` and OS-generated `Os_*` GenData files, `sil/hal` first on the
  include path, generators as build steps.
- `tools/check_sip.py`: inventories `Os_*`/MCAL symbols and private OS headers used by the SIP
  (`grep -rhoE '\bOs_[A-Za-z0-9_]+\s*\('`), reports gaps for `SilOs_Compat.c` and compat headers
  (`Os_Types_Lcfg.h`, `Os_Hal_*`), flags `vMem`/`vMemAccM` instead of `Fls`, prints the CanIf infix.
- `docs/integrating-microsar.md`: EcuM startup/`main` mapping, Det routing (`--wrap=Det_ReportError`
  on GCC, Det callout on MSVC), SchM exclusive areas on spinlocks, enabling VFB tracing in DaVinci,
  NvM dir, variant handling, licence note.
- Windows: optional Vector XL virtual-channel backend (CANalyzer/CANoe without SIL Kit), high-resolution
  waitable timer for realtime mode, MSVC warning map, `docs/windows.md`.
- Verify (on the target project): real project builds with `-DSIL_SIP_DIR -DSIL_GENDATA_DIR`, reaches
  EcuM RUN, Com periodic PDUs on CANoe/`vcan0`, UDS `0x22` via python-udsoncan answered.

## Windows effort and risk

- The Windows-specific surface is small because of `sil/port` + C++17 std: Winsock init and a
  ~100-line socket shim, `QueryPerformanceCounter`/`QueryThreadCycleTime`, high-resolution waitable
  timer for realtime mode, `CreateProcess` re-exec, MSVC flags (`/std:c11 /Zc:preprocessor`), msys2 for
  MinGW CI. SIL Kit is loaded dynamically, so MSVC/MinGW runtime mixing is avoided.
- No SocketCAN on Windows: external tooling goes through SIL Kit (CANoe), the UDP bus (python-can via
  our plugin), or the optional Vector XL virtual channel backend.
- Estimated extra effort for Windows from day one: ~15-20% of total. Retrofitting later would be
  ~35-45% because of threading/socket/timer assumptions and vendor-code warning noise.
- Riskiest Windows items: realtime-mode timer precision, SIL Kit registry availability in Windows CI,
  and whether the specific SIP compiles warning-clean under MSVC (MISRA C90 code normally does; MinGW-w64
  is the GCC-parity fallback).

## Risks and unknowns
- Vector licence: running the SIP on a host/in CI may need Vector's consent; the stand-in keeps the repo clean.
- Hidden OS/HW coupling in generated code (Vector-OS-private headers, `Os_Hal`, vector tables):
  mitigated by `check_sip.py` and generated compatibility headers.
- `vMem`/`vMemAccM` stacks need a vMem emulation (same file-backed core as Fls).
- 64-bit pitfalls in vendor/integration code: cast warnings as errors, UBSan, `-m32` job.
- Multi-core inside one tick is nondeterministic by design; `--lockstep-cores` for debugging.
- MCAL ISR naming varies per derivative; `irq_map.yaml` override.
- LIN with open-source tooling has no standard; the UDP LIN bus is project-specific, SIL Kit is the
  interoperable path.
- SocketCAN tests are timing-dependent: CI integration tests run on UDP/SIL Kit backends, vcan tests are smoke.
- Trace volume with large RTEs: lossy ring with overflow counters and per-event filters.
- Determinism breakers in application code (`rand()`, host time, pointer values): documented; `--seed`.
- SIL Kit v5 C API drift: pin version, dynamic binding, test against 5.0.x.
- Optional realistic stand-in: the open `ssas-public` AUTOSAR BSW is "evaluation and study only" and
  must not be vendored; it can be used locally to exercise Com/Dcm flows before the SIP.

## Verification summary
- Unit: `ctest` (OSEK semantics incl. IRQ masking/nesting, trace writers) + `pytest tools/` (ECUC parser,
  generators reproduce hand-written reference byte-identically).
- Integration: `pytest tests/sil` on the demo (Linux: SocketCAN + UDP; Windows: UDP), incl. SPI IRQ
  timing, Icu edge -> ISR -> task, NvM persistence across re-exec reset, time-stepping determinism
  (same trace hash on repeated runs in single-core and lockstep modes).
- Co-simulation: SIL Kit registry + vcan adapter in Linux CI; CANoe manually from Windows.
- Profiling: BTF opens in Trace Compass; Perfetto JSON in ui.perfetto.dev; stats CSV asserted.
- Sanitizers: ASan (single core) and TSan (2-core demo) jobs green.

## References
- Vector SIL Kit: https://github.com/vectorgrp/sil-kit (C API headers under `SilKit/include/silkit/capi`)
- SIL Kit vcan adapter: https://github.com/vectorgrp/sil-kit-adapters-vcan
- BTF specification: https://wiki.eclipse.org/images/e/e6/TA_BTF_Specification_2.1.3_Eclipse_Auto_IWG.pdf
- AUTOSAR ARTI: https://www.autosar.org/fileadmin/standards/R25-11/CP/AUTOSAR_CP_SWS_ARTI.pdf
- MICROSAR VFB tracing workflow (iSYSTEM): http://www.isystem.com/downloads/winIDEA/help/vector-runnable-profiling-instrumentation.html
- Linux LIN status (not merged): https://lwn.net/Articles/972207/
- python-can interfaces: https://python-can.readthedocs.io/en/main/interfaces/udp_multicast.html
