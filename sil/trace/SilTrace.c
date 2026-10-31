/* SilTrace.c - per-core ring buffers and name table. */
#include "SilTrace.h"
#include "sil_port.h"

#include <stdlib.h>
#include <string.h>

#define SILTRACE_MAX_CORES 8u
#define SILTRACE_NAMES_PER_KIND 4096u

typedef struct {
    SilTrace_Record *buf;
    size_t cap;
    volatile uint64_t head;    /* number of records ever written */
    volatile uint64_t dropped;
    sil_mutex_t *lock;
} SilTrace_Ring;

static SilTrace_Ring g_rings[SILTRACE_MAX_CORES];
static unsigned g_numCores;
static volatile uint32_t g_enabled;
static SilTrace_ClockFn g_clock;
static SilTrace_CtxFn g_ctx;
static char **g_names[SILTRACE_NAME_KINDS];
static sil_mutex_t *g_nameLock;

static const char *const g_eventNames[SILTRACE_EV_COUNT] = {
    "none", "task_activate", "task_start", "task_preempt", "task_resume", "task_terminate", "task_wait",
    "task_release", "task_act_fail", "irq_raise", "isr_start", "isr_end", "alarm_set", "alarm_cancel",
    "alarm_expire", "event_set", "res_get", "res_release", "spin_req", "spin_got", "spin_rel", "xsig_send",
    "xsig_recv", "barrier_wait", "barrier_pass", "idle_start", "idle_end", "runnable_start", "runnable_stop",
    "int_lock", "int_unlock", "clock_jump", "os_error", "hook", "user", "bus_tx", "bus_rx"};

void SilTrace_Init(unsigned numCores, size_t recordsPerCore) {
    unsigned i;
    if (g_numCores != 0u) return;
    if (numCores > SILTRACE_MAX_CORES) numCores = SILTRACE_MAX_CORES;
    if (recordsPerCore < 1024u) recordsPerCore = 1024u;
    for (i = 0; i < numCores; ++i) {
        g_rings[i].buf = (SilTrace_Record *)calloc(recordsPerCore, sizeof(SilTrace_Record));
        if (g_rings[i].buf == NULL) sil_fatal("SilTrace: out of memory");
        g_rings[i].cap = recordsPerCore;
        g_rings[i].head = 0;
        g_rings[i].dropped = 0;
        g_rings[i].lock = sil_mutex_create();
    }
    g_nameLock = sil_mutex_create();
    g_numCores = numCores;
    g_enabled = 1u;
}

void SilTrace_Shutdown(void) {
    unsigned i, k;
    uint32_t n;
    for (i = 0; i < g_numCores; ++i) {
        free(g_rings[i].buf);
        g_rings[i].buf = NULL;
        sil_mutex_destroy(g_rings[i].lock);
    }
    for (k = 0; k < SILTRACE_NAME_KINDS; ++k) {
        if (g_names[k] != NULL) {
            for (n = 0; n < SILTRACE_NAMES_PER_KIND; ++n) free(g_names[k][n]);
            free(g_names[k]);
            g_names[k] = NULL;
        }
    }
    if (g_nameLock != NULL) sil_mutex_destroy(g_nameLock);
    g_nameLock = NULL;
    g_numCores = 0;
}

int SilTrace_IsEnabled(void) { return (g_numCores != 0u) && (sil_atomic_load32(&g_enabled) != 0u); }
void SilTrace_Enable(int on) { sil_atomic_store32(&g_enabled, on ? 1u : 0u); }
void SilTrace_SetClock(SilTrace_ClockFn fn) { g_clock = fn; }
void SilTrace_SetContextProvider(SilTrace_CtxFn fn) { g_ctx = fn; }

void SilTrace_EventAt(unsigned core, uint64_t t_ns, SilTrace_EventType type, uint32_t id, uint32_t aux) {
    SilTrace_Ring *r;
    SilTrace_Record rec;
    uint64_t idx;
    if (!SilTrace_IsEnabled()) return;
    if (core >= g_numCores) core = g_numCores - 1u;
    r = &g_rings[core];
    rec.t_ns = t_ns;
    rec.host_ns = sil_clock_ns();
    rec.id = id;
    rec.aux = aux;
    rec.type = (uint16_t)type;
    rec.core = (uint8_t)core;
    rec.ctx_kind = 3u;
    rec.ctx_id = 0xFFFFFFFFu;
    if (g_ctx != NULL) g_ctx(&rec.ctx_kind, &rec.ctx_id);
    sil_mutex_lock(r->lock);
    idx = r->head;
    if (idx >= r->cap) r->dropped++;
    r->buf[idx % r->cap] = rec;
    r->head = idx + 1u;
    sil_mutex_unlock(r->lock);
}

void SilTrace_Event(unsigned core, SilTrace_EventType type, uint32_t id, uint32_t aux) {
    uint64_t t = (g_clock != NULL) ? g_clock() : sil_clock_ns();
    SilTrace_EventAt(core, t, type, id, aux);
}

void SilTrace_SetName(SilTrace_NameKind kind, uint32_t id, const char *name) {
    if (kind >= SILTRACE_NAME_KINDS || id >= SILTRACE_NAMES_PER_KIND || name == NULL) return;
    if (g_nameLock == NULL) g_nameLock = sil_mutex_create();
    sil_mutex_lock(g_nameLock);
    if (g_names[kind] == NULL) g_names[kind] = (char **)calloc(SILTRACE_NAMES_PER_KIND, sizeof(char *));
    if (g_names[kind] != NULL) {
        size_t len = strlen(name) + 1u;
        free(g_names[kind][id]);
        g_names[kind][id] = (char *)malloc(len);
        if (g_names[kind][id] != NULL) memcpy(g_names[kind][id], name, len);
    }
    sil_mutex_unlock(g_nameLock);
}

const char *SilTrace_GetName(SilTrace_NameKind kind, uint32_t id) {
    if (kind >= SILTRACE_NAME_KINDS || id >= SILTRACE_NAMES_PER_KIND) return NULL;
    if (g_names[kind] == NULL) return NULL;
    return g_names[kind][id];
}

const char *SilTrace_EventName(SilTrace_EventType type) {
    if ((unsigned)type >= SILTRACE_EV_COUNT) return "?";
    return g_eventNames[type];
}

uint64_t SilTrace_Count(unsigned core) {
    if (core >= g_numCores) return 0;
    return g_rings[core].head;
}

uint64_t SilTrace_Dropped(unsigned core) {
    if (core >= g_numCores) return 0;
    return g_rings[core].dropped;
}

void SilTrace_Visit(unsigned core, SilTrace_VisitFn fn, void *user) {
    SilTrace_Ring *r;
    uint64_t head, start, i;
    if (core >= g_numCores) return;
    r = &g_rings[core];
    sil_mutex_lock(r->lock);
    head = r->head;
    start = (head > r->cap) ? head - r->cap : 0u;
    for (i = start; i < head; ++i) fn(&r->buf[i % r->cap], user);
    sil_mutex_unlock(r->lock);
}

unsigned SilTrace_NumCores(void);
unsigned SilTrace_NumCores(void) { return g_numCores; }
