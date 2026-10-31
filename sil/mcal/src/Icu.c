/* Icu.c - virtual input capture. Every channel sees a signal described in the SilIo registry
 * (SILIO_ICU, channel id): a static level (value) or a generator (freq Hz, duty 0..1). Edge counts,
 * period/duty measurements and timestamps are derived analytically from the generator; edges of a
 * static level set from outside and generator edges (up to 20 kHz) run the notifications. */
#include "Icu.h"
#include "SilIo.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#include <math.h>

#define ICU_MAX 64u
#define ICU_MAX_NOTIFY_HZ 20000.0

typedef struct {
    const SilIcu_ChannelCfg *cfg;
    uint8 activation;      /* Icu_ActivationType */
    uint8 notify;
    uint8 inputActive;
    uint8 counting;
    uint8 measuring;
    uint8 timestamping;
    uint8 lastLevel;
    uint32 generation;
    uint64 genStartNs;     /* generator edge counting starts here */
    uint32 staticEdges;    /* edges from level changes set from outside */
    uint32 frozenEdges;    /* count when counting was disabled */
    Icu_ValueType *tsBuf;
    uint16 tsSize, tsIndex, tsNotifyInterval, tsSinceNotify;
    uint8 tsCircular;
    uint32 timer;          /* edge notification/timestamp generator timer */
} Chan;

static Chan g_ch[ICU_MAX];
static sil_mutex_t *g_lock;
static uint8 g_init;

static Chan *chan_by_id(Icu_ChannelType id) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numIcuChannels && i < ICU_MAX; ++i)
        if (g_ch[i].cfg->channelId == id) return &g_ch[i];
    return NULL_PTR;
}

static double gen_freq(const Chan *c) { return SilIo_Get(SILIO_ICU, c->cfg->channelId, SILIO_F_FREQ); }
static double gen_duty(const Chan *c) {
    double d = SilIo_Get(SILIO_ICU, c->cfg->channelId, SILIO_F_DUTY);
    return d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
}
static double tick_hz(const Chan *c) { return c->cfg->tickHz ? (double)c->cfg->tickHz : SilIo_Get(SILIO_ICU, c->cfg->channelId, SILIO_F_TICKHZ); }
static Icu_ValueType to_ticks(const Chan *c, double seconds) { return (Icu_ValueType)(seconds * tick_hz(c) + 0.5); }

static uint32 edges_per_period(const Chan *c) { return (c->activation == ICU_BOTH_EDGES) ? 2u : 1u; }

static uint32 generator_edges(const Chan *c, uint64 now) {
    double f = gen_freq(c);
    if (f <= 0.0 || now <= c->genStartNs) return 0u;
    return (uint32)floor(((double)(now - c->genStartNs) / 1e9) * f) * edges_per_period(c);
}

static uint32 total_edges(const Chan *c, uint64 now) {
    if (!c->counting) return c->frozenEdges;
    return c->staticEdges + generator_edges(c, now);
}

static uint8 current_level(const Chan *c, uint64 now) {
    double f = gen_freq(c);
    if (f <= 0.0) return (SilIo_Get(SILIO_ICU, c->cfg->channelId, SILIO_F_VALUE) != 0.0) ? 1u : 0u;
    {
        double t = (double)(now - c->genStartNs) / 1e9 * f;
        double phase = t - floor(t);
        return (phase < gen_duty(c)) ? 1u : 0u;
    }
}

static void deliver_edge(Chan *c, uint64 now, int fromGenerator) {
    SilMcal_NotifyFn fn = NULL_PTR;
    /* lock held */
    c->inputActive = 1u;
    if (!fromGenerator && c->counting) c->staticEdges++;
    if (c->timestamping && c->tsBuf != NULL_PTR && c->tsSize > 0u) {
        if (c->tsIndex < c->tsSize) {
            c->tsBuf[c->tsIndex] = to_ticks(c, (double)now / 1e9);
            c->tsIndex++;
            if (c->tsIndex >= c->tsSize && c->tsCircular) c->tsIndex = 0u;
            if (c->tsNotifyInterval > 0u && ++c->tsSinceNotify >= c->tsNotifyInterval) {
                c->tsSinceNotify = 0u;
                if (c->notify) fn = c->cfg->timestampNotification;
            }
        }
    } else if (c->notify && c->cfg->mode == ICU_MODE_SIGNAL_EDGE_DETECT) {
        fn = c->cfg->signalNotification;
    }
    if (fn != NULL_PTR) {
        sil_mutex_unlock(g_lock);
        SilMcal_Notify(fn);
        sil_mutex_lock(g_lock);
    }
}

static void arm_generator(Chan *c);

static void gen_edge(void *arg) {
    Chan *c = (Chan *)arg;
    sil_mutex_lock(g_lock);
    if (c->timer) {
        c->timer = 0u;
        deliver_edge(c, SilOs_ClockNow(), 1);
        arm_generator(c);
    }
    sil_mutex_unlock(g_lock);
}

