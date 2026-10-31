/* Adc.c - virtual ADC: group conversions take their virtual time, results come from the SilIo registry
 * (SILIO_ADC, channel id, value = raw counts), notifications run in ISR context. */
#include "Adc.h"
#include "SilIo.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#define ADC_MAX_GROUPS 32u
#define ADC_MAX_CH 64u

typedef struct {
    const SilAdc_GroupCfg *cfg;
    volatile uint32 status;   /* Adc_StatusType */
    Adc_ValueGroupType *buf;
    uint8 notify;
    uint8 hwTrig;
    uint8 active;             /* conversions scheduled */
    uint16 sample;            /* samples completed in the current stream */
    uint16 lastSample;        /* index of the last completed sample */
    uint64 chMask;            /* enabled channels (bit per position) */
    uint32 timer;
} AdcGroup;

static AdcGroup g_grp[ADC_MAX_GROUPS];
static sil_mutex_t *g_lock;
static uint8 g_init;

static void ensure(void) {
    uint16 i;
    if (g_init) return;
    g_lock = sil_mutex_create();
    for (i = 0; i < SilMcal_Cfg.numAdcGroups && i < ADC_MAX_GROUPS; ++i) {
        g_grp[i].cfg = &SilMcal_Cfg.adcGroups[i];
        g_grp[i].chMask = ~(uint64)0;
    }
    g_init = 1u;
}

static AdcGroup *group(Adc_GroupType id) {
    uint16 i;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numAdcGroups && i < ADC_MAX_GROUPS; ++i)
        if (g_grp[i].cfg->groupId == id) return &g_grp[i];
    return NULL_PTR;
}

static uint64 conversion_ns(const AdcGroup *g) {
    if (g->cfg->conversionNs) return g->cfg->conversionNs;
    return (uint64)(g->cfg->numChannels ? g->cfg->numChannels : 1u) * 1000u; /* 1 us per channel */
}

static void schedule(AdcGroup *g, uint64 delayNs);

static void convert(void *arg) {
    AdcGroup *g = (AdcGroup *)arg;
    SilMcal_NotifyFn fn = NULL_PTR;
    sil_mutex_lock(g_lock);
    if (g->active && g->buf != NULL_PTR) {
        uint16 n = g->cfg->numChannels, i;
        uint16 samples = g->cfg->streaming ? (g->cfg->streamingSamples ? g->cfg->streamingSamples : 1u) : 1u;
        uint16 slot = g->cfg->streaming ? g->sample : 0u;
        for (i = 0; i < n && i < ADC_MAX_CH; ++i) {
            if (g->chMask & ((uint64)1 << i)) g->buf[(uint32)slot * n + i] = (Adc_ValueGroupType)SilIo_Get(SILIO_ADC, g->cfg->channels[i], SILIO_F_VALUE);
        }
        g->lastSample = slot;
        g->sample = (uint16)(slot + 1u);
        if (g->cfg->streaming && g->sample < samples) {
            g->status = ADC_BUSY;
            schedule(g, conversion_ns(g));
        } else {
            g->status = g->cfg->streaming ? ADC_STREAM_COMPLETED : ADC_COMPLETED;
            if (g->cfg->continuous || g->hwTrig) {
                g->sample = 0u;
                schedule(g, (g->hwTrig && !g->cfg->continuous) ? 1000000u : conversion_ns(g));
            } else {
                g->active = 0u;
            }
            if (g->notify) fn = g->cfg->notification;
        }
    }
    sil_mutex_unlock(g_lock);
    if (fn != NULL_PTR) SilMcal_Notify(fn);
}

static void schedule(AdcGroup *g, uint64 delayNs) { g->timer = SilOs_TimerSchedule(delayNs, convert, g); }

void Adc_Init(const Adc_ConfigType *ConfigPtr) {
    uint16 i;
    (void)ConfigPtr;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numAdcGroups && i < ADC_MAX_GROUPS; ++i) {
        g_grp[i].status = ADC_IDLE;
        g_grp[i].active = 0u;
        g_grp[i].notify = 0u;
        g_grp[i].hwTrig = 0u;
    }
}

void Adc_DeInit(void) {
    uint16 i;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numAdcGroups && i < ADC_MAX_GROUPS; ++i) Adc_StopGroupConversion(g_grp[i].cfg->groupId);
}

Std_ReturnType Adc_SetupResultBuffer(Adc_GroupType Group, Adc_ValueGroupType *DataBufferPtr) {
    AdcGroup *g = group(Group);
    if (g == NULL_PTR || DataBufferPtr == NULL_PTR) return E_NOT_OK;
    sil_mutex_lock(g_lock);
    g->buf = DataBufferPtr;
    sil_mutex_unlock(g_lock);
    return E_OK;
}

