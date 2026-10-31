# microsarSIL

Host-executable virtual ECU for Vector MICROSAR Classic projects: run the BSW + RTE + SWCs of an
AUTOSAR Classic ECU on a PC (Windows or Linux), with an emulated multi-core MICROSAR OS, virtual
time, interrupt injection, bus and peripheral models, and task/runnable level tracing for timing
analysis. No Vector or NXP code is part of this repository; a real project is linked in through
CMake options (see `docs/integrating-microsar.md`).

Reference project: the BR1 Cabin ECU (`C:\br1_cabin_ecu`, MICROSAR Classic on NXP S32E288, two
Cortex-R52 cores). Its inventory is in `docs/reference-project-br1-cabin.md`; the plan in `PLAN.md`.

## What works today (M0 to M4 of the plan)

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
- `tools/check_sip.py` inventories a MICROSAR project (OS API, Vector extensions, MCAL API surface);
  `tools/gen_mcal_cfg.py` generates the MCAL symbolic names (`DioConf_*`, `SpiConf_*`, ...);
  `tools/probe_sip_compile.py` syntax-checks real project sources on the host. Result for the
  Cabin ECU: 43 of 45 probed files (all Vector BSW modules, the full RTE, CDDs) compile with the
  SIL headers (`sil/hal`, `sil/mcal/inc`) and generated configuration (see `docs/integrating-microsar.md`).
- `sil/mcal/src/Can_43_FLEXCAN.c` **virtual CAN driver** with the NXP RTD API names
  (`Can_43_FLEXCAN_Init/SetControllerMode/Write/MainFunction_*`, error state, interrupt control) and
  the FlexCAN handler entry points (`CAN<n>_ORED_*_MB_IRQHandler`, ...) that the OS wrappers call.
  Controllers, hardware objects and filters come from the project's `Can` ECUC through
  `tools/gen_can_cfg.py` (verified on the Cabin ECU: 6 controllers, 129 HOHs, 262 filters). RX reaches
  `CanIf_RxIndication` in ISR context, TX confirmations arrive after the frame's bus time, bus-off can
  be injected and recovered.
- `sil/bus` **bus backends**: `null`, `loop:<name>` (in-process link between controllers) and
  `udp[:<group>:<port>]` (multicast, Windows and Linux; wire format in `sil_bus.h`).
- `sil/ctrl` **control server** (`--control`): newline JSON over TCP to step virtual time exactly
  (`run_until`, `step`, `pause`, `resume`), read statistics, inject and capture CAN frames, raise
  ISRs, force bus-off, write traces and stop the ECU.
- `python/microsarsil`: `SilProcess` (starts an ECU and finds its control port), `SilControl` (the
  commands above), `SilUdpSocket` (the UDP bus without dependencies), a python-can interface
  (`can.Bus(interface="silcan", channel="239.0.0.1:52000")`) and pytest fixtures (`sil_exe`, `sil_ecu`).
- `sil/mcal/src` **virtual MCAL** with the NXP RTD API names: Dio/Port, Adc (group conversions with
  virtual conversion time and notifications), Pwm, Icu (edge counting, period/duty measurement,
  timestamps, edge notifications from a per-channel signal generator or a level set from outside),
  Gpt, Spi (sequences/jobs/channels from the ECUC, synchronous and asynchronous transfers, slave
  models: echo, zero, ones, register file, or a C callback), I2c (register file slaves), Uart (captured
  and echoed to the log), file backed Fee and MemAcc (persist across runs in `--nv=<dir>`), Lin master
  channels (slave responses from a table), Mcu/Platform/OsIf/Pit_Ip/Mcl stubs with `Mcu_PerformReset`
  re-executing the process. Tables come from the project's MCAL ECUC files through
  `tools/gen_periph_cfg.py` (verified on the Cabin ECU: 84 Dio channels, 7 ADC groups, 39 SPI sequences,
  117 Fee blocks, ...). Notifications run in ISR context through an OS internal soft ISR per core.
