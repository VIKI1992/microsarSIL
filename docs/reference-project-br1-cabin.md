# Reference project: BR1 Cabin ECU (CBCU) — facts that shape the SIL

Inventory taken 2026-10-31 from `C:\br1_cabin_ecu` (branch `RT1ECUESW-9442-ADC-round-robin-read`),
SIP `SIP_2_0_0_HF1_R34_1.0.0` (MICROSAR Classic, DaVinci R34 beta), NXP RTD `SW32ZE_RTD_R21-11_2.0.0_HF01`.
No Vector or NXP code is copied here; this file records names, counts and conventions only.
Regenerate the raw numbers with `python tools/check_sip.py C:\br1_cabin_ecu`.

## Target and toolchain

| Item | Value |
|---|---|
| MCU | NXP S32E288 (OS derivative `S32E288`, Cortex-R52 cluster), 2 cores used by the OS |
| Compiler | GCC 10.2 arm-none-eabi (`Tools/compiler/gcc_v10.2`), `-mcpu=cortex-r52 -mfloat-abi=hard`, 32-bit |
| Build | CMake 3.22 + Unix Makefiles, root `CMakeLists.txt` + `Configs/CmakeModules/{arm_toolchain,mcal_config,sip_config,cdd_config}.cmake`, 1193 compile units |
| Defines | `BRS_COMP_GCCNXP BRS_PLATFORM_ARM ECUNAME=0u TRUE=1u FALSE=0u` |
| Include order (relevant) | `Generated_BSW` (106), `Generated_BSW/Components` (107), `Generated_BSW/include` (108, MCAL generated), `BSw/StartUpCode/inc` (109), ..., `Sip/Components/Os/Implementation` (165), `_Common/Implementation` (171), RTD plugin `include` dirs (173..205) |
| Compiler abstraction | `BSw/StartUpCode/inc/Compiler.h` (plain GCC macros), `Compiler_Cfg.h` chains the MSR4 `Compiler_Cfg_MSR4_*.h` files and ends in `#include "Os_Compiler_Cfg.h"` (generated, 83 empty `OS_<NAME>_CODE` macros) |
| Platform types | `Generated_BSW/include/Platform_Types.h` (RTD generated, `CPU_TYPE_32`), `_Common/Implementation/Std_Types.h` |
| MemMap | `BSw/StartUpCode/inc/MemMap.h` includes `MemMap_MSR4_*` + custom `_Pragma("GCC section ...")` for FETK reference pages; `Generated_BSW/Components/*_MemMap.h` per SWC |
| Host machine (this PC) | Windows 11, MinGW-w64 GCC 12.2 (posix thread model, `std::thread` works, no `<threads.h>`), CMake 3.22, make, gdb; Python 3.13 with python-can 4.6, cantools, pytest, PyYAML, lxml; no MSVC, no clang, no ninja |

## OS (MICROSAR OS 4.8, HAL 4.5, SC1, EXTENDED status)

Source: `Config/ECUC/Cabin_ECU_Os_Os_ecuc.arxml` (1.4 MB) and `Generated_BSW/Os_*_Cfg.h`.

