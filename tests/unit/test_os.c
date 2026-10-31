/*
 * test_os.c - OSEK/AUTOSAR semantics tests for SilOs, run as an ECU on the demo configuration.
 *
 * Task_100ms_Core0 (prio 10, basic, autostarted by alarm at t=100 ms) drives the scenarios. Tasks and ISRs
 * append entries to a log; every scenario asserts the exact order of the core-0 entries since its mark.
 * Phase 2 (a second activation of the driver) checks what the lower priority tasks did after phase 1.
 */
#include "Os.h"
#include "SilOs.h"
#include "sil_runtime.h"
#include "CanIf_Cbk.h"
#include "sil_port.h"

#include <stdio.h>
#include <string.h>

/* ---- log ------------------------------------------------------------------------------ */
typedef struct {
    const char *tag;
    uint32 arg;
    uint64 t_ns;
    uint8 core;
} LogEntry;

#define LOG_MAX 4096
static LogEntry g_log[LOG_MAX];
static volatile uint32 g_logn;
static sil_mutex_t *g_logLock;
static uint32 g_mark;
static int g_failures;
static int g_checks;
static uint32 g_errorHookCalls;
static StatusType g_lastHookError;
static OSServiceIdType g_lastHookService;
static int g_phase;
static int g_canRaiseSpiInIsr;
static int g_hiChains;

static void logx(const char *tag, uint32 arg) {
    sil_mutex_lock(g_logLock);
    if (g_logn < LOG_MAX) {
        g_log[g_logn].tag = tag;
        g_log[g_logn].arg = arg;
        g_log[g_logn].t_ns = SilOs_ClockNow();
        g_log[g_logn].core = (uint8)GetCoreID();
        g_logn++;
    }
    sil_mutex_unlock(g_logLock);
}
#define LOG(tag) logx(tag, 0u)

static void fail(const char *what, const char *detail) {
    g_failures++;
    sil_log("FAIL: %s %s", what, detail != NULL ? detail : "");
}

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) fail(#cond, __func__);                                         \
    } while (0)

/* Compare the core-0 (or any core if coreFilter == 0xFF) entries since mark with the expected tags. */
static void expect_seq(uint8 coreFilter, const char *const *tags, uint32 n, const char *name) {
    uint32 i, j = 0;
    char detail[512];
    int ok = 1;
    sil_mutex_lock(g_logLock);
    for (i = g_mark; i < g_logn; ++i) {
        if (coreFilter != 0xFFu && g_log[i].core != coreFilter) continue;
        if (j < n && strcmp(g_log[i].tag, tags[j]) == 0) {
            j++;
        } else {
            ok = 0;
        }
    }
    if (j != n) ok = 0;
    g_checks++;
    if (!ok) {
        size_t len = 0;
        detail[0] = '\0';
        for (i = g_mark; i < g_logn && len < sizeof(detail) - 40u; ++i) {
            if (coreFilter != 0xFFu && g_log[i].core != coreFilter) continue;
            len += (size_t)snprintf(detail + len, sizeof(detail) - len, "%s ", g_log[i].tag);
        }
        g_failures++;
        sil_log("FAIL: %s: expected [%s...] got [%s]", name, tags[0], detail);
    }
    sil_mutex_unlock(g_logLock);
}

static uint32 count_tag(const char *tag) {
    uint32 i, n = 0;
    sil_mutex_lock(g_logLock);
    for (i = 0; i < g_logn; ++i)
        if (strcmp(g_log[i].tag, tag) == 0) n++;
    sil_mutex_unlock(g_logLock);
    return n;
}

static const LogEntry *find_last(const char *tag) {
    uint32 i;
    const LogEntry *e = NULL;
    sil_mutex_lock(g_logLock);
    for (i = 0; i < g_logn; ++i)
        if (strcmp(g_log[i].tag, tag) == 0) e = &g_log[i];
    sil_mutex_unlock(g_logLock);
    return e;
}

static void mark(void) { g_mark = g_logn; }

/* wait (consuming virtual time) until a tag has been logged at least n times */
static int wait_count(const char *tag, uint32 n, uint32 maxMs) {
    uint32 ms;
    for (ms = 0; ms < maxMs; ++ms) {
        if (count_tag(tag) >= n) return 1;
        SilOs_ConsumeTime(1000000u);
    }
    return count_tag(tag) >= n;
}

