/*
 * SilTrace.h - lightweight per-core event recorder.
 *
 * Every OS and runtime event is a fixed 32-byte record in a per-core ring
 * buffer. Records carry the virtual time (ns) and the host monotonic time
 * (ns), so both the schedule and the host cost can be visualised. Names of
 * objects are kept in a string table so that records stay small.
 */
#ifndef SIL_TRACE_H
#define SIL_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SILTRACE_EV_NONE = 0,
    SILTRACE_EV_TASK_ACTIVATE,   /* id = task, aux = activating context id or 0xFFFFFFFF */
    SILTRACE_EV_TASK_START,      /* id = task */
    SILTRACE_EV_TASK_PREEMPT,    /* id = task preempted, aux = preempting context */
    SILTRACE_EV_TASK_RESUME,     /* id = task */
    SILTRACE_EV_TASK_TERMINATE,  /* id = task */
    SILTRACE_EV_TASK_WAIT,       /* id = task, aux = event mask low 32 bit */
    SILTRACE_EV_TASK_RELEASE,    /* id = task released from waiting, aux = mask */
    SILTRACE_EV_TASK_ACT_FAIL,   /* id = task, aux = E_OS_LIMIT */
    SILTRACE_EV_IRQ_RAISE,       /* id = isr */
    SILTRACE_EV_ISR_START,       /* id = isr */
    SILTRACE_EV_ISR_END,         /* id = isr */
    SILTRACE_EV_ALARM_SET,       /* id = alarm, aux = expiry tick */
    SILTRACE_EV_ALARM_CANCEL,    /* id = alarm */
    SILTRACE_EV_ALARM_EXPIRE,    /* id = alarm, aux = tick */
    SILTRACE_EV_EVENT_SET,       /* id = task, aux = mask low 32 bit */
    SILTRACE_EV_RES_GET,         /* id = resource */
    SILTRACE_EV_RES_RELEASE,     /* id = resource */
    SILTRACE_EV_SPIN_REQ,        /* id = spinlock */
    SILTRACE_EV_SPIN_GOT,        /* id = spinlock, aux = spin iterations */
    SILTRACE_EV_SPIN_REL,        /* id = spinlock */
    SILTRACE_EV_XSIG_SEND,       /* id = target core, aux = service */
    SILTRACE_EV_XSIG_RECV,       /* id = source core, aux = service */
    SILTRACE_EV_BARRIER_WAIT,    /* id = barrier */
    SILTRACE_EV_BARRIER_PASS,    /* id = barrier */
    SILTRACE_EV_IDLE_START,      /* core idle */
    SILTRACE_EV_IDLE_END,
    SILTRACE_EV_RUNNABLE_START,  /* id = runnable id (string table kind RUNNABLE) */
    SILTRACE_EV_RUNNABLE_STOP,
    SILTRACE_EV_INT_LOCK,        /* id = kind (0 all,1 suspend all,2 suspend os), aux = nesting */
    SILTRACE_EV_INT_UNLOCK,
    SILTRACE_EV_CLOCK_JUMP,      /* aux = jump length in us */
    SILTRACE_EV_OS_ERROR,        /* id = StatusType, aux = service id */
    SILTRACE_EV_HOOK,            /* id = hook kind */
    SILTRACE_EV_USER,            /* id, aux free */
    SILTRACE_EV_BUS_TX,          /* id = controller, aux = can id */
    SILTRACE_EV_BUS_RX,
    SILTRACE_EV_COUNT
} SilTrace_EventType;

typedef enum {
    SILTRACE_NAME_CORE = 0,
    SILTRACE_NAME_TASK,
    SILTRACE_NAME_ISR,
    SILTRACE_NAME_ALARM,
    SILTRACE_NAME_RESOURCE,
    SILTRACE_NAME_SPINLOCK,
    SILTRACE_NAME_BARRIER,
    SILTRACE_NAME_RUNNABLE,
    SILTRACE_NAME_COUNTER,
    SILTRACE_NAME_USER,
    SILTRACE_NAME_KINDS
} SilTrace_NameKind;

typedef struct {
    uint64_t t_ns;      /* virtual time */
    uint64_t host_ns;   /* host monotonic time */
    uint32_t id;
    uint32_t aux;
    uint16_t type;      /* SilTrace_EventType */
    uint8_t core;
    uint8_t ctx_kind;   /* 0 idle, 1 task, 2 isr, 3 foreign */
    uint32_t ctx_id;    /* current context id at the time of the event */
} SilTrace_Record;      /* 32 bytes */

/* Initialise: numCores ring buffers of recordsPerCore records. Safe to call once. */
void SilTrace_Init(unsigned numCores, size_t recordsPerCore);
void SilTrace_Shutdown(void);
int SilTrace_IsEnabled(void);
void SilTrace_Enable(int on);

/* Current-time provider: the OS registers its virtual clock here. */
typedef uint64_t (*SilTrace_ClockFn)(void);
void SilTrace_SetClock(SilTrace_ClockFn fn);
/* Context provider (kind, id) for the calling thread, registered by the OS. */
typedef void (*SilTrace_CtxFn)(uint8_t *kind, uint32_t *id);
void SilTrace_SetContextProvider(SilTrace_CtxFn fn);

void SilTrace_Event(unsigned core, SilTrace_EventType type, uint32_t id, uint32_t aux);
/* Same but with an explicit timestamp (used when the time is known precisely). */
void SilTrace_EventAt(unsigned core, uint64_t t_ns, SilTrace_EventType type, uint32_t id, uint32_t aux);

void SilTrace_SetName(SilTrace_NameKind kind, uint32_t id, const char *name);
const char *SilTrace_GetName(SilTrace_NameKind kind, uint32_t id);
const char *SilTrace_EventName(SilTrace_EventType type);

/* Statistics */
uint64_t SilTrace_Count(unsigned core);
uint64_t SilTrace_Dropped(unsigned core);

/* Writers. All return 0 on success. The `extra` string table is embedded. */
int SilTrace_WriteSiltrace(const char *path);
int SilTrace_WritePerfetto(const char *path);
int SilTrace_WriteBtf(const char *path);
/* Rimac profiling format: <dir>/core<n>.bin (52-bit tick << 12 | id) plus
 * <dir>/id_runnables.xml and <dir>/id_isr.xml using the id mapping of the
 * runnable/ISR string tables. tick_hz is the emulated lifetimer frequency. */
int SilTrace_WriteRimacBin(const char *dir, uint64_t tick_hz);

/* Iterate records in chronological order per core (for tests/tools). */
typedef void (*SilTrace_VisitFn)(const SilTrace_Record *r, void *user);
void SilTrace_Visit(unsigned core, SilTrace_VisitFn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* SIL_TRACE_H */
