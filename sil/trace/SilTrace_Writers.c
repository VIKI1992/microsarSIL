/* SilTrace_Writers.c - .siltrace, Perfetto (Chrome JSON), BTF and Rimac .bin writers. */
#include "SilTrace.h"
#include "sil_port.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned SilTrace_NumCores(void);

/* ------------------------------------------------------------------------- */
/* Merge helper: collect all records of all cores sorted by (t_ns, host_ns). */
typedef struct {
    SilTrace_Record *recs;
    size_t n, cap;
} RecVec;

static void collect(const SilTrace_Record *r, void *user) {
    RecVec *v = (RecVec *)user;
    if (v->n == v->cap) {
        size_t ncap = v->cap ? v->cap * 2u : 4096u;
        SilTrace_Record *nr = (SilTrace_Record *)realloc(v->recs, ncap * sizeof(SilTrace_Record));
        if (nr == NULL) return;
        v->recs = nr;
        v->cap = ncap;
    }
    v->recs[v->n++] = *r;
}

static int cmp_rec(const void *a, const void *b) {
    const SilTrace_Record *x = (const SilTrace_Record *)a, *y = (const SilTrace_Record *)b;
    if (x->t_ns != y->t_ns) return x->t_ns < y->t_ns ? -1 : 1;
    if (x->host_ns != y->host_ns) return x->host_ns < y->host_ns ? -1 : 1;
    return 0;
}

static RecVec collect_all(void) {
    RecVec v = {NULL, 0, 0};
    unsigned c;
    for (c = 0; c < SilTrace_NumCores(); ++c) SilTrace_Visit(c, collect, &v);
    if (v.n > 1) qsort(v.recs, v.n, sizeof(SilTrace_Record), cmp_rec);
    return v;
}

static const char *name_or(SilTrace_NameKind kind, uint32_t id, char *buf, size_t len, const char *prefix) {
    const char *n = SilTrace_GetName(kind, id);
    if (n != NULL) return n;
    snprintf(buf, len, "%s%" PRIu32, prefix, id);
    return buf;
}

/* ------------------------------------------------------------------------- */
/* .siltrace: little endian binary. Header "SILTRACE1", u32 numCores, u32 numNames
 * (kind,id,len,name)*, u64 count, records. */