/* lock held: periodic edge events for notifications and timestamps, bounded in rate */
static void arm_generator(Chan *c) {
    double f = gen_freq(c);
    int wanted = (c->notify && c->cfg->mode == ICU_MODE_SIGNAL_EDGE_DETECT) || c->timestamping;
    if (c->timer) {
        SilOs_TimerCancel(c->timer);
        c->timer = 0u;
    }
    if (!wanted || f <= 0.0 || f > ICU_MAX_NOTIFY_HZ) return;
    c->timer = SilOs_TimerSchedule((uint64)(1e9 / (f * (double)edges_per_period(c))), gen_edge, c);
}

static void on_io_change(void *user, SilIo_Kind kind, uint32 id, uint8 field, double oldV, double newV) {
    Chan *c;
    (void)user;
    (void)kind;
    sil_mutex_lock(g_lock);
    c = chan_by_id((Icu_ChannelType)id);
    if (c != NULL_PTR) {
        uint64 now = SilOs_ClockNow();
        if (field == SILIO_F_VALUE && gen_freq(c) <= 0.0) {
            uint8 level = (newV != 0.0) ? 1u : 0u;
            uint8 old = (oldV != 0.0) ? 1u : 0u;
            if (level != old) {
                int rising = level != 0u;
                if (c->activation == ICU_BOTH_EDGES || (rising && c->activation == ICU_RISING_EDGE) || (!rising && c->activation == ICU_FALLING_EDGE))
                    deliver_edge(c, now, 0);
                c->lastLevel = level;
            }
        } else if (field == SILIO_F_FREQ || field == SILIO_F_DUTY) {
            /* freeze what the old generator produced, restart the new one now */
            if (c->counting) c->staticEdges += generator_edges(c, now);
            c->genStartNs = now;
            arm_generator(c);
        }
    }
    sil_mutex_unlock(g_lock);
}

static void ensure(void) {
    uint16 i;
    if (g_init) return;
    g_lock = sil_mutex_create();
    for (i = 0; i < SilMcal_Cfg.numIcuChannels && i < ICU_MAX; ++i) {
        g_ch[i].cfg = &SilMcal_Cfg.icuChannels[i];
        g_ch[i].activation = g_ch[i].cfg->startEdge;
        g_ch[i].counting = 1u;
    }
    g_init = 1u;
    SilIo_Subscribe(SILIO_ICU, on_io_change, NULL_PTR);
}

static Chan *chan(Icu_ChannelType id) {
    ensure();
    return chan_by_id(id);
}

/* ---- API ------------------------------------------------------------------ */
void Icu_Init(const Icu_ConfigType *ConfigPtr) {
    uint16 i;
    uint64 now;
    (void)ConfigPtr;
    ensure();
    now = SilOs_ClockNow();
    sil_mutex_lock(g_lock);
    for (i = 0; i < SilMcal_Cfg.numIcuChannels && i < ICU_MAX; ++i) {
        Chan *c = &g_ch[i];
        c->activation = c->cfg->startEdge;
        c->notify = 0u;
        c->inputActive = 0u;
        c->genStartNs = now;
        c->staticEdges = 0u;
        c->frozenEdges = 0u;
        c->counting = (c->cfg->mode == ICU_MODE_EDGE_COUNTER) ? 1u : 0u;
        c->measuring = 0u;
        c->timestamping = 0u;
        c->lastLevel = current_level(c, now);
    }
    sil_mutex_unlock(g_lock);
}

void Icu_DeInit(void) {
    uint16 i;
    ensure();
    sil_mutex_lock(g_lock);
    for (i = 0; i < SilMcal_Cfg.numIcuChannels && i < ICU_MAX; ++i) {
        g_ch[i].notify = 0u;
        g_ch[i].timestamping = 0u;
        arm_generator(&g_ch[i]);
    }
    sil_mutex_unlock(g_lock);
}

void Icu_SetMode(Icu_ModeType Mode) { (void)Mode; }
void Icu_DisableWakeup(Icu_ChannelType Channel) { (void)Channel; }
void Icu_EnableWakeup(Icu_ChannelType Channel) { (void)Channel; }
void Icu_CheckWakeup(uint32 WakeupSource) { (void)WakeupSource; }

void Icu_SetActivationCondition(Icu_ChannelType Channel, Icu_ActivationType Activation) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    c->activation = (uint8)Activation;
    arm_generator(c);
    sil_mutex_unlock(g_lock);
}

void Icu_EnableNotification(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    c->notify = 1u;
    arm_generator(c);
    sil_mutex_unlock(g_lock);
}

void Icu_DisableNotification(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    c->notify = 0u;
    arm_generator(c);
    sil_mutex_unlock(g_lock);
}

