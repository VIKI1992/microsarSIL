/*
 * Can_43_FLEXCAN.c - virtual CAN driver with the NXP RTD API names, on top of sil_bus backends.
 *
 * Interrupt driven controllers: frames received from the bus backend are queued and the RX ISR of
 * the controller is raised; the handler (SilCan_Isr, reached through the generated CAN<n>_*_IRQHandler
 * functions and the OS wrappers) matches the frame against the receive hardware objects and calls
 * CanIf_RxIndication in ISR context. Can_43_FLEXCAN_Write sends the frame to the bus immediately and
 * schedules the TX confirmation after the frame's bus time; the confirmation is delivered from the TX
 * ISR (CanIf_TxConfirmation). Polling controllers are served by the MainFunctions instead.
 *
 * Locking: every controller has a host mutex for its queues and statistics. SilOs_RaiseIrq is never
 * called while a driver mutex is held (the raised ISR takes the same mutex).
 */
#include "Can_43_FLEXCAN.h"
#include "SilCan.h"
#include "CanIf_Cbk.h"
#include "SilOs.h"
#include "SilTrace.h"
#include "sil_port.h"

#include <string.h>

#define SILCAN_MAX_CTRL 16u
#define SILCAN_RXQ 256u
#define SILCAN_TXQ 64u
#define SILCAN_MAX_HOH_INFLIGHT 256u
#define SILCAN_ID_EXT_FLAG 0x80000000u
#define SILCAN_ID_FD_FLAG 0x40000000u

typedef struct {
    PduIdType pduId;
    uint32 hohIdx;
} TxSlot;

typedef struct {
    const SilCan_ControllerCfg *cfg;
    volatile uint32 state;   /* Can_ControllerStateType */
    volatile uint32 busOff;
    uint32 intDisable;       /* DisableControllerInterrupts nesting (under lock) */
    uint32 irqPending;       /* raises deferred while interrupts are disabled: bit per kind */
    uint32 isr[SILCAN_ISR_KINDS];
    SilBus *bus;
    char busSpec[64];
    sil_mutex_t *lock;
    SilBus_Frame rxq[SILCAN_RXQ];
    uint32 rxHead, rxCount;
    TxSlot txDone[SILCAN_TXQ];
    uint32 txHead, txCount;
    uint32 inFlight[SILCAN_MAX_HOH_INFLIGHT]; /* per HOH index: frames waiting for confirmation */
    SilCan_Stats stats;
} Ctrl;

typedef struct {
    uint8 ctrlIdx;
    uint32 hohIdx;
    PduIdType pduId;
} TxTimerArg;

static Ctrl g_ctrl[SILCAN_MAX_CTRL];
static uint8 g_init;
static SilCan_SniffFn g_sniff;
static void *g_sniffUser;
static TxTimerArg g_txArgs[SILCAN_MAX_CTRL * SILCAN_TXQ];
static volatile uint32 g_txArgNext;

/* ------------------------------------------------------------------------- */
static uint8 ctrl_index(const Ctrl *c) { return (uint8)(c - g_ctrl); }

static unsigned trace_core(void) {
    CoreIdType c = SilOs_CurrentCore();
    return ((unsigned)c < SILOS_MAX_CORES) ? (unsigned)c : 0u;
}

static Ctrl *ctrl_by_id(uint8 controllerId) {
    uint8 i;
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i)
        if (SilCan_Cfg.controllers[i].controllerId == controllerId) return &g_ctrl[i];
    return NULL_PTR;
}

static const SilCan_HohCfg *hoh_by_object(uint32 objectId, uint32 *idx) {
    uint32 i;
    for (i = 0; i < SilCan_Cfg.numHohs; ++i) {
        if (SilCan_Cfg.hohs[i].objectId == objectId) {
            *idx = i;
            return &SilCan_Cfg.hohs[i];
        }
    }
    return NULL_PTR;
}

