/*
 * demo_ecu.c - stand-in two core ECU exercising the OS features of the BR1 reference project:
 * RTE style extended tasks driven by SetEvent alarms, a basic task with multiple activations,
 * a schedule table, resources, spinlocks, cross core events (XSignal), a start-up barrier, IOC,
 * Cat2 ISRs raised by peripheral models with virtual latency, runnable instrumentation, and the
 * virtual CAN driver (Can_43_FLEXCAN API) with a tiny CanIf stand-in.
 *
 * CAN demo protocol (controller 0 = CT_CAN0, controller 1 = CT_CAN1, wired back to back through the
 * in-process link "loop:demo" unless --can<N>=... says otherwise):
 *   0x100  every 10 ms from CAN0: u32 counter, u8 buttons, u8 bus-off count, u8 rx count, u8 0
 *   0x201  command to CAN0 (any data): the ECU answers with 0x301 carrying the same data
 *   CAN1 receives everything the loop carries (0x100 through a FULL mailbox, the rest BASIC)
 */
#include "Os.h"
#include "SilOs.h"
#include "SilTrace.h"
#include "sil_runtime.h"
#include "sil_port.h"
#include "Can_43_FLEXCAN.h"
#include "SilCan.h"
#include "CanIf_Cbk.h"

#include <stdio.h>
#include <string.h>

/* ---- runnable ids for tracing (would come from id_runnables.xml in the real project) ------ */
enum { R_1ms_Sense = 0, R_CanRx, R_SpiDone, R_10ms_Control, R_100ms_Diag, R_Bg_RamCheck, R_10ms_Core1, R_Event_Core1, R_Hi_Sched, R_Coop, R_CanTx, R_COUNT };
static const char *const runnableNames[R_COUNT] = {"R_1ms_Sense", "R_CanRx", "R_SpiDone", "R_10ms_Control", "R_100ms_Diag",
                                                   "R_Bg_RamCheck", "R_10ms_Core1", "R_Event_Core1", "R_Hi_Sched", "R_Coop", "R_CanTx"};
#define RUNNABLE(id, etNs, body)      \
    do {                              \
        SilOs_RunnableStart(id);      \
        body;                         \
        SilOs_ConsumeTime(etNs);      \
        SilOs_RunnableStop(id);       \
    } while (0)

/* ---- peripheral models ------------------------------------------------------------------- */
static uint32 g_spiMiso;
static void spi_transfer_done(void *arg) {
    g_spiMiso = (uint32)(uintptr_t)arg ^ 0xA5u; /* slave echo model */
    SilOs_RaiseIrq(Isr_SpiDone);                 /* virtual timer callback: OS lock not held */
}
static void spi_start_transfer(uint32 mosi) {
    /* 8 bytes at 1 MHz = 64 us */
    SilOs_TimerSchedule(64000u, spi_transfer_done, (void *)(uintptr_t)mosi);
}

static void button_model(void *arg) {
    (void)arg;
    SilOs_RaiseIrq(Isr_Button);
    SilOs_TimerSchedule(333000000u, button_model, NULL);
}

/* ---- shared data ------------------------------------------------------------------------- */
static uint32 g_sharedCounter;      /* protected by Res_Shared on core 0 */
static uint32 g_crossCoreValue;     /* protected by Spin_Shared */
static uint32 g_spiDone, g_buttons;
static volatile uint32_t g_core1Events; /* written on core 1, read on core 0 */

/* ---- CanIf stand-in: the callbacks of the virtual CAN driver (CanIf in a real project) ---- */
#define DEMO_ID_STATUS 0x100u
#define DEMO_ID_CMD 0x201u
#define DEMO_ID_REPLY 0x301u
static uint32 g_canRx, g_canTxConf, g_canBusOff, g_canTxBusy, g_canCmd;
static volatile uint32 g_canRestart; /* bit per controller: recover from bus-off (what CanSM does) */
static uint8 g_cmdData[64];
static uint8 g_cmdLen;
static volatile uint32 g_cmdPending;

void CanIf_RxIndication(const Can_HwType *Mailbox, const PduInfoType *PduInfoPtr) {
    g_canRx++;
    if (Mailbox->ControllerId == CanConf_CanController_CT_CAN0 && (Mailbox->CanId & 0x1FFFFFFFu) == DEMO_ID_CMD) {
        g_cmdLen = (uint8)(PduInfoPtr->SduLength > 64u ? 64u : PduInfoPtr->SduLength);
        memcpy(g_cmdData, PduInfoPtr->SduDataPtr, g_cmdLen);
        g_cmdPending = 1u;
    }
    (void)SetEvent(Task_1ms_Core0, Ev_CanRx);
}
void CanIf_TxConfirmation(PduIdType CanTxPduId) {
    (void)CanTxPduId;
    g_canTxConf++;
}
void CanIf_ControllerBusOff(uint8 ControllerId) {
    g_canBusOff++;
    g_canRestart |= (1u << ControllerId);
}
void CanIf_ControllerModeIndication(uint8 ControllerId, Can_ControllerStateType ControllerMode) {
    (void)ControllerId;
    (void)ControllerMode;
}

