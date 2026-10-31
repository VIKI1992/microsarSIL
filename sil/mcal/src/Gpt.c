/* Gpt.c - virtual general purpose timer: channels run on the virtual clock, notifications in ISR context. */
#include "Gpt.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#define GPT_MAX 32u

typedef struct {
    const SilGpt_ChannelCfg *cfg;
    uint8 running;
    uint8 expired;
    uint8 notify;
    uint32 generation;
    uint32 value;       /* ticks */
    uint64 startNs;
    uint64 durationNs;
    uint32 timer;
} Chan;

static Chan g_ch[GPT_MAX];
static sil_mutex_t *g_lock;
static uint8 g_init;

static void ensure(void) {
    uint16 i;
    if (g_init) return;
    g_lock = sil_mutex_create();
    for (i = 0; i < SilMcal_Cfg.numGptChannels && i < GPT_MAX; ++i) g_ch[i].cfg = &SilMcal_Cfg.gptChannels[i];
    g_init = 1u;
}

static Chan *chan(Gpt_ChannelType id) {
    uint16 i;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numGptChannels && i < GPT_MAX; ++i)
        if (g_ch[i].cfg->channelId == id) return &g_ch[i];
    return NULL_PTR;
}

static uint64 ticks_to_ns(const Chan *c, uint32 ticks) {
    uint32 hz = c->cfg->tickHz ? c->cfg->tickHz : 1000000u;
    return ((uint64)ticks * 1000000000u) / hz;
}

static uint32 ns_to_ticks(const Chan *c, uint64 ns) {
    uint32 hz = c->cfg->tickHz ? c->cfg->tickHz : 1000000u;
    return (uint32)((ns * hz) / 1000000000u);
}

static void expire(void *arg) {
    Chan *c = (Chan *)arg;
    SilMcal_NotifyFn fn = NULL_PTR;
    sil_mutex_lock(g_lock);
    if (c->running) {
        if (c->cfg->continuous) {
            c->startNs += c->durationNs;
            c->timer = SilOs_TimerSchedule(c->durationNs, expire, c);
        } else {
            c->running = 0u;
            c->expired = 1u;
        }
        if (c->notify) fn = c->cfg->notification;
    }
    sil_mutex_unlock(g_lock);
    if (fn != NULL_PTR) SilMcal_Notify(fn);
}

void Gpt_Init(const Gpt_ConfigType *ConfigPtr) {
    uint16 i;
    (void)ConfigPtr;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numGptChannels && i < GPT_MAX; ++i) {
        g_ch[i].running = 0u;
        g_ch[i].expired = 0u;
        g_ch[i].notify = 0u;
    }
}

void Gpt_DeInit(void) {
    uint16 i;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numGptChannels && i < GPT_MAX; ++i) Gpt_StopTimer(g_ch[i].cfg->channelId);
}

void Gpt_StartTimer(Gpt_ChannelType Channel, Gpt_ValueType Value) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR || Value == 0u) return;
    sil_mutex_lock(g_lock);
    if (c->timer) SilOs_TimerCancel(c->timer);
    c->value = Value;
    c->durationNs = ticks_to_ns(c, Value);
    if (c->durationNs == 0u) c->durationNs = 1u;
    c->startNs = SilOs_ClockNow();
    c->running = 1u;
    c->expired = 0u;
    c->timer = SilOs_TimerSchedule(c->durationNs, expire, c);
    sil_mutex_unlock(g_lock);
}

void Gpt_StopTimer(Gpt_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    if (c->timer) SilOs_TimerCancel(c->timer);
    c->timer = 0u;
    c->running = 0u;
    sil_mutex_unlock(g_lock);
}

Gpt_ValueType Gpt_GetTimeElapsed(Gpt_ChannelType Channel) {
    Chan *c = chan(Channel);
    Gpt_ValueType v = 0;
    if (c == NULL_PTR) return 0;
    sil_mutex_lock(g_lock);
    if (c->running) {
        uint64 now = SilOs_ClockNow();
        uint64 el = (now > c->startNs) ? now - c->startNs : 0u;
        if (el > c->durationNs) el = c->durationNs;
        v = ns_to_ticks(c, el);
    } else if (c->expired) {
        v = c->value;
    }
    sil_mutex_unlock(g_lock);
    return v;
}

Gpt_ValueType Gpt_GetTimeRemaining(Gpt_ChannelType Channel) {
    Chan *c = chan(Channel);
    Gpt_ValueType el;
    if (c == NULL_PTR) return 0;
    el = Gpt_GetTimeElapsed(Channel);
    return (c->running || c->expired) ? (c->value > el ? c->value - el : 0u) : 0u;
}

void Gpt_EnableNotification(Gpt_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c != NULL_PTR) c->notify = 1u;
}
void Gpt_DisableNotification(Gpt_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c != NULL_PTR) c->notify = 0u;
}
void Gpt_SetMode(Gpt_ModeType Mode) { (void)Mode; }
void Gpt_DisableWakeup(Gpt_ChannelType Channel) { (void)Channel; }
void Gpt_EnableWakeup(Gpt_ChannelType Channel) { (void)Channel; }

Std_ReturnType Gpt_GetPredefTimerValue(Gpt_PredefTimerType PredefTimer, uint32 *TimeValuePtr) {
    uint64 us = SilOs_ClockNow() / 1000u;
    if (TimeValuePtr == NULL_PTR) return E_NOT_OK;
    switch (PredefTimer) {
    case GPT_PREDEF_TIMER_1US_16BIT: *TimeValuePtr = (uint32)(us & 0xFFFFu); break;
    case GPT_PREDEF_TIMER_1US_24BIT: *TimeValuePtr = (uint32)(us & 0xFFFFFFu); break;
    case GPT_PREDEF_TIMER_1US_32BIT: *TimeValuePtr = (uint32)us; break;
    default: *TimeValuePtr = (uint32)(us / 100u); break;
    }
    return E_OK;
}

void Gpt_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr) {
    if (VersionInfoPtr == NULL_PTR) return;
    VersionInfoPtr->vendorID = GPT_VENDOR_ID;
    VersionInfoPtr->moduleID = GPT_MODULE_ID;
    VersionInfoPtr->sw_major_version = 1u;
    VersionInfoPtr->sw_minor_version = 0u;
    VersionInfoPtr->sw_patch_version = 0u;
}