static void ensure_setup(void) {
    uint8 i;
    if (g_init) return;
    memset(g_ctrl, 0, sizeof(g_ctrl));
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i) {
        Ctrl *c = &g_ctrl[i];
        uint32 k;
        c->cfg = &SilCan_Cfg.controllers[i];
        c->state = CAN_CS_UNINIT;
        c->lock = sil_mutex_create();
        for (k = 0; k < SILCAN_ISR_KINDS; ++k) c->isr[k] = 0xFFFFFFFFu;
        strncpy(c->busSpec, "null", sizeof(c->busSpec) - 1u);
    }
    g_init = 1u;
}

static void resolve_isrs(Ctrl *c) {
    const char *names[SILCAN_ISR_KINDS];
    uint32 k;
    names[SILCAN_ISR_MB] = c->cfg->isrRx;
    names[SILCAN_ISR_RXFIFO] = c->cfg->isrTx;
    names[SILCAN_ISR_BUSOFF] = c->cfg->isrBusOff;
    sil_mutex_lock(c->lock);
    for (k = 0; k < SILCAN_ISR_KINDS; ++k) {
        if (c->isr[k] == 0xFFFFFFFFu && names[k] != NULL_PTR && names[k][0] != '\0') {
            uint32 id = SilOs_IsrByHandler(names[k]);
            if (id < SilOs_NumIsrs()) c->isr[k] = id;
        }
    }
    sil_mutex_unlock(c->lock);
}

static void raise_isr(Ctrl *c, SilCan_IsrKind kind) {
    uint32 id;
    int deferred;
    sil_mutex_lock(c->lock);
    id = c->isr[kind];
    deferred = (c->intDisable != 0u);
    if (deferred) c->irqPending |= (1u << kind);
    sil_mutex_unlock(c->lock);
    if (!deferred && id < SilOs_NumIsrs()) SilOs_RaiseIrq(id);
}

static void stat_inc(Ctrl *c, uint32 *field) {
    sil_mutex_lock(c->lock);
    (*field)++;
    sil_mutex_unlock(c->lock);
}

static void bus_rx(void *user, const SilBus_Frame *frame) { SilCan_Receive((uint8)(uintptr_t)user, frame); }

static void open_bus(Ctrl *c) {
    if (c->bus != NULL_PTR) SilBus_Close(c->bus);
    c->bus = SilBus_Open(c->busSpec, bus_rx, (void *)(uintptr_t)ctrl_index(c));
}

/* frame time on the bus: arbitration/control bits at the nominal rate, data at the data rate */
static uint64 frame_time_ns(const Ctrl *c, const SilBus_Frame *f) {
    uint64 ctrlBits = (f->flags & SILBUS_FLAG_EXT) ? 67u : 47u;
    uint64 dataBits = (uint64)f->len * 8u;
    uint64 baud = c->cfg->baud ? c->cfg->baud : 500000u;
    uint64 dbaud = ((f->flags & SILBUS_FLAG_FD) && c->cfg->fdBaud) ? c->cfg->fdBaud : baud;
    return (ctrlBits * 1000000000u) / baud + (dataBits * 1000000000u) / dbaud;
}

static int id_matches(const SilCan_HohCfg *h, uint32 id, int ext) {
    uint16 k;
    if (h->idType == SILCAN_ID_STANDARD && ext) return 0;
    if (h->idType == SILCAN_ID_EXTENDED && !ext) return 0;
    if (h->numFilters == 0u) return 1;
    for (k = 0; k < h->numFilters; ++k) {
        const SilCan_FilterCfg *f = &h->filters[k];
        uint32 mask = h->isFull ? 0x1FFFFFFFu : f->mask;
        if (f->idType == SILCAN_ID_STANDARD && ext) continue;
        if (f->idType == SILCAN_ID_EXTENDED && !ext) continue;
        if ((id & mask) == (f->code & mask)) return 1;
    }
    return 0;
}

static const SilCan_HohCfg *match_rx(const Ctrl *c, uint32 id, int ext) {
    uint32 i;
    int pass;
    /* FULL objects first (dedicated mailboxes), then BASIC */
    for (pass = 1; pass >= 0; --pass) {
        for (i = 0; i < SilCan_Cfg.numHohs; ++i) {
            const SilCan_HohCfg *h = &SilCan_Cfg.hohs[i];
            if (h->isTx || h->controller != ctrl_index(c) || (int)h->isFull != pass) continue;
            if (id_matches(h, id, ext)) return h;
        }
    }
    return NULL_PTR;
}

