/* SilIo.c - virtual I/O registry: a small hash table of (kind, id) entries with numeric fields. */
#include "SilIo.h"
#include "sil_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SILIO_CAP 8192u
#define SILIO_MAX_SUBS 8u

typedef struct {
    uint32 key;
    uint8 used;
    uint8 set[SILIO_FIELDS];
    double fields[SILIO_FIELDS];
} Entry;

typedef struct {
    SilIo_ChangeFn fn;
    void *user;
} Sub;

static Entry g_tab[SILIO_CAP];
static Sub g_subs[SILIO_KINDS][SILIO_MAX_SUBS];
static uint8 g_numSubs[SILIO_KINDS];
static sil_mutex_t *g_lock;
static volatile uint32 g_init;

static void ensure_init(void) {
    if (sil_atomic_load32(&g_init)) return;
    {
        static volatile uint32 claiming;
        if (sil_atomic_cas32(&claiming, 0u, 1u)) {
            g_lock = sil_mutex_create();
            sil_atomic_store32(&g_init, 1u);
        } else {
            while (!sil_atomic_load32(&g_init)) sil_thread_yield();
        }
    }
}

static uint32 make_key(SilIo_Kind kind, uint32 id) { return ((uint32)kind << 24) | (id & 0xFFFFFFu) | 0x80000000u; }

static Entry *find(uint32 key, int create) {
    uint32 h = (key * 2654435761u) % SILIO_CAP;
    uint32 n;
    for (n = 0; n < SILIO_CAP; ++n) {
        Entry *e = &g_tab[(h + n) % SILIO_CAP];
        if (!e->used) {
            if (!create) return NULL_PTR;
            memset(e, 0, sizeof(*e));
            e->used = 1u;
            e->key = key;
            return e;
        }
        if (e->key == key) return e;
    }
    return NULL_PTR;
}

static double default_value(SilIo_Kind kind, uint8 field) {
    if (field == SILIO_F_DUTY) return 0.5;
    if (field == SILIO_F_TICKHZ) return 1000000.0;
    (void)kind;
    return 0.0;
}

double SilIo_Get(SilIo_Kind kind, uint32 id, uint8 field) {
    Entry *e;
    double v;
    if (kind >= SILIO_KINDS || field >= SILIO_FIELDS) return 0.0;
    ensure_init();
    sil_mutex_lock(g_lock);
    e = find(make_key(kind, id), 0);
    v = (e != NULL_PTR && e->set[field]) ? e->fields[field] : default_value(kind, field);
    sil_mutex_unlock(g_lock);
    return v;
}

int SilIo_IsSet(SilIo_Kind kind, uint32 id, uint8 field) {
    Entry *e;
    int r;
    if (kind >= SILIO_KINDS || field >= SILIO_FIELDS) return 0;
    ensure_init();
    sil_mutex_lock(g_lock);
    e = find(make_key(kind, id), 0);
    r = (e != NULL_PTR && e->set[field]) ? 1 : 0;
    sil_mutex_unlock(g_lock);
    return r;
}

void SilIo_Set(SilIo_Kind kind, uint32 id, uint8 field, double value) {
    Entry *e;
    double old;
    Sub subs[SILIO_MAX_SUBS];
    uint8 n, i;
    if (kind >= SILIO_KINDS || field >= SILIO_FIELDS) return;
    ensure_init();
    sil_mutex_lock(g_lock);
    e = find(make_key(kind, id), 1);
    if (e == NULL_PTR) {
        sil_mutex_unlock(g_lock);
        sil_log("SilIo: table full");
        return;
    }
    old = e->set[field] ? e->fields[field] : default_value(kind, field);
    e->fields[field] = value;
    e->set[field] = 1u;
    n = g_numSubs[kind];
    for (i = 0; i < n; ++i) subs[i] = g_subs[kind][i];
    sil_mutex_unlock(g_lock);
    for (i = 0; i < n; ++i) subs[i].fn(subs[i].user, kind, id, field, old, value);
}

