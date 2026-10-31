/* MemAcc.c - file backed memory access (AUTOSAR MemAcc) plus Mem_43_INFLS / Mem_43_EXFLS stubs.
 * Every address area is one image file <nvdir>/memacc<areaId>.bin covering its logical address range;
 * jobs complete in MemAcc_MainFunction. Erased memory reads 0xFF. */
#include "MemAcc.h"
#include "Mem_43_EXFLS.h"
#include "Mem_43_INFLS.h"
#include "SilMcal.h"
#include "sil_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMACC_MAX_AREAS 16u

typedef struct {
    const SilMemAcc_AreaCfg *cfg;
    uint8 *image;
    uint32 base;          /* lowest logical address */
    uint32 size;          /* bytes covered */
    uint8 loaded;
    volatile uint32 status;   /* MemAcc_JobStatusType */
    volatile uint32 result;   /* MemAcc_JobResultType */
    MemAcc_JobType job;
    MemAcc_AddressType addr;
    MemAcc_LengthType len;
    MemAcc_DataType *dst;
    const MemAcc_DataType *src;
    uint32 mainCalls;
} Area;

static Area g_area[MEMACC_MAX_AREAS];
static uint8 g_init;

static void area_path(const Area *a, char *buf, size_t n) { snprintf(buf, n, "%s/memacc%u.bin", SilNv_Dir(), (unsigned)a->cfg->areaId); }

static void area_load(Area *a) {
    char path[600];
    FILE *f;
    if (a->loaded) return;
    a->loaded = 1u;
    memset(a->image, 0xFF, a->size);
    area_path(a, path, sizeof path);
    f = fopen(path, "rb");
    if (f != NULL_PTR) {
        size_t got = fread(a->image, 1, a->size, f);
        (void)got;
        fclose(f);
    }
}

static void area_store(const Area *a) {
    char path[600];
    FILE *f;
    area_path(a, path, sizeof path);
    f = fopen(path, "wb");
    if (f == NULL_PTR) {
        sil_log("MemAcc: cannot write %s", path);
        return;
    }
    fwrite(a->image, 1, a->size, f);
    fclose(f);
}

static Area *area(MemAcc_AddressAreaIdType id) {
    uint16 i;
    for (i = 0; i < SilMcal_Cfg.numMemAccAreas && i < MEMACC_MAX_AREAS; ++i)
        if (g_area[i].cfg->areaId == id) return &g_area[i];
    return NULL_PTR;
}

void MemAcc_Init(const MemAcc_ConfigType *ConfigPtr) {
    uint16 i;
    (void)ConfigPtr;
    if (!g_init) {
        for (i = 0; i < SilMcal_Cfg.numMemAccAreas && i < MEMACC_MAX_AREAS; ++i) {
            Area *a = &g_area[i];
            uint16 s;
            uint32 lo = 0xFFFFFFFFu, hi = 0u;
            a->cfg = &SilMcal_Cfg.memAccAreas[i];
            for (s = 0; s < a->cfg->numSub; ++s) {
                const SilMemAcc_SubAreaCfg *sub = &a->cfg->sub[s];
                if (sub->logicalStart < lo) lo = sub->logicalStart;
                if (sub->logicalStart + sub->length > hi) hi = sub->logicalStart + sub->length;
            }
            if (a->cfg->numSub == 0u) {
                lo = 0u;
                hi = 65536u;
            }
            a->base = lo;
            a->size = hi - lo;
            if (a->size > (64u << 20)) a->size = 64u << 20; /* cap the host image at 64 MB */
            a->image = (uint8 *)malloc(a->size);
            a->loaded = 0u;
        }
        g_init = 1u;
    }
    for (i = 0; i < SilMcal_Cfg.numMemAccAreas && i < MEMACC_MAX_AREAS; ++i) {
        g_area[i].status = MEMACC_JOB_IDLE;
        g_area[i].result = MEMACC_MEM_OK;
        g_area[i].job = MEMACC_NO_JOB;
    }
}

void MemAcc_DeInit(void) {}

void MemAcc_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr) {
    if (VersionInfoPtr == NULL_PTR) return;
    VersionInfoPtr->vendorID = MEMACC_VENDOR_ID;
    VersionInfoPtr->moduleID = MEMACC_MODULE_ID;
    VersionInfoPtr->sw_major_version = 1u;
    VersionInfoPtr->sw_minor_version = 0u;
    VersionInfoPtr->sw_patch_version = 0u;
}