/* TX confirmation timer callback: runs outside the OS lock */
static void tx_done(void *arg) {
    TxTimerArg *a = (TxTimerArg *)arg;
    Ctrl *c = &g_ctrl[a->ctrlIdx];
    sil_mutex_lock(c->lock);
    if (c->txCount < SILCAN_TXQ) {
        uint32 idx = (c->txHead + c->txCount) % SILCAN_TXQ;
        c->txDone[idx].pduId = a->pduId;
        c->txDone[idx].hohIdx = a->hohIdx;
        c->txCount++;
    }
    sil_mutex_unlock(c->lock);
    if (!c->cfg->txPolling) raise_isr(c, SILCAN_ISR_RXFIFO);
}

static void deliver_rx(Ctrl *c) {
    for (;;) {
        SilBus_Frame f;
        const SilCan_HohCfg *match;
        int ext;
        sil_mutex_lock(c->lock);
        if (c->rxCount == 0u) {
            sil_mutex_unlock(c->lock);
            return;
        }
        f = c->rxq[c->rxHead];
        c->rxHead = (c->rxHead + 1u) % SILCAN_RXQ;
        c->rxCount--;
        sil_mutex_unlock(c->lock);
        if (sil_atomic_load32(&c->state) != CAN_CS_STARTED) continue;
        ext = (f.flags & SILBUS_FLAG_EXT) ? 1 : 0;
        match = match_rx(c, f.id, ext);
        if (match == NULL_PTR) {
            stat_inc(c, &c->stats.rxDropped);
            continue;
        }
        {
            Can_HwType hw;
            PduInfoType pdu;
            hw.CanId = f.id | (ext ? SILCAN_ID_EXT_FLAG : 0u) | ((f.flags & SILBUS_FLAG_FD) ? SILCAN_ID_FD_FLAG : 0u);
            hw.Hoh = (Can_HwHandleType)match->objectId;
            hw.ControllerId = c->cfg->controllerId;
            pdu.SduDataPtr = f.data;
            pdu.SduLength = (PduLengthType)f.len;
            pdu.MetaDataPtr = NULL_PTR;
            stat_inc(c, &c->stats.rxFrames);
            SilTrace_Event(trace_core(), SILTRACE_EV_BUS_RX, c->cfg->controllerId, f.id);
            if (g_sniff != NULL_PTR) g_sniff(g_sniffUser, ctrl_index(c), 0, &f);
            CanIf_RxIndication(&hw, &pdu);
        }
    }
}

static void deliver_tx_confirmations(Ctrl *c) {
    for (;;) {
        TxSlot s;
        sil_mutex_lock(c->lock);
        if (c->txCount == 0u) {
            sil_mutex_unlock(c->lock);
            return;
        }
        s = c->txDone[c->txHead];
        c->txHead = (c->txHead + 1u) % SILCAN_TXQ;
        c->txCount--;
        if (s.hohIdx < SILCAN_MAX_HOH_INFLIGHT && c->inFlight[s.hohIdx] > 0u) c->inFlight[s.hohIdx]--;
        sil_mutex_unlock(c->lock);
        if (sil_atomic_load32(&c->state) == CAN_CS_STARTED) CanIf_TxConfirmation(s.pduId);
    }
}

static void set_state(Ctrl *c, uint32 state) {
    sil_atomic_store32(&c->state, state);
    sil_mutex_lock(c->lock);
    c->stats.state = (uint8)state;
    sil_mutex_unlock(c->lock);
}

/* ------------------------------------------------------------------------- */
/* SIL side API                                                               */
void SilCan_Setup(void) { ensure_setup(); }

int SilCan_SetBus(uint8 controllerIdx, const char *busSpec) {
    Ctrl *c;
    ensure_setup();
    if (controllerIdx >= SilCan_Cfg.numControllers || controllerIdx >= SILCAN_MAX_CTRL || busSpec == NULL_PTR) return -1;
    c = &g_ctrl[controllerIdx];
    strncpy(c->busSpec, busSpec, sizeof(c->busSpec) - 1u);
    c->busSpec[sizeof(c->busSpec) - 1u] = '\0';
    if (sil_atomic_load32(&c->state) != CAN_CS_UNINIT) open_bus(c);
    return 0;
}