static void can_send(Can_HwHandleType hth, uint32 id, const uint8 *data, uint8 len, PduIdType pdu) {
    Can_PduType p;
    p.id = id;
    p.length = len;
    p.sdu = (uint8 *)data;
    p.swPduHandle = pdu;
    if (Can_43_FLEXCAN_Write(hth, &p) != E_OK) g_canTxBusy++;
}

/* ---- hooks ------------------------------------------------------------------------------- */
void StartupHook(void) { sil_log("StartupHook on core %u", (unsigned)GetCoreID()); }
void ShutdownHook(StatusType Fatalerror) {
    sil_log("ShutdownHook(%u): canrx=%u cantx=%u busoff=%u spi=%u buttons=%u core1events=%u shared=%u", (unsigned)Fatalerror, g_canRx,
            g_canTxConf, g_canBusOff, g_spiDone, g_buttons, (unsigned)sil_atomic_load32(&g_core1Events), g_sharedCounter);
}
void ErrorHook(StatusType Error) {
    sil_log("ErrorHook: status %u service 0x%02X", (unsigned)Error, (unsigned)OSErrorGetServiceId());
}

/* ---- core entry (the project's main) ----------------------------------------------------- */
static void demo_core_entry(CoreIdType core) {
    Os_InitMemory();
    Os_Init();
    if (core == OS_CORE_ID_0) {
        StatusType st;
        uint32 i;
        for (i = 0; i < R_COUNT; ++i) SilTrace_SetName(SILTRACE_NAME_RUNNABLE, i, runnableNames[i]);
        StartCore(OS_CORE_ID_1, &st);
    }
    StartOS(OSDEFAULTAPPMODE);
}

/* ---- tasks core 0 ------------------------------------------------------------------------ */
TASK(InitTask_Core0) {
    /* driver init as EcuM_AL_DriverInitTwo + CanSM would do it */
    Can_43_FLEXCAN_Init(NULL_PTR);
    (void)Can_43_FLEXCAN_SetControllerMode(CanConf_CanController_CT_CAN0, CAN_CS_STARTED);
    (void)Can_43_FLEXCAN_SetControllerMode(CanConf_CanController_CT_CAN1, CAN_CS_STARTED);
    (void)ActivateTask(Task_1ms_Core0);  /* extended tasks are activated once, as Rte_Start does */
    (void)ActivateTask(Task_10ms_Core0);
    (void)SetRelAlarm(Al_1ms_Core0, 1, 1);
    (void)SetRelAlarm(Al_10ms_Core0, 10, 10);
    (void)StartScheduleTableRel(SchTbl_Core0, 1);
    (void)SilOs_TimerSchedule(100000000u, button_model, NULL);
    (void)Os_BarrierSynchronize(Barrier_Init);
    (void)TerminateTask();
}

TASK(Task_1ms_Core0) {
    static uint32 cycle;
    EventMaskType ev;
    for (;;) {
        (void)WaitEvent(Ev_1ms | Ev_CanRx | Ev_SpiDone);
        (void)GetEvent(Task_1ms_Core0, &ev);
        (void)ClearEvent(ev & (Ev_1ms | Ev_CanRx | Ev_SpiDone));
        if (ev & Ev_1ms) {
            RUNNABLE(R_1ms_Sense, 50000u, {
                cycle++;
                if ((cycle % 5u) == 0u) spi_start_transfer(cycle);
            });
        }
        if (ev & Ev_CanRx) {
            RUNNABLE(R_CanRx, 20000u, {
                (void)IocWrite_Ioc_Speed_0(g_canRx * 3u);
                if (g_cmdPending) {
                    g_cmdPending = 0u;
                    g_canCmd++;
                    can_send(CanConf_CanHardwareObject_Can0_Tx, DEMO_ID_REPLY, g_cmdData, g_cmdLen, 1u);
                }
            });
        }
        if (ev & Ev_SpiDone) {
            RUNNABLE(R_SpiDone, 10000u, { g_spiDone++; });
        }
    }
}

