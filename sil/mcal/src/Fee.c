/* Fee.c - file backed flash EEPROM emulation replacing the NXP Fee (and the Mem/MemAcc stack below it).
 * Blocks come from the generated table; the image <nvdir>/fee.bin holds a header, a block directory
 * (number, size, valid) and the data. One job at a time, completed in Fee_MainFunction like the real
 * module, with NvM_JobEndNotification / NvM_JobErrorNotification from the generated configuration. */
#include "Fee.h"
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEE_MAX_BLOCKS 512u
#define FEE_MAGIC "SILFEE1\0"

typedef enum { JOB_NONE = 0, JOB_READ, JOB_WRITE, JOB_INVALIDATE, JOB_ERASE } JobKind;

typedef struct {
    uint8 valid;
    uint8 *data;
} Block;

static Block g_blk[FEE_MAX_BLOCKS];
static volatile uint32 g_status;    /* MemIf_StatusType */
static volatile uint32 g_result;    /* MemIf_JobResultType */
static JobKind g_job;
static uint16 g_jobBlock, g_jobOffset, g_jobLength;
static uint8 *g_jobBuf;
static const uint8 *g_jobSrc;
static uint32 g_mainCalls;
static char g_path[600];
static uint8 g_init;

static const SilFee_BlockCfg *block_cfg(uint16 number, uint32 *idx) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numFeeBlocks && i < FEE_MAX_BLOCKS; ++i) {
        if (SilMcal_Cfg.feeBlocks[i].blockNumber == number) {
            *idx = i;
            return &SilMcal_Cfg.feeBlocks[i];
        }
    }
    return NULL_PTR;
}

static void image_path(void) { snprintf(g_path, sizeof g_path, "%s/fee.bin", SilNv_Dir()); }

static void image_load(void) {
    FILE *f;
    uint8 hdr[8];
    uint16 i;
    image_path();
    f = fopen(g_path, "rb");
    if (f == NULL_PTR) return;
    if (fread(hdr, 1, 8, f) != 8u || memcmp(hdr, FEE_MAGIC, 8) != 0) {
        fclose(f);
        sil_log("Fee: %s is not a SIL Fee image, starting empty", g_path);
        return;
    }
    for (;;) {
        uint8 rec[5];
        uint16 number, size;
        uint32 idx;
        const SilFee_BlockCfg *c;
        if (fread(rec, 1, 5, f) != 5u) break;
        number = (uint16)(rec[0] | (rec[1] << 8));
        size = (uint16)(rec[2] | (rec[3] << 8));
        c = block_cfg(number, &idx);
        if (c != NULL_PTR && size == c->size) {
            g_blk[idx].valid = rec[4];
            if (fread(g_blk[idx].data, 1, size, f) != size) break;
        } else {
            if (fseek(f, size, SEEK_CUR) != 0) break; /* block no longer configured */
        }
    }
    fclose(f);
    for (i = 0; i < SilMcal_Cfg.numFeeBlocks && i < FEE_MAX_BLOCKS; ++i)
        if (g_blk[i].valid) break;
}

static void image_store(void) {
    FILE *f;
    uint16 i;
    image_path();
    f = fopen(g_path, "wb");
    if (f == NULL_PTR) {
        sil_log("Fee: cannot write %s", g_path);
        return;
    }
    fwrite(FEE_MAGIC, 1, 8, f);
    for (i = 0; i < SilMcal_Cfg.numFeeBlocks && i < FEE_MAX_BLOCKS; ++i) {
        const SilFee_BlockCfg *c = &SilMcal_Cfg.feeBlocks[i];
        uint8 rec[5];
        rec[0] = (uint8)c->blockNumber;
        rec[1] = (uint8)(c->blockNumber >> 8);
        rec[2] = (uint8)c->size;
        rec[3] = (uint8)(c->size >> 8);
        rec[4] = g_blk[i].valid;
        fwrite(rec, 1, 5, f);
        fwrite(g_blk[i].data, 1, c->size, f);
    }
    fclose(f);
}

void Fee_Init(const Fee_ConfigType *ConfigPtr) {
    uint16 i;
    (void)ConfigPtr;
    if (!g_init) {
        for (i = 0; i < SilMcal_Cfg.numFeeBlocks && i < FEE_MAX_BLOCKS; ++i) {
            g_blk[i].data = (uint8 *)calloc(SilMcal_Cfg.feeBlocks[i].size ? SilMcal_Cfg.feeBlocks[i].size : 1u, 1u);
            g_blk[i].valid = 0u;
        }
        image_load();
        g_init = 1u;
    }
    g_job = JOB_NONE;
    g_status = MEMIF_IDLE;
    g_result = MEMIF_JOB_OK;
}

