# Integrating a MICROSAR Classic project (M6, informed by the BR1 Cabin ECU)

This is the path from the demo to running `C:\br1_cabin_ecu` on the host. Nothing from the SIP
or the project enters this repository; the integration lives in the project's build (or a sibling
directory) and points at `microsarSIL` with `-DSIL_ROOT`.

## 1. Inventory

```
python tools/check_sip.py C:\br1_cabin_ecu --json cabin_inventory.json
```

Results for the Cabin ECU are in `reference-project-br1-cabin.md`: 2 cores, 18 tasks, 71 ISRs all
with special function names (driver handlers), 101 SetEvent/ActivateTask alarms, 84 spinlocks,
2 barriers, 8 IOC channels; OS API used is the standard set plus `Os_GetTaskStackUsage`,
`Os_InitialEnableInterruptSources`, `Os_BarrierSynchronize`, interrupt source control, peripheral
access and stack usage queries, all provided by SilOs.

## 2. Generate the OS configuration

```
python tools/gen_os_cfg.py -o <build>/gen --isr-bodies-from C:\br1_cabin_ecu\BSw\DaVinciWorkSpace\Generated_BSW ^
   C:\br1_cabin_ecu\BSw\DaVinciWorkSpace\Config\ECUC\Cabin_ECU_Os_Os_ecuc.arxml ^
   C:\br1_cabin_ecu\BSw\DaVinciWorkSpace\Config\ECUC\Cabin_ECU_EcuC_EcuC_ecuc.arxml
```

`--isr-bodies-from` finds the four `ISR(LinIsr_n)` bodies in `Lin_Lcfg.c`, so wrappers are
generated only for the 63 driver handlers (`CAN15_ORED_IRQHandler`, `Spi_Ip_SPI_2_IRQHandler`,
`LPI2C1_Master_Slave_IRQHandler`, `EMIOS1_CH3_IRQn`, ...) which the virtual MCAL implements.

## 3. Include path order

1. `<build>/gen` (generated OS headers) and `sil/os`, `sil/os/compat`, `sil/hal` (host
   `Compiler.h`, `Platform_Types.h`, `Std_Types.h`, no-op `MemMap.h`, `Os_*_MemMap.h`).
2. The project's own `BSw/StartUpCode/inc` **minus** `Compiler.h`, `MemMap.h`, `PlatformTypes.h`
   (the project `Compiler_Cfg.h` chain is reusable: it ends in the generated `Os_Compiler_Cfg.h`).
3. `Generated_BSW`, `Generated_BSW/Components`, then the Vector `Sip/Components/<Mod>/Implementation`
   directories of the modules that are compiled, then the virtual MCAL headers instead of the RTD
   plugin directories.

The generated `Generated_BSW/Os_*` files, `Generated_BSW/include` + `src` (RTD configuration),
`vBrs_*`, `vLinkGen_*` and `BSw/StartUpCode/src/{ARM*,BrsHw*,BrsMain*,Etimer_Workaround,ddr}` are
**not** compiled; `main` is replaced by `sil_ecu_entry(core)`:

```c
void sil_ecu_entry(CoreIdType core) { Os_InitMemory(); Os_Init(); EcuM_Init(); }
```

`EcuM_Init` -> `EcuM_AL_DriverInitOne` -> `EcuM_StartOS` -> `StartCore(1)` + `StartOS` already match
the reference project's callouts. `Default_Init_Task*` bodies come from `BrsMain.c`; they are
plain tasks (`EcuM_StartupTwo`, `Os_InitialEnableInterruptSources`) and can be compiled from a
small SIL copy of that file section.

## 4. Virtual MCAL (API surface from the inventory)

