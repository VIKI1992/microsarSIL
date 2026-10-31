/* sil_runtime.c - command line runtime around SilOs. */
#include "sil_runtime.h"
#include "SilTrace.h"
#include "sil_port.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SilRt_Options g_opt;
static volatile uint32_t g_tracesWritten;

void SilRt_Usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --time=virtual | --time=realtime[:factor]\n"
            "  --duration=<seconds>   stop after this virtual time (0 = until ShutdownOS)\n"
            "  --trace=<file.siltrace> --perfetto=<file.json> --btf=<file.btf> --rimac=<dir>\n"
            "  --trace-records=<n> --pin-cores --quiet --help\n",
            prog);
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

int SilRt_ParseArgs(SilRt_Options *opt, int argc, char **argv) {
    int i;
    memset(opt, 0, sizeof(*opt));
    SilOs_DefaultOptions(&opt->os);
    opt->argc = argc;
    opt->argv = argv;
    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            SilRt_Usage(argv[0]);
            return 1;
        } else if (starts(a, "--time=")) {
            const char *v = a + 7;
            if (strcmp(v, "virtual") == 0) {
                opt->os.timeMode = SILOS_TIME_VIRTUAL;
            } else if (starts(v, "realtime")) {
                opt->os.timeMode = SILOS_TIME_REALTIME;
                if (v[8] == ':') opt->os.realtimeFactor = atof(v + 9);
                if (opt->os.realtimeFactor <= 0.0) opt->os.realtimeFactor = 1.0;
            } else {
                fprintf(stderr, "unknown time mode %s\n", v);
                return -1;
            }
        } else if (starts(a, "--duration=")) {
            opt->os.durationNs = (uint64)(atof(a + 11) * 1e9);
        } else if (starts(a, "--trace=")) {
            opt->tracePath = a + 8;
        } else if (starts(a, "--perfetto=")) {
            opt->perfettoPath = a + 11;
        } else if (starts(a, "--btf=")) {
            opt->btfPath = a + 6;
        } else if (starts(a, "--rimac=")) {
            opt->rimacDir = a + 8;
        } else if (starts(a, "--trace-records=")) {
            opt->os.traceRecordsPerCore = (size_t)atol(a + 16);
        } else if (strcmp(a, "--pin-cores") == 0) {
            opt->os.pinCores = 1u;
        } else if (strcmp(a, "--quiet") == 0) {
            opt->quiet = 1;
        } else if (starts(a, "--seed=")) {
            opt->os.seed = (uint32)atol(a + 7);
        } else if (strcmp(a, "--resume") == 0) {
            /* reserved for reset emulation */
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            SilRt_Usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

const SilRt_Options *SilRt_GetOptions(void) { return &g_opt; }

void SilRt_WriteTraces(void) {
    unsigned c;
    if (!sil_atomic_cas32(&g_tracesWritten, 0u, 1u)) return;
    if (g_opt.tracePath != NULL && SilTrace_WriteSiltrace(g_opt.tracePath) != 0) sil_log("cannot write %s", g_opt.tracePath);
    if (g_opt.perfettoPath != NULL && SilTrace_WritePerfetto(g_opt.perfettoPath) != 0) sil_log("cannot write %s", g_opt.perfettoPath);
    if (g_opt.btfPath != NULL && SilTrace_WriteBtf(g_opt.btfPath) != 0) sil_log("cannot write %s", g_opt.btfPath);
    if (g_opt.rimacDir != NULL && SilTrace_WriteRimacBin(g_opt.rimacDir, 100000000u) != 0) sil_log("cannot write %s", g_opt.rimacDir);
    if (!g_opt.quiet) {
        for (c = 0; c < SilOs_Cfg.numCores; ++c)
            sil_log("core%u: %llu trace records (%llu dropped)", c, (unsigned long long)SilTrace_Count(c),
                    (unsigned long long)SilTrace_Dropped(c));
    }
}

static void on_shutdown(StatusType error, int allCores) {
    (void)allCores;
    if (!g_opt.quiet) sil_log("ShutdownOS(%u) at t=%.6f s", (unsigned)error, (double)SilOs_ClockNow() / 1e9);
}

static void on_signal(int sig) {
    (void)sig;
    SilOs_RequestStop(130);
}

int SilRt_Run(const SilRt_Options *opt, SilOs_CoreEntryFn entry) {
    int code;
    g_opt = *opt;
    if (!g_opt.quiet) {
        sil_log("microsarSIL runtime: %u core(s), time=%s%s, duration=%.3f s", (unsigned)SilOs_Cfg.numCores,
                g_opt.os.timeMode == SILOS_TIME_REALTIME ? "realtime" : "virtual",
                g_opt.os.timeMode == SILOS_TIME_REALTIME ? "" : " (event driven)", (double)g_opt.os.durationNs / 1e9);
    }
    SilOs_Setup(&g_opt.os, entry);
    SilOs_SetShutdownCallback(on_shutdown);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    SilOs_StartMaster();
    code = SilOs_WaitShutdown();
    SilRt_WriteTraces();
    if (!g_opt.quiet) sil_log("exit %d at t=%.6f s, %llu OS errors", code, (double)SilOs_ClockNow() / 1e9, (unsigned long long)SilOs_ErrorCount());
    fflush(stdout);
    fflush(stderr);
    return code;
}

int SilRt_Main(int argc, char **argv, SilOs_CoreEntryFn entry) {
    SilRt_Options opt;
    int r = SilRt_ParseArgs(&opt, argc, argv);
    if (r != 0) return r < 0 ? 2 : 0;
    r = SilRt_Run(&opt, entry);
    sil_process_exit(r);
}