void Adc_StartGroupConversion(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    if (g == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    if (g->buf != NULL_PTR && !g->active) {
        g->active = 1u;
        g->sample = 0u;
        g->status = ADC_BUSY;
        schedule(g, conversion_ns(g));
    }
    sil_mutex_unlock(g_lock);
}

void Adc_StopGroupConversion(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    if (g == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    if (g->timer) SilOs_TimerCancel(g->timer);
    g->timer = 0u;
    g->active = 0u;
    g->status = ADC_IDLE;
    sil_mutex_unlock(g_lock);
}

Std_ReturnType Adc_ReadGroup(Adc_GroupType Group, Adc_ValueGroupType *DataBufferPtr) {
    AdcGroup *g = group(Group);
    Std_ReturnType r = E_NOT_OK;
    if (g == NULL_PTR || DataBufferPtr == NULL_PTR) return E_NOT_OK;
    sil_mutex_lock(g_lock);
    if (g->buf != NULL_PTR && (g->status == ADC_COMPLETED || g->status == ADC_STREAM_COMPLETED)) {
        uint16 n = g->cfg->numChannels, i;
        for (i = 0; i < n; ++i) DataBufferPtr[i] = g->buf[(uint32)g->lastSample * n + i];
        g->status = g->active ? ADC_BUSY : ADC_IDLE;
        r = E_OK;
    }
    sil_mutex_unlock(g_lock);
    return r;
}

void Adc_EnableHardwareTrigger(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    if (g == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    g->hwTrig = 1u;
    if (g->buf != NULL_PTR && !g->active) {
        g->active = 1u;
        g->sample = 0u;
        g->status = ADC_BUSY;
        schedule(g, 1000000u);
    }
    sil_mutex_unlock(g_lock);
}

void Adc_DisableHardwareTrigger(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    if (g == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    g->hwTrig = 0u;
    if (g->timer) SilOs_TimerCancel(g->timer);
    g->timer = 0u;
    g->active = 0u;
    g->status = ADC_IDLE;
    sil_mutex_unlock(g_lock);
}

void Adc_EnableGroupNotification(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    if (g != NULL_PTR) g->notify = 1u;
}
void Adc_DisableGroupNotification(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    if (g != NULL_PTR) g->notify = 0u;
}

Adc_StatusType Adc_GetGroupStatus(Adc_GroupType Group) {
    AdcGroup *g = group(Group);
    return (g != NULL_PTR) ? (Adc_StatusType)g->status : ADC_IDLE;
}

Adc_StreamNumSampleType Adc_GetStreamLastPointer(Adc_GroupType Group, Adc_ValueGroupType **PtrToSamplePtr) {
    AdcGroup *g = group(Group);
    Adc_StreamNumSampleType n = 0;
    if (PtrToSamplePtr == NULL_PTR) return 0;
    *PtrToSamplePtr = NULL_PTR;
    if (g == NULL_PTR) return 0;
    sil_mutex_lock(g_lock);
    if (g->buf != NULL_PTR && (g->status == ADC_COMPLETED || g->status == ADC_STREAM_COMPLETED)) {
        *PtrToSamplePtr = &g->buf[(uint32)g->lastSample * g->cfg->numChannels];
        n = (Adc_StreamNumSampleType)(g->lastSample + 1u);
        g->status = g->active ? ADC_BUSY : ADC_IDLE;
    }
    sil_mutex_unlock(g_lock);
    return n;
}

void Adc_EnableChannel(Adc_GroupType Group, Adc_ChannelType Channel) {
    AdcGroup *g = group(Group);
    uint16 i;
    if (g == NULL_PTR) return;
    for (i = 0; i < g->cfg->numChannels && i < ADC_MAX_CH; ++i)
        if (g->cfg->channels[i] == Channel) g->chMask |= ((uint64)1 << i);
}

void Adc_DisableChannel(Adc_GroupType Group, Adc_ChannelType Channel) {
    AdcGroup *g = group(Group);
    uint16 i;
    if (g == NULL_PTR) return;
    for (i = 0; i < g->cfg->numChannels && i < ADC_MAX_CH; ++i)
        if (g->cfg->channels[i] == Channel) g->chMask &= ~((uint64)1 << i);
}

void Adc_Calibration(Adc_HwUnitType Unit, Adc_CalibrationStatusType *pStatus) {
    (void)Unit;
    if (pStatus != NULL_PTR) *pStatus = ADC_CALIBRATION_OK;
}

void Adc_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = ADC_VENDOR_ID;
    versioninfo->moduleID = ADC_MODULE_ID;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}