| Topic | Fact |
|---|---|
| Cores | 2 (`OsCore0` master, `OsCore1`); `OsNumberOfCores=2`, XSignal ON (synchronous), `OsUseXSignalAsyncApiCalls=false` |
| OS-Applications | `SystemApplication_OsCore0` (IdleTask_OsCore0, CounterIsr_SystemTimer, XSignalIsr_OsCore0, counter SystemTimer_Core0), `OsApplication_Trusted_Core0` (10 tasks, 67 ISRs, 89 alarms), `SystemApplication_OsCore1` (IdleTask_OsCore1, Default_Init_Task_Core1_Trusted, XSignalIsr_OsCore1, CounterIsr_SystemTimer_Core1, counter SystemTimer_Core1), `OsApplication_Trusted_Core1` (5 tasks, 0 ISRs, 12 alarms). All trusted; no memory, timing or service protection |
| Status / hooks | `OsStatus=EXTENDED`; ShutdownHook ON (`ShutdownHook` calls `EcuM_Shutdown()`), StartupHook/ErrorHook/PreTask/PostTask/ProtectionHook OFF; `Os_PanicHook` implemented in `Os_Callout_Stubs.c`; `OsUseGetServiceId=false`, `OsUseParameterAccess=false` |
| Stack monitoring | ON + stack usage measurement ON; `SWC_stackMonitor` calls `Os_GetTaskStackUsage(<task>)` for every task and includes `Os_Stack_Cfg.h` and `Os_Types_Lcfg.h` |
| Counters | 2 hardware counters, one per core: `SystemTimer_Core0`, `SystemTimer_Core1`; `OsSecondsPerTick=0.001`, `OsCounterTicksPerBase=40000`, `OsCounterMaxAllowedValue=1000000000`, `OsCounterMinCycle=1`, algorithm HEAP, driver = ARM EL1 physical timer (`Os_TimerPfrtIsr`, compare based, no periodic tick). No software counters |
| Alarms | 101 total (89 on core 0, 12 on core 1); actions: 100 SetEvent, 1 ActivateTask, 0 callbacks, 0 IncrementCounter; no autostart; all named `Rte_Al_TE_*` except one |
| Schedule tables | none |
| Events | 120 `Rte_Ev_*` (`OsEventMask` AUTO except `Rte_Ev_WP_ApplicationRunTask_50_5000ms_Core1` = 2) |
| Resources | 1 STANDARD resource `OsResource` (users `Default_BSW_Async_Task_Core0`, `BSW_Memory_Task_Core0`), `OsUseResScheduler=false`; no internal resources |
| Spinlocks | 84: 72 with `LOCK_CAT2_INTERRUPTS` (RTE cross-core data), 12 with `LOCK_NOTHING`; the non-RTE ones are `I2CWrapperSpinlock_01 PCA_9539_Device1_Spinlock DetExternalsSpinlock UartTraceSpinlock WdgIfSpinlock`; `GetSpinlock`/`ReleaseSpinlock` appear 240/273 times in the generated RTE |
| IOC | 8 unqueued `OsIocCommunication` channels (`Rte_CtIcICAL{BMS,DCDC,SGW,WiperCU}_*` and their `Rte_RxTimeout_*` twins), `uint8` atomic data, 3..6 senders, sender and receiver both on core 0; RTE calls `IocWrite_<ch>_<senderId>(value)` and `IocRead_<ch>(&value)`; `OsIocIncludeHeader=Rte.h` |
| Barriers | 2 (`OsBarrier_Core0_Waits_Core1_To_Finish_Rte_Start`, `OsBarrier_Rte_Start_Synch_Point`), attendees `Default_Init_Task` + `Default_Init_Task_Core1`; `Os_BarrierSynchronize()` is called from `BswM_Callout_Stubs.c` |
| ISRs | 71, all CATEGORY_2, `OsIsrEnableNesting=false` everywhere, priorities 15 (timers), 16 (SPI 2/3/5/7, XSignal), 32 (CAN, LIN, I2C, I3C, UART, eMIOS, most DIPORT), 33 (DIPORT 22/23/26/27/29); 4 ISR levels. Every ISR has `OsIsrSpecialFunctionName` = the driver handler (`CAN15_ORED_IRQHandler`, `Spi_Ip_SPI_2_IRQHandler`, `LinIsr_4`, `Os_TimerPfrtIsr`, ...), so the only `ISR(x)` bodies in project code are `ISR(LinIsr_n){ Lin_Interrupt(ch); }` in `Lin_Lcfg.c` |
| ISR to peripheral | CAN: FLEXCAN 0, 4, 9, 11, 13, 14, 15 each with {ORED, EnhancedData, ORED_0_31, 32_63, 64_95, 96_127_MB}; LIN `Lin_Channel_{4,5,10,11}_ISR`; SPI 2, 3, 5, 7; LPI2C 1, 2; I3C0; LinFlex9 UART; eMIOS0 CH8, eMIOS1 CH3/4/8/12/14; RTU DIPORT INT18/22/23/26/27/29/30 |
| Task queues | 10 priority queues per core; `OsTaskActivation=1` for every task |

