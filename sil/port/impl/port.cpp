// port.cpp - sil_port.h implementation on C++17 standard primitives.
#include "sil_port.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::chrono::nanoseconds to_ns(uint64_t ns) { return std::chrono::nanoseconds(static_cast<int64_t>(ns)); }

void set_current_thread_name(const char *name) {
    if (name == nullptr) return;
#if defined(_WIN32)
    // SetThreadDescription exists from Windows 10 1607; resolve dynamically so older systems still run.
    using Fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription")));
    if (fn != nullptr) {
        wchar_t wname[128];
        size_t n = 0;
        while (name[n] != '\0' && n < 127) { wname[n] = static_cast<wchar_t>(static_cast<unsigned char>(name[n])); ++n; }
        wname[n] = L'\0';
        fn(GetCurrentThread(), wname);
    }
#elif defined(__linux__)
    char buf[16];
    std::strncpy(buf, name, 15);
    buf[15] = '\0';
    pthread_setname_np(pthread_self(), buf);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#endif
}

} // namespace

struct sil_thread {
    std::thread th;
    std::string name;
    sil_thread_fn fn;
    void *arg;
};

struct sil_sem {
    std::mutex m;
    std::condition_variable cv;
    unsigned count;
};

struct sil_mutex {
    std::mutex m;
};

struct sil_cond {
    std::condition_variable cv;
};

extern "C" {

/* ---- threads ---------------------------------------------------------- */
sil_thread_t *sil_thread_create(const char *name, sil_thread_fn fn, void *arg) {
    auto *t = new sil_thread{};
    t->name = name != nullptr ? name : "sil";
    t->fn = fn;
    t->arg = arg;
    t->th = std::thread([t]() {
        set_current_thread_name(t->name.c_str());
        t->fn(t->arg);
    });
    return t;
}

void sil_thread_join(sil_thread_t *t) {
    if (t == nullptr) return;
    if (t->th.joinable()) t->th.join();
    delete t;
}

void sil_thread_detach(sil_thread_t *t) {
    if (t == nullptr) return;
    if (t->th.joinable()) t->th.detach();
    delete t;
}

void sil_thread_yield(void) { std::this_thread::yield(); }

uintptr_t sil_thread_self_id(void) {
#if defined(_WIN32)
    return static_cast<uintptr_t>(GetCurrentThreadId());
#else
    return static_cast<uintptr_t>(pthread_self());
#endif
}

void sil_thread_pin(unsigned cpu) {
#if defined(_WIN32)
    if (cpu < 64) SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

uint64_t sil_thread_cpu_ns(void) {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) == 0) return 0;
    ULARGE_INTEGER ku, uu;
    ku.LowPart = k.dwLowDateTime; ku.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (ku.QuadPart + uu.QuadPart) * 100u; // 100 ns units
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

/* ---- semaphores ------------------------------------------------------- */
sil_sem_t *sil_sem_create(unsigned initial) {
    auto *s = new sil_sem{};
    s->count = initial;
    return s;
}

void sil_sem_destroy(sil_sem_t *s) { delete s; }

void sil_sem_post(sil_sem_t *s) {
    {
        std::lock_guard<std::mutex> l(s->m);
        ++s->count;
    }
    s->cv.notify_one();
}

void sil_sem_wait(sil_sem_t *s) {
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [s] { return s->count > 0; });
    --s->count;
}

int sil_sem_wait_for(sil_sem_t *s, uint64_t timeout_ns) {
    std::unique_lock<std::mutex> l(s->m);
    if (!s->cv.wait_for(l, to_ns(timeout_ns), [s] { return s->count > 0; })) return 0;
    --s->count;
    return 1;
}

/* ---- mutex / cond ----------------------------------------------------- */
sil_mutex_t *sil_mutex_create(void) { return new sil_mutex{}; }
void sil_mutex_destroy(sil_mutex_t *m) { delete m; }
void sil_mutex_lock(sil_mutex_t *m) { m->m.lock(); }
void sil_mutex_unlock(sil_mutex_t *m) { m->m.unlock(); }

sil_cond_t *sil_cond_create(void) { return new sil_cond{}; }
void sil_cond_destroy(sil_cond_t *c) { delete c; }

void sil_cond_wait(sil_cond_t *c, sil_mutex_t *m) {
    std::unique_lock<std::mutex> l(m->m, std::adopt_lock);
    c->cv.wait(l);
    l.release();
}

