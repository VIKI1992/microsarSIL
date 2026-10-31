/* SilMcal_Stubs.c - Mcu, Platform, OsIf, Pit_Ip and Mcl: hardware initialisation has no meaning on the
 * host; the time related services run on the virtual clock. */
#include "Mcu.h"
#include "Platform.h"
#include "OsIf.h"
#include "Pit_Ip.h"
#include "CDD_Mcl.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

uint32 SilMcu_ResetReason(void);
void SilMcu_PerformReset(void);

/* ---- Mcu ------------------------------------------------------------------- */
void Mcu_Init(const Mcu_ConfigType *ConfigPtr) { (void)ConfigPtr; }
Std_ReturnType Mcu_InitRamSection(Mcu_RamSectionType RamSection) {
    (void)RamSection;
    return E_OK;
}
Std_ReturnType Mcu_InitClock(Mcu_ClockType ClockSetting) {
    (void)ClockSetting;
    return E_OK;
}
Std_ReturnType Mcu_DistributePllClock(void) { return E_OK; }
Mcu_PllStatusType Mcu_GetPllStatus(void) { return MCU_PLL_LOCKED; }
Mcu_ResetType Mcu_GetResetReason(void) { return (Mcu_ResetType)SilMcu_ResetReason(); }
Mcu_RawResetType Mcu_GetResetRawValue(void) { return (Mcu_RawResetType)SilMcu_ResetReason(); }
void Mcu_PerformReset(void) { SilMcu_PerformReset(); }
void Mcu_SetMode(Mcu_ModeType McuMode) { (void)McuMode; }
void Mcu_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = MCU_VENDOR_ID;
    versioninfo->moduleID = MCU_MODULE_ID;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}
uint32 Mcu_GetClockFrequency(uint32 ClockName) {
    (void)ClockName;
    return 100000000u;
}
void Mcu_SRAMRetentionConfig(uint32 SRAMRetenConfig) { (void)SRAMRetenConfig; }
void Mcu_Disable_CMU(uint32 CmuIndex) { (void)CmuIndex; }

/* ---- Platform -------------------------------------------------------------- */
void Platform_Init(const Platform_ConfigType *pConfig) { (void)pConfig; }
uint8 Platform_GetCoreID(void) {
    CoreIdType c = SilOs_CurrentCore();
    return (c == OS_CORE_ID_INVALID) ? 0u : (uint8)c;
}
Std_ReturnType Platform_SetIrq(IRQn_Type eIrqNumber, boolean bEnable) {
    (void)eIrqNumber;
    (void)bEnable;
    return E_OK;
}
Std_ReturnType Platform_SetIrqPriority(IRQn_Type eIrqNumber, uint8 u8Priority) {
    (void)eIrqNumber;
    (void)u8Priority;
    return E_OK;
}
Std_ReturnType Platform_GetIrqPriority(IRQn_Type eIrqNumber, uint8 *u8Priority) {
    (void)eIrqNumber;
    if (u8Priority != NULL_PTR) *u8Priority = 0u;
    return E_OK;
}
Std_ReturnType Platform_InstallIrqHandler(IRQn_Type eIrqNumber, const Platform_IrqHandlerType pfNewHandler, Platform_IrqHandlerType *pfOldHandler) {
    (void)eIrqNumber;
    (void)pfNewHandler;
    if (pfOldHandler != NULL_PTR) *pfOldHandler = NULL_PTR;
    return E_OK;
}
Std_ReturnType Platform_MruEnableChannel(uint8 u8MruChannel) {
    (void)u8MruChannel;
    return E_OK;
}
Std_ReturnType Platform_MruDisableChannel(uint8 u8MruChannel) {
    (void)u8MruChannel;
    return E_OK;
}
Std_ReturnType Platform_MruResetChannel(uint8 u8MruChannel) {
    (void)u8MruChannel;
    return E_OK;
}
Std_ReturnType Platform_MruResetInstance(uint8 u8MruInstance) {
    (void)u8MruInstance;
    return E_OK;
}

/* ---- OsIf ------------------------------------------------------------------ */
static uint32 g_osifHz[3] = {0u, 0u, 0u};

static uint32 osif_hz(OsIf_CounterType sel) {
    uint32 i = (sel < 3u) ? (uint32)sel : 0u;
    return g_osifHz[i] ? g_osifHz[i] : (uint32)SilMcal_LifetimerHz();
}
void OsIf_Init(const void *Config) { (void)Config; }
uint32 OsIf_GetCounter(OsIf_CounterType SelectedCounter) { return (uint32)((SilOs_ClockNow() * osif_hz(SelectedCounter)) / 1000000000u); }
uint32 OsIf_GetElapsed(uint32 *CurrentRef, OsIf_CounterType SelectedCounter) {
    uint32 now = OsIf_GetCounter(SelectedCounter), delta;
    if (CurrentRef == NULL_PTR) return 0u;
    delta = now - *CurrentRef;
    *CurrentRef = now;
    return delta;
}
void OsIf_SetTimerFrequency(uint32 Freq, OsIf_CounterType SelectedCounter) {
    if (SelectedCounter < 3u) g_osifHz[SelectedCounter] = Freq;
}
uint32 OsIf_MicrosToTicks(uint32 Micros, OsIf_CounterType SelectedCounter) { return (uint32)(((uint64)Micros * osif_hz(SelectedCounter)) / 1000000u); }
uint8 OsIf_GetCoreID(void) { return Platform_GetCoreID(); }
uint32 OsIf_GetUserId(void) { return 0u; }
void OsIf_SuspendAllInterrupts(void) { SuspendAllInterrupts(); }
void OsIf_ResumeAllInterrupts(void) { ResumeAllInterrupts(); }

/* ---- Pit_Ip lifetimer (the profiling timestamp source of the reference project) --------------- */
void Pit_Ip_SetLifetimeTimer(uint8 instance) { (void)instance; }
uint64 Pit_Ip_GetLifetimeTimer(uint8 instance) {
    (void)instance;
    return (uint64)(((double)SilOs_ClockNow() / 1e9) * (double)SilMcal_LifetimerHz());
}
uint32 Pit_Ip_GetCurrentTimer(uint8 instance, uint8 channel) {
    (void)channel;
    return (uint32)Pit_Ip_GetLifetimeTimer(instance);
}

/* ---- Mcl ------------------------------------------------------------------- */
void Mcl_Init(const Mcl_ConfigType *ConfigPtr) { (void)ConfigPtr; }
void Mcl_DeInit(void) {}
uint32 Mcl_GetUserID(void) { return 0u; }