Icu_InputStateType Icu_GetInputState(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    Icu_InputStateType s = ICU_IDLE;
    if (c == NULL_PTR) return ICU_IDLE;
    sil_mutex_lock(g_lock);
    if (c->inputActive || (gen_freq(c) > 0.0 && c->cfg->mode == ICU_MODE_SIGNAL_EDGE_DETECT)) s = ICU_ACTIVE;
    c->inputActive = 0u;
    sil_mutex_unlock(g_lock);
    return s;
}

void Icu_StartTimestamp(Icu_ChannelType Channel, Icu_ValueType *BufferPtr, uint16 BufferSize, uint16 NotifyInterval) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR || BufferPtr == NULL_PTR || BufferSize == 0u) return;
    sil_mutex_lock(g_lock);
    c->tsBuf = BufferPtr;
    c->tsSize = BufferSize;
    c->tsIndex = 0u;
    c->tsNotifyInterval = NotifyInterval;
    c->tsSinceNotify = 0u;
    c->tsCircular = 0u;
    c->timestamping = 1u;
    arm_generator(c);
    sil_mutex_unlock(g_lock);
}

void Icu_StopTimestamp(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    c->timestamping = 0u;
    arm_generator(c);
    sil_mutex_unlock(g_lock);
}

Icu_IndexType Icu_GetTimestampIndex(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    return (c != NULL_PTR) ? c->tsIndex : 0u;
}

void Icu_ResetEdgeCount(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    c->staticEdges = 0u;
    c->frozenEdges = 0u;
    c->genStartNs = SilOs_ClockNow();
    sil_mutex_unlock(g_lock);
}

void Icu_EnableEdgeCount(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    if (!c->counting) {
        c->counting = 1u;
        c->staticEdges = c->frozenEdges;
        c->genStartNs = SilOs_ClockNow();
    }
    sil_mutex_unlock(g_lock);
}

void Icu_DisableEdgeCount(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    if (c->counting) {
        c->frozenEdges = total_edges(c, SilOs_ClockNow());
        c->counting = 0u;
    }
    sil_mutex_unlock(g_lock);
}

void Icu_EnableEdgeDetection(Icu_ChannelType Channel) { Icu_EnableNotification(Channel); }
void Icu_DisableEdgeDetection(Icu_ChannelType Channel) { Icu_DisableNotification(Channel); }

Icu_EdgeNumberType Icu_GetEdgeNumbers(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    Icu_EdgeNumberType n;
    if (c == NULL_PTR) return 0;
    sil_mutex_lock(g_lock);
    n = total_edges(c, SilOs_ClockNow());
    sil_mutex_unlock(g_lock);
    return n;
}

void Icu_StartSignalMeasurement(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c != NULL_PTR) c->measuring = 1u;
}
void Icu_StopSignalMeasurement(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    if (c != NULL_PTR) c->measuring = 0u;
}

Icu_ValueType Icu_GetTimeElapsed(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    double f, period;
    if (c == NULL_PTR) return 0;
    f = gen_freq(c);
    if (f <= 0.0) return 0;
    period = 1.0 / f;
    switch (c->cfg->measProperty) {
    case ICU_HIGH_TIME: return to_ticks(c, period * gen_duty(c));
    case ICU_LOW_TIME: return to_ticks(c, period * (1.0 - gen_duty(c)));
    default: return to_ticks(c, period);
    }
}

void Icu_GetDutyCycleValues(Icu_ChannelType Channel, Icu_DutyCycleType *DutyCycleValues) {
    Chan *c = chan(Channel);
    double f;
    if (DutyCycleValues == NULL_PTR) return;
    DutyCycleValues->ActiveTime = 0u;
    DutyCycleValues->PeriodTime = 0u;
    if (c == NULL_PTR) return;
    f = gen_freq(c);
    if (f <= 0.0) return;
    DutyCycleValues->PeriodTime = to_ticks(c, 1.0 / f);
    DutyCycleValues->ActiveTime = to_ticks(c, gen_duty(c) / f);
}

Icu_LevelType Icu_GetInputLevel(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    return (c != NULL_PTR) ? current_level(c, SilOs_ClockNow()) : 0u;
}

Icu_ValueType Icu_GetCaptureRegisterValue(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    return (c != NULL_PTR) ? to_ticks(c, (double)SilOs_ClockNow() / 1e9) : 0u;
}

Icu_ValueType Icu_GetPulseWidth(Icu_ChannelType Channel) {
    Chan *c = chan(Channel);
    double f;
    if (c == NULL_PTR) return 0;
    f = gen_freq(c);
    return (f > 0.0) ? to_ticks(c, gen_duty(c) / f) : 0u;
}

void Icu_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = ICU_VENDOR_ID;
    versioninfo->moduleID = ICU_MODULE_ID;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}