| Module | Must provide | Model |
|---|---|---|
| Can_43_FLEXCAN (**done, M3**) | `Init, SetControllerMode, Write, MainFunction_{Read,Write,BusOff,Mode,Wakeup}(+_CanMainFunctionRWPeriods_n), GetControllerErrorState, GetControllerRx/TxErrorCounter, SetBaudrate, DisableControllerInterrupts`, `CAN<n>_ORED*/EnhancedData_IRQHandler` | per controller queue + bus backend (UDP, SIL Kit, SocketCAN); RX in ISR context via `CanIf_RxIndication`, TX confirmation via `CanIf_TxConfirmation` |
| Lin (Vector driver replaced, **done, M4**) | `Lin_Init/InitMemory/SendFrame/GetStatus/GoToSleep(Internal)/Wakeup(Internal)/CheckWakeup/Interrupt` | 4 master channels, frame response from LDF or Python slaves, `LinIsr_n` through `Lin_Interrupt` |
| Spi (**done, M4**; device models open) | `Init, SetupEB, SyncTransmit, AsyncTransmit, GetSequenceResult, GetStatus, GetHWUnitStatus, SetAsyncMode, SetHWUnitAsyncMode, MainFunction_Handling`, `Spi_Ip_SPI_{2,3,5,7}_IRQHandler` | sequences/jobs/channels from `SpiSequence`/`SpiJob`/`SpiChannel`; slave models for ADS7038 (x10), BTS710336 (x3), BTS722204 (x2), DRV8910, L9305 |
| Dio, Port, Pwm, Icu, Adc, Gpt (**done, M4**) | the APIs listed in the inventory | `VirtualIo` named values settable from the control server; Icu edges and Adc groups raise their eMIOS/ADC ISRs |
| I2c, Uart (**done, M4**: register file slaves, UART to the log) | `I2c_SyncTransmit/AsyncTransmit/GetStatus/Init`, `Uart_SyncSend/AsyncSend/AsyncReceive/Abort/GetStatus/Init` | PCA9539 port expander and temperature sensor register models; UART to stdout |
| Mcu, Platform, OsIf, Pit_Ip, Mcl (**done, M4**) | `Mcu_Init/InitClock/GetPllStatus/DistributePllClock/SetMode/PerformReset/GetResetReason`, `Platform_Init/GetCoreID/Mru*`, `OsIf_Init/GetCoreID/GetUserId`, `Pit_Ip_Get/SetLifetimeTimer` | stubs; `PerformReset` re-executes the process; lifetime timer = virtual clock at 100 MHz |
| Fee (NXP) / MemAcc / Mem_43_* (**done, M4**) | `Fee_*` job API, `MemAcc_*`, `Mem_43_*_Init/MainFunction` | file-backed emulation, persists across resets |
| Crypto_43_HSE, Hse_Ip | `Init, MainFunction, KeyElementGet, Exts_FormatKeyCatalogs`, `Hse_Ip_GetHseStatus/SendHseEvent` | software stand-in completing jobs (optionally through vSecPrim) |
| eMcem, sBoot, sCheck, Bist, SafetyBase, Wdg stub | listed APIs | stubs returning "no fault" |

`Generated_BSW/include/<Mod>_Cfg.h` are replaced by SIL versions generated from the MCAL ECUC
files so that the symbolic names (`DioConf_DioChannel_*`, `SpiConf_SpiSequence_*`, ...) keep their
values.

Peripheral tables: `tools/gen_periph_cfg.py -o <gen> Config/ECUC/Cabin_ECU_{Dio,Adc,Pwm,Icu,Gpt,Spi,Fee,MemAcc,Mem,I2c,Uart,Lin}_*_ecuc.arxml`
(`silos_add_ecu` runs it on the ARXML list). It also emits the handler functions of the SPI units, I2C
and UART channels and the `ISR(LinIsr_<n>)` bodies, so `Lin_Lcfg.c` of the Vector driver is not compiled.
Interrupt handlers no virtual peripheral serves (`ETIMER_1_0_CH4_IRQ`, `EMIOS1_CH3_IRQ`, `I3c0_Isr`, ...)
get empty bodies through `--isr-stubs NAME,...`; the Icu/Pwm models run their notifications through the
soft ISR instead. Notification functions named in the ECUC (`BTS7103_SpiSequence1WriteVerifyEndNotification`,
`IcuChannel_HALL4_EdgeDetection`, `NvM_JobEndNotification`, ...) are declared extern and must exist in the
linked project code.