/* ---- hooks ---------------------------------------------------------------------------- */
void StartupHook(void) { LOG("startuphook"); }
void ShutdownHook(StatusType Fatalerror) {
    (void)Fatalerror;
    sil_log("test_os: %d checks, %d failures", g_checks, g_failures);
}
void ErrorHook(StatusType Error) {
    g_errorHookCalls++;
    g_lastHookError = Error;
    g_lastHookService = OSErrorGetServiceId();
}

/* ---- core entry ----------------------------------------------------------------------- */
static void entry(CoreIdType core) {
    Os_InitMemory();
    Os_Init();
    if (core == OS_CORE_ID_0) {
        StatusType st;
        g_logLock = sil_mutex_create();
        StartCore(OS_CORE_ID_1, &st);
    }
    StartOS(OSDEFAULTAPPMODE);
}

/* ---- helper tasks ---------------------------------------------------------------------- */
TASK(InitTask_Core0) {
    LOG("init0_start");
    (void)Os_BarrierSynchronize(Barrier_Init);
    LOG("init0_pass");
    (void)TerminateTask();
}

TASK(InitTask_Core1) {
    LOG("init1_start");
    (void)ActivateTask(Task_10ms_Core1);
    (void)ActivateTask(Task_Event_Core1);
    (void)Os_BarrierSynchronize(Barrier_Init);
    LOG("init1_pass");
    (void)TerminateTask();
}

TASK(Task_Hi_Core0) {
    LOG("hi");
    if (g_hiChains) {
        g_hiChains = 0;
        (void)ChainTask(Task_Hi_Core0); /* chain to itself: runs again immediately */
    }
    (void)TerminateTask();
}

TASK(Task_Coop_Core0) {
    LOG("coop");
    (void)TerminateTask();
}

TASK(Task_Bg_Core0) {
    LOG("bg");
    (void)TerminateTask();
}

TASK(Task_1ms_Core0) {
    EventMaskType ev;
    LOG("t1_start");
    for (;;) {
        TickType tick = 0;
        (void)WaitEvent(Ev_1ms | Ev_CanRx | Ev_SpiDone);
        (void)GetEvent(Task_1ms_Core0, &ev);
        (void)ClearEvent(ev);
        (void)GetCounterValue(SystemTimer_Core0, &tick);
        if (ev & Ev_1ms) logx("t1_ev_1ms", tick);
        if (ev & Ev_CanRx) logx("t1_ev_canrx", tick);
        if (ev & Ev_SpiDone) logx("t1_ev_spi", tick);
    }
}

TASK(Task_10ms_Core0) {
    LOG("t10_start");
    for (;;) {
        TickType tick = 0;
        (void)WaitEvent(Ev_10ms);
        (void)ClearEvent(Ev_10ms);
        (void)GetCounterValue(SystemTimer_Core0, &tick);
        logx("t10_ev", tick);
    }
}

TASK(Task_10ms_Core1) {
    for (;;) {
        uint32 v = 0;
        (void)WaitEvent(Ev_10ms_Core1);
        (void)ClearEvent(Ev_10ms_Core1);
        (void)GetSpinlock(Spin_Plain);
        LOG("c1_spin_got");
        (void)ReleaseSpinlock(Spin_Plain);
        (void)IocRead_Ioc_Speed(&v);
        logx("c1_ioc", v);
    }
}

TASK(Task_Event_Core1) {
    for (;;) {
        (void)WaitEvent(Ev_Trigger_Core1);
        (void)ClearEvent(Ev_Trigger_Core1);
        LOG("t_ev_core1");
    }
}

ISR(Isr_CanRx) {
    LOG("isr_canrx");
    if (g_canRaiseSpiInIsr) {
        SilOs_RaiseIrq(Isr_SpiDone);
        SilOs_PreemptionPoint();
        LOG("isr_canrx_end");
    }
    (void)SetEvent(Task_1ms_Core0, Ev_CanRx);
}

ISR(Isr_SpiDone) {
    LOG("isr_spi");
    (void)SetEvent(Task_1ms_Core0, Ev_SpiDone);
}

ISR(Isr_Button) {
    LOG("btn_start");
    SilOs_RaiseIrq(Isr_SpiDone);
    SilOs_PreemptionPoint();
    LOG("btn_end");
}

/* ---- scenarios ------------------------------------------------------------------------- */
static void s_preemption(void) {
    static const char *const seq[] = {"drv_a", "hi", "drv_b"};
    mark();
    LOG("drv_a");
    CHECK(ActivateTask(Task_Hi_Core0) == E_OK);
    LOG("drv_b");
    expect_seq(0, seq, 3, "preemption by higher priority task");
}

