/*
 * Os_Api.h - OSEK/AUTOSAR OS API of SilOs (host-emulated MICROSAR OS), independent of the ECU configuration.
 * Applications include Os.h, which adds the generated configuration headers.
 *
 * The API names and semantics follow AUTOSAR SWS_Os (R20-11) plus the Vector
 * MICROSAR extensions that integration code commonly calls.
 */
#ifndef OS_API_H
#define OS_API_H

#include "Os_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OS_VENDOR_ID (0xFFFFu)
#define OS_MODULE_ID (1u)
#define OS_AR_RELEASE_MAJOR_VERSION (4u)
#define OS_AR_RELEASE_MINOR_VERSION (4u)
#define OS_SW_MAJOR_VERSION (0u)
#define OS_SW_MINOR_VERSION (1u)
#define OS_SW_PATCH_VERSION (0u)

/* ---- task / ISR definition macros ------------------------------------- */
#define TASK(TaskName) void Os_Task_##TaskName(void)
#define ISR(IsrName) void Os_Isr_##IsrName(void)
#define DeclareTask(TaskName) extern void Os_Task_##TaskName(void)
#define DeclareISR(IsrName) extern void Os_Isr_##IsrName(void)
#define DeclareEvent(EventName)
#define DeclareResource(ResourceName)
#define DeclareAlarm(AlarmName)
#define ALARMCALLBACK(AlarmCallbackName) void AlarmCallbackName(void)

#define OSDEFAULTAPPMODE_VALUE ((AppModeType)0u)
#define DONOTCARE ((AppModeType)0xFFFFFFFFu)
#define INVALID_OSAPPLICATION ((ApplicationType)0xFFFFFFFFu)
#define OS_CORE_ID_INVALID ((CoreIdType)0xFFFFFFFFu)

/* ---- task management -------------------------------------------------- */
StatusType ActivateTask(TaskType TaskID);
StatusType TerminateTask(void);
StatusType ChainTask(TaskType TaskID);
StatusType Schedule(void);
StatusType GetTaskID(TaskRefType TaskID);
StatusType GetTaskState(TaskType TaskID, TaskStateRefType State);

/* ---- interrupt handling ----------------------------------------------- */
void EnableAllInterrupts(void);
void DisableAllInterrupts(void);
void ResumeAllInterrupts(void);
void SuspendAllInterrupts(void);
void ResumeOSInterrupts(void);
void SuspendOSInterrupts(void);
StatusType Os_DisableInterruptSource(ISRType ISRID);
StatusType Os_EnableInterruptSource(ISRType ISRID, boolean ClearPending);
StatusType Os_ClearPendingInterrupt(ISRType ISRID);
StatusType Os_IsInterruptSourceEnabled(ISRType ISRID, boolean *IsEnabled);
StatusType Os_IsInterruptPending(ISRType ISRID, boolean *IsPending);
void Os_InitialEnableInterruptSources(boolean ClearPending);
void Os_EnableInterruptsPreStart(void);
/* AUTOSAR names */
StatusType DisableInterruptSource(ISRType ISRID);
StatusType EnableInterruptSource(ISRType ISRID, boolean ClearPending);
StatusType ClearPendingInterrupt(ISRType ISRID);
ISRType GetISRID(void);

/* ---- resources -------------------------------------------------------- */
StatusType GetResource(ResourceType ResID);
StatusType ReleaseResource(ResourceType ResID);

/* ---- events ----------------------------------------------------------- */
StatusType SetEvent(TaskType TaskID, EventMaskType Mask);
StatusType ClearEvent(EventMaskType Mask);
StatusType GetEvent(TaskType TaskID, EventMaskRefType Event);
StatusType WaitEvent(EventMaskType Mask);

/* ---- counters and alarms ---------------------------------------------- */
StatusType GetAlarmBase(AlarmType AlarmID, AlarmBaseRefType Info);
StatusType GetAlarm(AlarmType AlarmID, TickRefType Tick);
StatusType SetRelAlarm(AlarmType AlarmID, TickType increment, TickType cycle);
StatusType SetAbsAlarm(AlarmType AlarmID, TickType start, TickType cycle);
StatusType CancelAlarm(AlarmType AlarmID);
StatusType IncrementCounter(CounterType CounterID);
StatusType GetCounterValue(CounterType CounterID, TickRefType Value);
StatusType GetElapsedValue(CounterType CounterID, TickRefType Value, TickRefType ElapsedValue);

