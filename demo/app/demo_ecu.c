/*
 * demo_ecu.c - stand-in two core ECU exercising the OS features of the BR1 reference project:
 * RTE style extended tasks driven by SetEvent alarms, a basic task with multiple activations,
 * a schedule table, resources, spinlocks, cross core events (XSignal), a start-up barrier, IOC,
 * Cat2 ISRs raised by peripheral models with virtual latency, and runnable instrumentation.
 */
#include "Os.h"
#include "SilOs.h"
#include "SilTrace.h"
#include "sil_runtime.h"
#include "sil_port.h"

#include <stdio.h>

/* ---- runnable ids for tracing (would come from id_runnables.xml in the real project) ------ */
enum { R_1ms_Sense = 0, R_CanRx, R_SpiDone, R_10ms_Control, R_100ms_Diag, R_Bg_RamCheck, R_10ms_Core1, R_Event_Core1, R_Hi_Sched, R_Coop, R_COUNT };
static const char *const runnableNames[R_COUNT] = {"R_1ms_Sense", "R_CanRx", "R_SpiDone", "R_10ms_Control", "R_100ms_Diag",
                                                   "R_Bg_RamCheck", "R_10ms_Core1", "R_Event_Core1", "R_Hi_Sched", "R_Coop"};
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
    SilOs_RaiseIrq(Isr_SpiDone);                 /* called from a virtual timer: OS lock not held here? see below */
}
static void spi_start_transfer(uint32 mosi) {
    /* 8 bytes at 1 MHz = 64 us */
    SilOs_TimerSchedule(64000u, spi_transfer_done, (void *)(uintptr_t)mosi);
}

static void can_bus_model(void *arg) {
    (void)arg;
    SilOs_RaiseIrq(Isr_CanRx);
    SilOs_TimerSchedule(7000000u, can_bus_model, NULL); /* a frame every 7 ms */
}

static void button_model(void *arg) {
    (void)arg;
    SilOs_RaiseIrq(Isr_Button);
    SilOs_TimerSchedule(333000000u, button_model, NULL);
}

/* ---- shared data ------------------------------------------------------------------------- */
static uint32 g_sharedCounter;      /* protected by Res_Shared on core 0 */
static uint32 g_crossCoreValue;     /* protected by Spin_Shared */
static uint32 g_canFrames, g_spiDone, g_buttons, g_core1Events;

/* ---- hooks ------------------------------------------------------------------------------- */
void StartupHook(void) { sil_log("StartupHook on core %u", (unsigned)GetCoreID()); }
void ShutdownHook(StatusType Fatalerror) {
    sil_log("ShutdownHook(%u): can=%u spi=%u buttons=%u core1events=%u shared=%u", (unsigned)Fatalerror, g_canFrames, g_spiDone,
            g_buttons, g_core1Events, g_sharedCounter);
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
    (void)ActivateTask(Task_1ms_Core0);  /* extended tasks are activated once, as Rte_Start does */
    (void)ActivateTask(Task_10ms_Core0);
    (void)SetRelAlarm(Al_1ms_Core0, 1, 1);
    (void)SetRelAlarm(Al_10ms_Core0, 10, 10);
    (void)StartScheduleTableRel(SchTbl_Core0, 1);
    (void)SilOs_TimerSchedule(3000000u, can_bus_model, NULL);
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
                g_canFrames++;
                (void)IocWrite_Ioc_Speed_0(g_canFrames * 3u);
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
    }
}

TASK(Task_100ms_Core0) {
    static uint32 n;
    RUNNABLE(R_100ms_Diag, 1000000u, {
        n++;
        if ((n % 10u) == 0u)
            sil_log("t=%.3f s: can=%u spi=%u shared=%u core1events=%u", (double)SilOs_ClockNow() / 1e9, g_canFrames, g_spiDone,
                    g_sharedCounter, g_core1Events);
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
        RUNNABLE(R_Event_Core1, 150000u, { g_core1Events++; });
    }
}

/* ---- ISRs (core 0) ----------------------------------------------------------------------- */
ISR(Isr_CanRx) { (void)SetEvent(Task_1ms_Core0, Ev_CanRx); }
ISR(Isr_SpiDone) { (void)SetEvent(Task_1ms_Core0, Ev_SpiDone); }
ISR(Isr_Button) { g_buttons++; }

/* ---- main -------------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    SilRt_Options opt;
    int r = SilRt_ParseArgs(&opt, argc, argv);
    if (r != 0) return r < 0 ? 2 : 0;
    if (opt.os.durationNs == 0u) opt.os.durationNs = 2000000000u; /* 2 s default for the demo */
    r = SilRt_Run(&opt, demo_core_entry);
    sil_process_exit(r);
}