static void s_lower_prio_waits(void) {
    TaskStateType st = SUSPENDED;
    static const char *const seq[] = {"drv_c"};
    mark();
    CHECK(ActivateTask(Task_Coop_Core0) == E_OK);
    CHECK(GetTaskState(Task_Coop_Core0, &st) == E_OK && st == READY);
    LOG("drv_c");
    expect_seq(0, seq, 1, "lower priority task does not preempt");
    /* it runs after the driver terminates: checked in phase 2 */
}

static void s_extended_events(void) {
    TaskStateType st = SUSPENDED;
    static const char *const seq[] = {"t1_start", "drv_d", "t1_ev_canrx", "drv_e"};
    mark();
    CHECK(ActivateTask(Task_1ms_Core0) == E_OK);
    LOG("drv_d");
    CHECK(GetTaskState(Task_1ms_Core0, &st) == E_OK && st == WAITING);
    CHECK(SetEvent(Task_1ms_Core0, Ev_CanRx) == E_OK);
    LOG("drv_e");
    CHECK(GetTaskState(Task_1ms_Core0, &st) == E_OK && st == WAITING);
    expect_seq(0, seq, 4, "extended task wait/release");
    /* SetEvent on a basic task and on a suspended extended task */
    CHECK(SetEvent(Task_Hi_Core0, 1u) == E_OS_ACCESS);
    CHECK(SetEvent(Task_10ms_Core0, Ev_10ms) == E_OS_STATE);
}

static void s_multiple_activation(void) {
    uint32 before = g_errorHookCalls;
    CHECK(ActivateTask(Task_Bg_Core0) == E_OK);
    CHECK(ActivateTask(Task_Bg_Core0) == E_OK);
    CHECK(ActivateTask(Task_Bg_Core0) == E_OK);
    CHECK(ActivateTask(Task_Bg_Core0) == E_OS_LIMIT);
    CHECK(g_errorHookCalls == before + 1u);
    CHECK(g_lastHookError == E_OS_LIMIT && g_lastHookService == OSServiceId_ActivateTask);
    CHECK(OSError_ActivateTask_TaskID() == Task_Bg_Core0);
}

static void s_resource_ceiling(void) {
    static const char *const seq[] = {"drv_in_res", "hi", "drv_in_res2", "t10_start", "drv_after_res"};
    mark();
    CHECK(GetResource(Res_Shared) == E_OK);
    LOG("drv_in_res");
    CHECK(ActivateTask(Task_10ms_Core0) == E_OK); /* prio 20 <= ceiling 30: must wait */
    CHECK(ActivateTask(Task_Hi_Core0) == E_OK);   /* prio 35 > ceiling: preempts */
    LOG("drv_in_res2");
    CHECK(GetResource(Res_Shared) == E_OS_ACCESS);
    CHECK(ReleaseResource(Res_Shared) == E_OK);
    LOG("drv_after_res");
    CHECK(ReleaseResource(Res_Shared) == E_OS_NOFUNC);
    expect_seq(0, seq, 5, "resource ceiling");
}

static void s_isr_delivery(void) {
    static const char *const seq[] = {"drv_f", "isr_canrx", "t1_ev_canrx", "drv_g"};
    mark();
    LOG("drv_f");
    SilOs_RaiseIrq(Isr_CanRx);
    SilOs_PreemptionPoint();
    LOG("drv_g");
    expect_seq(0, seq, 4, "ISR delivery and task release from ISR");
}

static void s_interrupt_lock(void) {
    static const char *const seq[] = {"drv_locked", "drv_locked2", "isr_canrx", "t1_ev_canrx", "drv_unlocked"};
    mark();
    SuspendAllInterrupts();
    SilOs_RaiseIrq(Isr_CanRx);
    SilOs_PreemptionPoint();
    LOG("drv_locked");
    SuspendAllInterrupts();
    ResumeAllInterrupts();
    LOG("drv_locked2"); /* still locked: nesting */
    ResumeAllInterrupts();
    LOG("drv_unlocked");
    expect_seq(0, seq, 5, "SuspendAllInterrupts nesting");
    mark();
    SuspendOSInterrupts();
    SilOs_RaiseIrq(Isr_SpiDone);
    SilOs_PreemptionPoint();
    LOG("drv_os_locked");
    ResumeOSInterrupts();
    LOG("drv_os_unlocked");
    {
        static const char *const seq2[] = {"drv_os_locked", "isr_spi", "t1_ev_spi", "drv_os_unlocked"};
        expect_seq(0, seq2, 4, "SuspendOSInterrupts");
    }
}

