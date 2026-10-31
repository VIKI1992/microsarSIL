/* Spi.c - virtual SPI handler/driver. Sequences, jobs, channels and external devices come from the
 * generated tables; every job's MOSI stream goes to the slave model of its device, the model fills
 * MISO. Synchronous transfers consume the bus time on the calling task; asynchronous transfers end
 * after the bus time through the unit's ISR (interrupt mode) or Spi_MainFunction_Handling (polling). */
#include "Spi.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#include <string.h>

#define SPI_MAX_CH 128u
#define SPI_MAX_JOBS 160u
#define SPI_MAX_SEQ 64u
#define SPI_MAX_DEV 32u
#define SPI_MAX_UNITS 8u
#define SPI_IB_BYTES 256u
#define SPI_XFER_BYTES 1024u

typedef struct {
    const Spi_DataBufferType *ebSrc;
    Spi_DataBufferType *ebDst;
    Spi_NumberOfDataType ebLength;  /* elements */
    uint8 ibTx[SPI_IB_BYTES];
    uint8 ibRx[SPI_IB_BYTES];
} ChanState;

typedef struct {
    SilSpi_SlaveFn fn;
    void *user;
} Slave;

typedef struct {
    volatile uint32 result;  /* Spi_SeqResultType */
    uint32 timer;
    uint8 async;             /* pending asynchronous transfer */
} SeqState;

static ChanState g_ch[SPI_MAX_CH];
static volatile uint32 g_jobResult[SPI_MAX_JOBS];
static SeqState g_seq[SPI_MAX_SEQ];
static Slave g_slave[SPI_MAX_DEV];
static volatile uint32 g_unitPending[SPI_MAX_UNITS];  /* sequences in flight per unit */
static volatile uint32 g_unitDone[SPI_MAX_UNITS];     /* completed, waiting for the ISR / main function */
static uint8 g_unitAsyncMode[SPI_MAX_UNITS];         /* Spi_AsyncModeType */
static uint8 g_asyncMode;
static volatile uint32 g_status; /* Spi_StatusType */
static sil_mutex_t *g_lock;
static uint8 g_init;

static void slave_echo(void *u, uint8 d, const uint8 *mosi, uint8 *miso, uint32 n, uint8 w) {
    (void)u; (void)d; (void)w;
    memcpy(miso, mosi, n);
}
static void slave_zero(void *u, uint8 d, const uint8 *mosi, uint8 *miso, uint32 n, uint8 w) {
    (void)u; (void)d; (void)mosi; (void)w;
    memset(miso, 0, n);
}
static void slave_ones(void *u, uint8 d, const uint8 *mosi, uint8 *miso, uint32 n, uint8 w) {
    (void)u; (void)d; (void)mosi; (void)w;
    memset(miso, 0xFF, n);
}
/* register file device: byte 0 = register | 0x80 for read; write: following bytes stored; read: following bytes returned */
static void slave_regfile(void *u, uint8 d, const uint8 *mosi, uint8 *miso, uint32 n, uint8 w) {
    (void)u; (void)w;
    if (n == 0u) return;
    if (mosi[0] & 0x80u) {
        miso[0] = 0u;
        if (n > 1u) SilMcal_RegRead(0u, d, (uint8)(mosi[0] & 0x7Fu), miso + 1, n - 1u);
    } else {
        if (n > 1u) SilMcal_RegWrite(0u, d, mosi[0], mosi + 1, n - 1u);
        memset(miso, 0, n);
    }
}

static void ensure(void) {
    uint32 i;
    if (g_init) return;
    g_lock = sil_mutex_create();
    for (i = 0; i < SPI_MAX_DEV; ++i) g_slave[i].fn = slave_echo;
    for (i = 0; i < SPI_MAX_UNITS; ++i) g_unitAsyncMode[i] = SPI_INTERRUPT_MODE;
    g_asyncMode = SPI_INTERRUPT_MODE;
    g_status = SPI_UNINIT;
    g_init = 1u;
}

void SilSpi_SetSlave(uint8 device, SilSpi_SlaveFn fn, void *user) {
    ensure();
    if (device >= SPI_MAX_DEV) return;
    g_slave[device].fn = fn ? fn : slave_echo;
    g_slave[device].user = user;
}

