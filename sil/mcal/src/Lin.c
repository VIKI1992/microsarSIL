/* Lin.c - virtual LIN master channels. A header is "sent" when LinIf calls Lin_SendFrame; after the
 * frame time the channel's interrupt is raised (ISR body Lin_Interrupt) and the status becomes
 * LIN_TX_OK, LIN_RX_OK (slave response from the response table, SilLin_SetResponse) or
 * LIN_RX_NO_RESPONSE. All traffic is captured for the control server. */
#include "Lin.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#include <stdio.h>
#include <string.h>

#define LIN_MAX_CH 8u
#define LIN_MAX_RESP 64u

typedef struct {
    uint8 used;
    uint8 channel;
    uint8 pid;
    uint8 len;
    uint8 data[8];
} Response;

typedef struct {
    volatile uint32 status;  /* Lin_StatusType */
    uint8 sleeping;
    uint8 rx[8];
    Lin_PduType pdu;
    uint8 txData[8];
    uint32 timer;
    volatile uint32 done;    /* frame finished, waiting for the ISR */
} Chan;

static Chan g_ch[LIN_MAX_CH];
static Response g_resp[LIN_MAX_RESP];
static sil_mutex_t *g_lock;
static uint8 g_init;

const Lin_ConfigType Lin_Config = {0u};

static void ensure(void) {
    uint8 i;
    if (g_lock != NULL_PTR) return;
    g_lock = sil_mutex_create();
    for (i = 0; i < LIN_MAX_CH; ++i) g_ch[i].status = LIN_NOT_OK;
}

void SilLin_SetResponse(uint8 channel, uint8 pid, const uint8 *data, uint8 len) {
    uint32 i, freeSlot = LIN_MAX_RESP;
    ensure();
    sil_mutex_lock(g_lock);
    for (i = 0; i < LIN_MAX_RESP; ++i) {
        if (g_resp[i].used && g_resp[i].channel == channel && g_resp[i].pid == (pid & 0x3Fu)) break;
        if (!g_resp[i].used && freeSlot == LIN_MAX_RESP) freeSlot = i;
    }
    if (i == LIN_MAX_RESP) i = freeSlot;
    if (i < LIN_MAX_RESP) {
        if (len == 0u) {
            g_resp[i].used = 0u;
        } else {
            g_resp[i].used = 1u;
            g_resp[i].channel = channel;
            g_resp[i].pid = pid & 0x3Fu;
            g_resp[i].len = len > 8u ? 8u : len;
            memcpy(g_resp[i].data, data, g_resp[i].len);
        }
    }
    sil_mutex_unlock(g_lock);
}

static const Response *find_response(uint8 channel, uint8 pid) {
    uint32 i;
    for (i = 0; i < LIN_MAX_RESP; ++i)
        if (g_resp[i].used && g_resp[i].channel == channel && g_resp[i].pid == (pid & 0x3Fu)) return &g_resp[i];
    return NULL_PTR;
}

static uint64 frame_ns(uint8 channel, uint8 dl) {
    uint32 baud = (channel < SilMcal_Cfg.numLinChannels && SilMcal_Cfg.linChannels[channel].baud) ? SilMcal_Cfg.linChannels[channel].baud : 19200u;
    /* header 34 bits + response (dl + 1) * 10 bits, nominal */
    return ((uint64)(34u + ((uint32)dl + 1u) * 10u) * 1000000000u) / baud;
}

static void frame_done(void *arg) {
    Chan *c = (Chan *)arg;
    uint8 channel = (uint8)(c - g_ch);
    char handler[32];
    uint8 hwUart = (channel < SilMcal_Cfg.numLinChannels) ? SilMcal_Cfg.linChannels[channel].hwUart : channel;
    c->timer = 0u;
    sil_atomic_store32(&c->done, 1u);
    snprintf(handler, sizeof handler, "LinIsr_%u", (unsigned)hwUart);
    if (!SilMcal_RaiseIsr(handler)) Lin_Interrupt(channel); /* no OS ISR for the channel: complete directly */
}