int sil_cond_wait_for(sil_cond_t *c, sil_mutex_t *m, uint64_t timeout_ns) {
    std::unique_lock<std::mutex> l(m->m, std::adopt_lock);
    auto r = c->cv.wait_for(l, to_ns(timeout_ns));
    l.release();
    return r == std::cv_status::no_timeout ? 1 : 0;
}

void sil_cond_notify_one(sil_cond_t *c) { c->cv.notify_one(); }
void sil_cond_notify_all(sil_cond_t *c) { c->cv.notify_all(); }

/* ---- atomics ---------------------------------------------------------- */
uint32_t sil_atomic_load32(const volatile uint32_t *p) {
    return std::atomic_load(reinterpret_cast<const volatile std::atomic<uint32_t> *>(p));
}
void sil_atomic_store32(volatile uint32_t *p, uint32_t v) {
    std::atomic_store(reinterpret_cast<volatile std::atomic<uint32_t> *>(p), v);
}
uint32_t sil_atomic_add32(volatile uint32_t *p, uint32_t v) {
    return std::atomic_fetch_add(reinterpret_cast<volatile std::atomic<uint32_t> *>(p), v);
}
int sil_atomic_cas32(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return std::atomic_compare_exchange_strong(reinterpret_cast<volatile std::atomic<uint32_t> *>(p), &expected, desired) ? 1 : 0;
}
uint64_t sil_atomic_load64(const volatile uint64_t *p) {
    return std::atomic_load(reinterpret_cast<const volatile std::atomic<uint64_t> *>(p));
}
void sil_atomic_store64(volatile uint64_t *p, uint64_t v) {
    std::atomic_store(reinterpret_cast<volatile std::atomic<uint64_t> *>(p), v);
}
uint64_t sil_atomic_add64(volatile uint64_t *p, uint64_t v) {
    return std::atomic_fetch_add(reinterpret_cast<volatile std::atomic<uint64_t> *>(p), v);
}
void sil_atomic_fence(void) { std::atomic_thread_fence(std::memory_order_seq_cst); }

/* ---- time ------------------------------------------------------------- */
uint64_t sil_clock_ns(void) {
    static const Clock::time_point origin = Clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin).count());
}

uint64_t sil_wallclock_ns(void) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void sil_sleep_ns(uint64_t ns) { std::this_thread::sleep_for(to_ns(ns)); }

void sil_sleep_until_ns(uint64_t deadline_ns) {
    for (;;) {
        uint64_t now = sil_clock_ns();
        if (now >= deadline_ns) return;
        uint64_t rem = deadline_ns - now;
        // Coarse sleep, then spin the last ~200 us for accuracy (Windows timer granularity is ~1 ms).
        if (rem > 1500000ull) {
            std::this_thread::sleep_for(to_ns(rem - 1000000ull));
        } else if (rem > 200000ull) {
            std::this_thread::yield();
        } else {
            // busy wait
        }
    }
}

void sil_timestamp_iso(char *buf, size_t len) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tmv);
}

/* ---- process ---------------------------------------------------------- */
int sil_process_reexec(int argc, char **argv, const char *extra_arg) {
#if defined(_WIN32)
    std::string cmd;
    for (int i = 0; i < argc; ++i) {
        if (i) cmd += ' ';
        cmd += '"';
        cmd += argv[i];
        cmd += '"';
    }
    if (extra_arg != nullptr) {
        cmd += " \"";
        cmd += extra_arg;
        cmd += '"';
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd;
    if (CreateProcessA(nullptr, &mutable_cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi) == 0) return -1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::fflush(nullptr);
    ExitProcess(0);
#else
    std::fflush(nullptr);
    char **args = static_cast<char **>(std::calloc(static_cast<size_t>(argc) + 2, sizeof(char *)));
    for (int i = 0; i < argc; ++i) args[i] = argv[i];
    args[argc] = const_cast<char *>(extra_arg);
    args[argc + (extra_arg != nullptr ? 1 : 0)] = nullptr;
    execv(argv[0], args);
    return -1;
#endif
}

void sil_process_exit(int code) {
    std::fflush(nullptr);
    std::_Exit(code);
}

const char *sil_getenv(const char *name) { return std::getenv(name); }

/* ---- misc ------------------------------------------------------------- */
static std::mutex g_log_mutex;

void sil_log(const char *fmt, ...) {
    std::lock_guard<std::mutex> l(g_log_mutex);
    std::fputs("[sil] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void sil_fatal(const char *fmt, ...) {
    {
        std::lock_guard<std::mutex> l(g_log_mutex);
        std::fputs("[sil] FATAL: ", stderr);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
    std::abort();
}

} // extern "C"