/* ---- schedule tables -------------------------------------------------- */
StatusType StartScheduleTableRel(ScheduleTableType ScheduleTableID, TickType Offset);
StatusType StartScheduleTableAbs(ScheduleTableType ScheduleTableID, TickType Start);
StatusType StopScheduleTable(ScheduleTableType ScheduleTableID);
StatusType NextScheduleTable(ScheduleTableType ScheduleTableID_From, ScheduleTableType ScheduleTableID_To);
StatusType GetScheduleTableStatus(ScheduleTableType ScheduleTableID, ScheduleTableStatusRefType ScheduleStatus);
StatusType StartScheduleTableSynchron(ScheduleTableType ScheduleTableID);
StatusType SyncScheduleTable(ScheduleTableType ScheduleTableID, TickType Value);
StatusType SetScheduleTableAsync(ScheduleTableType ScheduleTableID);

/* ---- OS execution control --------------------------------------------- */
AppModeType GetActiveApplicationMode(void);
void StartOS(AppModeType Mode);
void ShutdownOS(StatusType Error);
void ShutdownAllCores(StatusType Error);

/* ---- multi-core ------------------------------------------------------- */
CoreIdType GetCoreID(void);
uint32 GetNumberOfActivatedCores(void);
void StartCore(CoreIdType CoreID, StatusType *Status);
void StartNonAutosarCore(CoreIdType CoreID, StatusType *Status);
StatusType GetSpinlock(SpinlockIdType SpinlockId);
StatusType ReleaseSpinlock(SpinlockIdType SpinlockId);
StatusType TryToGetSpinlock(SpinlockIdType SpinlockId, TryToGetSpinlockType *Success);
StatusType ControlIdle(CoreIdType CoreID, IdleModeType IdleMode);

/* ---- OS applications -------------------------------------------------- */
ApplicationType GetApplicationID(void);
ApplicationType GetCurrentApplicationID(void);
StatusType GetApplicationState(ApplicationType Application, ApplicationStateRefType Value);
StatusType TerminateApplication(ApplicationType Application, RestartType RestartOption);
StatusType AllowAccess(void);
ObjectAccessType CheckObjectAccess(ApplicationType ApplID, ObjectTypeType ObjectType, uint32 ObjectID);
ApplicationType CheckObjectOwnership(ObjectTypeType ObjectType, uint32 ObjectID);
AccessType CheckISRMemoryAccess(ISRType ISRID, void *Address, uint32 Size);
AccessType CheckTaskMemoryAccess(TaskType TaskID, void *Address, uint32 Size);
StatusType CallTrustedFunction(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams);
StatusType Os_CallTrustedFunctionAsNonTrusted(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams);
StatusType Os_CallFastTrustedFunction(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams);
StatusType Os_CallNonTrustedFunction(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams);

/* ---- hooks and error information -------------------------------------- */
OSServiceIdType OSErrorGetServiceId(void);
StatusType Os_GetDetailedError(Os_ErrorInformationType *ErrorRef);
uint32 Os_ErrorGetParameter(uint8 index);
#define OSError_ActivateTask_TaskID() ((TaskType)Os_ErrorGetParameter(0u))
#define OSError_ChainTask_TaskID() ((TaskType)Os_ErrorGetParameter(0u))
#define OSError_SetEvent_TaskID() ((TaskType)Os_ErrorGetParameter(0u))
#define OSError_SetEvent_Mask() ((EventMaskType)Os_ErrorGetParameter(1u))
#define OSError_SetRelAlarm_AlarmID() ((AlarmType)Os_ErrorGetParameter(0u))
#define OSError_SetRelAlarm_increment() ((TickType)Os_ErrorGetParameter(1u))
#define OSError_SetRelAlarm_cycle() ((TickType)Os_ErrorGetParameter(2u))
#define OSError_SetAbsAlarm_AlarmID() ((AlarmType)Os_ErrorGetParameter(0u))
#define OSError_CancelAlarm_AlarmID() ((AlarmType)Os_ErrorGetParameter(0u))
#define OSError_GetResource_ResID() ((ResourceType)Os_ErrorGetParameter(0u))
#define OSError_ReleaseResource_ResID() ((ResourceType)Os_ErrorGetParameter(0u))
#define OSError_GetSpinlock_SpinlockId() ((SpinlockIdType)Os_ErrorGetParameter(0u))
#define OSError_ReleaseSpinlock_SpinlockId() ((SpinlockIdType)Os_ErrorGetParameter(0u))