int SilTrace_WriteSiltrace(const char *path) {
    FILE *f = fopen(path, "wb");
    RecVec v;
    unsigned k;
    uint32_t id, numNames = 0;
    uint64_t cnt;
    if (f == NULL) return -1;
    fwrite("SILTRACE1", 1, 9, f);
    {
        uint32_t nc = SilTrace_NumCores();
        fwrite(&nc, 4, 1, f);
    }
    for (k = 0; k < SILTRACE_NAME_KINDS; ++k)
        for (id = 0; id < 4096u; ++id)
            if (SilTrace_GetName((SilTrace_NameKind)k, id) != NULL) numNames++;
    fwrite(&numNames, 4, 1, f);
    for (k = 0; k < SILTRACE_NAME_KINDS; ++k) {
        for (id = 0; id < 4096u; ++id) {
            const char *n = SilTrace_GetName((SilTrace_NameKind)k, id);
            if (n != NULL) {
                uint32_t kk = k, len = (uint32_t)strlen(n);
                fwrite(&kk, 4, 1, f);
                fwrite(&id, 4, 1, f);
                fwrite(&len, 4, 1, f);
                fwrite(n, 1, len, f);
            }
        }
    }
    v = collect_all();
    cnt = v.n;
    fwrite(&cnt, 8, 1, f);
    if (v.n) fwrite(v.recs, sizeof(SilTrace_Record), v.n, f);
    free(v.recs);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Perfetto / Chrome trace event JSON.
 * pid 1 = "ECU schedule (virtual time)", one tid per core for the running context lane and one
 * tid per core for runnables; pid 2 = "Host cost" with the same lanes using host time.
 * Instant events for activations, IRQ raise, alarms; flow events for activation -> start. */
typedef struct {
    FILE *f;
    int first;
} JsonOut;

static void json_sep(JsonOut *o) {
    if (!o->first) fputs(",\n", o->f);
    o->first = 0;
}

static void json_escape(FILE *f, const char *s) {
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
}

static void emit_meta(JsonOut *o, int pid, int tid, const char *what, const char *name) {
    json_sep(o);
    fprintf(o->f, "{\"ph\":\"M\",\"pid\":%d,\"tid\":%d,\"name\":\"%s\",\"args\":{\"name\":\"", pid, tid, what);
    json_escape(o->f, name);
    fputs("\"}}", o->f);
}

static void emit_slice(JsonOut *o, int pid, int tid, const char *cat, const char *name, double ts_us, double dur_us,
                       const char *argk, uint32_t argv_) {
    json_sep(o);
    fprintf(o->f, "{\"ph\":\"X\",\"pid\":%d,\"tid\":%d,\"cat\":\"%s\",\"name\":\"", pid, tid, cat);
    json_escape(o->f, name);
    fprintf(o->f, "\",\"ts\":%.3f,\"dur\":%.3f", ts_us, dur_us);
    if (argk != NULL) fprintf(o->f, ",\"args\":{\"%s\":%" PRIu32 "}", argk, argv_);
    fputs("}", o->f);
}

static void emit_instant(JsonOut *o, int pid, int tid, const char *cat, const char *name, double ts_us, const char *argk,
                         uint32_t argv_) {
    json_sep(o);
    fprintf(o->f, "{\"ph\":\"i\",\"s\":\"t\",\"pid\":%d,\"tid\":%d,\"cat\":\"%s\",\"name\":\"", pid, tid, cat);
    json_escape(o->f, name);
    fprintf(o->f, "\",\"ts\":%.3f", ts_us);
    if (argk != NULL) fprintf(o->f, ",\"args\":{\"%s\":%" PRIu32 "}", argk, argv_);
    fputs("}", o->f);
}

static void emit_flow(JsonOut *o, int pid, int tid, const char *ph, uint64_t flowid, double ts_us) {
    json_sep(o);
    fprintf(o->f, "{\"ph\":\"%s\",\"pid\":%d,\"tid\":%d,\"cat\":\"flow\",\"name\":\"act\",\"id\":%" PRIu64 ",\"ts\":%.3f%s}",
            ph, pid, tid, flowid, ts_us, (ph[0] == 'f') ? ",\"bp\":\"e\"" : "");
}

#define MAX_OBJ 4096u
#define MAX_CORES 8u

int SilTrace_WritePerfetto(const char *path) {
    FILE *f = fopen(path, "wb");
    JsonOut o;
    RecVec v;
    size_t i;
    unsigned c;
    char nb[64];
    /* open slice bookkeeping */
    uint64_t taskStart[MAX_CORES][MAX_OBJ];      /* virtual start of the current run segment per task */
    uint64_t taskHost[MAX_CORES][MAX_OBJ];
    uint64_t isrStart[MAX_CORES][MAX_OBJ], isrHost[MAX_CORES][MAX_OBJ];
    uint64_t runStart[MAX_CORES][MAX_OBJ], runHost[MAX_CORES][MAX_OBJ];
    uint64_t idleStart[MAX_CORES], idleHost[MAX_CORES];
    uint64_t actFlow[MAX_OBJ];
    uint64_t flowCounter = 1;
    unsigned numCores = SilTrace_NumCores();
    if (f == NULL) return -1;
    if (numCores > MAX_CORES) numCores = MAX_CORES;
    memset(taskStart, 0xFF, sizeof(taskStart));
    memset(isrStart, 0xFF, sizeof(isrStart));
    memset(runStart, 0xFF, sizeof(runStart));
    memset(idleStart, 0xFF, sizeof(idleStart));
    memset(actFlow, 0, sizeof(actFlow));
    memset(taskHost, 0, sizeof(taskHost));
    memset(isrHost, 0, sizeof(isrHost));
    memset(runHost, 0, sizeof(runHost));
    memset(idleHost, 0, sizeof(idleHost));
    o.f = f;
    o.first = 1;
    fputs("{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n", f);
    emit_meta(&o, 1, 0, "process_name", "ECU schedule (virtual time)");
    emit_meta(&o, 2, 0, "process_name", "Host cost (host time)");
    for (c = 0; c < numCores; ++c) {
        char lane[64];
        snprintf(lane, sizeof lane, "core%u: tasks/ISRs", c);
        emit_meta(&o, 1, (int)(c * 10 + 1), "thread_name", lane);
        emit_meta(&o, 2, (int)(c * 10 + 1), "thread_name", lane);
        snprintf(lane, sizeof lane, "core%u: runnables", c);
        emit_meta(&o, 1, (int)(c * 10 + 2), "thread_name", lane);
        emit_meta(&o, 2, (int)(c * 10 + 2), "thread_name", lane);
        snprintf(lane, sizeof lane, "core%u: events", c);
        emit_meta(&o, 1, (int)(c * 10 + 3), "thread_name", lane);
    }
    v = collect_all();
    for (i = 0; i < v.n; ++i) {
        const SilTrace_Record *r = &v.recs[i];
        unsigned core = r->core < MAX_CORES ? r->core : 0;
        int tidRun = (int)(core * 10 + 1), tidRnbl = (int)(core * 10 + 2), tidEv = (int)(core * 10 + 3);
        double ts = (double)r->t_ns / 1000.0, hts = (double)r->host_ns / 1000.0;
        uint32_t id = r->id < MAX_OBJ ? r->id : 0;
        switch ((SilTrace_EventType)r->type) {
        case SILTRACE_EV_TASK_ACTIVATE:
            actFlow[id] = flowCounter++;
            emit_instant(&o, 1, tidEv, "task", "activate", ts, "task", r->id);
            emit_flow(&o, 1, tidEv, "s", actFlow[id], ts);
            break;
        case SILTRACE_EV_TASK_START:
        case SILTRACE_EV_TASK_RESUME:
            taskStart[core][id] = r->t_ns;
            taskHost[core][id] = r->host_ns;
            if (r->type == SILTRACE_EV_TASK_START && actFlow[id] != 0) {
                emit_flow(&o, 1, tidRun, "f", actFlow[id], ts);
                actFlow[id] = 0;
            }
            break;
        case SILTRACE_EV_TASK_PREEMPT:
        case SILTRACE_EV_TASK_TERMINATE:
        case SILTRACE_EV_TASK_WAIT:
            if (taskStart[core][id] != UINT64_MAX) {
                const char *n = name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task");
                emit_slice(&o, 1, tidRun, "task", n, (double)taskStart[core][id] / 1000.0,
                           (double)(r->t_ns - taskStart[core][id]) / 1000.0, "task", r->id);
                emit_slice(&o, 2, tidRun, "task", n, (double)taskHost[core][id] / 1000.0,
                           (double)(r->host_ns - taskHost[core][id]) / 1000.0, "task", r->id);
                taskStart[core][id] = UINT64_MAX;
            }
            break;
        case SILTRACE_EV_ISR_START:
            isrStart[core][id] = r->t_ns;
            isrHost[core][id] = r->host_ns;
            break;
        case SILTRACE_EV_ISR_END:
            if (isrStart[core][id] != UINT64_MAX) {
                const char *n = name_or(SILTRACE_NAME_ISR, r->id, nb, sizeof nb, "isr");
                emit_slice(&o, 1, tidRun, "isr", n, (double)isrStart[core][id] / 1000.0,
                           (double)(r->t_ns - isrStart[core][id]) / 1000.0, "isr", r->id);
                emit_slice(&o, 2, tidRun, "isr", n, (double)isrHost[core][id] / 1000.0,
                           (double)(r->host_ns - isrHost[core][id]) / 1000.0, "isr", r->id);
                isrStart[core][id] = UINT64_MAX;
            }
            break;
        case SILTRACE_EV_RUNNABLE_START:
            runStart[core][id] = r->t_ns;
            runHost[core][id] = r->host_ns;
            break;
        case SILTRACE_EV_RUNNABLE_STOP:
            if (runStart[core][id] != UINT64_MAX) {
                const char *n = name_or(SILTRACE_NAME_RUNNABLE, r->id, nb, sizeof nb, "runnable");
                emit_slice(&o, 1, tidRnbl, "runnable", n, (double)runStart[core][id] / 1000.0,
                           (double)(r->t_ns - runStart[core][id]) / 1000.0, "id", r->id);
                emit_slice(&o, 2, tidRnbl, "runnable", n, (double)runHost[core][id] / 1000.0,
                           (double)(r->host_ns - runHost[core][id]) / 1000.0, "id", r->id);
                runStart[core][id] = UINT64_MAX;
            }
            break;
        case SILTRACE_EV_IDLE_START:
            idleStart[core] = r->t_ns;
            idleHost[core] = r->host_ns;
            break;
        case SILTRACE_EV_IDLE_END:
            if (idleStart[core] != UINT64_MAX) {
                emit_slice(&o, 1, tidRun, "idle", "idle", (double)idleStart[core] / 1000.0,
                           (double)(r->t_ns - idleStart[core]) / 1000.0, NULL, 0);
                emit_slice(&o, 2, tidRun, "idle", "idle", (double)idleHost[core] / 1000.0,
                           (double)(r->host_ns - idleHost[core]) / 1000.0, NULL, 0);
                idleStart[core] = UINT64_MAX;
            }
            break;
        case SILTRACE_EV_IRQ_RAISE:
            emit_instant(&o, 1, tidEv, "irq", name_or(SILTRACE_NAME_ISR, r->id, nb, sizeof nb, "irq"), ts, "isr", r->id);
            break;
        case SILTRACE_EV_ALARM_EXPIRE:
            emit_instant(&o, 1, tidEv, "alarm", name_or(SILTRACE_NAME_ALARM, r->id, nb, sizeof nb, "alarm"), ts, "tick", r->aux);
            break;
        case SILTRACE_EV_EVENT_SET:
            emit_instant(&o, 1, tidEv, "event", "SetEvent", ts, "task", r->id);
            break;
        case SILTRACE_EV_SPIN_GOT:
            if (r->aux > 0)
                emit_instant(&o, 1, tidEv, "spin", name_or(SILTRACE_NAME_SPINLOCK, r->id, nb, sizeof nb, "spinlock"), ts,
                             "spins", r->aux);
            break;
        case SILTRACE_EV_OS_ERROR:
            emit_instant(&o, 1, tidEv, "error", "OS error", ts, "status", r->id);
            break;
        case SILTRACE_EV_CLOCK_JUMP:
            emit_instant(&o, 1, tidEv, "clock", "clock jump", ts, "us", r->aux);
            break;
        case SILTRACE_EV_USER:
            emit_instant(&o, 1, tidEv, "user", name_or(SILTRACE_NAME_USER, r->id, nb, sizeof nb, "user"), ts, "aux", r->aux);
            break;
        case SILTRACE_EV_BUS_TX:
            emit_instant(&o, 1, tidEv, "bus", "CAN TX", ts, "canid", r->aux);
            break;
        case SILTRACE_EV_BUS_RX:
            emit_instant(&o, 1, tidEv, "bus", "CAN RX", ts, "canid", r->aux);
            break;
        default:
            break;
        }
        (void)hts;
    }
    free(v.recs);
    fputs("\n]}\n", f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* BTF 2.1.3: time,source,sourceInstance,targetType,target,targetInstance,event,note */
int SilTrace_WriteBtf(const char *path) {
    FILE *f = fopen(path, "wb");
    RecVec v;
    size_t i;
    char buf[64], nb[64], nb2[64];
    if (f == NULL) return -1;
    sil_timestamp_iso(buf, sizeof buf);
    fprintf(f, "#version 2.1.3\n#creator microsarSIL\n#creationDate %s\n#timeScale ns\n", buf);
    v = collect_all();
    for (i = 0; i < v.n; ++i) {
        const SilTrace_Record *r = &v.recs[i];
        const char *core;
        snprintf(nb2, sizeof nb2, "Core_%u", r->core);
        core = nb2;
        switch ((SilTrace_EventType)r->type) {
        case SILTRACE_EV_TASK_ACTIVATE:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,activate\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_TASK_START:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,start\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_TASK_RESUME:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,resume\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_TASK_PREEMPT:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,preempt\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_TASK_TERMINATE:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,terminate\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_TASK_WAIT:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,wait\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_TASK_RELEASE:
            fprintf(f, "%" PRIu64 ",%s,0,T,%s,0,release\n", r->t_ns, core, name_or(SILTRACE_NAME_TASK, r->id, nb, sizeof nb, "task"));
            break;
        case SILTRACE_EV_ISR_START:
            fprintf(f, "%" PRIu64 ",%s,0,I,%s,0,start\n", r->t_ns, core, name_or(SILTRACE_NAME_ISR, r->id, nb, sizeof nb, "isr"));
            break;
        case SILTRACE_EV_ISR_END:
            fprintf(f, "%" PRIu64 ",%s,0,I,%s,0,terminate\n", r->t_ns, core, name_or(SILTRACE_NAME_ISR, r->id, nb, sizeof nb, "isr"));
            break;
        case SILTRACE_EV_RUNNABLE_START:
            fprintf(f, "%" PRIu64 ",%s,0,R,%s,0,start\n", r->t_ns, core, name_or(SILTRACE_NAME_RUNNABLE, r->id, nb, sizeof nb, "runnable"));
            break;
        case SILTRACE_EV_RUNNABLE_STOP:
            fprintf(f, "%" PRIu64 ",%s,0,R,%s,0,terminate\n", r->t_ns, core, name_or(SILTRACE_NAME_RUNNABLE, r->id, nb, sizeof nb, "runnable"));
            break;
        case SILTRACE_EV_RES_GET:
            fprintf(f, "%" PRIu64 ",%s,0,SEM,%s,0,lock\n", r->t_ns, core, name_or(SILTRACE_NAME_RESOURCE, r->id, nb, sizeof nb, "res"));
            break;
        case SILTRACE_EV_RES_RELEASE:
            fprintf(f, "%" PRIu64 ",%s,0,SEM,%s,0,unlock\n", r->t_ns, core, name_or(SILTRACE_NAME_RESOURCE, r->id, nb, sizeof nb, "res"));
            break;
        case SILTRACE_EV_SPIN_GOT:
            fprintf(f, "%" PRIu64 ",%s,0,SEM,%s,0,lock\n", r->t_ns, core, name_or(SILTRACE_NAME_SPINLOCK, r->id, nb, sizeof nb, "spin"));
            break;
        case SILTRACE_EV_SPIN_REL:
            fprintf(f, "%" PRIu64 ",%s,0,SEM,%s,0,unlock\n", r->t_ns, core, name_or(SILTRACE_NAME_SPINLOCK, r->id, nb, sizeof nb, "spin"));
            break;
        case SILTRACE_EV_ALARM_EXPIRE:
            fprintf(f, "%" PRIu64 ",%s,0,STI,%s,0,trigger\n", r->t_ns, core, name_or(SILTRACE_NAME_ALARM, r->id, nb, sizeof nb, "alarm"));
            break;
        default:
            break;
        }
    }
    free(v.recs);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Rimac profiling format. Runnable start/stop ids come from the string table ids:
 * runnable id n -> start marker 2n, stop marker 2n+1 (same convention as id_runnables.xml).
 * ISR ids are offset by 2048 so that ISR n -> start 2048+2n, stop 2048+2n+1. */
int SilTrace_WriteRimacBin(const char *dir, uint64_t tick_hz) {
    unsigned c, numCores = SilTrace_NumCores();
    char path[1024];
    uint32_t id;
    FILE *fx;
    for (c = 0; c < numCores; ++c) {
        RecVec v = {NULL, 0, 0};
        size_t i;
        FILE *f;
        snprintf(path, sizeof path, "%s/core%u.bin", dir, c);
        f = fopen(path, "wb");
        if (f == NULL) return -1;
        SilTrace_Visit(c, collect, &v);
        for (i = 0; i < v.n; ++i) {
            const SilTrace_Record *r = &v.recs[i];
            uint64_t ticks = (r->t_ns / 1000u) * tick_hz / 1000000u; /* ns -> ticks */
            uint64_t marker;
            uint64_t rec;
            if (r->type == SILTRACE_EV_RUNNABLE_START) marker = 2u * (uint64_t)r->id;
            else if (r->type == SILTRACE_EV_RUNNABLE_STOP) marker = 2u * (uint64_t)r->id + 1u;
            else if (r->type == SILTRACE_EV_ISR_START) marker = 2048u + 2u * (uint64_t)r->id;
            else if (r->type == SILTRACE_EV_ISR_END) marker = 2048u + 2u * (uint64_t)r->id + 1u;
            else continue;
            if (marker > 4095u) continue;
            rec = ((ticks & ((1ull << 52) - 1u)) << 12) | (marker & 0xFFFu);
            fwrite(&rec, 8, 1, f);
        }
        free(v.recs);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/id_runnables.xml", dir);
    fx = fopen(path, "wb");
    if (fx == NULL) return -1;
    fputs("<?xml version=\"1.0\" ?>\n<Runnables>\n", fx);
    for (id = 0; id < 1024u; ++id) {
        const char *n = SilTrace_GetName(SILTRACE_NAME_RUNNABLE, id);
        if (n != NULL)
            fprintf(fx, "   <Runnable name=\"%s\">\n      <Start id=\"%" PRIu32 "\"/>\n      <Stop id=\"%" PRIu32 "\"/>\n   </Runnable>\n", n,
                    2u * id, 2u * id + 1u);
    }
    fputs("</Runnables>\n", fx);
    fclose(fx);
    snprintf(path, sizeof path, "%s/id_isr.xml", dir);
    fx = fopen(path, "wb");
    if (fx == NULL) return -1;
    fputs("<?xml version=\"1.0\" ?>\n<Runnables>\n", fx);
    for (id = 0; id < 1024u; ++id) {
        const char *n = SilTrace_GetName(SILTRACE_NAME_ISR, id);
        if (n != NULL)
            fprintf(fx, "   <Runnable name=\"%s\">\n      <Start id=\"%" PRIu32 "\"/>\n      <Stop id=\"%" PRIu32 "\"/>\n   </Runnable>\n", n,
                    2048u + 2u * id, 2048u + 2u * id + 1u);
    }
    fputs("</Runnables>\n", fx);
    fclose(fx);
    return 0;
}