static void s_isr_nesting(void) {
    static const char *const seq[] = {"btn_start", "isr_spi", "btn_end", "t1_ev_spi", "drv_h"};
    static const char *const seq2[] = {"isr_canrx", "isr_canrx_end", "isr_spi", "t1_ev_canrx", "t1_ev_spi", "drv_i"};
    mark();
    SilOs_RaiseIrq(Isr_Button); /* nesting enabled: SpiDone (more urgent) preempts it */
    SilOs_PreemptionPoint();
    LOG("drv_h");
    expect_seq(0, seq, 5, "ISR nesting enabled");
    mark();
    g_canRaiseSpiInIsr = 1;
    SilOs_RaiseIrq(Isr_CanRx); /* nesting disabled: SpiDone runs after CanRx */
    SilOs_PreemptionPoint();
    g_canRaiseSpiInIsr = 0;
    LOG("drv_i");
    expect_seq(0, seq2, 6, "ISR nesting disabled");
}

static void s_alarm_oneshot(void) {
    TickType t0 = 0;
    const LogEntry *e;
    mark();
    CHECK(GetCounterValue(SystemTimer_Core0, &t0) == E_OK);
    CHECK(SetRelAlarm(Al_1ms_Core0, 5, 0) == E_OK);
    CHECK(SetRelAlarm(Al_1ms_Core0, 5, 0) == E_OS_STATE);
    SilOs_ConsumeTime(10000000u);
    e = find_last("t1_ev_1ms");
    CHECK(e != NULL);
    if (e != NULL) CHECK(e->arg == t0 + 5u);
    CHECK(count_tag("t1_ev_1ms") == 1u);
    {
        TickType rem = 0;
        CHECK(GetAlarm(Al_1ms_Core0, &rem) == E_OS_NOFUNC);
        CHECK(CancelAlarm(Al_1ms_Core0) == E_OS_NOFUNC);
    }
}

static void s_alarm_cyclic(void) {
    TickType t0 = 0, rem = 0;
    uint32 before = count_tag("t10_ev");
    CHECK(GetCounterValue(SystemTimer_Core0, &t0) == E_OK);
    CHECK(SetRelAlarm(Al_10ms_Core0, 10, 10) == E_OK);
    CHECK(GetAlarm(Al_10ms_Core0, &rem) == E_OK && rem == 10u);
    SilOs_ConsumeTime(35000000u);
    CHECK(count_tag("t10_ev") == before + 3u);
    {
        const LogEntry *e = find_last("t10_ev");
        CHECK(e != NULL && e->arg == t0 + 30u);
    }
    CHECK(CancelAlarm(Al_10ms_Core0) == E_OK);
    SilOs_ConsumeTime(20000000u);
    CHECK(count_tag("t10_ev") == before + 3u);
    /* value checks */
    CHECK(SetRelAlarm(Al_10ms_Core0, 0, 0) == E_OS_VALUE);
    CHECK(SetRelAlarm(Al_10ms_Core0, 1, 2000000000u) == E_OS_VALUE);
    CHECK(SetRelAlarm(9999u, 1, 0) == E_OS_ID);
}

static void s_schedule_table(void) {
    uint64 a0 = SilOs_TaskActivations(Task_Hi_Core0);
    uint32 h0 = count_tag("hi");
    ScheduleTableStatusType st = 99u;
    CHECK(StartScheduleTableRel(SchTbl_Core0, 10) == E_OK);
    CHECK(GetScheduleTableStatus(SchTbl_Core0, &st) == E_OK && st == SCHEDULETABLE_RUNNING);
    CHECK(StartScheduleTableRel(SchTbl_Core0, 10) == E_OS_STATE);
    SilOs_ConsumeTime(120000000u); /* expiry points at +10, +35, +60, +85, +110 */
    CHECK(SilOs_TaskActivations(Task_Hi_Core0) == a0 + 5u);
    CHECK(count_tag("hi") == h0 + 5u);
    CHECK(StopScheduleTable(SchTbl_Core0) == E_OK);
    CHECK(StopScheduleTable(SchTbl_Core0) == E_OS_NOFUNC);
    SilOs_ConsumeTime(60000000u);
    CHECK(SilOs_TaskActivations(Task_Hi_Core0) == a0 + 5u);
}

