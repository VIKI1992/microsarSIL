/*
 * SilOs_Internal.h - internal data structures shared by the SilOs sources.
 *
 * Threading model: one host thread per task, per ISR and per core (the core
 * thread is the init/idle context). A single global lock protects all OS
 * state. Exactly one context per core holds the core's "run token"; a context
 * switch hands the token over with per-context semaphores.
 */
#ifndef SILOS_INTERNAL_H
#define SILOS_INTERNAL_H

#include "SilOs.h"
#include "SilTrace.h"
#include "sil_port.h"

#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SILOS_CTX_IDLE 0u
#define SILOS_CTX_TASK 1u
#define SILOS_CTX_ISR 2u

#define SILOS_MAX_NEST 16u
#define SILOS_PRIO_MAX 0xFFFFFFF0u
#define SILOS_STATE_BUSY 0u
#define SILOS_STATE_IDLE 1u
#define SILOS_STATE_WAITING 2u

struct SilOs_Core;

typedef struct SilOs_Ctx {
    uint8 kind;
    uint32 id;
    struct SilOs_Core *core;
    sil_thread_t *thread;
    sil_sem_t *run;
    /* interrupt lock state owned by this context */
    uint32 disableAll;
    uint32 suspendAll;
    uint32 suspendOs;
    uint32 extraSuspendOs;   /* from isr-shared resources / CAT2 spinlocks */
    uint32 extraSuspendAll;  /* from ALL_INTERRUPTS spinlocks */
    uint8 holdsToken;
    uint8 inErrorHook;
    jmp_buf jb;
} SilOs_Ctx;

typedef struct SilOs_Task {
    const SilOs_TaskCfg *cfg;
    SilOs_Ctx ctx;
    uint8 state;             /* TaskStateType */
    uint8 started;           /* body started since last activation */
    uint32 actCount;         /* queued activations incl. running */
    uint64 seq;              /* FIFO order within priority */
    uint32 curPrio;
    uint32 prioStack[SILOS_MAX_NEST];
    uint8 prioDepth;
    uint32 resStack[SILOS_MAX_NEST];
    uint8 resDepth;
    uint32 spinStack[SILOS_MAX_NEST];
    uint8 spinDepth;
    EventMaskType events;
    EventMaskType waitMask;
    uint64 activations;
    uint8 chainTo;           /* ChainTask pending */
    uint32 chainTarget;
    uint8 yielding;          /* inside Schedule() */
} SilOs_Task;

typedef struct SilOs_Isr {
    const SilOs_IsrCfg *cfg;
    SilOs_Ctx ctx;
    uint8 pending;
    uint8 enabled;
    uint8 running;
    uint32 urgency;          /* higher = more urgent */
    uint32 resStack[SILOS_MAX_NEST];
    uint8 resDepth;
    uint32 spinStack[SILOS_MAX_NEST];
    uint8 spinDepth;
    uint64 runs;
} SilOs_Isr;

typedef struct {
    uint8 armed;
    Os_TickType64 expiry;    /* absolute ticks of the counter */
    TickType cycle;
} SilOs_Alarm;

typedef struct {
    Os_TickType64 value;     /* software counters */
    uint64 originNs;         /* hardware counters: virtual time at which the counter was 0 */
    uint8 hasNext;
    Os_TickType64 nextExpiry;/* earliest armed alarm / expiry point in abs ticks */
} SilOs_Counter;

typedef struct {
    uint8 locked;
    uint8 ownerKind;
    uint32 ownerId;
    uint32 prevPrio;
} SilOs_Resource;

typedef struct {
    uint8 locked;
    uint32 ownerCore;
    uint8 ownerKind;
    uint32 ownerId;
} SilOs_Spinlock;

typedef struct {
    uint16 arrived;
    uint32 generation;
} SilOs_Barrier;

typedef struct {
    uint8 status;            /* ScheduleTableStatusType */
    Os_TickType64 startTick; /* absolute tick of offset 0 of the current round */
    uint16 nextPoint;        /* index of next expiry point */
    uint32 nextTable;        /* NextScheduleTable target */
} SilOs_SchedTbl;

typedef struct SilOs_XsigReq {
    void (*fn)(struct SilOs_XsigReq *req);
    uint32 a;
    uint32 b;
    EventMaskType mask;
    StatusType result;
    volatile uint32 done;
    uint8 srcCore;
    struct SilOs_XsigReq *next;
} SilOs_XsigReq;

typedef struct {
    uint32 handle;
    uint64 due;
    SilOs_TimerFn fn;
    void *arg;
    uint8 used;
} SilOs_VTimer;