int SilSpi_SetSlaveModel(uint8 device, const char *model) {
    SilSpi_SlaveFn fn;
    if (model == NULL_PTR) return -1;
    if (strcmp(model, "echo") == 0) fn = slave_echo;
    else if (strcmp(model, "zero") == 0) fn = slave_zero;
    else if (strcmp(model, "ones") == 0) fn = slave_ones;
    else if (strcmp(model, "regfile") == 0) fn = slave_regfile;
    else return -1;
    SilSpi_SetSlave(device, fn, NULL_PTR);
    return 0;
}

static const SilSpi_ChannelCfg *chan_cfg(Spi_ChannelType id, uint32 *idx) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numSpiChannels && i < SPI_MAX_CH; ++i) {
        if (SilMcal_Cfg.spiChannels[i].channelId == id) {
            *idx = i;
            return &SilMcal_Cfg.spiChannels[i];
        }
    }
    return NULL_PTR;
}

static const SilSpi_SequenceCfg *seq_cfg(Spi_SequenceType id, uint32 *idx) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numSpiSequences && i < SPI_MAX_SEQ; ++i) {
        if (SilMcal_Cfg.spiSequences[i].sequenceId == id) {
            *idx = i;
            return &SilMcal_Cfg.spiSequences[i];
        }
    }
    return NULL_PTR;
}

static uint32 bytes_per_element(const SilSpi_ChannelCfg *c) { return (c->dataWidth > 16u) ? 4u : (c->dataWidth > 8u ? 2u : 1u); }

static uint8 unit_of_seq(const SilSpi_SequenceCfg *s) {
    if (s->numJobs == 0u) return 0u;
    {
        const SilSpi_JobCfg *j = &SilMcal_Cfg.spiJobs[s->jobs[0]];
        return (j->device < SilMcal_Cfg.numSpiDevices) ? SilMcal_Cfg.spiDevices[j->device].unit : 0u;
    }
}

/* run every job of the sequence through the slave models; returns the bus time in ns */
static uint64 execute_sequence(const SilSpi_SequenceCfg *s, uint32 seqIdx) {
    uint64 ns = 0;
    uint16 ji;
    static uint8 tx[SPI_XFER_BYTES], rx[SPI_XFER_BYTES];
    (void)seqIdx;
    for (ji = 0; ji < s->numJobs; ++ji) {
        const SilSpi_JobCfg *j = &SilMcal_Cfg.spiJobs[s->jobs[ji]];
        const SilSpi_DeviceCfg *dev = (j->device < SilMcal_Cfg.numSpiDevices) ? &SilMcal_Cfg.spiDevices[j->device] : NULL_PTR;
        uint32 baud = (dev != NULL_PTR && dev->baud) ? dev->baud : 1000000u;
        uint16 ci;
        for (ci = 0; ci < j->numChannels; ++ci) {
            uint32 cidx = j->channels[ci];
            const SilSpi_ChannelCfg *c = &SilMcal_Cfg.spiChannels[cidx];
            ChanState *st = &g_ch[cidx];
            uint32 bpe = bytes_per_element(c);
            uint32 n, k;
            if (c->isEb) {
                n = (uint32)st->ebLength * bpe;
                if (n > SPI_XFER_BYTES) n = SPI_XFER_BYTES;
                if (st->ebSrc != NULL_PTR) memcpy(tx, st->ebSrc, n);
                else for (k = 0; k < n; ++k) tx[k] = (uint8)(c->defaultData >> (8u * (k % bpe)));
            } else {
                n = (uint32)c->ibNBuffers * bpe;
                if (n > SPI_IB_BYTES) n = SPI_IB_BYTES;
                memcpy(tx, st->ibTx, n);
            }
            memset(rx, 0, n);
            if (j->device < SPI_MAX_DEV && g_slave[j->device].fn != NULL_PTR) g_slave[j->device].fn(g_slave[j->device].user, j->device, tx, rx, n, c->dataWidth);
            if (c->isEb) {
                if (st->ebDst != NULL_PTR) memcpy(st->ebDst, rx, n);
            } else {
                memcpy(st->ibRx, rx, n);
            }
            ns += ((uint64)n * 8u * 1000000000u) / baud;
            {
                SilMcal_Event ev;
                memset(&ev, 0, sizeof ev);
                ev.kind = SILMCAL_EV_SPI;
                ev.channel = j->device;
                ev.dir = 2u;
                ev.id = s->sequenceId;
                ev.len = (uint8)(n > 64u ? 64u : n);
                memcpy(ev.tx, tx, ev.len);
                memcpy(ev.rx, rx, ev.len);
                SilMcal_Capture(&ev);
            }
        }
        if (dev != NULL_PTR) ns += dev->csNs;
        g_jobResult[s->jobs[ji]] = SPI_JOB_OK;
    }
    return ns ? ns : 1u;
}

