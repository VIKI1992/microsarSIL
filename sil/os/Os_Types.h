/*
 * Os_Types.h - OSEK/AUTOSAR OS data types for SilOs.
 *
 * Object handle enums (TaskType, ISRType, ...) are generated per project into
 * Os_Types_Lcfg.h by tools/gen_os_cfg.py and included from Os.h.
 */
#ifndef OS_TYPES_H
#define OS_TYPES_H

#include "Std_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OSEK status codes ------------------------------------------------ */
#define E_OS_ACCESS (1u)
#define E_OS_CALLEVEL (2u)
#define E_OS_ID (3u)
#define E_OS_LIMIT (4u)
#define E_OS_NOFUNC (5u)
#define E_OS_RESOURCE (6u)
#define E_OS_STATE (7u)
#define E_OS_VALUE (8u)
/* ---- AUTOSAR status codes --------------------------------------------- */
#define E_OS_SERVICEID (9u)
#define E_OS_ILLEGAL_ADDRESS (10u)
#define E_OS_MISSINGEND (11u)
#define E_OS_DISABLEDINT (12u)
#define E_OS_STACKFAULT (13u)
#define E_OS_PARAM_POINTER (14u)
#define E_OS_PROTECTION_MEMORY (15u)
#define E_OS_PROTECTION_TIME (16u)
#define E_OS_PROTECTION_ARRIVAL (17u)
#define E_OS_PROTECTION_LOCKED (18u)
#define E_OS_PROTECTION_EXCEPTION (19u)
#define E_OS_SPINLOCK (20u)
#define E_OS_INTERFERENCE_DEADLOCK (21u)
#define E_OS_NESTING_DEADLOCK (22u)
#define E_OS_CORE (23u)
#define E_OS_SYS_ASSERTION (24u)
#define E_OS_SYS_API_ERROR (25u)
#define E_OS_SYS_NO_RESTART (26u)
#define E_OS_SYS_UNKNOWN_SERVICE (27u)

/* ---- IOC status codes ------------------------------------------------- */
#define IOC_E_OK (0u)
#define IOC_E_NOK (1u)
#define IOC_E_LIMIT (130u)
#define IOC_E_LOST_DATA (64u)
#define IOC_E_NO_DATA (131u)

/* ---- basic types ------------------------------------------------------ */
typedef uint32 TaskType;
typedef TaskType *TaskRefType;
typedef uint8 TaskStateType;
typedef TaskStateType *TaskStateRefType;
#define RUNNING ((TaskStateType)0u)
#define WAITING ((TaskStateType)1u)
#define READY ((TaskStateType)2u)
#define SUSPENDED ((TaskStateType)3u)

typedef uint64 EventMaskType;
typedef EventMaskType *EventMaskRefType;

typedef uint32 ResourceType;
typedef uint32 ISRType;
typedef uint32 AlarmType;
typedef uint32 CounterType;
typedef uint32 ScheduleTableType;
typedef uint32 AppModeType;
typedef uint32 ApplicationType;
typedef uint32 CoreIdType;
typedef uint32 SpinlockIdType;
typedef uint32 TrustedFunctionIndexType;
typedef uint32 Os_NonTrustedFunctionIndexType;
typedef uint32 Os_FastTrustedFunctionIndexType;
typedef void *TrustedFunctionParameterRefType;
typedef uint32 Os_BarrierIdType;
typedef uint32 Os_PeripheralIdType;
typedef uint32 Os_PrioritySetIdType;

typedef uint32 TickType;
typedef TickType *TickRefType;
typedef uint64 Os_TickType64;

typedef struct {
    TickType maxallowedvalue;
    TickType ticksperbase;
    TickType mincycle;
} AlarmBaseType;
typedef AlarmBaseType *AlarmBaseRefType;

typedef uint8 ScheduleTableStatusType;
typedef ScheduleTableStatusType *ScheduleTableStatusRefType;
#define SCHEDULETABLE_STOPPED ((ScheduleTableStatusType)0u)
#define SCHEDULETABLE_NEXT ((ScheduleTableStatusType)1u)
#define SCHEDULETABLE_WAITING ((ScheduleTableStatusType)2u)
#define SCHEDULETABLE_RUNNING ((ScheduleTableStatusType)3u)
#define SCHEDULETABLE_RUNNING_AND_SYNCHRONOUS ((ScheduleTableStatusType)4u)

typedef uint8 ApplicationStateType;
typedef ApplicationStateType *ApplicationStateRefType;
#define APPLICATION_ACCESSIBLE ((ApplicationStateType)0u)
#define APPLICATION_RESTARTING ((ApplicationStateType)1u)
#define APPLICATION_TERMINATED ((ApplicationStateType)2u)

typedef uint8 RestartType;
#define RESTART ((RestartType)0u)
#define NO_RESTART ((RestartType)1u)

typedef uint8 ObjectAccessType;
#define ACCESS ((ObjectAccessType)0u)
#define NO_ACCESS ((ObjectAccessType)1u)