typedef struct SilOs_Core {
    CoreIdType id;
    uint8 started;           /* StartCore called (or master) */
    uint8 osStarted;         /* StartOS completed */
    uint8 autosar;
    uint8 idleState;         /* SILOS_STATE_* */
    SilOs_Ctx idle;          /* init/idle context: the core thread */
    SilOs_Ctx *current;
    uint32 isrStack[SILOS_MAX_NEST];
    uint8 isrDepth;
    uint32 curTask;          /* running or interrupted task */
    uint64 consumeTarget;    /* UINT64_MAX if none */
    int (*waitPred)(void *); /* predicate of the context spinning on this core, NULL if none */
    void *waitArg;
    SilOs_XsigReq *xsigHead;
    SilOs_XsigReq *xsigTail;
    uint32 xsigIsr;
    uint32 timerIsr;
    uint64 seqCounter;
    AppModeType appMode;
    sil_thread_t *thread;
    uint64 idleSinceHost;
} SilOs_Core;

typedef struct {
    sil_mutex_t *lock;
    sil_cond_t *progress;    /* broadcast on every state change */
    SilOs_Core cores[SILOS_MAX_CORES];
    SilOs_Task *tasks;
    SilOs_Isr *isrs;
    SilOs_Alarm *alarms;
    SilOs_Counter *counters;
    SilOs_Resource *resources;
    SilOs_Spinlock *spinlocks;
    SilOs_Barrier *barriers;
    SilOs_SchedTbl *schedTbls;
    const SilOs_Config *cfg;
    uint32 numStartedCores;
    uint32 numOsStarted;
    uint32 numAutostarted;
    AppModeType appMode;
    volatile uint32 shutdown;
    int exitCode;
    SilOs_RuntimeOptions opt;
    SilOs_CoreEntryFn coreEntry;
    SilOs_ShutdownFn onShutdown;
    /* clock */
    volatile uint64 nowNs;
    uint64 hostOriginNs;
    SilOs_VTimer vtimers[64];
    uint32 vtimerNext;
    /* errors */
    Os_ErrorInformationType lastError;
    uint64 errorCount;
    uint8 setupDone;
    sil_thread_t *timeDriver;
} SilOs_Global;

extern SilOs_Global silos_g;
extern SIL_THREAD_LOCAL SilOs_Ctx *silos_self;

/* ---- engine (SilOs_Core.c) --------------------------------------------- */
void silos_lock(void);
void silos_unlock(void);
void silos_notify(void);
SilOs_Ctx *silos_pick(SilOs_Core *core);
void silos_switch(SilOs_Core *core, SilOs_Ctx *next);   /* lock held, caller is current */
void silos_dispatch(SilOs_Core *core);                  /* lock held, caller is current */
void silos_spin_wait(SilOs_Core *core, int (*pred)(void *), void *arg, uint64 targetNs); /* lock held */
void silos_time_maybe_advance(void);                    /* lock held */
void silos_raise_irq_locked(ISRType isr);
StatusType silos_activate_task_locked(SilOs_Core *core, SilOs_Task *t, uint32 fromCtx);
StatusType silos_set_event_locked(SilOs_Task *t, EventMaskType mask);
void silos_xsig_call(CoreIdType target, void (*fn)(SilOs_XsigReq *), uint32 a, uint32 b, EventMaskType mask, StatusType *result);
void silos_error(OSServiceIdType service, StatusType status, uint32 p1, uint32 p2, uint32 p3); /* lock NOT held */
uint64 silos_now_locked(void);
int silos_core_of_ctx(void);
SilOs_Task *silos_current_task(void);
SilOs_Isr *silos_current_isr(void);
void silos_terminate_current_locked(SilOs_Task *t);     /* marks suspended/ready and longjmps to the task loop */
void silos_run_hook_locked(void (*hook)(void));

/* ---- time (SilOs_Time.c) ------------------------------------------------ */
void silos_time_init(void);
Os_TickType64 silos_counter_now_locked(uint32 counter);
uint64 silos_next_event_ns_locked(void);                /* UINT64_MAX if none */
void silos_fire_due_locked(void);
void silos_timer_isr_body(void);
void silos_counter_update_next_locked(uint32 counter);
void silos_autostart_time_locked(SilOs_Core *core);

/* ---- misc --------------------------------------------------------------- */
void silos_xsignal_isr_body(void);
void silos_xsig_process_locked(SilOs_Core *core);

#ifdef __cplusplus
}
#endif

#endif /* SILOS_INTERNAL_H */