void Lin_Interrupt(uint8 Channel) {
    Chan *c;
    ensure();
    if (Channel >= LIN_MAX_CH) return;
    c = &g_ch[Channel];
    if (!sil_atomic_load32(&c->done)) return;
    sil_atomic_store32(&c->done, 0u);
    sil_mutex_lock(g_lock);
    if (c->status == LIN_TX_BUSY) {
        c->status = LIN_TX_OK;
    } else if (c->status == LIN_RX_BUSY) {
        const Response *r = find_response(Channel, c->pdu.Pid);
        if (r != NULL_PTR) {
            memset(c->rx, 0, sizeof c->rx);
            memcpy(c->rx, r->data, r->len);
            c->status = LIN_RX_OK;
        } else {
            c->status = LIN_RX_NO_RESPONSE;
        }
        {
            SilMcal_Event ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = SILMCAL_EV_LIN;
            ev.channel = Channel;
            ev.dir = 1u;
            ev.id = c->pdu.Pid;
            ev.len = (r != NULL_PTR) ? r->len : 0u;
            if (r != NULL_PTR) memcpy(ev.rx, r->data, r->len);
            SilMcal_Capture(&ev);
        }
    }
    sil_mutex_unlock(g_lock);
}

void Lin_InitMemory(void) { ensure(); }

void Lin_Init(const Lin_ConfigType *Config) {
    uint8 i;
    (void)Config;
    ensure();
    for (i = 0; i < LIN_MAX_CH; ++i) {
        g_ch[i].status = LIN_OPERATIONAL;
        g_ch[i].sleeping = 0u;
    }
    g_init = 1u;
}

void Lin_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = LIN_VENDOR_ID;
    versioninfo->moduleID = LIN_MODULE_ID;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}

Std_ReturnType Lin_CheckWakeup(uint8 Channel) {
    (void)Channel;
    return E_OK;
}

Std_ReturnType Lin_SendFrame(uint8 Channel, const Lin_PduType *PduInfoPtr) {
    Chan *c;
    SilMcal_Event ev;
    ensure();
    if (!g_init || Channel >= LIN_MAX_CH || PduInfoPtr == NULL_PTR) return E_NOT_OK;
    c = &g_ch[Channel];
    sil_mutex_lock(g_lock);
    if (c->sleeping) {
        sil_mutex_unlock(g_lock);
        return E_NOT_OK;
    }
    if (c->timer) SilOs_TimerCancel(c->timer);
    c->pdu = *PduInfoPtr;
    if (PduInfoPtr->Drc == LIN_FRAMERESPONSE_TX) {
        uint8 n = PduInfoPtr->Dl > 8u ? 8u : PduInfoPtr->Dl;
        if (PduInfoPtr->SduPtr != NULL_PTR) memcpy(c->txData, PduInfoPtr->SduPtr, n);
        c->status = LIN_TX_BUSY;
        memset(&ev, 0, sizeof ev);
        ev.kind = SILMCAL_EV_LIN;
        ev.channel = Channel;
        ev.dir = 0u;
        ev.id = PduInfoPtr->Pid;
        ev.len = n;
        memcpy(ev.tx, c->txData, n);
        SilMcal_Capture(&ev);
    } else {
        c->status = LIN_RX_BUSY;
    }
    sil_atomic_store32(&c->done, 0u);
    c->timer = SilOs_TimerSchedule(frame_ns(Channel, PduInfoPtr->Dl), frame_done, c);
    sil_mutex_unlock(g_lock);
    if (c->timer == 0u) frame_done(c);
    return E_OK;
}

Std_ReturnType Lin_GoToSleep(uint8 Channel) {
    ensure();
    if (Channel >= LIN_MAX_CH) return E_NOT_OK;
    g_ch[Channel].sleeping = 1u;
    g_ch[Channel].status = LIN_CH_SLEEP;
    return E_OK;
}
Std_ReturnType Lin_GoToSleepInternal(uint8 Channel) { return Lin_GoToSleep(Channel); }

Std_ReturnType Lin_Wakeup(uint8 Channel) {
    ensure();
    if (Channel >= LIN_MAX_CH) return E_NOT_OK;
    g_ch[Channel].sleeping = 0u;
    g_ch[Channel].status = LIN_OPERATIONAL;
    return E_OK;
}
Std_ReturnType Lin_WakeupInternal(uint8 Channel) { return Lin_Wakeup(Channel); }

Lin_StatusType Lin_GetStatus(uint8 Channel, uint8 **Lin_SduPtr) {
    Chan *c;
    Lin_StatusType s;
    ensure();
    if (Channel >= LIN_MAX_CH) return LIN_NOT_OK;
    c = &g_ch[Channel];
    sil_mutex_lock(g_lock);
    s = (Lin_StatusType)c->status;
    if (Lin_SduPtr != NULL_PTR) *Lin_SduPtr = c->rx;
    sil_mutex_unlock(g_lock);
    return s;
}
