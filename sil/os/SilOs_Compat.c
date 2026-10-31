/*
 * SilOs_Compat.c - Vector MICROSAR OS extension functions that have no meaning on the host.
 */
#include "SilOs_Internal.h"

void Os_InitMemory(void) {}
void Os_Init(void) {}
void Os_InitInterruptOnly(void) {}
void Os_EnterPreStartTask(void) {}

void Os_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL) return;
    versioninfo->vendorID = OS_VENDOR_ID;
    versioninfo->moduleID = OS_MODULE_ID;
    versioninfo->sw_major_version = OS_SW_MAJOR_VERSION;
    versioninfo->sw_minor_version = OS_SW_MINOR_VERSION;
    versioninfo->sw_patch_version = OS_SW_PATCH_VERSION;
}

StatusType Os_GetExceptionContext(Os_ExceptionContextRefType Context) {
    if (Context == NULL) return E_OS_PARAM_POINTER;
    Context->Pc = 0u;
    Context->Sp = 0u;
    Context->Lr = 0u;
    Context->Cpsr = 0u;
    return E_OK;
}

StatusType Os_SetExceptionContext(const Os_ExceptionContextType *Context) { (void)Context; return E_OK; }
Os_AddressOfConstType Os_GetExceptionAddress(void) { return 0u; }

StatusType Os_AddDeferredPriorities(Os_PrioritySetIdType PrioritySetID) { (void)PrioritySetID; return E_OK; }
StatusType Os_SetDeferredPriorities(Os_PrioritySetIdType PrioritySetID) { (void)PrioritySetID; return E_OK; }
StatusType Os_RemoveDeferredPriorities(Os_PrioritySetIdType PrioritySetID) { (void)PrioritySetID; return E_OK; }

StatusType CallTrustedFunction(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams) {
    (void)FunctionIndex; (void)FunctionParams; return E_OS_SERVICEID;
}
StatusType Os_CallTrustedFunctionAsNonTrusted(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams) {
    (void)FunctionIndex; (void)FunctionParams; return E_OS_SERVICEID;
}
StatusType Os_CallFastTrustedFunction(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams) {
    (void)FunctionIndex; (void)FunctionParams; return E_OS_SERVICEID;
}
StatusType Os_CallNonTrustedFunction(TrustedFunctionIndexType FunctionIndex, TrustedFunctionParameterRefType FunctionParams) {
    (void)FunctionIndex; (void)FunctionParams; return E_OS_SERVICEID;
}

/* Stack usage: the host has no fixed task stacks; report a small constant so that
 * stack monitors see a plausible, never critical value. */
uint32 Os_GetTaskStackUsage(TaskType TaskID) { return (TaskID < silos_g.cfg->numTasks) ? silos_g.cfg->tasks[TaskID].stackSize / 4u : 0u; }
uint32 Os_GetISRStackUsage(ISRType ISRID) { (void)ISRID; return 128u; }
uint32 Os_GetKernelStackUsage(CoreIdType CoreID) { (void)CoreID; return 128u; }
uint32 Os_GetStartupHookStackUsage(CoreIdType CoreID) { (void)CoreID; return 64u; }
uint32 Os_GetErrorHookStackUsage(CoreIdType CoreID) { (void)CoreID; return 64u; }
uint32 Os_GetShutdownHookStackUsage(CoreIdType CoreID) { (void)CoreID; return 64u; }
uint32 Os_GetProtectionHookStackUsage(CoreIdType CoreID) { (void)CoreID; return 64u; }
uint32 Os_GetInitHookStackUsage(CoreIdType CoreID) { (void)CoreID; return 64u; }
uint32 Os_GetNonTrustedFunctionStackUsage(Os_NonTrustedFunctionIndexType FunctionIndex) { (void)FunctionIndex; return 64u; }

uint8 Os_ReadPeripheral8(Os_PeripheralIdType PeripheralID, const volatile uint8 *Address) { (void)PeripheralID; return *Address; }
uint16 Os_ReadPeripheral16(Os_PeripheralIdType PeripheralID, const volatile uint16 *Address) { (void)PeripheralID; return *Address; }
uint32 Os_ReadPeripheral32(Os_PeripheralIdType PeripheralID, const volatile uint32 *Address) { (void)PeripheralID; return *Address; }
void Os_WritePeripheral8(Os_PeripheralIdType PeripheralID, volatile uint8 *Address, uint8 Value) { (void)PeripheralID; *Address = Value; }
void Os_WritePeripheral16(Os_PeripheralIdType PeripheralID, volatile uint16 *Address, uint16 Value) { (void)PeripheralID; *Address = Value; }
void Os_WritePeripheral32(Os_PeripheralIdType PeripheralID, volatile uint32 *Address, uint32 Value) { (void)PeripheralID; *Address = Value; }
void Os_ModifyPeripheral8(Os_PeripheralIdType PeripheralID, volatile uint8 *Address, uint8 ClearMask, uint8 SetMask) {
    (void)PeripheralID; *Address = (uint8)((*Address & ClearMask) | SetMask);
}
void Os_ModifyPeripheral16(Os_PeripheralIdType PeripheralID, volatile uint16 *Address, uint16 ClearMask, uint16 SetMask) {
    (void)PeripheralID; *Address = (uint16)((*Address & ClearMask) | SetMask);
}
void Os_ModifyPeripheral32(Os_PeripheralIdType PeripheralID, volatile uint32 *Address, uint32 ClearMask, uint32 SetMask) {
    (void)PeripheralID; *Address = (*Address & ClearMask) | SetMask;
}