### Tasks

| Task | Core | Prio | Sched | Type | Trigger events | Notes |
|---|---|---|---|---|---|---|
| IdleTask_OsCore0 / _OsCore1 | 0 / 1 | idle | FULL | basic | - | autostart ANY |
| Default_Init_Task | 0 | 40 | NON | basic | - | autostart; `EcuM_StartupTwo()` then TerminateTask (body in `BrsMain.c`) |
| Default_Init_Task_Trusted | 0 | 49 | NON | basic | - | autostart; `Os_InitialEnableInterruptSources(FALSE)` |
| Default_Init_Task_Core1 | 1 | 50 | NON | basic | - | autostart; `EcuM_StartupTwo()` |
| Default_Init_Task_Core1_Trusted | 1 | 49 | NON | basic | - | autostart; `Os_InitialEnableInterruptSources(FALSE)` |
| ApplicationInitTask_Core0 / _Core1 | 0 / 1 | 40 | NON | basic | - | RTE init runnables |
| CALInitTask | 0 | 40 | NON | basic | - | RTE |
| Default_BSW_Async_Task_Core0 | 0 | 30 | FULL | extended | 1/5/10/20/50/100/250/1000 ms cyclic + Com/Xcp events | BSW main functions, uses `OsResource` |
| Default_BSW_Async_Task_Core1 | 1 | 30 | FULL | extended | 1/10 ms | |
| AppBtsSupervisorProxy | 0 | 25 | FULL | extended | 4 events | |
| CALRunTask | 0 | 20 | FULL | extended | 5/10/50/100/200/500/1000 ms + MSI events | 8 KB stack |
| BSW_Memory_Task_Core0 | 0 | 19 | FULL | basic | - | uses `OsResource`; NvM/Fee main functions |
| ApplicationRunTask_0_10ms_Core0 / _Core1 | 0 / 1 | 12 | FULL | extended | 1/5/10/20 ms + run events | core 1 has a 32 KB stack |
| ApplicationRunTask_50_5000ms_Core0 / _Core1 | 0 / 1 | 11 | FULL | extended | 50..5000 ms | |

RTE task bodies (`Rte_OsApplication_Trusted_Core{0,1}.c`, 72k + 14k lines) are endless
`for(;;){ WaitEvent(mask); GetEvent; ClearEvent; if (ev & X) { runnables... } }` loops; cross-core data goes
through `GetSpinlock`/`ReleaseSpinlock` + `Rte_MemCmp` + `SetEvent(<task on the other core>)`.

### OS API actually used outside the OS

Standard API (call sites over GenData + ASw + StartUp + Vector BSW): `SuspendAllInterrupts`/`ResumeAllInterrupts` 990,
`SetEvent` 905, `ReleaseSpinlock` 273, `GetSpinlock` 240, `SetRelAlarm` 104, `CancelAlarm` 75, `GetApplicationID` 53,
`SuspendOSInterrupts`/`ResumeOSInterrupts` 16/28, `TerminateTask` 23, `GetCoreID` 21, `GetEvent`/`ClearEvent` 14/14,
`ActivateTask` 13, `WaitEvent` 11, `GetCounterValue` 8, `DisableAllInterrupts`/`EnableAllInterrupts` 8/6,
`GetElapsedValue` 7, `GetResource`/`ReleaseResource` 5/5, `ShutdownOS` 4, `Schedule` 4, `StartOS`, `StartCore`,
`IocWrite_*`/`IocRead_*`. Not used: ChainTask, GetTaskState, GetTaskID, SetAbsAlarm, GetAlarm, GetAlarmBase,
IncrementCounter, schedule tables, TryToGetSpinlock, ShutdownAllCores, GetISRID.