/* completion of an asynchronous sequence: results, notifications (ISR or main function context) */
static void finish_async(uint32 seqIdx) {
    const SilSpi_SequenceCfg *s = &SilMcal_Cfg.spiSequences[seqIdx];
    uint16 ji;
    sil_mutex_lock(g_lock);
    g_seq[seqIdx].async = 0u;
    if (g_seq[seqIdx].result == SPI_SEQ_PENDING) g_seq[seqIdx].result = SPI_SEQ_OK;
    {
        uint32 pending = 0u, i;
        for (i = 0; i < SilMcal_Cfg.numSpiSequences && i < SPI_MAX_SEQ; ++i) pending += g_seq[i].async;
        if (pending == 0u) g_status = SPI_IDLE;
    }
    sil_mutex_unlock(g_lock);
    for (ji = 0; ji < s->numJobs; ++ji) SilMcal_Notify(SilMcal_Cfg.spiJobs[s->jobs[ji]].endNotification);
    SilMcal_Notify(s->endNotification);
}

/* called in the unit's ISR (Spi_Ip_SPI_<n>_IRQHandler) or from the main function */
void SilSpi_Isr(uint8 unitIdx);
void SilSpi_Isr(uint8 unitIdx) {
    uint32 i;
    ensure();
    if (unitIdx >= SPI_MAX_UNITS) return;
    if (sil_atomic_load32(&g_unitDone[unitIdx]) == 0u) return;
    for (i = 0; i < SilMcal_Cfg.numSpiSequences && i < SPI_MAX_SEQ; ++i) {
        int done;
        sil_mutex_lock(g_lock);
        done = g_seq[i].async && g_seq[i].timer == 0u && unit_of_seq(&SilMcal_Cfg.spiSequences[i]) == unitIdx;
        sil_mutex_unlock(g_lock);
        if (done) {
            sil_atomic_add32(&g_unitDone[unitIdx], (uint32)-1);
            finish_async(i);
        }
    }
}

static void async_done(void *arg) {
    uint32 seqIdx = (uint32)(uintptr_t)arg;
    const SilSpi_SequenceCfg *s = &SilMcal_Cfg.spiSequences[seqIdx];
    uint8 unit = unit_of_seq(s);
    const char *handler = (unit < SilMcal_Cfg.numSpiUnits) ? SilMcal_Cfg.spiUnits[unit].isrHandler : NULL_PTR;
    sil_mutex_lock(g_lock);
    g_seq[seqIdx].timer = 0u;
    sil_mutex_unlock(g_lock);
    sil_atomic_add32(&g_unitDone[unit], 1u);
    if (g_unitAsyncMode[unit] == SPI_INTERRUPT_MODE && g_asyncMode == SPI_INTERRUPT_MODE) {
        if (!SilMcal_RaiseIsr(handler)) {
            /* no ISR for this unit in the OS configuration: complete through the soft ISR */
            SilSpi_Isr(unit);
        }
    }
    /* polling mode: Spi_MainFunction_Handling completes it */
}

/* ---- API ------------------------------------------------------------------ */
void Spi_Init(const Spi_ConfigType *ConfigPtr) {
    uint32 i;
    (void)ConfigPtr;
    ensure();
    for (i = 0; i < SPI_MAX_SEQ; ++i) {
        g_seq[i].result = SPI_SEQ_OK;
        g_seq[i].async = 0u;
    }
    for (i = 0; i < SPI_MAX_JOBS; ++i) g_jobResult[i] = SPI_JOB_OK;
    for (i = 0; i < SilMcal_Cfg.numSpiChannels && i < SPI_MAX_CH; ++i) {
        const SilSpi_ChannelCfg *c = &SilMcal_Cfg.spiChannels[i];
        uint32 k, bpe = bytes_per_element(c);
        for (k = 0; k < SPI_IB_BYTES; ++k) g_ch[i].ibTx[k] = (uint8)(c->defaultData >> (8u * (k % bpe)));
    }
    g_status = SPI_IDLE;
}

