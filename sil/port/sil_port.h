/*
 * sil_port.h - host abstraction used by every SIL component written in C.
 *
 * The implementation (sil/port/impl/port.cpp) is C++17 on top of std::thread,
 * std::mutex, std::condition_variable and std::chrono so that Windows and
 * Linux behave identically. Nothing here is AUTOSAR specific.
 */
#ifndef SIL_PORT_H
#define SIL_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define SIL_THREAD_LOCAL __declspec(thread)
#define SIL_NORETURN __declspec(noreturn)
#else
#define SIL_THREAD_LOCAL __thread
#define SIL_NORETURN __attribute__((noreturn))
#endif

typedef struct sil_thread sil_thread_t;
typedef struct sil_sem sil_sem_t;
typedef struct sil_mutex sil_mutex_t;
typedef struct sil_cond sil_cond_t;

typedef void (*sil_thread_fn)(void *arg);

/* ---- threads ---------------------------------------------------------- */
/* Creates and starts a named thread. The name is visible in debuggers. */
sil_thread_t *sil_thread_create(const char *name, sil_thread_fn fn, void *arg);
void sil_thread_join(sil_thread_t *t);
/* Detach and forget: the thread object is released when the thread exits. */
void sil_thread_detach(sil_thread_t *t);
void sil_thread_yield(void);
/* Opaque id of the calling thread (stable for the thread's lifetime). */
uintptr_t sil_thread_self_id(void);
/* Pin the calling thread to one host CPU (best effort, may be ignored). */
void sil_thread_pin(unsigned cpu);
/* CPU time consumed by the calling thread, in nanoseconds. */
uint64_t sil_thread_cpu_ns(void);

/* ---- semaphores (counting, used for the run-token hand over) ---------- */
sil_sem_t *sil_sem_create(unsigned initial);
void sil_sem_destroy(sil_sem_t *s);
void sil_sem_post(sil_sem_t *s);
void sil_sem_wait(sil_sem_t *s);
/* Returns 1 if acquired, 0 on timeout. */
int sil_sem_wait_for(sil_sem_t *s, uint64_t timeout_ns);

/* ---- mutex + condition variable --------------------------------------- */
sil_mutex_t *sil_mutex_create(void);
void sil_mutex_destroy(sil_mutex_t *m);
void sil_mutex_lock(sil_mutex_t *m);
void sil_mutex_unlock(sil_mutex_t *m);

sil_cond_t *sil_cond_create(void);
void sil_cond_destroy(sil_cond_t *c);
/* Must be called with m locked. */
void sil_cond_wait(sil_cond_t *c, sil_mutex_t *m);
/* Returns 1 if notified, 0 on timeout. Must be called with m locked. */
int sil_cond_wait_for(sil_cond_t *c, sil_mutex_t *m, uint64_t timeout_ns);
void sil_cond_notify_one(sil_cond_t *c);
void sil_cond_notify_all(sil_cond_t *c);

/* ---- atomics (sequentially consistent) --------------------------------- */
uint32_t sil_atomic_load32(const volatile uint32_t *p);
void sil_atomic_store32(volatile uint32_t *p, uint32_t v);
uint32_t sil_atomic_add32(volatile uint32_t *p, uint32_t v); /* returns old */
int sil_atomic_cas32(volatile uint32_t *p, uint32_t expected, uint32_t desired);
uint64_t sil_atomic_load64(const volatile uint64_t *p);
void sil_atomic_store64(volatile uint64_t *p, uint64_t v);
uint64_t sil_atomic_add64(volatile uint64_t *p, uint64_t v);
void sil_atomic_fence(void);

/* ---- time ------------------------------------------------------------- */
/* Monotonic host clock in nanoseconds (steady, never goes backwards). */
uint64_t sil_clock_ns(void);
/* Wall clock as unix time in nanoseconds (for file headers only). */
uint64_t sil_wallclock_ns(void);
void sil_sleep_ns(uint64_t ns);
/* Sleep until the monotonic clock reaches the given value. */
void sil_sleep_until_ns(uint64_t deadline_ns);
/* ISO-8601 timestamp of now into buf. */
void sil_timestamp_iso(char *buf, size_t len);

/* ---- process ---------------------------------------------------------- */
/* Replaces (or re-spawns) the current process with the same arguments plus
 * extra_arg. Used for ECU reset emulation. Never returns on success. */
int sil_process_reexec(int argc, char **argv, const char *extra_arg);
SIL_NORETURN void sil_process_exit(int code);
const char *sil_getenv(const char *name);

/* ---- misc ------------------------------------------------------------- */
/* Printf to stderr with a "[sil] " prefix; thread safe. */
void sil_log(const char *fmt, ...);
/* Abort with message (used for internal invariants). */
SIL_NORETURN void sil_fatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* SIL_PORT_H */