Vector extensions: `Os_GetTaskStackUsage` (20), `Os_InitialEnableInterruptSources` (18), `Os_BarrierSynchronize` (6),
`Os_Init`, `Os_InitMemory`, `Os_InitInterruptOnly`, `Os_EnableInterruptSource`, `Os_DisableInterruptSource`,
`Os_IsInterruptSourceEnabled`, `Os_IsInterruptPending`, `Os_ClearPendingInterrupt`, `Os_ReadPeripheral8/16/32`,
`Os_WritePeripheral8/16/32`, `Os_ModifyPeripheral8/16/32`, `Os_GetDetailedError`, `Os_GetCoreStartState`,
`Os_EnableInterruptsPreStart`, `Os_Get/SetExceptionContext`, `Os_GetExceptionAddress`,
`Os_Get{Kernel,ISR,StartupHook,ShutdownHook,ErrorHook,ProtectionHook,InitHook,NonTrustedFunction}StackUsage`,
`Os_CallTrustedFunctionAsNonTrusted`, `Os_CallFastTrustedFunction`, `Os_Add/Set/RemoveDeferredPriorities`,
`Os_PanicHook` (callout).

OS headers included by non-OS code: `Os.h` (35), `Os_Common.h` (15), `Os_Application_Lcfg.h` (11), `Os_Types.h`
(cdd_i2c_wrapper_cfg.h), `Os_Stack_Cfg.h` + `Os_Types_Lcfg.h` (CtApStackMonitor.c), `Os_Compiler_Cfg.h`
(Compiler_Cfg chain), `Os_Hal_Compiler_GccNxpInt.h` (CDD_FETK only), `Os_{Var,Const,Code,Stacks}_MemMap.h`
(everywhere). The generated `Os_Lcfg.h` declares `Os_Task_<name>()` and `Os_Isr_<special>()`, i.e.
`TASK(x)` expands to `void Os_Task_x(void)` and `ISR(x)` to `void Os_Isr_x(void)`. Object handles are C enums in
`Os_Types_Lcfg.h` (`TaskType`, `ISRType`, `AlarmType`, `CounterType`, `ResourceType`, `SpinlockIdType`,
`ApplicationType`, `Os_BarrierIdType`, `Os_PeripheralIdType`, ...) plus `#define <name> <name>` guards;
`OS_TASKID_COUNT=18`, `OS_ISRID_COUNT=71`, `OS_ALARMID_COUNT=101`.

## RTE

| Topic | Fact |
|---|---|
| Files | `Rte.c` (12k lines), `Rte_OsApplication_Trusted_Core0.c` (72k), `Rte_OsApplication_Trusted_Core1.c` (14k), `Rte_Hook.h` (19k lines, `RTE_VFB_TRACE` defaults to 0), `Rte_Type.h`, `Rte_Cfg.h`, `Components/Rte_<SWC>.h` + `<SWC>_MemMap.h` |
| VFB tracing | **not generated**: task bodies contain no `Rte_Runnable_*_Start/Return` calls; `Rte_Hook.h` only lists the hook names in a comment block. The project instruments runnables itself: `Tests/Profiling/instrumentSrc.py` inserts `MEASURE_TIMESTAMP_RUNNABLE_CORE_<n>(id);` around every call marked by the RTE's `/* call runnable */` comment (958 markers: 750 on core 0, 208 on core 1), with IDs from `Tests/Profiling/id_runnables.xml` (743 runnables) and `id_isr.xml` (74) |
| Exclusive areas | 625 `SchM_Enter_*` macros expand to `SuspendAllInterrupts()` (ALL_INTERRUPT_BLOCKING), 4 are empty; no `Rte_Enter_*` functions (the RTE uses spinlocks and implicit buffers instead) |
| Multi-core | per task RTE buffers (`Rte_<Task>.Rte_RB.*`), cross-core writes guarded by `Rte_Spinlock_*`, cross-core triggers via `SetEvent` on the other core (XSignal), IOC for the 4 multiplexor signals |

## Startup (both cores execute the same `main`)

