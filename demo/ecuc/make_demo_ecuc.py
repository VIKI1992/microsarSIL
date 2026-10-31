#!/usr/bin/env python3
"""Builds demo/ecuc/DemoEcu.arxml: a two core stand-in ECU with the object kinds the
BR1 reference project uses (extended RTE-style tasks, SetEvent alarms, spinlocks, XSignal,
barriers, IOC) plus a schedule table and a basic task with multiple activations."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from ecuc_build import Cont, EcucModule  # noqa: E402

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

# user ISRs on core 0 (lower priority value = more urgent, as on ARM GIC)
os_.add(Cont("OsIsr", "Isr_CanRx", {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": 32, "OsIsrEnableNesting": False,
                                     "OsIsrInitialEnableInterruptSource": True, "OsIsrStackSize": 1024}))
os_.add(Cont("OsIsr", "Isr_SpiDone", {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": 16, "OsIsrEnableNesting": False,
                                       "OsIsrInitialEnableInterruptSource": True, "OsIsrStackSize": 1024}))
os_.add(Cont("OsIsr", "Isr_Button", {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": 33, "OsIsrEnableNesting": True,
                                      "OsIsrInitialEnableInterruptSource": True, "OsIsrStackSize": 1024}))

# CAN driver ISRs on core 0: the special function names are the FlexCAN handler functions that
# tools/gen_can_cfg.py emits; the OS wrappers Os_Isr_<handler> call them (as in the reference project)
for ch in (0, 1):
    for kind, handler in (("Rx", f"CAN{ch}_ORED_0_31_MB_IRQHandler"), ("Tx", f"CAN{ch}_ORED_32_63_MB_IRQHandler"),
                          ("BusOff", f"CAN{ch}_ORED_IRQHandler")):
        os_.add(Cont("OsIsr", f"CanIsr_{ch}_{kind}", {"OsIsrCategory": "CATEGORY_2", "OsIsrInterruptPriority": 32,
                                                      "OsIsrEnableNesting": False, "OsIsrInitialEnableInterruptSource": True,
                                                      "OsIsrSpecialFunctionName": handler, "OsIsrStackSize": 1024}))
CAN_ISRS = [f"CanIsr_{ch}_{kind}" for ch in (0, 1) for kind in ("Rx", "Tx", "BusOff")]

# events
events = ["Ev_1ms", "Ev_10ms", "Ev_CanRx", "Ev_10ms_Core1", "Ev_Trigger_Core1", "Ev_SpiDone"]
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
task("Task_1ms_Core0", 30, events=["Ev_1ms", "Ev_CanRx", "Ev_SpiDone"], resources=["Res_Shared"])
task("Task_10ms_Core0", 20, events=["Ev_10ms"], resources=["Res_Shared"])
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
              "OsAppIsrRef": ["Isr_CanRx", "Isr_SpiDone", "Isr_Button"] + CAN_ISRS,
              "OsAppAlarmRef": ["Al_1ms_Core0", "Al_10ms_Core0", "Al_100ms_Core0"],
              "OsAppScheduleTableRef": ["SchTbl_Core0"], "OsAppResourceRef": ["Res_Shared"]}))
os_.add(Cont("OsApplication", "App_Core1", {"OsTrusted": True},
             {"OsApplicationCoreRef": "OsCore1",
              "OsAppTaskRef": ["InitTask_Core1", "Task_10ms_Core1", "Task_Event_Core1"],
              "OsAppAlarmRef": ["Al_10ms_Core1"]}))

out = Path(__file__).resolve().parent / "DemoEcu.arxml"
os_.write(out)
print(f"wrote {out}")

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
out = Path(__file__).resolve().parent / "DemoEcu_Can.arxml"
can.write(out)
print(f"wrote {out}")