MemAcc_JobResultType MemAcc_GetJobResult(MemAcc_AddressAreaIdType AddressAreaId) {
    Area *a = area(AddressAreaId);
    return (a != NULL_PTR) ? (MemAcc_JobResultType)a->result : MEMACC_MEM_FAILED;
}

MemAcc_JobStatusType MemAcc_GetJobStatus(MemAcc_AddressAreaIdType AddressAreaId) {
    Area *a = area(AddressAreaId);
    return (a != NULL_PTR) ? (MemAcc_JobStatusType)a->status : MEMACC_JOB_IDLE;
}

Std_ReturnType MemAcc_GetMemoryInfo(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType QueryAddress, MemAcc_MemoryInfoType *MemoryInfoPtr) {
    Area *a = area(AddressAreaId);
    uint16 s;
    if (a == NULL_PTR || MemoryInfoPtr == NULL_PTR) return E_NOT_OK;
    for (s = 0; s < a->cfg->numSub; ++s) {
        const SilMemAcc_SubAreaCfg *sub = &a->cfg->sub[s];
        if (QueryAddress >= sub->logicalStart && QueryAddress < sub->logicalStart + sub->length) {
            MemoryInfoPtr->logicalStartAddress = sub->logicalStart;
            MemoryInfoPtr->physicalStartAddress = sub->logicalStart;
            MemoryInfoPtr->maxOffset = sub->length - 1u;
            MemoryInfoPtr->readPageSize = 1u;
            MemoryInfoPtr->writePageSize = sub->writePageSize ? sub->writePageSize : 8u;
            MemoryInfoPtr->eraseSectorSize = sub->eraseSectorSize ? sub->eraseSectorSize : 4096u;
            MemoryInfoPtr->hwId = 0u;
            return E_OK;
        }
    }
    return E_NOT_OK;
}

Std_ReturnType MemAcc_GetProcessedLength(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_LengthType *ProcessedLength) {
    Area *a = area(AddressAreaId);
    if (a == NULL_PTR || ProcessedLength == NULL_PTR) return E_NOT_OK;
    *ProcessedLength = (a->status == MEMACC_JOB_IDLE) ? a->len : 0u;
    return E_OK;
}

Std_ReturnType MemAcc_GetJobInfo(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_JobInfoType *JobInfoPtr) {
    Area *a = area(AddressAreaId);
    if (a == NULL_PTR || JobInfoPtr == NULL_PTR) return E_NOT_OK;
    JobInfoPtr->addressAreaId = AddressAreaId;
    JobInfoPtr->currentJob = a->job;
    JobInfoPtr->logicalAddress = a->addr;
    JobInfoPtr->length = a->len;
    JobInfoPtr->currentAddressAreaOffset = a->addr - a->base;
    JobInfoPtr->jobResult = (MemAcc_JobResultType)a->result;
    return E_OK;
}

void MemAcc_Cancel(MemAcc_AddressAreaIdType AddressAreaId) {
    Area *a = area(AddressAreaId);
    if (a == NULL_PTR || a->status != MEMACC_JOB_PENDING) return;
    a->status = MEMACC_JOB_IDLE;
    a->result = MEMACC_MEM_CANCELED;
    a->job = MEMACC_NO_JOB;
}

static Std_ReturnType start(MemAcc_AddressAreaIdType id, MemAcc_JobType job, MemAcc_AddressType addr, MemAcc_LengthType len, MemAcc_DataType *dst,
                            const MemAcc_DataType *src) {
    Area *a = area(id);
    if (!g_init || a == NULL_PTR || a->status == MEMACC_JOB_PENDING) return E_NOT_OK;
    if (addr < a->base || (uint64)addr + len > (uint64)a->base + a->size) return E_NOT_OK;
    area_load(a);
    a->job = job;
    a->addr = addr;
    a->len = len;
    a->dst = dst;
    a->src = src;
    a->mainCalls = 0u;
    a->status = MEMACC_JOB_PENDING;
    a->result = MEMACC_MEM_OK;
    return E_OK;
}

