/* SilMcal_Common.c - helpers shared by the virtual MCAL modules: notification context, traffic capture,
 * register files of slave models, reset hook, lifetimer frequency, NV file directory. */
#include "SilMcal.h"
#include "SilOs.h"
#include "sil_port.h"

#include <stdio.h>
#include <string.h>

/* ---- notifications in OS context ------------------------------------------ */
#define NOTIFY_RING 256u
static SilMcal_NotifyFn g_notifyRing[NOTIFY_RING];
static volatile uint32 g_notifyNext;

static void notify_tramp(void *arg) {
    SilMcal_NotifyFn fn = *(SilMcal_NotifyFn *)arg;
    if (fn != NULL_PTR) fn();
}

void SilMcal_RunInIsr(void (*fn)(void *), void *arg) {
    if (fn == NULL_PTR) return;
    if (SilOs_InOsContext()) {
        fn(arg);
        return;
    }
    if (!SilOs_RunInIsr(SilOs_Cfg.masterCore, fn, arg)) {
        static uint32 warned;
        if (!warned) {
            warned = 1u;
            sil_log("SilMcal: no soft ISR configured on core %u, running notifications on the model thread", (unsigned)SilOs_Cfg.masterCore);
        }
        fn(arg);
    }
}

void SilMcal_Notify(SilMcal_NotifyFn fn) {
    SilMcal_NotifyFn *slot;
    if (fn == NULL_PTR) return;
    if (SilOs_InOsContext()) {
        fn();
        return;
    }
    slot = &g_notifyRing[sil_atomic_add32(&g_notifyNext, 1u) % NOTIFY_RING];
    *slot = fn;
    SilMcal_RunInIsr(notify_tramp, slot);
}

int SilMcal_RaiseIsr(const char *handler) {
    ISRType id;
    if (handler == NULL_PTR || handler[0] == '\0') return 0;
    id = SilOs_IsrByHandler(handler);
    if (id >= SilOs_NumIsrs()) return 0;
    SilOs_RaiseIrq(id);
    return 1;
}

void SilMcal_UnhandledIsr(const char *handler) {
    static uint32 count;
    if (count < 8u) sil_log("SilMcal: interrupt %s raised but no virtual peripheral serves it", handler);
    count++;
}

/* ---- traffic capture -------------------------------------------------------- */
#define CAP_SIZE 2048u
static SilMcal_Event g_cap[CAP_SIZE];
static uint64 g_capWritten, g_capRead;
static sil_mutex_t *g_capLock;

static sil_mutex_t *cap_lock(void) {
    static volatile uint32 state;
    if (sil_atomic_load32(&state) == 2u) return g_capLock;
    if (sil_atomic_cas32(&state, 0u, 1u)) {
        g_capLock = sil_mutex_create();
        sil_atomic_store32(&state, 2u);
    } else {
        while (sil_atomic_load32(&state) != 2u) sil_thread_yield();
    }
    return g_capLock;
}

void SilMcal_Capture(const SilMcal_Event *ev) {
    sil_mutex_t *l = cap_lock();
    sil_mutex_lock(l);
    g_cap[g_capWritten % CAP_SIZE] = *ev;
    g_cap[g_capWritten % CAP_SIZE].t_ns = SilOs_ClockNow();
    g_capWritten++;
    sil_mutex_unlock(l);
}

uint32 SilMcal_TakeEvents(SilMcal_Event *out, uint32 max, uint64 *dropped) {
    sil_mutex_t *l = cap_lock();
    uint32 n = 0;
    sil_mutex_lock(l);
    if (dropped != NULL_PTR) *dropped = 0u;
    if (g_capWritten > g_capRead + CAP_SIZE) {
        if (dropped != NULL_PTR) *dropped = g_capWritten - CAP_SIZE - g_capRead;
        g_capRead = g_capWritten - CAP_SIZE;
    }
    while (g_capRead < g_capWritten && n < max) {
        out[n++] = g_cap[g_capRead % CAP_SIZE];
        g_capRead++;
    }
    sil_mutex_unlock(l);
    return n;
}

/* ---- register files --------------------------------------------------------- */
#define REG_DEVICES 64u
typedef struct {
    uint8 used;
    uint8 bus;
    uint16 device;
    uint8 regs[256];
} RegFile;
static RegFile g_regs[REG_DEVICES];

static RegFile *regfile(uint8 bus, uint16 device, int create) {
    uint32 i;
    for (i = 0; i < REG_DEVICES; ++i)
        if (g_regs[i].used && g_regs[i].bus == bus && g_regs[i].device == device) return &g_regs[i];
    if (!create) return NULL_PTR;
    for (i = 0; i < REG_DEVICES; ++i) {
        if (!g_regs[i].used) {
            memset(&g_regs[i], 0, sizeof(g_regs[i]));
            g_regs[i].used = 1u;
            g_regs[i].bus = bus;
            g_regs[i].device = device;
            return &g_regs[i];
        }
    }
    return NULL_PTR;
}

void SilMcal_RegWrite(uint8 bus, uint16 device, uint8 reg, const uint8 *data, uint32 len) {
    sil_mutex_t *l = cap_lock();
    RegFile *rf;
    uint32 i;
    sil_mutex_lock(l);
    rf = regfile(bus, device, 1);
    if (rf != NULL_PTR)
        for (i = 0; i < len; ++i) rf->regs[(uint8)(reg + i)] = data[i];
    sil_mutex_unlock(l);
}

void SilMcal_RegRead(uint8 bus, uint16 device, uint8 reg, uint8 *data, uint32 len) {
    sil_mutex_t *l = cap_lock();
    RegFile *rf;
    uint32 i;
    sil_mutex_lock(l);
    rf = regfile(bus, device, 0);
    for (i = 0; i < len; ++i) data[i] = (rf != NULL_PTR) ? rf->regs[(uint8)(reg + i)] : 0u;
    sil_mutex_unlock(l);
}

/* ---- reset, lifetimer, NV directory ------------------------------------------ */
static void (*g_resetHook)(void);
static uint32 g_resetReason;
static uint64 g_lifetimerHz = 100000000u;
static char g_nvDir[512] = ".";

void SilMcu_SetResetHook(void (*fn)(void)) { g_resetHook = fn; }
void SilMcu_SetResetReason(uint32 reason) { g_resetReason = reason; }
void SilMcal_SetLifetimerHz(uint64 hz) { g_lifetimerHz = hz ? hz : 100000000u; }
uint64 SilMcal_LifetimerHz(void) { return g_lifetimerHz; }
void SilNv_SetDir(const char *dir) {
    if (dir == NULL_PTR || dir[0] == '\0') return;
    strncpy(g_nvDir, dir, sizeof(g_nvDir) - 1u);
    g_nvDir[sizeof(g_nvDir) - 1u] = '\0';
}
const char *SilNv_Dir(void) { return g_nvDir; }

/* used by the Mcu stub */
uint32 SilMcu_ResetReason(void);
uint32 SilMcu_ResetReason(void) { return g_resetReason; }
void SilMcu_PerformReset(void);
void SilMcu_PerformReset(void) {
    sil_log("Mcu_PerformReset at t=%.6f s", (double)SilOs_ClockNow() / 1e9);
    if (g_resetHook != NULL_PTR) g_resetHook();
    else SilOs_RequestStop(0x52);
}