void SilIo_Subscribe(SilIo_Kind kind, SilIo_ChangeFn fn, void *user) {
    if (kind >= SILIO_KINDS || fn == NULL_PTR) return;
    ensure_init();
    sil_mutex_lock(g_lock);
    if (g_numSubs[kind] < SILIO_MAX_SUBS) {
        g_subs[kind][g_numSubs[kind]].fn = fn;
        g_subs[kind][g_numSubs[kind]].user = user;
        g_numSubs[kind]++;
    }
    sil_mutex_unlock(g_lock);
}

static const char *const kindNames[SILIO_KINDS] = {"dio", "port", "adc", "pwm", "icu"};
static const char *const fieldNames[SILIO_FIELDS] = {"value", "period", "freq", "duty", "tickhz", "idle"};

const char *SilIo_KindName(SilIo_Kind kind) { return (kind < SILIO_KINDS) ? kindNames[kind] : "?"; }
const char *SilIo_FieldName(uint8 field) { return (field < SILIO_FIELDS) ? fieldNames[field] : "?"; }

const char *SilIo_Symbol(SilIo_Kind kind, uint32 id) {
    uint16 i;
    for (i = 0; i < SilIo_NumNames; ++i)
        if (SilIo_Names[i].kind == (uint8)kind && SilIo_Names[i].id == id) return SilIo_Names[i].symbol;
    return NULL_PTR;
}

static int parse_field(const char *s, uint8 *field) {
    uint8 f;
    if (s == NULL_PTR || *s == '\0') {
        *field = SILIO_F_VALUE;
        return 0;
    }
    if (*s == '.') ++s;
    if (strcmp(s, "level") == 0) {
        *field = SILIO_F_VALUE;
        return 0;
    }
    for (f = 0; f < SILIO_FIELDS; ++f) {
        if (strcmp(s, fieldNames[f]) == 0) {
            *field = f;
            return 0;
        }
    }
    return -1;
}

int SilIo_Parse(const char *key, SilIo_Kind *kind, uint32 *id, uint8 *field) {
    char buf[160];
    char *dot1, *dot2;
    uint8 k;
    uint16 i;
    if (key == NULL_PTR || strlen(key) >= sizeof buf) return -1;
    strcpy(buf, key);
    dot1 = strchr(buf, '.');
    /* symbolic name first: "DioConf_DioChannel_X[.field]" */
    if (dot1 != NULL_PTR) *dot1 = '\0';
    for (i = 0; i < SilIo_NumNames; ++i) {
        if (strcmp(SilIo_Names[i].symbol, buf) == 0) {
            *kind = (SilIo_Kind)SilIo_Names[i].kind;
            *id = SilIo_Names[i].id;
            return parse_field(dot1 != NULL_PTR ? dot1 + 1 : NULL_PTR, field);
        }
    }
    if (dot1 == NULL_PTR) return -1;
    for (k = 0; k < SILIO_KINDS; ++k)
        if (strcmp(buf, kindNames[k]) == 0) break;
    if (k >= SILIO_KINDS) return -1;
    *kind = (SilIo_Kind)k;
    dot2 = strchr(dot1 + 1, '.');
    if (dot2 != NULL_PTR) *dot2 = '\0';
    *id = (uint32)strtoul(dot1 + 1, NULL_PTR, 0);
    return parse_field(dot2 != NULL_PTR ? dot2 + 1 : NULL_PTR, field);
}

void SilIo_Visit(SilIo_VisitFn fn, void *user) {
    uint32 n;
    uint8 f;
    Entry snapshot[256];
    uint32 count = 0, start = 0;
    if (fn == NULL_PTR) return;
    ensure_init();
    /* copy in chunks so that the callback runs without the lock */
    for (;;) {
        sil_mutex_lock(g_lock);
        count = 0;
        for (n = start; n < SILIO_CAP && count < 256u; ++n) {
            if (g_tab[n].used) snapshot[count++] = g_tab[n];
        }
        start = n;
        sil_mutex_unlock(g_lock);
        for (n = 0; n < count; ++n)
            for (f = 0; f < SILIO_FIELDS; ++f)
                if (snapshot[n].set[f])
                    fn(user, (SilIo_Kind)((snapshot[n].key >> 24) & 0x7Fu), snapshot[n].key & 0xFFFFFFu, f, snapshot[n].fields[f]);
        if (start >= SILIO_CAP) break;
    }
}