Std_ReturnType Spi_DeInit(void) {
    ensure();
    if (g_status == SPI_BUSY) return E_NOT_OK;
    g_status = SPI_UNINIT;
    return E_OK;
}

Std_ReturnType Spi_WriteIB(Spi_ChannelType Channel, const Spi_DataBufferType *DataBufferPtr) {
    uint32 idx;
    const SilSpi_ChannelCfg *c = chan_cfg(Channel, &idx);
    uint32 n;
    ensure();
    if (c == NULL_PTR || c->isEb) return E_NOT_OK;
    n = (uint32)c->ibNBuffers * bytes_per_element(c);
    if (n > SPI_IB_BYTES) n = SPI_IB_BYTES;
    if (DataBufferPtr != NULL_PTR) memcpy(g_ch[idx].ibTx, DataBufferPtr, n);
    else {
        uint32 k, bpe = bytes_per_element(c);
        for (k = 0; k < n; ++k) g_ch[idx].ibTx[k] = (uint8)(c->defaultData >> (8u * (k % bpe)));
    }
    return E_OK;
}

Std_ReturnType Spi_ReadIB(Spi_ChannelType Channel, Spi_DataBufferType *DataBufferPointer) {
    uint32 idx;
    const SilSpi_ChannelCfg *c = chan_cfg(Channel, &idx);
    uint32 n;
    ensure();
    if (c == NULL_PTR || c->isEb || DataBufferPointer == NULL_PTR) return E_NOT_OK;
    n = (uint32)c->ibNBuffers * bytes_per_element(c);
    if (n > SPI_IB_BYTES) n = SPI_IB_BYTES;
    memcpy(DataBufferPointer, g_ch[idx].ibRx, n);
    return E_OK;
}

Std_ReturnType Spi_SetupEB(Spi_ChannelType Channel, const Spi_DataBufferType *SrcDataBufferPtr, Spi_DataBufferType *DesDataBufferPtr,
                           Spi_NumberOfDataType Length) {
    uint32 idx;
    const SilSpi_ChannelCfg *c = chan_cfg(Channel, &idx);
    ensure();
    if (c == NULL_PTR || !c->isEb) return E_NOT_OK;
    if (c->ebMaxLength && Length > c->ebMaxLength) return E_NOT_OK;
    g_ch[idx].ebSrc = SrcDataBufferPtr;
    g_ch[idx].ebDst = DesDataBufferPtr;
    g_ch[idx].ebLength = Length;
    return E_OK;
}

static Std_ReturnType start_sequence(Spi_SequenceType Sequence, int sync) {
    uint32 idx;
    const SilSpi_SequenceCfg *s = seq_cfg(Sequence, &idx);
    uint64 ns;
    uint16 ji;
    ensure();
    if (s == NULL_PTR || g_status == SPI_UNINIT) return E_NOT_OK;
    sil_mutex_lock(g_lock);
    if (g_seq[idx].result == SPI_SEQ_PENDING) {
        sil_mutex_unlock(g_lock);
        return E_NOT_OK;
    }
    g_seq[idx].result = SPI_SEQ_PENDING;
    for (ji = 0; ji < s->numJobs; ++ji) g_jobResult[s->jobs[ji]] = SPI_JOB_PENDING;
    g_status = SPI_BUSY;
    ns = execute_sequence(s, idx);
    if (sync) {
        uint32 pending = 0u, i;
        g_seq[idx].result = SPI_SEQ_OK;
        for (i = 0; i < SilMcal_Cfg.numSpiSequences && i < SPI_MAX_SEQ; ++i) pending += g_seq[i].async;
        if (pending == 0u) g_status = SPI_IDLE;
        sil_mutex_unlock(g_lock);
        SilOs_ConsumeTime(ns);
        for (ji = 0; ji < s->numJobs; ++ji) SilMcal_Notify(SilMcal_Cfg.spiJobs[s->jobs[ji]].endNotification);
        SilMcal_Notify(s->endNotification);
    } else {
        g_seq[idx].async = 1u;
        sil_atomic_add32(&g_unitPending[unit_of_seq(s)], 1u);
        g_seq[idx].timer = SilOs_TimerSchedule(ns, async_done, (void *)(uintptr_t)idx);
        sil_mutex_unlock(g_lock);
        if (g_seq[idx].timer == 0u) async_done((void *)(uintptr_t)idx);
    }
    return E_OK;
}

