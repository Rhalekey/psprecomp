/* Original host-thread adapter. Only the opt-in scheduler includes this file.
 * No guest state or scheduling policy belongs here. */
#ifndef PSPRECOMP_HOST_THREAD_H
#define PSPRECOMP_HOST_THREAD_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

static void host_thread_error(void) {
    fputs("psprecomp: host synchronization failed\n", stderr);
    abort();
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <mmsystem.h>

/* MinGW-based Clang ignores __declspec(thread) with only a warning, which
 * silently makes these variables shared between threads. */
#ifdef _MSC_VER
#define HOST_TLS __declspec(thread)
#else
#define HOST_TLS _Thread_local
#endif
typedef HANDLE host_thread;
typedef SRWLOCK host_mutex;
typedef CONDITION_VARIABLE host_cond;
#define HOST_MUTEX_INIT SRWLOCK_INIT
#define HOST_COND_INIT CONDITION_VARIABLE_INIT
static void host_mutex_init(host_mutex *m) { InitializeSRWLock(m); }
static void host_mutex_destroy(host_mutex *m) { (void)m; }
static void host_cond_init(host_cond *c) { InitializeConditionVariable(c); }
static void host_cond_destroy(host_cond *c) { (void)c; }
static void host_mutex_lock(host_mutex *m) { AcquireSRWLockExclusive(m); }
static void host_mutex_unlock(host_mutex *m) { ReleaseSRWLockExclusive(m); }
static void host_cond_signal(host_cond *c) { WakeConditionVariable(c); }
static void host_cond_broadcast(host_cond *c) { WakeAllConditionVariable(c); }
static void host_cond_wait(host_cond *c, host_mutex *m) {
    if (!SleepConditionVariableSRW(c, m, INFINITE, 0)) host_thread_error();
}
/* Windows condition waits have millisecond granularity. Round UP so a short
 * park never becomes a nonblocking poll; this is not a real-time guarantee.
 *
 * Both the wait and the clock used to follow the system timer, which ticks
 * every 15.6 ms by default: a 1 ms park cost 15.6 ms of host time, and WTF's
 * audio thread, which parks once per 5.8 ms buffer, ran 17 times slower than
 * real time. host_timer_init() asks for 1 ms resolution, and deadlines use the
 * performance counter rather than GetTickCount64. */
static void host_timer_init(void) { timeBeginPeriod(1); }
static uint64_t host_deadline_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000ull +
           (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000ull / (uint64_t)freq.QuadPart;
}
static int host_cond_until(host_cond *c, host_mutex *m, uint64_t deadline) {
    uint64_t now = host_deadline_us();
    if (now >= deadline) return 1;
    uint64_t ms = (deadline - now + 999ull) / 1000ull;
    DWORD delay = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    if (SleepConditionVariableSRW(c, m, delay, 0)) return 0;
    if (GetLastError() == ERROR_TIMEOUT) return 1;
    host_thread_error();
    return 1;
}
static uint64_t host_epoch_us(void) {
    FILETIME ft;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&ft);
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    /* FILETIME is 100 ns since 1601; expose Unix-epoch microseconds. */
    return value.QuadPart / 10ull - 11644473600000000ull;
}
typedef struct { void *(*entry)(void *); void *arg; } host_start;
static unsigned __stdcall host_thread_entry(void *arg) {
    host_start start = *(host_start *)arg;
    free(arg);
    start.entry(start.arg);
    return 0;
}
static int host_thread_create(host_thread *thread, size_t stack,
                              void *(*entry)(void *), void *arg) {
    host_start *start = (host_start *)malloc(sizeof *start);
    if (!start) return -1;
    start->entry = entry;
    start->arg = arg;
    uintptr_t handle = _beginthreadex(NULL, (unsigned)stack, host_thread_entry,
                                     start, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!handle) { free(start); return -1; }
    *thread = (HANDLE)handle;
    return 0;
}
static void host_thread_join(host_thread thread) {
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) host_thread_error();
    if (!CloseHandle(thread)) host_thread_error();
}
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#define HOST_TLS _Thread_local
typedef pthread_t host_thread;
typedef pthread_mutex_t host_mutex;
typedef pthread_cond_t host_cond;
#define HOST_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define HOST_COND_INIT PTHREAD_COND_INITIALIZER
static void host_mutex_init(host_mutex *m) { if (pthread_mutex_init(m, NULL)) host_thread_error(); }
static void host_mutex_destroy(host_mutex *m) { if (pthread_mutex_destroy(m)) host_thread_error(); }
static void host_cond_init(host_cond *c) { if (pthread_cond_init(c, NULL)) host_thread_error(); }
static void host_cond_destroy(host_cond *c) { if (pthread_cond_destroy(c)) host_thread_error(); }
static void host_mutex_lock(host_mutex *m) { if (pthread_mutex_lock(m)) host_thread_error(); }
static void host_mutex_unlock(host_mutex *m) { if (pthread_mutex_unlock(m)) host_thread_error(); }
static void host_cond_signal(host_cond *c) { if (pthread_cond_signal(c)) host_thread_error(); }
static void host_cond_broadcast(host_cond *c) { if (pthread_cond_broadcast(c)) host_thread_error(); }
static void host_cond_wait(host_cond *c, host_mutex *m) { if (pthread_cond_wait(c, m)) host_thread_error(); }
static void host_timer_init(void) { }   /* POSIX sleeps are already fine-grained */
static uint64_t host_deadline_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) host_thread_error();
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t host_epoch_us(void) { return host_deadline_us(); }
static int host_cond_until(host_cond *c, host_mutex *m, uint64_t deadline) {
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline / 1000000ull);
    ts.tv_nsec = (long)((deadline % 1000000ull) * 1000ull);
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc && rc != ETIMEDOUT) host_thread_error();
    return rc == ETIMEDOUT;
}
static int host_thread_create(host_thread *thread, size_t stack,
                              void *(*entry)(void *), void *arg) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, stack);
    if (!rc) rc = pthread_create(thread, &attr, entry, arg);
    if (pthread_attr_destroy(&attr)) host_thread_error();
    return rc;
}
static void host_thread_join(host_thread thread) { if (pthread_join(thread, NULL)) host_thread_error(); }
#endif
#endif