const char *SilCan_GetBusSpec(uint8 controllerIdx) {
    ensure_setup();
    return (controllerIdx < SILCAN_MAX_CTRL) ? g_ctrl[controllerIdx].busSpec : "";
}

void SilCan_SetIsr(uint8 controllerIdx, SilCan_IsrKind kind, uint32 isrId) {
    ensure_setup();
    if (controllerIdx < SILCAN_MAX_CTRL && kind < SILCAN_ISR_KINDS) {
        sil_mutex_lock(g_ctrl[controllerIdx].lock);
        g_ctrl[controllerIdx].isr[kind] = isrId;
        sil_mutex_unlock(g_ctrl[controllerIdx].lock);
    }
}

void SilCan_Receive(uint8 controllerIdx, const SilBus_Frame *frame) {
    Ctrl *c;
    ensure_setup();
    if (controllerIdx >= SilCan_Cfg.numControllers || controllerIdx >= SILCAN_MAX_CTRL || frame == NULL_PTR) return;
    c = &g_ctrl[controllerIdx];
    sil_mutex_lock(c->lock);
    if (c->rxCount < SILCAN_RXQ) {
        c->rxq[(c->rxHead + c->rxCount) % SILCAN_RXQ] = *frame;
        c->rxCount++;
    } else {
        c->stats.rxDropped++;
    }
    sil_mutex_unlock(c->lock);
    if (!c->cfg->rxPolling) raise_isr(c, SILCAN_ISR_MB);
}

void SilCan_InjectBusOff(uint8 controllerIdx) {
    ensure_setup();
    if (controllerIdx >= SilCan_Cfg.numControllers || controllerIdx >= SILCAN_MAX_CTRL) return;
    sil_atomic_store32(&g_ctrl[controllerIdx].busOff, 1u);
    if (!g_ctrl[controllerIdx].cfg->busoffPolling) raise_isr(&g_ctrl[controllerIdx], SILCAN_ISR_BUSOFF);
}

void SilCan_Isr(uint8 controllerIdx, SilCan_IsrKind kind) {
    Ctrl *c;
    ensure_setup();
    if (controllerIdx >= SilCan_Cfg.numControllers || controllerIdx >= SILCAN_MAX_CTRL) return;
    c = &g_ctrl[controllerIdx];
    if (kind == SILCAN_ISR_BUSOFF) {
        if (sil_atomic_load32(&c->busOff) && sil_atomic_load32(&c->state) == CAN_CS_STARTED) {
            set_state(c, CAN_CS_STOPPED);
            stat_inc(c, &c->stats.busOff);
            CanIf_ControllerBusOff(c->cfg->controllerId);
        }
        return;
    }
    deliver_tx_confirmations(c);
    deliver_rx(c);
}

void SilCan_GetStats(uint8 controllerIdx, SilCan_Stats *stats) {
    ensure_setup();
    if (stats == NULL_PTR) return;
    if (controllerIdx >= SILCAN_MAX_CTRL) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    sil_mutex_lock(g_ctrl[controllerIdx].lock);
    *stats = g_ctrl[controllerIdx].stats;
    sil_mutex_unlock(g_ctrl[controllerIdx].lock);
    stats->state = (uint8)sil_atomic_load32(&g_ctrl[controllerIdx].state);
}

uint8 SilCan_NumControllers(void) { return SilCan_Cfg.numControllers; }

const char *SilCan_ControllerName(uint8 controllerIdx) {
    return (controllerIdx < SilCan_Cfg.numControllers) ? SilCan_Cfg.controllers[controllerIdx].name : "";
}

void SilCan_SetSniffer(SilCan_SniffFn fn, void *user) {
    g_sniff = fn;
    g_sniffUser = user;
}