`BrsMainStartup.c` (vBRS) calls `main()` in `BrsMain.c`: hardware init (`BrsMainInit`: `BrsMain_Appl_Init`, PLL,
port, LED callbacks), then `Os_InitMemory(); Os_Init(); EcuM_Init();` on every core.
`EcuM_AL_DriverInitZero` (core 0: `*_InitMemory` of all BSW + `Xcp_Init`, `CanXcp_Init`; core 1:
`Dem_SatellitePreInit`), `EcuM_AL_DriverInitOne` (core 0: `Det_Init`, `Mcu_Init/InitClock/GetPllStatus/
DistributePllClock/SetMode`, `BswM_PreInit`, `OsIf_Init`, `Gpt_Init`, `Port_Init`, `Mcl_Init`, `Pwm_Init`,
`I2c_Init`, `Uart_Init`, `CDD_PEXSRV_init`; all cores: `Platform_Init`, `Wdg_Stub_Init`), `EcuM_StartOS`
(core 0 calls `StartCore(1)` first, then every core calls `StartOS`). Autostart `Default_Init_Task(_Core1)`
runs `EcuM_StartupTwo()`, whose BswM init actions (`BswM_Lcfg_OsApplication_Trusted_Core0.c`) call
`Can_43_FLEXCAN_Init, CanIf_Init, CanSM_Init, CanNm_Init, CanTp_Init, Com_Init, ComM_Init, PduR_Init, IpduM_Init,
Nm_Init, Lin_Init, LinIf_Init, LinSM_Init, Spi_Init, Adc_Init, Icu_Init, Platform_Init, MemAcc_Init,
Mem_43_INFLS_Init, Mem_43_EXFLS_Init, Fee_Init, NvM_Init (+ReadAll), Crypto_43_HSE_Init, CryIf_Init, Csm_Init,
CDD_CryptoSup_Init, Dcm_Init, FiM_Init, E2EXf_Init`, then the barriers `synch_barrier_pre_rte_start` (both cores)
and `synch_barrier_core0_waits_core1_to_finish_rte_start`, then `Rte_Start`. `ShutdownHook` calls `EcuM_Shutdown`;
`Mcu_PerformReset` is used 6 times, `Mcu_GetResetReason` twice.

## Communication

| Bus | Facts |
|---|---|
| CAN | 6 controllers (`Can_43_FLEXCAN`, NXP RTD): BODY=FLEXCAN_15, CHASSIS=FLEXCAN_11, ELECTRICAL=FLEXCAN_14, HYBRID=FLEXCAN_0, POWERTRAIN=FLEXCAN_4, BACKBONE=FLEXCAN_9 (CanIf controller ids 0..5 in that order). Rx/Tx/BusOff processing INTERRUPT on all, FD baud rate configs on 3 controllers, 129 HOHs: per controller 1 BASIC Tx (MIXED id, 32 HW objects) + 1 BASIC Rx EXTENDED + 1 BASIC Rx STANDARD (missing on ELECTRICAL/POWERTRAIN) + 7..36 FULL Rx; 262 HW filters. CanIf maps `Can_Write` to `Can_43_FLEXCAN_Write` and `Can_SetControllerMode` to `Can_43_FLEXCAN_SetControllerMode`; also used: `Can_43_FLEXCAN_Init`, `MainFunction_{Read,Write,BusOff,Mode,Wakeup}` (+ `_CanMainFunctionRWPeriods_0..10`), `Can_GetControllerErrorState`, `Can_GetControllerRx/TxErrorCounter`, `Can_SetBaudrate`, `Can_DisableControllerInterrupts`. DBCs: `Res/CAN_DBCs/BR1_{Backbone,Body,Chassis,Electrical,Hybrid,Powertrain}.dbc` (submodule). XCP runs on BACKBONE_CAN (CanXcp) |
| LIN | Vector `Lin` driver (`/MICROSAR/Lin_S32CommonChassis/Lin`), 4 channels `CN_CBCU_LIN0..3` (hardware LinFlexD 4, 5, 10, 11), LinIf master only, 4 LinIf channels, LDFs `Res/LDFs/CBCU_LIN0..3.ldf`. LinIf uses `Lin_Init/InitMemory/SendFrame/GetStatus/GoToSleep/GoToSleepInternal/Wakeup/WakeupInternal/CheckWakeup`; ISR bodies `ISR(LinIsr_n){ Lin_Interrupt(ch); }` |
| Com | 457 I-PDUs, 517 signals; CanTp 4 channels; Dcm 255 DIDs; NvM 110 blocks |
| Ethernet / FlexRay | none |