Std_ReturnType Spi_AsyncTransmit(Spi_SequenceType Sequence) { return start_sequence(Sequence, 0); }
Std_ReturnType Spi_SyncTransmit(Spi_SequenceType Sequence) { return start_sequence(Sequence, 1); }

Spi_StatusType Spi_GetStatus(void) {
    ensure();
    return (Spi_StatusType)g_status;
}

Spi_JobResultType Spi_GetJobResult(Spi_JobType Job) {
    uint16 i;
    ensure();
    for (i = 0; i < SilMcal_Cfg.numSpiJobs && i < SPI_MAX_JOBS; ++i)
        if (SilMcal_Cfg.spiJobs[i].jobId == Job) return (Spi_JobResultType)g_jobResult[i];
    return SPI_JOB_FAILED;
}

Spi_SeqResultType Spi_GetSequenceResult(Spi_SequenceType Sequence) {
    uint32 idx;
    ensure();
    if (seq_cfg(Sequence, &idx) == NULL_PTR) return SPI_SEQ_FAILED;
    return (Spi_SeqResultType)g_seq[idx].result;
}

Spi_StatusType Spi_GetHWUnitStatus(Spi_HWUnitType HWUnit) {
    uint32 i, busy = 0u;
    ensure();
    if (g_status == SPI_UNINIT) return SPI_UNINIT;
    sil_mutex_lock(g_lock);
    for (i = 0; i < SilMcal_Cfg.numSpiSequences && i < SPI_MAX_SEQ; ++i)
        if (g_seq[i].async && unit_of_seq(&SilMcal_Cfg.spiSequences[i]) == HWUnit) busy = 1u;
    sil_mutex_unlock(g_lock);
    return busy ? SPI_BUSY : SPI_IDLE;
}

void Spi_Cancel(Spi_SequenceType Sequence) {
    uint32 idx;
    ensure();
    if (seq_cfg(Sequence, &idx) == NULL_PTR) return;
    sil_mutex_lock(g_lock);
    if (g_seq[idx].async) {
        if (g_seq[idx].timer) SilOs_TimerCancel(g_seq[idx].timer);
        g_seq[idx].timer = 0u;
        g_seq[idx].async = 0u;
        g_seq[idx].result = SPI_SEQ_CANCELED;
    }
    sil_mutex_unlock(g_lock);
}

Std_ReturnType Spi_SetAsyncMode(Spi_AsyncModeType Mode) {
    ensure();
    g_asyncMode = (uint8)Mode;
    return E_OK;
}

Std_ReturnType Spi_SetHWUnitAsyncMode(Spi_HWUnitType HWUnit, Spi_AsyncModeType AsyncMode) {
    ensure();
    if (HWUnit >= SPI_MAX_UNITS) return E_NOT_OK;
    g_unitAsyncMode[HWUnit] = (uint8)AsyncMode;
    return E_OK;
}

void Spi_MainFunction_Handling(void) {
    uint8 u;
    ensure();
    for (u = 0; u < SPI_MAX_UNITS; ++u)
        if (sil_atomic_load32(&g_unitDone[u]) != 0u) SilSpi_Isr(u);
}

void Spi_GetVersionInfo(Std_VersionInfoType *versioninfo) {
    if (versioninfo == NULL_PTR) return;
    versioninfo->vendorID = SPI_VENDOR_ID;
    versioninfo->moduleID = SPI_MODULE_ID;
    versioninfo->sw_major_version = 1u;
    versioninfo->sw_minor_version = 0u;
    versioninfo->sw_patch_version = 0u;
}