/* ------------------------------------------------------------------------- */
/* AUTOSAR API                                                                */
void Can_43_FLEXCAN_Init(const Can_43_FLEXCAN_ConfigType *Config) {
    uint8 i;
    (void)Config;
    ensure_setup();
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i) {
        Ctrl *c = &g_ctrl[i];
        sil_mutex_lock(c->lock);
        c->rxHead = c->rxCount = 0u;
        c->txHead = c->txCount = 0u;
        c->intDisable = 0u;
        c->irqPending = 0u;
        memset(c->inFlight, 0, sizeof(c->inFlight));
        sil_mutex_unlock(c->lock);
        sil_atomic_store32(&c->busOff, 0u);
        resolve_isrs(c);
        open_bus(c);
        set_state(c, CAN_CS_STOPPED);
    }
}

void Can_43_FLEXCAN_DeInit(void) {
    uint8 i;
    ensure_setup();
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i) {
        set_state(&g_ctrl[i], CAN_CS_UNINIT);
        if (g_ctrl[i].bus != NULL_PTR) {
            SilBus_Close(g_ctrl[i].bus);
            g_ctrl[i].bus = NULL_PTR;
        }
    }
}

void Can_43_FLEXCAN_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = CAN_43_FLEXCAN_VENDOR_ID;
    versioninfo->moduleID = CAN_43_FLEXCAN_MODULE_ID;
    versioninfo->sw_major_version = CAN_43_FLEXCAN_SW_MAJOR_VERSION;
    versioninfo->sw_minor_version = CAN_43_FLEXCAN_SW_MINOR_VERSION;
    versioninfo->sw_patch_version = CAN_43_FLEXCAN_SW_PATCH_VERSION;
}

Std_ReturnType Can_43_FLEXCAN_SetBaudrate(uint8 Controller, uint16 BaudRateConfigID) {
    (void)BaudRateConfigID;
    return (ctrl_by_id(Controller) != NULL_PTR) ? E_OK : E_NOT_OK;
}

Std_ReturnType Can_43_FLEXCAN_SetControllerMode(uint8 Controller, Can_ControllerStateType Transition) {
    Ctrl *c = ctrl_by_id(Controller);
    uint32 state;
    if (c == NULL_PTR) return E_NOT_OK;
    state = sil_atomic_load32(&c->state);
    if (state == CAN_CS_UNINIT) return E_NOT_OK;
    switch (Transition) {
    case CAN_CS_STARTED:
        if (state == CAN_CS_SLEEP) return E_NOT_OK;
        sil_atomic_store32(&c->busOff, 0u);
        set_state(c, CAN_CS_STARTED);
        break;
    case CAN_CS_STOPPED:
        set_state(c, CAN_CS_STOPPED);
        sil_mutex_lock(c->lock);
        c->txHead = c->txCount = 0u;
        memset(c->inFlight, 0, sizeof(c->inFlight));
        sil_mutex_unlock(c->lock);
        break;
    case CAN_CS_SLEEP:
        if (state != CAN_CS_STOPPED) return E_NOT_OK;
        set_state(c, CAN_CS_SLEEP);
        break;
    default:
        return E_NOT_OK;
    }
    CanIf_ControllerModeIndication(Controller, (Can_ControllerStateType)sil_atomic_load32(&c->state));
    return E_OK;
}

void Can_43_FLEXCAN_DisableControllerInterrupts(uint8 Controller) {
    Ctrl *c = ctrl_by_id(Controller);
    if (c == NULL_PTR) return;
    sil_mutex_lock(c->lock);
    c->intDisable++;
    sil_mutex_unlock(c->lock);
}

void Can_43_FLEXCAN_EnableControllerInterrupts(uint8 Controller) {
    Ctrl *c = ctrl_by_id(Controller);
    uint32 pending = 0u, k;
    if (c == NULL_PTR) return;
    sil_mutex_lock(c->lock);
    if (c->intDisable > 0u) {
        c->intDisable--;
        if (c->intDisable == 0u) {
            pending = c->irqPending;
            c->irqPending = 0u;
        }
    }
    sil_mutex_unlock(c->lock);
    for (k = 0; k < SILCAN_ISR_KINDS; ++k)
        if (pending & (1u << k)) raise_isr(c, (SilCan_IsrKind)k);
}