/* Hooks implemented by the integrator (compiled in according to Os_Cfg.h). */
void StartupHook(void);
void ShutdownHook(StatusType Fatalerror);
void ErrorHook(StatusType Error);
void PreTaskHook(void);
void PostTaskHook(void);
ProtectionReturnType ProtectionHook(StatusType Fatalerror);
void Os_PanicHook(Os_PanicStatusType Status);

/* ---- Vector MICROSAR extensions --------------------------------------- */
void Os_InitMemory(void);
void Os_Init(void);
void Os_InitInterruptOnly(void);
void Os_EnterPreStartTask(void);
void Os_GetVersionInfo(Std_VersionInfoType *versioninfo);
StatusType Os_GetExceptionContext(Os_ExceptionContextRefType Context);
StatusType Os_SetExceptionContext(const Os_ExceptionContextType *Context);
Os_AddressOfConstType Os_GetExceptionAddress(void);
StatusType Os_GetCoreStartState(CoreIdType CoreID, Os_CoreStartStateType *CoreState, StatusType *Status);
StatusType Os_BarrierSynchronize(Os_BarrierIdType BarrierID);
StatusType Os_AddDeferredPriorities(Os_PrioritySetIdType PrioritySetID);
StatusType Os_SetDeferredPriorities(Os_PrioritySetIdType PrioritySetID);
StatusType Os_RemoveDeferredPriorities(Os_PrioritySetIdType PrioritySetID);

/* Stack usage queries: SilOs reports the configured stack size as "used" is unknown; returns 0. */
uint32 Os_GetTaskStackUsage(TaskType TaskID);
uint32 Os_GetISRStackUsage(ISRType ISRID);
uint32 Os_GetKernelStackUsage(CoreIdType CoreID);
uint32 Os_GetStartupHookStackUsage(CoreIdType CoreID);
uint32 Os_GetErrorHookStackUsage(CoreIdType CoreID);
uint32 Os_GetShutdownHookStackUsage(CoreIdType CoreID);
uint32 Os_GetProtectionHookStackUsage(CoreIdType CoreID);
uint32 Os_GetInitHookStackUsage(CoreIdType CoreID);
uint32 Os_GetNonTrustedFunctionStackUsage(Os_NonTrustedFunctionIndexType FunctionIndex);

/* Peripheral access: on the host these are plain memory accesses. */
uint8 Os_ReadPeripheral8(Os_PeripheralIdType PeripheralID, const volatile uint8 *Address);
uint16 Os_ReadPeripheral16(Os_PeripheralIdType PeripheralID, const volatile uint16 *Address);
uint32 Os_ReadPeripheral32(Os_PeripheralIdType PeripheralID, const volatile uint32 *Address);
void Os_WritePeripheral8(Os_PeripheralIdType PeripheralID, volatile uint8 *Address, uint8 Value);
void Os_WritePeripheral16(Os_PeripheralIdType PeripheralID, volatile uint16 *Address, uint16 Value);
void Os_WritePeripheral32(Os_PeripheralIdType PeripheralID, volatile uint32 *Address, uint32 Value);
void Os_ModifyPeripheral8(Os_PeripheralIdType PeripheralID, volatile uint8 *Address, uint8 ClearMask, uint8 SetMask);
void Os_ModifyPeripheral16(Os_PeripheralIdType PeripheralID, volatile uint16 *Address, uint16 ClearMask, uint16 SetMask);
void Os_ModifyPeripheral32(Os_PeripheralIdType PeripheralID, volatile uint32 *Address, uint32 ClearMask, uint32 SetMask);

/* Alias names used by some generators */
#define Os_GetCoreID() GetCoreID()

#ifdef __cplusplus
}
#endif

#endif /* OS_API_H */
