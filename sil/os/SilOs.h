/*
 * SilOs.h - SIL specific interface of the emulated OS: virtual time, interrupt
 * injection, execution-time model, runtime integration. Not part of the AUTOSAR API.
 */
#ifndef SILOS_H
#define SILOS_H

#include "Os_Api.h"
#include "SilOs_Cfg.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SILOS_TIME_VIRTUAL = 0, SILOS_TIME_REALTIME = 1 } SilOs_TimeMode;

typedef struct {
    SilOs_TimeMode timeMode;
    double realtimeFactor;     /* 1.0 = wall clock; 2.0 = twice as fast */
    uint64 durationNs;         /* stop after this virtual time (0 = unlimited) */
    uint8 lockstepCores;       /* reserved */
    uint8 pinCores;            /* pin core threads to host CPUs */
    size_t traceRecordsPerCore;/* 0 = default */
    uint32 seed;
} SilOs_RuntimeOptions;

typedef void (*SilOs_CoreEntryFn)(CoreIdType core);
typedef void (*SilOs_ShutdownFn)(StatusType error, int allCores);
typedef void (*SilOs_TimerFn)(void *arg);

/* Runtime integration ----------------------------------------------------- */
void SilOs_DefaultOptions(SilOs_RuntimeOptions *opt);
/* Prepare the OS emulation; must be called once from the host main thread before any OS API. */
void SilOs_Setup(const SilOs_RuntimeOptions *opt, SilOs_CoreEntryFn coreEntry);
/* Start the master core: creates its host thread and calls coreEntry(master). */
void SilOs_StartMaster(void);
/* Register a callback invoked when ShutdownOS/ShutdownAllCores completes (called from an OS context). */
void SilOs_SetShutdownCallback(SilOs_ShutdownFn fn);
/* Block the host main thread until the OS shut down or the duration limit was reached. Returns exit code. */
int SilOs_WaitShutdown(void);
/* Request shutdown from a host thread (control server, signal handler). */
void SilOs_RequestStop(int exitCode);

/* Virtual time ------------------------------------------------------------ */
uint64 SilOs_ClockNow(void);                 /* ns of virtual time */
SilOs_TimeMode SilOs_ClockMode(void);
/* One-shot virtual timer: fn(arg) is called (from an OS internal context, OS lock not held) when the
 * virtual time reaches now+delay. Typical use: a peripheral model raising an IRQ with latency;
 * callbacks may call SilOs_RaiseIrq and SilOs_TimerSchedule. Returns a handle (>0) or 0. */
uint32 SilOs_TimerSchedule(uint64 delayNs, SilOs_TimerFn fn, void *arg);
void SilOs_TimerCancel(uint32 handle);

/* Interrupt injection ----------------------------------------------------- */
/* Thread safe, callable from any host thread, task, ISR or timer callback. */
void SilOs_RaiseIrq(ISRType isr);
/* Look up objects by name. Return the object count (invalid id) if unknown. */
ISRType SilOs_IsrByName(const char *name);
TaskType SilOs_TaskByName(const char *name);
uint32 SilOs_NumTasks(void);
uint32 SilOs_NumIsrs(void);

/* Execution time model ---------------------------------------------------- */
/* Advance virtual time by `ns` on behalf of the calling context. Alarms and IRQs that fall
 * into the interval are delivered on the way; the context may be preempted meanwhile. */
void SilOs_ConsumeTime(uint64 ns);
/* Explicit scheduling point for long computations: delivers pending interrupts and preempts
 * the caller if a higher priority task became ready. No effect in ISRs with interrupts locked. */
void SilOs_PreemptionPoint(void);

/* Tracing helpers (runnable instrumentation) ------------------------------- */
void SilOs_RunnableStart(uint32 id);
void SilOs_RunnableStop(uint32 id);
void SilOs_UserEvent(uint32 id, uint32 aux);

/* Introspection ----------------------------------------------------------- */
CoreIdType SilOs_CurrentCore(void);     /* OS_CORE_ID_INVALID from foreign threads */
int SilOs_InIsr(void);
int SilOs_IsStarted(CoreIdType core);
uint64 SilOs_TaskActivations(TaskType task);
uint64 SilOs_IsrRuns(ISRType isr);
uint64 SilOs_ErrorCount(void);

#ifdef __cplusplus
}
#endif

#endif /* SILOS_H */