Std_ReturnType Can_43_FLEXCAN_CheckWakeup(uint8 Controller) {
    (void)Controller;
    return E_NOT_OK;
}

Std_ReturnType Can_43_FLEXCAN_GetControllerErrorState(uint8 ControllerId, Can_ErrorStateType *ErrorStatePtr) {
    Ctrl *c = ctrl_by_id(ControllerId);
    if (c == NULL_PTR || ErrorStatePtr == NULL_PTR) return E_NOT_OK;
    *ErrorStatePtr = sil_atomic_load32(&c->busOff) ? CAN_ERRORSTATE_BUSOFF : CAN_ERRORSTATE_ACTIVE;
    return E_OK;
}

Std_ReturnType Can_43_FLEXCAN_GetControllerMode(uint8 Controller, Can_ControllerStateType *ControllerModePtr) {
    Ctrl *c = ctrl_by_id(Controller);
    if (c == NULL_PTR || ControllerModePtr == NULL_PTR) return E_NOT_OK;
    *ControllerModePtr = (Can_ControllerStateType)sil_atomic_load32(&c->state);
    return E_OK;
}

Std_ReturnType Can_43_FLEXCAN_GetControllerRxErrorCounter(uint8 ControllerId, uint8 *RxErrorCounterPtr) {
    if (ctrl_by_id(ControllerId) == NULL_PTR || RxErrorCounterPtr == NULL_PTR) return E_NOT_OK;
    *RxErrorCounterPtr = 0u;
    return E_OK;
}

Std_ReturnType Can_43_FLEXCAN_GetControllerTxErrorCounter(uint8 ControllerId, uint8 *TxErrorCounterPtr) {
    if (ctrl_by_id(ControllerId) == NULL_PTR || TxErrorCounterPtr == NULL_PTR) return E_NOT_OK;
    *TxErrorCounterPtr = 0u;
    return E_OK;
}

Std_ReturnType Can_43_FLEXCAN_Write(Can_HwHandleType Hth, const Can_PduType *PduInfo) {
    uint32 hohIdx = 0;
    const SilCan_HohCfg *h;
    Ctrl *c;
    SilBus_Frame f;
    TxTimerArg *arg;
    uint32 limit;
    ensure_setup();
    h = hoh_by_object(Hth, &hohIdx);
    if (h == NULL_PTR || !h->isTx || PduInfo == NULL_PTR || PduInfo->length > SILBUS_MAX_DATA) return E_NOT_OK;
    if (h->controller >= SILCAN_MAX_CTRL || hohIdx >= SILCAN_MAX_HOH_INFLIGHT) return E_NOT_OK;
    c = &g_ctrl[h->controller];
    if (sil_atomic_load32(&c->state) != CAN_CS_STARTED) return E_NOT_OK;
    limit = h->hwObjectCount ? h->hwObjectCount : 1u;
    sil_mutex_lock(c->lock);
    if (c->inFlight[hohIdx] >= limit) {
        c->stats.txBusy++;
        sil_mutex_unlock(c->lock);
        return CAN_BUSY;
    }
    c->inFlight[hohIdx]++;
    c->stats.txFrames++;
    sil_mutex_unlock(c->lock);
    memset(&f, 0, sizeof(f));
    f.id = PduInfo->id & 0x1FFFFFFFu;
    f.flags = (uint8)(((PduInfo->id & SILCAN_ID_EXT_FLAG) ? SILBUS_FLAG_EXT : 0u) | ((PduInfo->id & SILCAN_ID_FD_FLAG) ? SILBUS_FLAG_FD : 0u));
    f.len = PduInfo->length;
    if (PduInfo->sdu != NULL_PTR && f.len > 0u) memcpy(f.data, PduInfo->sdu, f.len);
    f.t_ns = SilOs_ClockNow();
    SilTrace_Event(trace_core(), SILTRACE_EV_BUS_TX, c->cfg->controllerId, f.id);
    if (g_sniff != NULL_PTR) g_sniff(g_sniffUser, h->controller, 1, &f);
    if (c->bus != NULL_PTR) (void)SilBus_Send(c->bus, &f);
    /* confirmation after the frame time */
    arg = &g_txArgs[sil_atomic_add32(&g_txArgNext, 1u) % (SILCAN_MAX_CTRL * SILCAN_TXQ)];
    arg->ctrlIdx = h->controller;
    arg->hohIdx = hohIdx;
    arg->pduId = PduInfo->swPduHandle;
    if (SilOs_TimerSchedule(frame_time_ns(c, &f), tx_done, arg) == 0u) tx_done(arg);
    return E_OK;
}

