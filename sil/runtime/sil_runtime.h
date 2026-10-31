/*
 * sil_runtime.h - command line driven host runtime around SilOs.
 *
 *   int main(int argc, char **argv) { return SilRt_Main(argc, argv, my_core_entry); }
 *
 * Options:
 *   --time=virtual            event driven virtual time (default)
 *   --time=realtime[:factor]  wall clock, optionally scaled
 *   --duration=<seconds>      stop after this much virtual time (float, 0 = run until ShutdownOS)
 *   --trace=<file.siltrace>   binary trace
 *   --perfetto=<file.json>    Chrome/Perfetto trace event JSON
 *   --btf=<file.btf>          BTF trace
 *   --rimac=<dir>             Rimac profiling .bin + id xml files
 *   --trace-records=<n>       ring size per core (default 262144)
 *   --pin-cores               pin core threads to host CPUs
 *   --quiet                   no banner
 */
#ifndef SIL_RUNTIME_H
#define SIL_RUNTIME_H

#include "SilOs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SilOs_RuntimeOptions os;
    const char *tracePath;
    const char *perfettoPath;
    const char *btfPath;
    const char *rimacDir;
    int quiet;
    int argc;
    char **argv;
} SilRt_Options;

/* Parse argv into opt (defaults first). Returns 0 on success, 1 if --help was requested, -1 on error. */
int SilRt_ParseArgs(SilRt_Options *opt, int argc, char **argv);
void SilRt_Usage(const char *prog);
/* Full runtime: parse, set up, run the master core, wait for shutdown, write traces. */
int SilRt_Main(int argc, char **argv, SilOs_CoreEntryFn entry);
/* Same with pre-parsed options (lets the application override defaults such as duration). */
int SilRt_Run(const SilRt_Options *opt, SilOs_CoreEntryFn entry);
const SilRt_Options *SilRt_GetOptions(void);
/* Write the configured trace outputs now (called automatically at shutdown). */
void SilRt_WriteTraces(void);

#ifdef __cplusplus
}
#endif

#endif /* SIL_RUNTIME_H */