- `sil/periph/SilIo.c` **virtual I/O registry**: every pin, analog input, PWM output and capture input is
  an entry test scripts set and read through the control server (`io_set`, `io_get`, `io_list`) by key
  (`dio.5`, `adc.1`, `icu.0.freq`) or generated symbolic name (`DioConf_DioChannel_BUTTON`); `--io=` sets
  initial values. Peripheral traffic (SPI, I2C, UART, LIN) is captured for assertions (`periph_recv`).
- `demo/` two core stand-in ECU with two CAN controllers wired back to back and a CanIf stand-in,
  every virtual peripheral in use (button to LED, ADC to PWM, ICU speed, GPT timeout, SPI sensor,
  I2C temperature, UART status line, LIN schedule, Fee odometer, MemAcc boot counter),
  `tests/unit/test_os.c` 103 OSEK semantics checks, `tests/sil` pytest (control server, time stepping
  determinism, CAN over the in-process link and over UDP), CI on Linux (gcc, clang+ASan, TSan) and
  Windows (MinGW, MSVC).

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

## Drive the demo from Python

```
python -m pytest -q tests/sil        # finds build/*/demo/demo_ecu(.exe) or uses MICROSARSIL_DEMO_EXE
```

```python
import sys; sys.path.insert(0, "python")
from microsarsil import SilProcess
with SilProcess("build/windows-mingw-br1tools/demo/demo_ecu.exe", paused=True) as ecu:
    c = ecu.control
    c.run_until(0.5)                        # exact: t = 500 ms and nothing in flight
    c.can_send(0, 0x201, bytes([1, 2]))     # a frame into controller 0
    c.step(0.002)
    print([f for f in c.can_recv() if f.dir == "tx"])   # the 0x301 reply
    print(c.stats()["tasks"])
```

`demo_ecu --io=adc.1=100 --io=icu.0.freq=500 --nv=build` starts with a temperature of 100 counts and a
500 Hz speed signal and keeps the Fee/MemAcc images in `build/`; the UART status line shows the result.
`demo_ecu --can0=udp --time=realtime` puts controller 0 on the UDP bus (239.0.0.1:52000), where
python-can (`interface="silcan"`) or any other process using the same datagram format sees it.

## Layout

```
sil/port      host abstraction (threads, semaphores, clocks) in C++17 behind a C API
sil/hal       Compiler.h, Platform_Types.h, Std_Types.h, MemMap.h for the host
sil/os        SilOs (Os_Api.h public API, SilOs.h SIL specific API, compat/ MICROSAR headers)
sil/trace     recorder and writers
sil/runtime   command line runtime (SilRt_Main, --control, --paused, --can<N>=)
sil/ctrl      control server (newline JSON over TCP)
sil/bus       CAN bus backends: null, loop (in-process), UDP multicast
sil/mcal/inc  virtual MCAL API headers (AUTOSAR + NXP RTD names): Can_43_FLEXCAN, Spi, Adc, Dio, Port, Gpt, Icu, Pwm, Mcu, ...
sil/mcal/src  virtual MCAL: Can_43_FLEXCAN, Dio/Port, Adc, Pwm, Icu, Gpt, Spi, CDD_I2c/CDD_Uart, Fee, MemAcc, Lin, Mcu & co.
sil/periph    virtual I/O registry (SilIo)
tools/        ecuc.py, gen_os_cfg.py, gen_can_cfg.py, gen_periph_cfg.py, gen_mcal_cfg.py, check_sip.py, probe_sip_compile.py, siltrace.py, ecuc_build.py
python/       microsarsil package (SilProcess, SilControl, UDP bus, python-can interface, pytest fixtures)
demo/         DemoEcu.arxml + DemoEcu_Can.arxml + DemoEcu_Mcal.arxml (built by make_demo_ecuc.py) and the demo application
tests/        unit/test_os.c (OS semantics), tools/ (pytest for the generators), sil/ (pytest against the demo ECU)
docs/         reference project inventory, architecture, integration guide
```