static void main_read(void) {
    uint8 i;
    ensure_setup();
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i)
        if (g_ctrl[i].cfg->rxPolling) deliver_rx(&g_ctrl[i]);
}

static void main_write(void) {
    uint8 i;
    ensure_setup();
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i)
        if (g_ctrl[i].cfg->txPolling) deliver_tx_confirmations(&g_ctrl[i]);
}

void Can_43_FLEXCAN_MainFunction_Write(void) { main_write(); }
void Can_43_FLEXCAN_MainFunction_Read(void) { main_read(); }
void Can_43_FLEXCAN_MainFunction_BusOff(void) {
    uint8 i;
    ensure_setup();
    for (i = 0; i < SilCan_Cfg.numControllers && i < SILCAN_MAX_CTRL; ++i)
        if (g_ctrl[i].cfg->busoffPolling) SilCan_Isr(i, SILCAN_ISR_BUSOFF);
}
void Can_43_FLEXCAN_MainFunction_Wakeup(void) {}
void Can_43_FLEXCAN_MainFunction_Mode(void) {}

#define SILCAN_PERIOD_FN(n) \
    void Can_43_FLEXCAN_MainFunction_Read_CanMainFunctionRWPeriods_##n(void) { main_read(); } \
    void Can_43_FLEXCAN_MainFunction_Write_CanMainFunctionRWPeriods_##n(void) { main_write(); }
SILCAN_PERIOD_FN(0) SILCAN_PERIOD_FN(1) SILCAN_PERIOD_FN(2) SILCAN_PERIOD_FN(3) SILCAN_PERIOD_FN(4) SILCAN_PERIOD_FN(5)
SILCAN_PERIOD_FN(6) SILCAN_PERIOD_FN(7) SILCAN_PERIOD_FN(8) SILCAN_PERIOD_FN(9) SILCAN_PERIOD_FN(10) SILCAN_PERIOD_FN(11)
SILCAN_PERIOD_FN(12) SILCAN_PERIOD_FN(13) SILCAN_PERIOD_FN(14) SILCAN_PERIOD_FN(15)

/* generic names used by Vector modules */
Std_ReturnType Can_GetControllerErrorState(uint8 ControllerId, Can_ErrorStateType *ErrorStatePtr) {
    return Can_43_FLEXCAN_GetControllerErrorState(ControllerId, ErrorStatePtr);
}
Std_ReturnType Can_GetControllerRxErrorCounter(uint8 ControllerId, uint8 *RxErrorCounterPtr) {
    return Can_43_FLEXCAN_GetControllerRxErrorCounter(ControllerId, RxErrorCounterPtr);
}
Std_ReturnType Can_GetControllerTxErrorCounter(uint8 ControllerId, uint8 *TxErrorCounterPtr) {
    return Can_43_FLEXCAN_GetControllerTxErrorCounter(ControllerId, TxErrorCounterPtr);
}
Std_ReturnType Can_SetBaudrate(uint8 Controller, uint16 BaudRateConfigID) { return Can_43_FLEXCAN_SetBaudrate(Controller, BaudRateConfigID); }
void Can_DisableControllerInterrupts(uint8 Controller) { Can_43_FLEXCAN_DisableControllerInterrupts(Controller); }
void Can_EnableControllerInterrupts(uint8 Controller) { Can_43_FLEXCAN_EnableControllerInterrupts(Controller); }
Std_ReturnType Can_CheckWakeup(uint8 Controller) { return Can_43_FLEXCAN_CheckWakeup(Controller); }
Std_ReturnType Can_GetControllerMode(uint8 Controller, Can_ControllerStateType *ControllerModePtr) {
    return Can_43_FLEXCAN_GetControllerMode(Controller, ControllerModePtr);
}