static void s_cross_core_event(void) {
    uint64 x0 = SilOs_IsrRuns(XSignalIsr_OsCore1);
    uint32 before = count_tag("t_ev_core1");
    CHECK(SetEvent(Task_Event_Core1, Ev_Trigger_Core1) == E_OK);
    CHECK(wait_count("t_ev_core1", before + 1u, 50u));
    CHECK(SilOs_IsrRuns(XSignalIsr_OsCore1) >= x0 + 1u);
    {
        const LogEntry *e = find_last("t_ev_core1");
        CHECK(e != NULL && e->core == 1u);
    }
}

static void s_spinlock_and_ioc(void) {
    const LogEntry *got;
    const LogEntry *rel;
    uint32 before = count_tag("c1_spin_got");
    CHECK(IocWrite_Ioc_Speed_0(42u) == IOC_E_OK);
    CHECK(GetSpinlock(Spin_Plain) == E_OK);
    CHECK(GetSpinlock(Spin_Plain) == E_OS_INTERFERENCE_DEADLOCK);
    CHECK(SetEvent(Task_10ms_Core1, Ev_10ms_Core1) == E_OK); /* core 1 now spins on Spin_Plain */
    SilOs_ConsumeTime(2000000u);
    CHECK(count_tag("c1_spin_got") == before);
    LOG("drv_spin_rel");
    CHECK(ReleaseSpinlock(Spin_Plain) == E_OK);
    CHECK(ReleaseSpinlock(Spin_Plain) == E_OS_STATE);
    CHECK(wait_count("c1_spin_got", before + 1u, 50u));
    got = find_last("c1_spin_got");
    rel = find_last("drv_spin_rel");
    CHECK(got != NULL && rel != NULL && got->t_ns >= rel->t_ns);
    CHECK(wait_count("c1_ioc", 1u, 50u));
    {
        const LogEntry *e = find_last("c1_ioc");
        CHECK(e != NULL && e->arg == 42u);
    }
    /* queued IOC */
    {
        uint16 v = 0;
        CHECK(IocReceive_Ioc_Samples(&v) == IOC_E_NO_DATA);
        CHECK(IocSend_Ioc_Samples_0(7u) == IOC_E_OK);
        CHECK(IocSend_Ioc_Samples_0(8u) == IOC_E_OK);
        CHECK(IocReceive_Ioc_Samples(&v) == IOC_E_OK && v == 7u);
        CHECK(IocReceive_Ioc_Samples(&v) == IOC_E_OK && v == 8u);
        CHECK(IocReceive_Ioc_Samples(&v) == IOC_E_NO_DATA);
    }
}

static void s_spinlock_cat2_lock(void) {
    /* LOCK_CAT2_INTERRUPTS: while holding Spin_Shared, Cat2 ISRs are not delivered */
    static const char *const seq[] = {"drv_j", "drv_k", "isr_canrx", "t1_ev_canrx", "drv_l"};
    mark();
    LOG("drv_j");
    CHECK(GetSpinlock(Spin_Shared) == E_OK);
    SilOs_RaiseIrq(Isr_CanRx);
    SilOs_PreemptionPoint();
    LOG("drv_k");
    CHECK(ReleaseSpinlock(Spin_Shared) == E_OK);
    LOG("drv_l");
    expect_seq(0, seq, 5, "spinlock lock method CAT2");
}

static void s_errors(void) {
    uint32 before = g_errorHookCalls;
    EventMaskType ev = 0;
    CHECK(ActivateTask(9999u) == E_OS_ID);
    CHECK(g_lastHookService == OSServiceId_ActivateTask && g_lastHookError == E_OS_ID);
    CHECK(GetResource(9999u) == E_OS_ID);
    CHECK(GetEvent(Task_Hi_Core0, &ev) == E_OS_ACCESS);
    CHECK(WaitEvent(1u) == E_OS_ACCESS); /* driver is a basic task */
    CHECK(g_errorHookCalls == before + 4u);
}

static void s_counters(void) {
    TickType v0 = 0, v1 = 0, elapsed = 0;
    CHECK(GetCounterValue(SystemTimer_Core0, &v0) == E_OK);
    SilOs_ConsumeTime(3000000u);
    v1 = v0;
    CHECK(GetElapsedValue(SystemTimer_Core0, &v1, &elapsed) == E_OK);
    CHECK(elapsed == 3u && v1 == v0 + 3u);
    CHECK(GetCounterValue(SystemTimer_Core1, &v0) == E_OK);
    CHECK(v0 == v1); /* both counters derive from the same virtual clock */
    CHECK(GetCounterValue(9999u, &v0) == E_OS_ID);
}