Std_ReturnType MemAcc_Read(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType SourceAddress, MemAcc_DataType *DestinationDataPtr,
                           MemAcc_LengthType Length) {
    if (DestinationDataPtr == NULL_PTR) return E_NOT_OK;
    return start(AddressAreaId, MEMACC_READ_JOB, SourceAddress, Length, DestinationDataPtr, NULL_PTR);
}
Std_ReturnType MemAcc_Write(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType TargetAddress, const MemAcc_DataType *SourceDataPtr,
                            MemAcc_LengthType Length) {
    if (SourceDataPtr == NULL_PTR) return E_NOT_OK;
    return start(AddressAreaId, MEMACC_WRITE_JOB, TargetAddress, Length, NULL_PTR, SourceDataPtr);
}
Std_ReturnType MemAcc_Erase(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType TargetAddress, MemAcc_LengthType Length) {
    return start(AddressAreaId, MEMACC_ERASE_JOB, TargetAddress, Length, NULL_PTR, NULL_PTR);
}
Std_ReturnType MemAcc_Compare(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType SourceAddress, const MemAcc_DataType *DataPtr,
                              MemAcc_LengthType Length) {
    if (DataPtr == NULL_PTR) return E_NOT_OK;
    return start(AddressAreaId, MEMACC_COMPARE_JOB, SourceAddress, Length, NULL_PTR, DataPtr);
}
Std_ReturnType MemAcc_BlankCheck(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType TargetAddress, MemAcc_LengthType Length) {
    return start(AddressAreaId, MEMACC_BLANKCHECK_JOB, TargetAddress, Length, NULL_PTR, NULL_PTR);
}
Std_ReturnType MemAcc_RequestLock(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType Address, MemAcc_LengthType Length,
                                  void (*LockNotificationFctPtr)(void)) {
    (void)AddressAreaId;
    (void)Address;
    (void)Length;
    if (LockNotificationFctPtr != NULL_PTR) LockNotificationFctPtr();
    return E_OK;
}
Std_ReturnType MemAcc_ReleaseLock(MemAcc_AddressAreaIdType AddressAreaId, MemAcc_AddressType Address, MemAcc_LengthType Length) {
    (void)AddressAreaId;
    (void)Address;
    (void)Length;
    return E_OK;
}

void MemAcc_MainFunction(void) {
    uint16 i;
    if (!g_init) return;
    for (i = 0; i < SilMcal_Cfg.numMemAccAreas && i < MEMACC_MAX_AREAS; ++i) {
        Area *a = &g_area[i];
        uint8 *p;
        MemAcc_JobResultType r = MEMACC_MEM_OK;
        if (a->status != MEMACC_JOB_PENDING) continue;
        if (++a->mainCalls < 2u) continue;
        p = a->image + (a->addr - a->base);
        switch (a->job) {
        case MEMACC_READ_JOB: memcpy(a->dst, p, a->len); break;
        case MEMACC_WRITE_JOB:
            memcpy(p, a->src, a->len);
            area_store(a);
            break;
        case MEMACC_ERASE_JOB:
            memset(p, 0xFF, a->len);
            area_store(a);
            break;
        case MEMACC_COMPARE_JOB: r = (memcmp(p, a->src, a->len) == 0) ? MEMACC_MEM_OK : MEMACC_MEM_INCONSISTENT; break;
        case MEMACC_BLANKCHECK_JOB: {
            MemAcc_LengthType k;
            for (k = 0; k < a->len; ++k)
                if (p[k] != 0xFFu) r = MEMACC_MEM_INCONSISTENT;
            break;
        }
        default: r = MEMACC_MEM_FAILED; break;
        }
        a->job = MEMACC_NO_JOB;
        a->result = r;
        a->status = MEMACC_JOB_IDLE;
        SilMcal_Notify(a->cfg->jobEndNotification);
    }
}

/* ---- Mem_43_INFLS / Mem_43_EXFLS (below MemAcc on the target; nothing to do here) ------------ */
void Mem_43_INFLS_Init(const Mem_43_INFLS_ConfigType *ConfigPtr) { (void)ConfigPtr; }
void Mem_43_INFLS_DeInit(void) {}
void Mem_43_INFLS_MainFunction(void) {}
void Mem_43_INFLS_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr) {
    if (VersionInfoPtr == NULL_PTR) return;
    VersionInfoPtr->vendorID = 43u;
    VersionInfoPtr->moduleID = 90u;
    VersionInfoPtr->sw_major_version = 1u;
    VersionInfoPtr->sw_minor_version = 0u;
    VersionInfoPtr->sw_patch_version = 0u;
}
void Mem_43_EXFLS_Init(const Mem_43_EXFLS_ConfigType *ConfigPtr) { (void)ConfigPtr; }
void Mem_43_EXFLS_DeInit(void) {}
void Mem_43_EXFLS_MainFunction(void) {}
void Mem_43_EXFLS_GetVersionInfo(Std_VersionInfoType *VersionInfoPtr) { Mem_43_INFLS_GetVersionInfo(VersionInfoPtr); }
