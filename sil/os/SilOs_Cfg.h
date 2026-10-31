/*
 * SilOs_Cfg.h - configuration tables consumed by SilOs.
 *
 * tools/gen_os_cfg.py emits an Os_Lcfg.c that defines `const SilOs_Config SilOs_Cfg`
 * from the project's ECUC (OsTask, OsIsr, OsAlarm, OsCounter, OsResource, OsSpinlock,
 * OsApplication, OsScheduleTable, OsBarrier, OsIoc containers).
 */
#ifndef SILOS_CFG_H
#define SILOS_CFG_H

#include "Os_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SILOS_INVALID_ID (0xFFFFFFFFu)
#define SILOS_MAX_CORES (8u)

typedef void (*SilOs_EntryFn)(void);

typedef enum { SILOS_SCHED_FULL = 0, SILOS_SCHED_NON = 1 } SilOs_ScheduleType;
typedef enum { SILOS_ALARM_ACTIVATETASK = 0, SILOS_ALARM_SETEVENT, SILOS_ALARM_CALLBACK, SILOS_ALARM_INCREMENTCOUNTER } SilOs_AlarmActionType;
typedef enum { SILOS_LOCK_NOTHING = 0, SILOS_LOCK_ALL_INTERRUPTS, SILOS_LOCK_CAT2_INTERRUPTS, SILOS_LOCK_WITH_RES_SCHEDULER } SilOs_SpinlockMethodType;
typedef enum { SILOS_AUTOSTART_NONE = 0, SILOS_AUTOSTART_ABSOLUTE, SILOS_AUTOSTART_RELATIVE, SILOS_AUTOSTART_SYNCHRON } SilOs_AutostartKind;

typedef struct {
    const char *name;
    SilOs_EntryFn entry;      /* Os_Task_<name>, NULL for the idle task */
    uint32 priority;          /* OsTaskPriority, higher = more important */
    uint16 core;
    uint16 app;
    uint8 schedule;           /* SilOs_ScheduleType */
    uint8 extended;           /* has OsTaskEventRef */
    uint8 isIdle;             /* OS internal idle task of the core */
    uint8 activations;        /* OsTaskActivation */
    uint32 autostartModes;    /* bit i set: autostart in AppMode i */
    uint32 internalResource;  /* SILOS_INVALID_ID if none */
    uint32 stackSize;
    EventMaskType ownedEvents;
} SilOs_TaskCfg;

typedef struct {
    const char *name;
    SilOs_EntryFn entry;      /* Os_Isr_<special name or name> */
    uint16 core;
    uint16 app;
    uint32 priority;          /* OsIsrInterruptPriority, higher = more urgent */
    uint8 category;           /* 1 or 2 */
    uint8 nesting;            /* OsIsrEnableNesting */
    uint8 initialEnabled;     /* OsIsrInitialEnableInterruptSource */
    uint8 kind;               /* 0 user, 1 system timer, 2 xsignal, 3 SIL soft ISR (peripheral callbacks) */
    uint32 source;            /* OsIsrInterruptSource (informational) */
    const char *handler;      /* OsIsrSpecialFunctionName (driver handler) or NULL_PTR */
} SilOs_IsrCfg;

typedef struct {
    const char *name;
    uint16 core;
    uint16 app;
    TickType maxAllowedValue;
    TickType ticksPerBase;
    TickType minCycle;
    uint64 nsPerTick;         /* OsSecondsPerTick * 1e9 */
    uint8 isHardware;
    uint32 timerIsr;          /* ISR id driving this counter (hardware counters) */
} SilOs_CounterCfg;

typedef struct {
    const char *name;
    uint32 counter;
    uint8 action;             /* SilOs_AlarmActionType */
    uint8 autostart;          /* SilOs_AutostartKind */
    uint16 app;
    uint32 autostartModes;
    uint32 task;              /* ACTIVATETASK / SETEVENT */
    EventMaskType eventMask;  /* SETEVENT */
    uint32 incCounter;        /* INCREMENTCOUNTER */
    SilOs_EntryFn callback;   /* CALLBACK */
    TickType start;           /* autostart start/offset */
    TickType cycle;           /* autostart cycle */
} SilOs_AlarmCfg;

typedef struct {
    const char *name;
    uint32 ceiling;           /* ceiling priority among accessing tasks */
    uint16 core;
    uint8 isrShared;          /* accessed from ISRs: masks cat2 while held */
    uint8 internal;           /* OsResourceProperty INTERNAL */
} SilOs_ResourceCfg;

typedef struct {
    const char *name;
    uint8 lockMethod;         /* SilOs_SpinlockMethodType */
    uint32 successor;         /* SILOS_INVALID_ID if none */
} SilOs_SpinlockCfg;

typedef struct {
    const char *name;
    uint16 attendees;
} SilOs_BarrierCfg;

typedef struct {
    const char *name;
    uint16 core;
    uint8 trusted;
} SilOs_AppCfg;

typedef struct {
    TickType offset;
    uint16 numTasks;
    const uint32 *tasks;
    uint16 numEvents;
    const uint32 *eventTasks;
    const EventMaskType *eventMasks;
} SilOs_ExpiryPointCfg;

typedef struct {
    const char *name;
    uint32 counter;
    TickType duration;
    uint8 repeating;
    uint8 autostart;          /* SilOs_AutostartKind */
    uint16 app;
    TickType autostartOffset;
    uint32 autostartModes;
    uint16 numExpiryPoints;
    const SilOs_ExpiryPointCfg *expiryPoints;
} SilOs_SchedTblCfg;

typedef struct {
    void (*startupHook)(void);
    void (*shutdownHook)(StatusType error);
    void (*errorHook)(StatusType error);
    void (*preTaskHook)(void);
    void (*postTaskHook)(void);
    ProtectionReturnType (*protectionHook)(StatusType error);
    void (*panicHook)(Os_PanicStatusType status);
} SilOs_HooksCfg;

typedef struct {
    uint8 numCores;
    uint8 masterCore;
    uint8 extendedStatus;
    uint8 numAppModes;
    const char *const *appModeNames;
    uint16 numTasks;
    const SilOs_TaskCfg *tasks;
    uint16 numIsrs;
    const SilOs_IsrCfg *isrs;
    uint16 numCounters;
    const SilOs_CounterCfg *counters;
    uint16 numAlarms;
    const SilOs_AlarmCfg *alarms;
    uint16 numResources;
    const SilOs_ResourceCfg *resources;
    uint16 numSpinlocks;
    const SilOs_SpinlockCfg *spinlocks;
    uint16 numBarriers;
    const SilOs_BarrierCfg *barriers;
    uint16 numApps;
    const SilOs_AppCfg *apps;
    uint16 numSchedTbls;
    const SilOs_SchedTblCfg *schedTbls;
    SilOs_HooksCfg hooks;
} SilOs_Config;

extern const SilOs_Config SilOs_Cfg;

#ifdef __cplusplus
}
#endif

#endif /* SILOS_CFG_H */