TASK(Task_10ms_Core0) {
    static uint32 n;
    for (;;) {
        (void)WaitEvent(Ev_10ms);
        (void)ClearEvent(Ev_10ms);
        RUNNABLE(R_10ms_Control, 300000u, {
            n++;
            (void)GetResource(Res_Shared);
            g_sharedCounter++;
            (void)ReleaseResource(Res_Shared);
            (void)GetSpinlock(Spin_Shared);
            g_crossCoreValue = n;
            (void)ReleaseSpinlock(Spin_Shared);
            if ((n % 10u) == 0u) (void)SetEvent(Task_Event_Core1, Ev_Trigger_Core1);
        });
        RUNNABLE(R_CanTx, 15000u, {
            uint8 d[8];
            uint32 restart = g_canRestart;
            uint8 c;
            for (c = 0; c < 2u; ++c) {
                if (restart & (1u << c)) {
                    g_canRestart &= ~(1u << c);
                    (void)Can_43_FLEXCAN_SetControllerMode(c, CAN_CS_STOPPED);
                    (void)Can_43_FLEXCAN_SetControllerMode(c, CAN_CS_STARTED);
                }
            }
            d[0] = (uint8)n;
            d[1] = (uint8)(n >> 8);
            d[2] = (uint8)(n >> 16);
            d[3] = (uint8)(n >> 24);
            d[4] = (uint8)g_buttons;
            d[5] = (uint8)g_canBusOff;
            d[6] = (uint8)g_canRx;
            d[7] = 0u;
            can_send(CanConf_CanHardwareObject_Can0_Tx, DEMO_ID_STATUS, d, 8u, 0u);
            Can_43_FLEXCAN_MainFunction_BusOff();
            Can_43_FLEXCAN_MainFunction_Mode();
        });
    }
}

TASK(Task_100ms_Core0) {
    static uint32 n;
    RUNNABLE(R_100ms_Diag, 1000000u, {
        n++;
        if ((n % 10u) == 0u)
            sil_log("t=%.3f s: canrx=%u cantx=%u cmd=%u spi=%u shared=%u core1events=%u", (double)SilOs_ClockNow() / 1e9, g_canRx,
                    g_canTxConf, g_canCmd, g_spiDone, g_sharedCounter, (unsigned)sil_atomic_load32(&g_core1Events));
        (void)ActivateTask(Task_Bg_Core0);
        (void)ActivateTask(Task_Bg_Core0);
    });
    (void)ChainTask(Task_Coop_Core0);
}

TASK(Task_Bg_Core0) {
    RUNNABLE(R_Bg_RamCheck, 100000u, {});
    (void)TerminateTask();
}

/* schedule table driven (every 25 ms) */
TASK(Task_Hi_Core0) {
    RUNNABLE(R_Hi_Sched, 40000u, {});
    (void)TerminateTask();
}

/* cooperative housekeeping, chained from the 100 ms task */
TASK(Task_Coop_Core0) {
    RUNNABLE(R_Coop, 500000u, {});
    (void)TerminateTask();
}

/* ---- tasks core 1 ------------------------------------------------------------------------ */
TASK(InitTask_Core1) {
    (void)ActivateTask(Task_10ms_Core1);
    (void)ActivateTask(Task_Event_Core1);
    (void)SetRelAlarm(Al_10ms_Core1, 10, 10);
    (void)Os_BarrierSynchronize(Barrier_Init);
    (void)TerminateTask();
}

TASK(Task_10ms_Core1) {
    uint32 speed = 0;
    uint16 sample = 0;
    for (;;) {
        (void)WaitEvent(Ev_10ms_Core1);
        (void)ClearEvent(Ev_10ms_Core1);
        RUNNABLE(R_10ms_Core1, 200000u, {
            uint32 v;
            (void)GetSpinlock(Spin_Shared);
            v = g_crossCoreValue;
            (void)ReleaseSpinlock(Spin_Shared);
            (void)IocRead_Ioc_Speed(&speed);
            sample = (uint16)(sample + 1u + (uint16)v);
            (void)IocSend_Ioc_Samples_0(sample);
        });
    }
}

TASK(Task_Event_Core1) {
    for (;;) {
        (void)WaitEvent(Ev_Trigger_Core1);
        (void)ClearEvent(Ev_Trigger_Core1);
        RUNNABLE(R_Event_Core1, 150000u, { (void)sil_atomic_add32(&g_core1Events, 1u); });
    }
}

/* ---- ISRs (core 0); the CAN ISRs are the driver's handlers reached through the OS wrappers --- */
ISR(Isr_CanRx) { (void)SetEvent(Task_1ms_Core0, Ev_CanRx); }
ISR(Isr_SpiDone) { (void)SetEvent(Task_1ms_Core0, Ev_SpiDone); }
ISR(Isr_Button) { g_buttons++; }

/* ---- main -------------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    SilRt_Options opt;
    int r = SilRt_ParseArgs(&opt, argc, argv);
    if (r != 0) return r < 0 ? 2 : 0;
    if (opt.os.durationNs == 0u && !opt.control) opt.os.durationNs = 2000000000u; /* 2 s default for the demo */
    /* both controllers on one in-process link; --can0=udp etc. override this */
    (void)SilCan_SetBus(0u, "loop:demo");
    (void)SilCan_SetBus(1u, "loop:demo");
    r = SilRt_Run(&opt, demo_core_entry);
    sil_process_exit(r);
}
