# microsarSIL

Host-executable virtual ECU for Vector MICROSAR Classic projects: run the BSW + RTE + SWCs of an
AUTOSAR Classic ECU on a PC (Windows or Linux), with an emulated multi-core MICROSAR OS, virtual
time, interrupt injection, bus and peripheral models, and task/runnable level tracing for timing
analysis. No Vector or NXP code is part of this repository; a real project is linked in through
CMake options (see `docs/integrating-microsar.md`).

Reference project: the BR1 Cabin ECU (`C:\br1_cabin_ecu`, MICROSAR Classic on NXP S32E288, two
Cortex-R52 cores). Its inventory is in `docs/reference-project-br1-cabin.md`; the plan in `PLAN.md`.

## What works today (M0 + M1 + M2 of the plan)

- `sil/os` **SilOs**: OSEK/AUTOSAR OS API (tasks basic/extended, FULL/NON, multiple activation,
  events, resources with ceiling, alarms incl. autostart, hardware and software counters, schedule
  tables, Cat2 ISRs with priorities/nesting/masking, interrupt locks, spinlocks with lock methods,
  cross-core ActivateTask/SetEvent through an emulated XSignal ISR, Vector barriers, IOC, hooks,
  EXTENDED status error reporting, Vector extension stubs). N cores run as N schedulers on host
  threads; one host thread per task and ISR with a run token per core.
- `sil/os/SilOs_Time.c` **virtual time**: event driven (time jumps to the next alarm/timer when
  every core is idle or waiting) or real time with a factor; `SilOs_ConsumeTime()` execution-time
  model; one-shot virtual timers for peripheral models.
- `sil/trace` **tracer**: per-core ring buffers, writers for Perfetto JSON (`ui.perfetto.dev`), BTF
  (Trace Compass / Vector TA), `.siltrace` (+ `tools/siltrace.py stats`), and the Rimac profiling
  format used by `Tests/Profiling` of the reference project.
- `tools/gen_os_cfg.py` generates the OS configuration from the project's **ECUC arxml** (the same
  files DaVinci Configurator writes): verified against the demo and the BR1 Cabin ECU
  (18 tasks, 71 ISRs, 101 alarms, 84 spinlocks, 8 IOC channels, 2 cores).
- `tools/check_sip.py` inventories a MICROSAR project (OS API, Vector extensions, MCAL API surface).
- `demo/` two core stand-in ECU, `tests/unit/test_os.c` 103 OSEK semantics checks, CI on
  Linux (gcc, clang+ASan, TSan) and Windows (MinGW, MSVC).

## Build (this machine: MinGW-w64 GCC 12.2 from Br1Tools)

```
cmake --preset windows-mingw-br1tools
cmake --build --preset windows-mingw-br1tools -j 8
ctest --preset windows-mingw-br1tools
```

Other presets: `linux-gcc`, `linux-clang-asan`, `linux-gcc-tsan`, `windows-msvc`, `windows-mingw`.

## Run the demo

```
build/windows-mingw-br1tools/demo/demo_ecu.exe --duration=2 --perfetto=demo.json --btf=demo.btf --trace=demo.siltrace
python tools/siltrace.py stats demo.siltrace
```

Open `demo.json` in https://ui.perfetto.dev (process "ECU schedule" shows tasks, ISRs, runnables
and idle per core in virtual time; process "Host cost" the same lanes in host time).
`--time=realtime:4` runs four times faster than wall clock; `--rimac=<dir>` writes `core<n>.bin`
and `id_*.xml` for `Tests/Profiling/ProfilingAnalyzeData.py`.

## Layout

```
sil/port      host abstraction (threads, semaphores, clocks) in C++17 behind a C API
sil/hal       Compiler.h, Platform_Types.h, Std_Types.h, MemMap.h for the host
sil/os        SilOs (Os_Api.h public API, SilOs.h SIL specific API, compat/ MICROSAR headers)
sil/trace     recorder and writers
sil/runtime   command line runtime (SilRt_Main)
tools/        ecuc.py, gen_os_cfg.py, check_sip.py, siltrace.py, ecuc_build.py
demo/         DemoEcu.arxml (built by make_demo_ecuc.py) and the demo application
tests/        unit/test_os.c (OS semantics), tools/ (pytest for the generators)
docs/         reference project inventory, architecture, integration guide
```