typedef uint8 ObjectTypeType;
#define OBJECT_TASK ((ObjectTypeType)0u)
#define OBJECT_ISR ((ObjectTypeType)1u)
#define OBJECT_ALARM ((ObjectTypeType)2u)
#define OBJECT_RESOURCE ((ObjectTypeType)3u)
#define OBJECT_COUNTER ((ObjectTypeType)4u)
#define OBJECT_SCHEDULETABLE ((ObjectTypeType)5u)

typedef uint8 AccessType;
typedef uint8 ProtectionReturnType;
#define PRO_IGNORE ((ProtectionReturnType)0u)
#define PRO_TERMINATETASKISR ((ProtectionReturnType)1u)
#define PRO_TERMINATEAPPL ((ProtectionReturnType)2u)
#define PRO_TERMINATEAPPL_RESTART ((ProtectionReturnType)3u)
#define PRO_SHUTDOWN ((ProtectionReturnType)4u)

typedef uint8 TryToGetSpinlockType;
#define TRYTOGETSPINLOCK_SUCCESS ((TryToGetSpinlockType)0u)
#define TRYTOGETSPINLOCK_NOSUCCESS ((TryToGetSpinlockType)1u)

typedef uint8 IdleModeType;
#define IDLE_NO_HALT ((IdleModeType)0u)

typedef uint32 OSServiceIdType;

/* ---- Vector MICROSAR specific types used by integration code ---------- */
typedef enum {
    OS_CORESTARTSTATE_ACTIVATED_ASR = 0,
    OS_CORESTARTSTATE_ACTIVATED_NONASR,
    OS_CORESTARTSTATE_INACTIVE
} Os_CoreStartStateType;

typedef uint32 Os_PanicStatusType;

typedef struct {
    StatusType Error;
    OSServiceIdType ServiceId;
    uint32 Parameter1;
    uint32 Parameter2;
    uint32 Parameter3;
} Os_ErrorInformationType;

typedef struct {
    uint32 Pc;
    uint32 Sp;
    uint32 Lr;
    uint32 Cpsr;
} Os_ExceptionContextType;
typedef Os_ExceptionContextType *Os_ExceptionContextRefType;
typedef uint32 Os_AddressOfConstType;
typedef uint8 Os_AppMaskType;

#define OS_CORE_ID_MASTER ((CoreIdType)0u)

/* Service ids reported by OSErrorGetServiceId(). */
#define OSServiceId_ActivateTask (0x01u)
#define OSServiceId_TerminateTask (0x02u)
#define OSServiceId_ChainTask (0x03u)
#define OSServiceId_Schedule (0x04u)
#define OSServiceId_GetTaskID (0x05u)
#define OSServiceId_GetTaskState (0x06u)
#define OSServiceId_DisableAllInterrupts (0x07u)
#define OSServiceId_EnableAllInterrupts (0x08u)
#define OSServiceId_SuspendAllInterrupts (0x09u)
#define OSServiceId_ResumeAllInterrupts (0x0Au)
#define OSServiceId_SuspendOSInterrupts (0x0Bu)
#define OSServiceId_ResumeOSInterrupts (0x0Cu)
#define OSServiceId_GetResource (0x0Du)
#define OSServiceId_ReleaseResource (0x0Eu)
#define OSServiceId_SetEvent (0x0Fu)
#define OSServiceId_ClearEvent (0x10u)
#define OSServiceId_GetEvent (0x11u)
#define OSServiceId_WaitEvent (0x12u)
#define OSServiceId_GetAlarmBase (0x13u)
#define OSServiceId_GetAlarm (0x14u)
#define OSServiceId_SetRelAlarm (0x15u)
#define OSServiceId_SetAbsAlarm (0x16u)
#define OSServiceId_CancelAlarm (0x17u)
#define OSServiceId_GetActiveApplicationMode (0x18u)
#define OSServiceId_StartOS (0x19u)
#define OSServiceId_ShutdownOS (0x1Au)
#define OSServiceId_GetCounterValue (0x1Bu)
#define OSServiceId_GetElapsedValue (0x1Cu)
#define OSServiceId_IncrementCounter (0x1Du)
#define OSServiceId_StartScheduleTableRel (0x1Eu)
#define OSServiceId_StartScheduleTableAbs (0x1Fu)
#define OSServiceId_StopScheduleTable (0x20u)
#define OSServiceId_NextScheduleTable (0x21u)
#define OSServiceId_GetScheduleTableStatus (0x22u)
#define OSServiceId_GetSpinlock (0x23u)
#define OSServiceId_ReleaseSpinlock (0x24u)
#define OSServiceId_TryToGetSpinlock (0x25u)
#define OSServiceId_StartCore (0x26u)
#define OSServiceId_StartNonAutosarCore (0x27u)
#define OSServiceId_ShutdownAllCores (0x28u)
#define OSServiceId_Os_BarrierSynchronize (0x29u)
#define OSServiceId_GetApplicationID (0x2Au)
#define OSServiceId_GetISRID (0x2Bu)
#define OSServiceId_Os_EnableInterruptSource (0x2Cu)
#define OSServiceId_Os_DisableInterruptSource (0x2Du)
#define OSServiceId_Os_ClearPendingInterrupt (0x2Eu)
#define OSServiceId_IocWrite (0x30u)
#define OSServiceId_IocRead (0x31u)

#ifdef __cplusplus
}
#endif

#endif /* OS_TYPES_H */