## MCAL (NXP RTD `*_TS_T31D53M20I0R0`) API used by project, generated and Vector code

Counts are call sites outside the driver itself.

| Module | API used | Callers |
|---|---|---|
| Dio | `WriteChannel` 83, `ReadChannel` 33, `FlipChannel`, `MaskedWritePort`, `GetVersionInfo`; 125 `DioConf_DioChannel_*` symbols | CDDs (BTN/BTS/DRV8910/GPIO tools/CanTrcv/LinTrcv), IoHwAb, SWCs |
| Port | `Port_Init` | EcuM |
| Mcu | `Init`, `InitClock`, `GetPllStatus`, `DistributePllClock`, `SetMode`, `PerformReset` 6, `GetResetReason` 2, `GetClockFrequency`, ... | EcuM, ResetMonitor, WdgM |
| Spi | `SetupEB` 48, `SyncTransmit` 16, `AsyncTransmit` 15, `GetSequenceResult` 9, `MainFunction_Handling` 5, `GetStatus`, `GetHWUnitStatus`, `SetAsyncMode`, `SetHWUnitAsyncMode`, `Init`; units SPI2/3/5/7; devices ADS7038 x10, BTS710336 x3, BTS722204 x2, DRV8910, L9305; 12 `SpiConf_SpiChannel_*`, 7 `SpiConf_SpiSequence_*` symbols | CDD_ADS(_NEW), BTS7103/7222 (+ESAXUMA1 drivers), DRV8910, Cdd_Mram |
| Adc | `StartGroupConversion` 4, `ReadGroup` 3, `ReadChannel` 3, `EnableChannel`/`DisableChannel`, `SetupResultBuffer`, `Enable/DisableHardwareTrigger`, `Calibration`, `Init`, ... (21 APIs) | CDD_AIN, SbcFs26 |
| Icu | 34 APIs: `GetDutyCycleValues` 8, `StartSignalMeasurement` 8, `GetEdgeNumbers` 6, `GetInputLevel` 6, `EnableEdgeDetection` 5, `EnableNotification` 5, `GetInputState` 4, `GetTimeElapsed` 4, `Enable/DisableEdgeCount`, `ResetEdgeCount`, `SetMode`, wheel speed extensions | CDD_Encoder, Cdd_HallSensor, CDD_PWM, CDD_WSS, CanTrcv/LinTrcv, Etimer_Workaround |
| Pwm | `SetDutyCycle` 56, `SetChannelDeadTime` 9, `SetPeriodAndDuty` 4, `SyncUpdate` 2, `GetChannelState`, `Init` | CDD_BTN8982/9970, BTS700X/7103/7222, IoHwAb, MotorCtrlDrv |
| Gpt | `Init`, `StartTimer`, `EnableNotification` 2, `SetMode` 3, `EnableWakeup` | EcuM |
| I2c (CDD_I2c) | `SyncTransmit` 27, `AsyncTransmit` 2, `GetStatus` 4, `Init` 2 | CDD_I2C_Wrapper, CDD_PortExpander (PCA9539), CDD_TempSensor, I2cWrapper, Sbc86 temp config |
| Uart (CDD_Uart) | `SyncSend` 11, `AsyncSend` 3, `AsyncReceive` 2, `Abort` 3, `GetStatus` 2, `Init` | Terminal.c (printf trace over LinFlex9) |
| MemAcc + Mem_43_INFLS/EXFLS | `MemAcc_{Init,Write,Erase,Compare,GetJobResult,GetJobStatus,MainFunction}`, `Mem_43_*_{Init,MainFunction}` | NXP Fee (below it), CDD_CryptoSup, KeyProvider |
| Fee (NXP) | `Init`, `Read`, `Write`, `Cancel`, `GetStatus`, `GetJobResult`, `InvalidateBlock`, `EraseImmediateBlock`, `MainFunction`, `SetMode`, `JobEndNotification` | MemIf, BswM, Mem_Callout_Stubs |
| Crypto_43_HSE + Hse_Ip | `Init`, `MainFunction`, `KeyElementGet`, `Exts_FormatKeyCatalogs`, `Hse_Ip_GetHseStatus/SendHseEvent` | CryIf/Csm stack, CDD_CryptoSup, KeyProvider |
| Platform / IntCtrl | `Platform_Init`, `Platform_GetCoreID`, `Platform_Mru*` | EcuM, BswM |
| OsIf | `OsIf_Init`, `OsIf_GetCoreID`, `OsIf_GetUserId` | EcuM, CDD_SafHandler, CDD_WSS |
| Pit_Ip | `GetLifetimeTimer` 4, `GetCurrentTimer` 2, `SetLifetimeTimer` | CDD_Lifetimer (free running 64-bit lifetime counter, the profiling timestamp source) |
| Safety (eMcem, sBoot, sCheck, Bist, SafetyBase) | `eMcem_Vfccu_*`, `sBoot_CheckConfiguration/GetError`, `sCheck_ExecuteStartupTests/...` | CDD_SafHandler |
| Wdg | project `Wdg_Stub` (`Wdg_Stub_Init/SetMode/SetTriggerCondition`), `Wdg_SetMode_AR`, `Wdg_SetTriggerCondition_AR` | WdgIf |
| Lin (Vector) | see Communication | LinIf |