static void s_chain(void) {
    static const char *const seq[] = {"drv_m", "hi", "hi", "drv_n"};
    mark();
    LOG("drv_m");
    g_hiChains = 1;
    CHECK(ActivateTask(Task_Hi_Core0) == E_OK); /* Hi chains itself once */
    LOG("drv_n");
    expect_seq(0, seq, 4, "ChainTask to itself");
}

static void s_consume_time_preempt(void) {
    /* while the driver consumes 20 ms, a cyclic 10 ms alarm must still release Task_10ms twice */
    uint32 before = count_tag("t10_ev");
    CHECK(SetRelAlarm(Al_10ms_Core0, 10, 10) == E_OK);
    SilOs_ConsumeTime(25000000u);
    CHECK(count_tag("t10_ev") == before + 2u);
    CHECK(CancelAlarm(Al_10ms_Core0) == E_OK);
}

TASK(Task_100ms_Core0) {
    if (g_phase == 0) {
        g_phase = 1;
        (void)CancelAlarm(Al_100ms_Core0);
        sil_log("test_os: phase 1 at t=%.3f ms", (double)SilOs_ClockNow() / 1e6);
        CHECK(count_tag("startuphook") == 2u);
        CHECK(count_tag("init0_pass") == 1u && count_tag("init1_pass") == 1u);
        s_preemption();
        s_lower_prio_waits();
        s_extended_events();
        s_multiple_activation();
        s_resource_ceiling();
        s_isr_delivery();
        s_interrupt_lock();
        s_isr_nesting();
        s_alarm_oneshot();
        s_alarm_cyclic();
        s_schedule_table();
        s_cross_core_event();
        s_spinlock_and_ioc();
        s_spinlock_cat2_lock();
        s_errors();
        s_counters();
        s_chain();
        s_consume_time_preempt();
        LOG("drv_end");
        (void)SetRelAlarm(Al_100ms_Core0, 20, 0); /* phase 2 after the low priority tasks ran */
        (void)TerminateTask();
    } else {
        sil_log("test_os: phase 2 at t=%.3f ms", (double)SilOs_ClockNow() / 1e6);
        CHECK(count_tag("bg") == 3u);
        CHECK(count_tag("coop") == 1u); /* activated in phase 1, ran after the driver terminated */
        {
            const LogEntry *end = find_last("drv_end");
            const LogEntry *coop = find_last("coop");
            CHECK(end != NULL && coop != NULL && coop->t_ns >= end->t_ns);
        }
        sil_log("test_os: done, %d checks, %d failures, %llu OS errors reported", g_checks, g_failures,
                (unsigned long long)SilOs_ErrorCount());
        SilOs_RequestStop(g_failures != 0 ? 1 : 0);
        ShutdownOS(E_OK);
    }
}

int main(int argc, char **argv) {
    SilRt_Options opt;
    int r = SilRt_ParseArgs(&opt, argc, argv);
    if (r != 0) return r < 0 ? 2 : 0;
    if (opt.os.durationNs == 0u) opt.os.durationNs = 10000000000u; /* safety net: 10 s virtual */
    r = SilRt_Run(&opt, entry);
    if (r == 0 && g_failures != 0) r = 1;
    sil_process_exit(r);
}

/* the virtual CAN driver is linked but never initialised here: CanIf stand-ins */
void CanIf_RxIndication(const Can_HwType *Mailbox, const PduInfoType *PduInfoPtr) { (void)Mailbox; (void)PduInfoPtr; }
void CanIf_TxConfirmation(PduIdType CanTxPduId) { (void)CanTxPduId; }
void CanIf_ControllerBusOff(uint8 ControllerId) { (void)ControllerId; }
void CanIf_ControllerModeIndication(uint8 ControllerId, Can_ControllerStateType ControllerMode) { (void)ControllerId; (void)ControllerMode; }

/* notification functions named in the demo MCAL configuration (the generated tables reference them) */
void Demo_AdcSenseNotification(void);
void Demo_HallEdgeNotification(void);
void Demo_GptTimeoutNotification(void);
void Demo_SpiSensorEndNotification(void);
void Demo_NvJobEndNotification(void);
void Demo_NvJobErrorNotification(void);
void Demo_AdcSenseNotification(void) {}
void Demo_HallEdgeNotification(void) {}
void Demo_GptTimeoutNotification(void) {}
void Demo_SpiSensorEndNotification(void) {}
void Demo_NvJobEndNotification(void) {}
void Demo_NvJobErrorNotification(void) {}
