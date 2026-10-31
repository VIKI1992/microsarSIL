# Architecture

## Threads and the run token

Every OS task and every Cat2 ISR is a host thread; every core has one more thread that is the
init context before `StartOS` and the idle context afterwards. A single global lock protects the
OS state. Per core exactly one context holds the *run token*: it is the only thread of that core
that executes ECU code. A context switch (`silos_switch`) sets `core->current`, posts the target's
semaphore and waits on its own. The scheduler (`silos_pick`) is evaluated at every switch point:
every OS service call, ISR end, `SilOs_PreemptionPoint()`, `SilOs_ConsumeTime()`, spin loops and
the idle loop.

`silos_pick` returns, in this order: the most urgent deliverable pending ISR (interrupt locks of
the running context, per-ISR enable flag, nesting flag and priority of the running ISR are
honoured), the running ISR, the highest priority READY/RUNNING task (FIFO within a priority,
non-preemptable tasks count as highest while running, resource ceilings raise the priority), the
idle context.

Basic tasks terminate with `longjmp` back to their thread loop, so a task body that returns
without `TerminateTask` is reported as `E_OS_MISSINGEND` and the thread is reused for the next
activation.

## Virtual time

`SilOs_ClockNow()` is a 64-bit nanosecond clock. In `virtual` mode it only moves when no core
can make progress: every started core is idle or spinning with an unsatisfied predicate, no
pending ISR is deliverable, no cross-core request is queued and no consume target is reached.
Then the clock jumps to the earliest of: next alarm or schedule-table expiry of any hardware
counter, next one-shot virtual timer, next `SilOs_ConsumeTime` target, the `--duration` limit.
Counter values derive from the clock (`ticks = now / nsPerTick`), so counters of all cores are
synchronous like the EL1 timers of the reference ECU. Expiries raise the counter's timer ISR
(`CounterIsr_SystemTimer_*` with special function `Os_TimerPfrtIsr` in MICROSAR), which processes
alarms and schedule tables in ISR context, exactly like the target.

In `realtime[:factor]` mode a driver thread fires due expiries while the clock follows the host
clock. Spin loops (spinlocks, barriers, cross-core waits) and consume loops let the core serve
interrupts and higher priority tasks while waiting.

## Cross core

`SetEvent`/`ActivateTask` on a task of another core enqueue a request to that core and raise its
XSignal ISR (`XSignalIsr_OsCore<n>`, special function `Os_XSigRecvIsrHandler`); the caller spins
until the request is done (synchronous XSignal as configured in the reference project). A waiting
core also serves requests addressed to itself, which avoids the deadlock of two cores signalling
each other from ISRs with nesting disabled. Alarm services on remote counters are executed
directly under the lock; spinlocks apply their lock method (`LOCK_CAT2_INTERRUPTS` masks Cat2
delivery on the holding core) before spinning, as MICROSAR does.

## Tracing

`SilTrace_Event` writes 32-byte records (virtual ns, host ns, type, core, id, aux, current context)
into a per-core ring. Events: task activate/start/preempt/resume/terminate/wait/release,
IRQ raise, ISR start/end, alarm set/cancel/expire, event set, resource get/release, spinlock
req/got/release, XSignal send/receive, barrier wait/pass, idle, runnable start/stop, OS errors,
clock jumps, user markers. Writers: `.siltrace`, Perfetto JSON (two processes: virtual schedule
and host cost), BTF 2.1.3, Rimac profiling `.bin` (52-bit lifetimer ticks at 100 MHz, 12-bit
marker id; runnable n -> markers 2n/2n+1, ISR n -> 2048+2n/2048+2n+1) with `id_runnables.xml`
and `id_isr.xml`.

## Configuration flow

`tools/ecuc.py` parses standard ECUC value XML (DaVinci export). `tools/gen_os_cfg.py` builds a
model (cores from `OsCore`/`EcucCoreDefinition`, OS-Applications and their core, tasks, ISRs with
`OsIsrSpecialFunctionName`, counters with their timer ISR, alarms with actions and autostart,
resources with computed ceilings, spinlocks with lock methods, barriers, schedule tables with
expiry points, IOC channels) and emits `Os_Types_Lcfg.h` (handle enums), `Os_Cfg.h` (event masks,
`OS_CFG_*`), `Os_Hal_Cfg.h` (`OS_MS2TICKS_<counter>`), `Os_Lcfg.h/.c` (prototypes, `SilOs_Cfg`
tables, `Os_Isr_<fn>` wrappers for driver handlers), `Os_Ioc_Lcfg.h/.c`, `Os_Stack_Cfg.h`,
`Os_Compiler_Cfg.h`. Event masks are assigned by greedy colouring so that all events of one task
have distinct bits (explicit `OsEventMask` values are kept). ISR priorities follow the ARM GIC
convention (lower value = more urgent) unless `--isr-prio-order=higher-is-higher`.

## Virtual CAN and bus backends

`sil/mcal/src/Can_43_FLEXCAN.c` implements the NXP RTD CAN API on top of `sil/bus`. Each controller
has a host mutex, an RX queue, a TX-confirmation queue and per-HOH in-flight counters. A frame from
the backend (the UDP receive thread, or the sender's thread for the in-process link) is queued and
the controller's RX ISR is raised with `SilOs_RaiseIrq`; the OS runs the generated handler
(`CAN<n>_ORED_0_31_MB_IRQHandler` -> `SilCan_Isr`), which matches FULL objects first, then BASIC
filters, and calls `CanIf_RxIndication` in ISR context. `Can_43_FLEXCAN_Write` sends immediately
and schedules a virtual timer for the frame time (47/67 control bits at the nominal rate, data bits
at the FD rate); its callback queues the confirmation and raises the TX ISR. Driver mutexes are
never held while an ISR is raised, because the ISR takes the same mutex. ISRs are found by the
handler name in `OsIsrSpecialFunctionName` (`SilOs_IsrByHandler`), so the OS tables of a MICROSAR
project match without renaming. `tools/gen_can_cfg.py` emits `SilCan_Cfg.c` (controllers, hardware
objects, filters, handler functions) from the `Can` ECUC; `silos_add_ecu()` runs it together with
`gen_os_cfg.py` and `gen_mcal_cfg.py`.

## Control server

`sil/ctrl` serves newline delimited JSON on TCP (`--control`). Time stepping rests on two OS hooks:
`SilOs_SetTimeLimit` caps the virtual clock (the time driver never jumps past it, so alarms at or
after the limit wait while zero-time activity still runs) and `SilOs_WaitQuiescent` blocks a host
thread until the clock reached the target and every core is idle with nothing due and no timer
callback in flight. `run_until` is therefore exact and repeatable: the same command sequence yields
the same task activations, ISR runs and bus frames. External events (UDP frames, `can_send`, `irq`)
that arrive while paused are processed at the paused time. The socket code is a separate
translation unit (`sil_ctrl_net.cpp`) because `windows.h` declares `SetEvent(HANDLE)`, which clashes
with the OS API, and the AUTOSAR `CONST` macro breaks `winnt.h`.

## Not emulated

Memory protection, timing protection, service protection, Cat1 ISRs, stack monitoring (stack
usage queries return constants), ORTI/ARTI, OS-Application termination/restart, synchronised
schedule tables.