CAN specifics: `Can_43_FLEXCAN_Init(NULL_PTR)` as the project calls it is accepted;
`tools/gen_can_cfg.py --channels 13` emits handler functions for FlexCAN channels the OS references
but no controller uses (the Cabin ECU has ISRs for channel 13); the generated `Can_43_FLEXCAN_PBcfg.c`
of the project is not compiled. `sil/hal/default/ComStack_Types.h` and `CanIf_Cbk.h` are fallbacks for
builds without a SIP: the project's own headers (`Sip/Components/_Common`, `CanIf`) must come first
on the include path.

## 5. Runnable tracing

The RTE of the reference project is generated without VFB trace hooks. The project's own
`Tests/Profiling/instrumentSrc.py` inserts `MEASURE_TIMESTAMP_RUNNABLE_CORE_<n>(id)` around every
`/* call runnable */` site; in the SIL these macros map to `SilOs_RunnableStart/Stop(id)` and the
`--rimac=<dir>` output feeds the existing `ProfilingAnalyzeData.py`. Enabling VFB tracing in
DaVinci instead works through a generated `Rte_Hook.c` (M5).

## 6. Open points to confirm on the project

- 64-bit host: the SIP compiles with `-Werror=pointer-to-int-cast`; a 32-bit MinGW build is the
  fallback.
- `SchM_Enter_*` are `SuspendAllInterrupts` (625 sites): fine with SilOs interrupt locks.
- `CDD_FETK` includes an OS HAL header and talks to ETAS hardware: excluded from the SIL build.
- NvM/Fee: the NXP Fee sits on MemAcc/Mem_43_INFLS; the SIL Fee replaces all three.

## 7. Host compile probe (2026-10-31)

`tools/probe_sip_compile.py` syntax-checks real project sources with the host compiler, the SIL
headers and the generated configuration (no virtual MCAL implementation needed yet):

```
python tools/gen_os_cfg.py -o build/gen-cabin --isr-bodies-from C:\br1_cabin_ecu\BSw\DaVinciWorkSpace\Generated_BSW ^
    <Os ecuc> <EcuC ecuc>
python tools/gen_mcal_cfg.py -o build/gen-cabin-mcal <Mcu Dio Port Spi Pwm Icu Adc Gpt Can I2c Uart MemAcc Fee Crypto ecuc files>
python tools/probe_sip_compile.py C:\br1_cabin_ecu --gen build/gen-cabin --extra-inc sil/mcal/inc build/gen-cabin-mcal ^
    --defines SIL_CPU_TYPE=32 --out build/probe_cabin.md
```

Result on the Cabin ECU: **43 of 45** probed files compile cleanly on the 64-bit MinGW host:
all 29 Vector BSW modules of the probe list (Det, Crc, VStdLib, EcuM, BswM, Com, PduR, CanIf,
CanSM, CanNm, Nm, ComM, CanTp, Dcm, Dem, NvM, MemIf, WdgM, WdgIf, LinIf, LinSM, IpduM, Csm,
CryIf, E2E, E2EXf, FiM, Xcp, CanXcp), the complete RTE (`Rte.c`, both
`Rte_OsApplication_Trusted_Core*.c`), the generated BswM/EcuM/CanIf/Com configuration, the OS
callouts, the stack monitor SWC and the I2C/AIN/BTS7222/Lifetimer CDDs. The two remaining files
are hardware init code that the SIL replaces anyway: `EcuM_Callout_Stubs.c` (`ddr_init.h`, DDR
controller) and `BswM_Callout_Stubs.c` (`CDD_Sbc_Fs8x.h`, the SBC driver stub comes with M4).

What made this work: `sil/hal` headers first on the include path, `SIL_CPU_TYPE=32` so the
EcuC/CanIf CPU type checks pass, `E_SAFETY_INVALID_*` values from the E2E transformer spec,
`sil/mcal/inc` with the AUTOSAR/RTD API headers (types match the Vector
`Can_DrvGeneralTypes_3rdPartyCanDrv.h` layout), and the generated symbolic names
(`McuConf_*`, `DioConf_*`, `SpiConf_*`, ... 975 names for the Cabin ECU).