void Fee_SetMode(MemIf_ModeType Mode) { (void)Mode; }

static Std_ReturnType start_job(JobKind kind, uint16 block, uint16 offset, uint8 *buf, const uint8 *src, uint16 len) {
    uint32 idx;
    if (!g_init || g_status != MEMIF_IDLE) return E_NOT_OK;
    if (block_cfg(block, &idx) == NULL_PTR) return E_NOT_OK;
    g_job = kind;
    g_jobBlock = block;
    g_jobOffset = offset;
    g_jobBuf = buf;
    g_jobSrc = src;
    g_jobLength = len;
    g_mainCalls = 0u;
    g_status = MEMIF_BUSY;
    g_result = MEMIF_JOB_PENDING;
    return E_OK;
}

Std_ReturnType Fee_Read(uint16 BlockNumber, uint16 BlockOffset, uint8 *DataBufferPtr, uint16 Length) {
    if (DataBufferPtr == NULL_PTR) return E_NOT_OK;
    return start_job(JOB_READ, BlockNumber, BlockOffset, DataBufferPtr, NULL_PTR, Length);
}

Std_ReturnType Fee_Write(uint16 BlockNumber, const uint8 *DataBufferPtr) {
    if (DataBufferPtr == NULL_PTR) return E_NOT_OK;
    return start_job(JOB_WRITE, BlockNumber, 0u, NULL_PTR, DataBufferPtr, 0u);
}

Std_ReturnType Fee_InvalidateBlock(uint16 BlockNumber) { return start_job(JOB_INVALIDATE, BlockNumber, 0u, NULL_PTR, NULL_PTR, 0u); }
Std_ReturnType Fee_EraseImmediateBlock(uint16 BlockNumber) { return start_job(JOB_ERASE, BlockNumber, 0u, NULL_PTR, NULL_PTR, 0u); }

void Fee_Cancel(void) {
    if (g_status == MEMIF_BUSY) {
        g_job = JOB_NONE;
        g_status = MEMIF_IDLE;
        g_result = MEMIF_JOB_CANCELED;
    }
}

MemIf_StatusType Fee_GetStatus(void) { return g_init ? (MemIf_StatusType)g_status : MEMIF_UNINIT; }
MemIf_JobResultType Fee_GetJobResult(void) { return (MemIf_JobResultType)g_result; }

void Fee_MainFunction(void) {
    uint32 idx;
    const SilFee_BlockCfg *c;
    MemIf_JobResultType result;
    if (!g_init || g_status != MEMIF_BUSY) return;
    if (++g_mainCalls < 2u) return; /* a job takes two main function cycles, as a small flash job would */
    c = block_cfg(g_jobBlock, &idx);
    result = MEMIF_JOB_OK;
    switch (g_job) {
    case JOB_READ:
        if (c == NULL_PTR) result = MEMIF_JOB_FAILED;
        else if (!g_blk[idx].valid) result = MEMIF_BLOCK_INVALID;
        else if ((uint32)g_jobOffset + g_jobLength > c->size) result = MEMIF_JOB_FAILED;
        else memcpy(g_jobBuf, g_blk[idx].data + g_jobOffset, g_jobLength);
        break;
    case JOB_WRITE:
        if (c == NULL_PTR) result = MEMIF_JOB_FAILED;
        else {
            memcpy(g_blk[idx].data, g_jobSrc, c->size);
            g_blk[idx].valid = 1u;
            image_store();
        }
        break;
    case JOB_INVALIDATE:
    case JOB_ERASE:
        if (c == NULL_PTR) result = MEMIF_JOB_FAILED;
        else {
            g_blk[idx].valid = 0u;
            memset(g_blk[idx].data, 0xFF, c->size);
            image_store();
        }
        break;
    default:
        result = MEMIF_JOB_FAILED;
        break;
    }
    g_job = JOB_NONE;
    g_result = result;
    g_status = MEMIF_IDLE;
    if (result == MEMIF_JOB_OK) SilMcal_Notify(SilMcal_Cfg.feeJobEndNotification);
    else SilMcal_Notify(SilMcal_Cfg.feeJobErrorNotification);
}

void Fee_JobEndNotification(void) {}
void Fee_JobErrorNotification(void) {}

void Fee_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr) {
    if (VersionInfoPtr == NULL_PTR) return;
    VersionInfoPtr->vendorID = FEE_VENDOR_ID;
    VersionInfoPtr->moduleID = FEE_MODULE_ID;
    VersionInfoPtr->sw_major_version = 1u;
    VersionInfoPtr->sw_minor_version = 0u;
    VersionInfoPtr->sw_patch_version = 0u;
}