Generated MCAL configuration lives in `Generated_BSW/include` (`*_Cfg.h`, `*_PBcfg.h`, IP layer configs: FlexCAN_Ip,
Lpspi, Emios_*, Siul2_*, Pit, Stm, Dma, Clock_Ip, Power_Ip, ...) and `Generated_BSW/src` (`*_Cfg.c`, `*_PBcfg.c`).
Symbolic names used by code follow `<Mod>Conf_<Container>_<Name>` (`DioConf_DioChannel_*`, `SpiConf_SpiSequence_*`,
`PwmConf_PwmChannel_*`, `IcuConf_IcuChannel_*`, `AdcConf_AdcGroup_*`, `GptConf_GptChannelConfiguration_*`, ...).

## Existing profiling tool chain (to stay compatible with)

`BSw/StartUpCode/profiling/Profiling_Trace.{c,h}` + `Tests/Profiling/*.py`: 64-bit records
`[timestamp 52 bit << 12 | id 12 bit]`, timestamp = `~CDD_Lifetimer_ticksGet()` (100 MHz lifetimer,
`PROFILING_TIMER_TO_MILLISECOND = 100000`), one ring buffer per core (512 KB) in LLC RAM, read out over XCP by
`Profiling.py` (Rimac Calibration Tool), decoded by `ProfilingAnalyzeData.py` (runnable statistics HTML, Gantt,
Perfetto `.pftrace` via `CustomTraceRecord.py`), IDs 0..4095 from `id_runnables.xml`/`id_isr.xml`
(Start/Stop id pairs). Macros: `MEASURE_TIMESTAMP_RUNNABLE_CORE_<n>(id)`, `MEASURE_TIMESTAMP_ISR_CORE_<n>(id)`.

## Test assets

`Tests/ECUSanityCheck` (ESCLib submodule: CAN/LIN/XCP based regression over DBC/LDF, NM frames per network,
flashing), `Tests/CAN_TX_Test` (CANalyzer projects + Tx period checker), `Tests/XCP` (A2L, calibration tool
project), `Res/UDS` (diagnostics database).
