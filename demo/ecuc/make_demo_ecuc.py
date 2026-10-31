#!/usr/bin/env python3
"""Builds the demo ECUC: DemoEcu.arxml (Os), DemoEcu_Can.arxml (Can) and DemoEcu_Mcal.arxml (Dio, Adc,
Pwm, Icu, Gpt, Spi, Fee, MemAcc, I2c, Uart, Lin) for a two core stand-in ECU with the object kinds the
BR1 reference project uses (extended RTE-style tasks, SetEvent alarms, spinlocks, XSignal, barriers, IOC,
driver ISRs through OsIsrSpecialFunctionName) plus a schedule table and a basic task with multiple
activations."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from ecuc_build import Cont, EcucModule, write_modules  # noqa: E402

HERE = Path(__file__).resolve().parent
os_ = EcucModule("Os", "/MICROSAR/Os")

os_.add(Cont("OsOS", "OsOS", {"OsNumberOfCores": 2, "OsStatus": "EXTENDED", "OsStackMonitoring": True, "OsUseResScheduler": False,
                              "OsScalabilityClass": "SC1", "OsUseGetServiceId": True, "OsUseParameterAccess": True},
              {"OsMasterCore": "OsCore0", "OsSystemTimer": "SystemTimer_Core0"},
              [Cont("OsHooks", "OsHooks", {"OsErrorHook": True, "OsShutdownHook": True, "OsStartupHook": True,
                                             "OsPreTaskHook": False, "OsPostTaskHook": False, "OsProtectionHook": False})]))
os_.add(Cont("OsCore", "OsCore0", {"OsCoreId": 0, "OsCoreIsAutostart": True}))
os_.add(Cont("OsCore", "OsCore1", {"OsCoreId": 1, "OsCoreIsAutostart": True}))
os_.add(Cont("OsAppMode", "OSDEFAULTAPPMODE"))
os_.add(Cont("OsAppMode", "OS_APPMODE_ANY"))

# counters: one hardware system timer per core, 1 ms tick
for c in (0, 1):
    os_.add(Cont("OsCounter", f"SystemTimer_Core{c}", {"OsCounterMaxAllowedValue": 1000000000, "OsCounterMinCycle": 1,
                                                        "OsCounterTicksPerBase": 40000, "OsCounterType": "HARDWARE",
                                                        "OsSecondsPerTick": 0.001},
                 subs=[Cont("OsDriver", "OsDriver", refs={"OsDriverIsrRef": f"CounterIsr_SystemTimer_Core{c}"})]))
    os_.add(Cont("OsIsr", f"CounterIsr_SystemTimer_Core{c}", {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": 15,
                                                               "OsIsrEnableNesting": False, "OsIsrInitialEnableInterruptSource": False,
                                                               "OsIsrSpecialFunctionName": "Os_TimerPfrtIsr", "OsIsrStackSize": 1024}))
    os_.add(Cont("OsIsr", f"XSignalIsr_OsCore{c}", {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": 16,
                                                     "OsIsrEnableNesting": False, "OsIsrInitialEnableInterruptSource": False,
                                                     "OsIsrSpecialFunctionName": "Os_XSigRecvIsrHandler", "OsIsrStackSize": 1024}))


def isr(name, prio, special=None, nesting=False):
    params = {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": prio, "OsIsrEnableNesting": nesting,
              "OsIsrInitialEnableInterruptSource": True, "OsIsrStackSize": 1024}
    if special:
        params["OsIsrSpecialFunctionName"] = special
    os_.add(Cont("OsIsr", name, params))
    return name


# user ISRs on core 0 (lower priority value = more urgent, as on ARM GIC)
USER_ISRS = [isr("Isr_CanRx", 32), isr("Isr_SpiDone", 16), isr("Isr_Button", 33, nesting=True)]
# driver ISRs: the special function names are the handler functions the virtual MCAL provides
# (CAN: tools/gen_can_cfg.py, SPI/I2C/UART: tools/gen_periph_cfg.py), reached through Os_Isr_<handler> wrappers
for ch in (0, 1):
    for kind, handler in (("Rx", f"CAN{ch}_ORED_0_31_MB_IRQHandler"), ("Tx", f"CAN{ch}_ORED_32_63_MB_IRQHandler"),
                          ("BusOff", f"CAN{ch}_ORED_IRQHandler")):
        USER_ISRS.append(isr(f"CanIsr_{ch}_{kind}", 32, handler))
USER_ISRS.append(isr("SpiIsr_0", 16, "Spi_Ip_SPI_0_IRQHandler"))
USER_ISRS.append(isr("I2cIsr_0", 32, "LPI2C0_Master_Slave_IRQHandler"))
USER_ISRS.append(isr("UartIsr_0", 32, "LINFLEXD0_UART_IRQHandler"))
# LIN: plain ISR bodies ISR(LinIsr_n){ Lin_Interrupt(ch); } as in the reference project (generated for the SIL)
USER_ISRS.append(isr("LinIsr_0", 32))
USER_ISRS.append(isr("LinIsr_1", 32))

# events
events = ["Ev_1ms", "Ev_10ms", "Ev_CanRx", "Ev_10ms_Core1", "Ev_Trigger_Core1", "Ev_SpiDone", "Ev_AdcDone", "Ev_NvDone"]
for e in events:
    os_.add(Cont("OsEvent", e, {"OsEventMask": "AUTO"}))


# tasks
def task(name, prio, sched="FULL", events=None, autostart=False, activation=1, stack=2048, resources=None):
    subs = [Cont("OsTaskAutostart", "OsTaskAutostart", refs={"OsTaskAppModeRef": ["OSDEFAULTAPPMODE"]})] if autostart else []
    refs = {}
    if events:
        refs["OsTaskEventRef"] = events
    if resources:
        refs["OsTaskResourceRef"] = resources
    os_.add(Cont("OsTask", name, {"OsTaskPriority": prio, "OsTaskSchedule": sched, "OsTaskActivation": activation,
                                  "OsTaskStackSize": stack, "OsTaskType": "AUTO"}, refs, subs))


task("IdleTask_OsCore0", 4294967295, autostart=True)
task("IdleTask_OsCore1", 4294967295, autostart=True)
task("InitTask_Core0", 40, "NON", autostart=True)
task("InitTask_Core1", 40, "NON", autostart=True)
task("Task_1ms_Core0", 30, events=["Ev_1ms", "Ev_CanRx", "Ev_SpiDone", "Ev_NvDone"], resources=["Res_Shared"])
task("Task_10ms_Core0", 20, events=["Ev_10ms", "Ev_AdcDone"], resources=["Res_Shared"])
task("Task_100ms_Core0", 10)
task("Task_Bg_Core0", 1, activation=3)
task("Task_Hi_Core0", 35)
task("Task_Coop_Core0", 5, "NON")
task("Task_10ms_Core1", 20, events=["Ev_10ms_Core1"])
task("Task_Event_Core1", 25, events=["Ev_Trigger_Core1"])


# alarms (all SetEvent except the 100 ms one, as in the reference project)
def alarm(name, counter, action, task, event=None, auto=None):
    act = Cont("OsAlarmAction", "OsAlarmAction")
    if action == "ActivateTask":
        act.add(Cont("OsAlarmActivateTask", "OsAlarmActivateTask", refs={"OsAlarmActivateTaskRef": task}))
    else:
        act.add(Cont("OsAlarmSetEvent", "OsAlarmSetEvent", refs={"OsAlarmSetEventRef": event, "OsAlarmSetEventTaskRef": task}))
    subs = [act]
    if auto:
        subs.append(Cont("OsAlarmAutostart", "OsAlarmAutostart", {"OsAlarmAutostartType": "RELATIVE", "OsAlarmAlarmTime": auto[0],
                                                                  "OsAlarmCycleTime": auto[1]},
                         refs={"OsAlarmAppModeRef": ["OSDEFAULTAPPMODE"]}))
    os_.add(Cont("OsAlarm", name, refs={"OsAlarmCounterRef": counter}, subs=subs))


alarm("Al_1ms_Core0", "SystemTimer_Core0", "SetEvent", "Task_1ms_Core0", "Ev_1ms")
alarm("Al_10ms_Core0", "SystemTimer_Core0", "SetEvent", "Task_10ms_Core0", "Ev_10ms")
alarm("Al_100ms_Core0", "SystemTimer_Core0", "ActivateTask", "Task_100ms_Core0", auto=(100, 100))
alarm("Al_10ms_Core1", "SystemTimer_Core1", "SetEvent", "Task_10ms_Core1", "Ev_10ms_Core1")

# schedule table on core 0 activating Task_Hi_Core0 twice per 50 ms round (two expiry points)
st = Cont("OsScheduleTable", "SchTbl_Core0", {"OsScheduleTableDuration": 50, "OsScheduleTableRepeating": True},
          refs={"OsScheduleTableCounterRef": "SystemTimer_Core0"})
st.add(Cont("OsScheduleTableExpiryPoint", "Ep_0", {"OsScheduleTblExpPointOffset": 0},
            subs=[Cont("OsScheduleTableTaskActivation", "Act_Bg", refs={"OsScheduleTableActivateTaskRef": "Task_Hi_Core0"})]))
st.add(Cont("OsScheduleTableExpiryPoint", "Ep_25", {"OsScheduleTblExpPointOffset": 25},
            subs=[Cont("OsScheduleTableTaskActivation", "Act_Hi2", refs={"OsScheduleTableActivateTaskRef": "Task_Hi_Core0"})]))
os_.add(st)

# resources, spinlocks, barriers
os_.add(Cont("OsResource", "Res_Shared", {"OsResourceProperty": "STANDARD"}))
os_.add(Cont("OsSpinlock", "Spin_Shared", {"OsSpinlockLockMethod": "LOCK_CAT2_INTERRUPTS", "OsSpinlockLockType": "STANDARD"}))
os_.add(Cont("OsSpinlock", "Spin_Plain", {"OsSpinlockLockMethod": "LOCK_NOTHING", "OsSpinlockLockType": "STANDARD"}))
os_.add(Cont("OsBarrier", "Barrier_Init", refs={"OsBarrierTaskRef": ["InitTask_Core0", "InitTask_Core1"]}))

# IOC: core 0 -> core 1 unqueued uint32 with two senders, and a queued channel
ioc = Cont("OsIoc", "OsIoc", {"OsIocIncludeHeader": "Std_Types.h"})
ioc.add(Cont("OsIocCommunication", "Ioc_Speed", {"OsIocBufferLength": 0},
             subs=[Cont("OsIocDataProperties", "Data0", {"OsIocDataType": "uint32", "OsIocDataTypeIsAtomic": True, "OsIocDataPropertyIndex": 0}),
                   Cont("OsIocSenderProperties", "S0", {"OsIocSenderId": 0}, {"OsIocSendingOsApplicationRef": "App_Core0"}),
                   Cont("OsIocSenderProperties", "S1", {"OsIocSenderId": 1}, {"OsIocSendingOsApplicationRef": "App_Core0"}),
                   Cont("OsIocReceiverProperties", "R", refs={"OsIocReceivingOsApplicationRef": "App_Core1"})]))
ioc.add(Cont("OsIocCommunication", "Ioc_Samples", {"OsIocBufferLength": 8},
             subs=[Cont("OsIocDataProperties", "Data0", {"OsIocDataType": "uint16", "OsIocDataTypeIsAtomic": True, "OsIocDataPropertyIndex": 0}),
                   Cont("OsIocSenderProperties", "S0", {"OsIocSenderId": 0}, {"OsIocSendingOsApplicationRef": "App_Core1"}),
                   Cont("OsIocReceiverProperties", "R", refs={"OsIocReceivingOsApplicationRef": "App_Core0"})]))
os_.add(ioc)

# applications
os_.add(Cont("OsApplication", "SystemApplication_OsCore0", {"OsTrusted": True},
             {"OsApplicationCoreRef": "OsCore0", "OsAppTaskRef": ["IdleTask_OsCore0"],
              "OsAppIsrRef": ["CounterIsr_SystemTimer_Core0", "XSignalIsr_OsCore0"], "OsAppCounterRef": ["SystemTimer_Core0"]}))
os_.add(Cont("OsApplication", "SystemApplication_OsCore1", {"OsTrusted": True},
             {"OsApplicationCoreRef": "OsCore1", "OsAppTaskRef": ["IdleTask_OsCore1"],
              "OsAppIsrRef": ["CounterIsr_SystemTimer_Core1", "XSignalIsr_OsCore1"], "OsAppCounterRef": ["SystemTimer_Core1"]}))
os_.add(Cont("OsApplication", "App_Core0", {"OsTrusted": True},
             {"OsApplicationCoreRef": "OsCore0",
              "OsAppTaskRef": ["InitTask_Core0", "Task_1ms_Core0", "Task_10ms_Core0", "Task_100ms_Core0", "Task_Bg_Core0", "Task_Hi_Core0", "Task_Coop_Core0"],
              "OsAppIsrRef": USER_ISRS,
              "OsAppAlarmRef": ["Al_1ms_Core0", "Al_10ms_Core0", "Al_100ms_Core0"],
              "OsAppScheduleTableRef": ["SchTbl_Core0"], "OsAppResourceRef": ["Res_Shared"]}))
os_.add(Cont("OsApplication", "App_Core1", {"OsTrusted": True},
             {"OsApplicationCoreRef": "OsCore1",
              "OsAppTaskRef": ["InitTask_Core1", "Task_10ms_Core1", "Task_Event_Core1"],
              "OsAppAlarmRef": ["Al_10ms_Core1"]}))

os_.write(HERE / "DemoEcu.arxml")
print(f"wrote {HERE / 'DemoEcu.arxml'}")

# ---- Can module (NXP RTD style container names): two controllers wired back to back by the demo ----
can = EcucModule("Can", "/TS_T40D34M50I0R0/Can")
can.add(Cont("CanGeneral", "CanGeneral", {"CanDevErrorDetect": False, "CanMainFunctionRwPeriod": 0.001}))
for ch in (0, 1):
    can.add(Cont("CanController", f"CT_CAN{ch}",
                 {"CanControllerId": ch, "CanHwChannel": f"FLEXCAN_{ch}", "CanControllerActivation": True,
                  "CanRxProcessing": "INTERRUPT", "CanTxProcessing": "INTERRUPT", "CanBusoffProcessing": "INTERRUPT"},
                 {"CanControllerDefaultBaudrate": f"CT_CAN{ch}/Baud500"},
                 [Cont("CanControllerBaudrateConfig", "Baud500", {"CanControllerBaudRate": 500, "CanControllerBaudRateConfigID": 0})]))


def hoh(name, oid, ctrl, kind, handle, idtype, count=1, filters=()):
    subs = [Cont("CanHwFilter", f"F{i}", {"CanHwFilterCode": code, "CanHwFilterMask": mask}) for i, (code, mask) in enumerate(filters)]
    can.add(Cont("CanHardwareObject", name, {"CanObjectId": oid, "CanObjectType": kind, "CanHandleType": handle, "CanIdType": idtype,
                                             "CanHwObjectCount": count}, {"CanControllerRef": ctrl}, subs))


hoh("Can0_Tx", 0, "CT_CAN0", "TRANSMIT", "BASIC", "MIXED", 8)
hoh("Can0_Rx_Cmd", 1, "CT_CAN0", "RECEIVE", "BASIC", "STANDARD", 1, [(0x200, 0x700)])   # commands 0x200..0x2FF
hoh("Can0_Rx_Ext", 2, "CT_CAN0", "RECEIVE", "BASIC", "EXTENDED", 1)                      # every extended id
hoh("Can1_Tx", 3, "CT_CAN1", "TRANSMIT", "BASIC", "MIXED", 8)
hoh("Can1_Rx_Status", 4, "CT_CAN1", "RECEIVE", "FULL", "STANDARD", 1, [(0x100, 0x7FF)]) # the status frame
hoh("Can1_Rx_Any", 5, "CT_CAN1", "RECEIVE", "BASIC", "MIXED", 1)
can.write(HERE / "DemoEcu_Can.arxml")
print(f"wrote {HERE / 'DemoEcu_Can.arxml'}")

# ---- the other MCAL modules (NXP RTD container and parameter names) ---------------------------------
D = "/TS_T40D34M50I0R0"
dio = EcucModule("Dio", f"{D}/Dio")
dio.add(Cont("DioPort", "PortA", {"DioPortId": 0}, subs=[
    Cont("DioChannel", "LED_GREEN", {"DioChannelId": 1}), Cont("DioChannel", "LED_RED", {"DioChannelId": 2}),
    Cont("DioChannel", "BUTTON", {"DioChannelId": 5}), Cont("DioChannel", "RELAY", {"DioChannelId": 7})]))
dio.add(Cont("DioPort", "PortB", {"DioPortId": 1}, subs=[Cont("DioChannel", "SENSOR_PWR", {"DioChannelId": 3})]))

adc = EcucModule("Adc", f"{D}/Adc")
adc.add(Cont("AdcHwUnit", "ADC_0", {"AdcHwUnitId": "ADC0", "AdcLogicalUnitId": 0}, subs=[
    Cont("AdcChannel", "AdcChannel_Vbat", {"AdcChannelId": 0, "AdcLogicalChannelId": 0}),
    Cont("AdcChannel", "AdcChannel_Temp", {"AdcChannelId": 1, "AdcLogicalChannelId": 1}),
    Cont("AdcGroup", "AdcGroup_Sense", {"AdcGroupId": 0, "AdcGroupConversionMode": "ADC_CONV_MODE_ONESHOT",
                                         "AdcGroupAccessMode": "ADC_ACCESS_MODE_SINGLE", "AdcGroupTriggSrc": "ADC_TRIGG_SRC_SW",
                                         "AdcStreamingNumSamples": 1, "AdcNotification": "Demo_AdcSenseNotification"},
         {"AdcGroupDefinition": ["AdcChannel_Vbat", "AdcChannel_Temp"]})]))

pwm = EcucModule("Pwm", f"{D}/Pwm")
pwm.add(Cont("PwmChannel", "PwmChannel_Fan", {"PwmChannelId": 0, "PwmPeriodDefault": 1000, "PwmDutycycleDefault": 0,
                                               "PwmPolarity": "PWM_HIGH", "PwmIdleState": "PWM_LOW"}))
pwm.add(Cont("PwmChannel", "PwmChannel_Heater", {"PwmChannelId": 1, "PwmPeriodDefault": 2000, "PwmDutycycleDefault": 0x4000,
                                                  "PwmPolarity": "PWM_HIGH", "PwmIdleState": "PWM_LOW"}))

icu = EcucModule("Icu", f"{D}/Icu")
icu.add(Cont("IcuChannel", "IcuChannel_Speed", {"IcuChannelId": 0, "IcuMeasurementMode": "ICU_MODE_SIGNAL_MEASUREMENT",
                                                 "IcuDefaultStartEdge": "ICU_RISING_EDGE"},
             subs=[Cont("IcuSignalMeasurement", "IcuSignalMeasurement", {"IcuSignalMeasurementProperty": "ICU_PERIOD_TIME"})]))
icu.add(Cont("IcuChannel", "IcuChannel_Hall", {"IcuChannelId": 1, "IcuMeasurementMode": "ICU_MODE_SIGNAL_EDGE_DETECT",
                                                "IcuDefaultStartEdge": "ICU_RISING_EDGE"},
             subs=[Cont("IcuSignalEdgeDetection", "IcuSignalEdgeDetection", {"IcuSignalNotification": "Demo_HallEdgeNotification"})]))
icu.add(Cont("IcuChannel", "IcuChannel_Pulses", {"IcuChannelId": 2, "IcuMeasurementMode": "ICU_MODE_EDGE_COUNTER",
                                                  "IcuDefaultStartEdge": "ICU_BOTH_EDGES"}))

gpt = EcucModule("Gpt", f"{D}/Gpt")
gpt.add(Cont("GptChannelConfiguration", "GptChannel_Timeout", {"GptChannelId": 0, "GptChannelMode": "GPT_CH_MODE_ONESHOT",
                                                               "GptChannelTickFrequency": 1000000, "GptChannelTickValueMax": 4294967295,
                                                               "GptNotification": "Demo_GptTimeoutNotification"}))

spi = EcucModule("Spi", f"{D}/Spi")
spi.add(Cont("SpiGeneral", "SpiGeneral", {"SpiLevelDelivered": 2}, subs=[
    Cont("SpiPhyUnit", "SpiPhyUnit_SPI0", {"SpiPhyUnitMapping": "SPI_0", "SpiPhyUnitMode": "SPI_MASTER"})]))
spi.add(Cont("SpiExternalDevice", "SpiExternalDevice_Sensor", {"SpiBaudrate": 1000000, "SpiCsIdentifier": "PCS0", "SpiHwUnit": "CSIB0",
                                                                "SpiTimeClk2Cs": 0.000001, "SpiTimeCs2Clk": 0.000001}))
spi.add(Cont("SpiChannel", "SpiChannel_Sensor", {"SpiChannelId": 0, "SpiChannelType": "EB", "SpiDataWidth": 8, "SpiEbMaxLength": 16,
                                                  "SpiIbNBuffers": 1, "SpiTransferStart": "MSB"}))
spi.add(Cont("SpiJob", "SpiJob_Sensor", {"SpiJobId": 0, "SpiJobPriority": 0}, {"SpiDeviceAssignment": "SpiExternalDevice_Sensor"},
             [Cont("SpiChannelList", "SpiChannelList", {"SpiChannelIndex": 0}, {"SpiChannelAssignment": "SpiChannel_Sensor"})]))
spi.add(Cont("SpiSequence", "SpiSequence_Sensor", {"SpiSequenceId": 0, "SpiInterruptibleSequence": False,
                                                    "SpiSeqEndNotification": "Demo_SpiSensorEndNotification"},
             {"SpiJobAssignment": ["SpiJob_Sensor"]}))

fee = EcucModule("Fee", f"{D}/Fee")
fee.add(Cont("FeeGeneral", "FeeGeneral", {"FeeVirtualPageSize": 8, "FeeNvmJobEndNotification": "Demo_NvJobEndNotification",
                                           "FeeNvmJobErrorNotification": "Demo_NvJobErrorNotification"}))
fee.add(Cont("FeeBlockConfiguration", "FeeBlock_Odometer", {"FeeBlockNumber": 1, "FeeBlockSize": 8, "FeeImmediateData": False}))
fee.add(Cont("FeeBlockConfiguration", "FeeBlock_Config", {"FeeBlockNumber": 2, "FeeBlockSize": 16, "FeeImmediateData": True}))

memacc = EcucModule("MemAcc", f"{D}/MemAcc")
memacc.add(Cont("MemAccAddressAreaConfiguration", "MemAccAddressArea_Data", {"MemAccAddressAreaId": 0, "MemAccJobEndNotificationName": "NULL_PTR"},
                subs=[Cont("MemAccSubAddressAreaConfiguration", "Sub0", {"MemAccLogicalStartAddress": 0, "MemAccNumberOfSectors": 4})]))

i2c = EcucModule("I2c", f"{D}/I2c")
i2c.add(Cont("I2cGlobalConfig", "I2cGlobalConfig", subs=[
    Cont("I2cChannel", "I2cChannel_Sensors", {"I2cChannelId": 0, "I2cHwChannel": "LPI2C_0", "I2cMasterSlaveConfiguration": "MASTER_MODE"},
         subs=[Cont("I2cMasterConfiguration", "I2cMasterConfiguration", {"I2cBaudRate": 400000})])]))

uart = EcucModule("Uart", f"{D}/Uart")
uart.add(Cont("UartChannel", "UartChannel_Terminal", {"UartChannelId": 0}, subs=[
    Cont("DetailModuleConfiguration", "DetailModuleConfiguration", {"UartHwChannel": "LinflexD_0", "DesireBaudrate": "LINFLEXD_UART_BAUDRATE_115200"})]))

lin = EcucModule("Lin", "/MICROSAR/Lin")
lin.add(Cont("LinGeneral", "LinGeneral", {"LinDevErrorDetect": True}))
for ch in (0, 1):
    lin.add(Cont("LinChannel", f"LinChannel_{ch}", {"LinChannelId": ch, "LinChannelBaudRate": 19200, "LinNodeType": "MASTER", "LinUart": ch,
                                                    "LinChannelWakeupSupport": False}))

write_modules(HERE / "DemoEcu_Mcal.arxml", [dio, adc, pwm, icu, gpt, spi, fee, memacc, i2c, uart, lin])
print(f"wrote {HERE / 'DemoEcu_Mcal.arxml'}")
