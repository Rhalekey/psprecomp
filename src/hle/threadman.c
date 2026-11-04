/* psprecomp — ThreadManForUser.
 *
 * Threads, semaphores, event flags and callbacks. This is the largest single
 * dependency any PSP game has: `module_start` typically does nothing but
 * create a thread, start it, and return.
 *
 * ## The execution model
 *
 * Default: StartThread runs inline, and unsatisfied waits return a timeout.
 * PSPRECOMP_REAL_THREADS opts into the experimental model described below.
 *
 * Recompiled functions are ordinary C functions over one register file
 * (`psp_cpu`), so the PSP thread model has to be built out of host threads.
 * What this implements, and why it looks the way it does:
 *
 *   - **One host thread per guest thread.** Each `sceKernelStartThread`
 *     creates a host thread and *returns*, which is the point: a game that
 *     starts a worker and then posts work to it never gets past its
 *     `StartThread` call under the old run-to-completion model, because the
 *     worker's body ran inline and never came back. Measured on LocoRoco: the
 *     worker sat in `while (!flag) sleep();` forever, waiting for the work the
 *     still-unreturned caller was going to post.
 *   - **One CPU at a time, handed over explicitly** (the baton below). The PSP
 *     has a single core and never runs two threads at once, so this is both
 *     faithful and the thing that keeps the shared register file coherent:
 *     whoever holds the baton owns `psp_cpu`, and a thread that gives up the
 *     CPU first saves its registers into its own record and reloads them when
 *     it gets the CPU back. A per-thread `HOST_TLS psp_cpu` would be the
 *     other way to do it, and would put a TLS lookup on every register access
 *     in millions of lines of generated code.
 *   - **Waits hand off the CPU.** `SleepThread` parks for one tick (or a
 *     wakeup), `WaitSema`/`WaitEventFlag` retry until signalled or timed out,
 *     `WaitThreadEnd` parks until the target exits, `DelayThread` parks for its
 *     duration (in virtual time -- see the clock below).
 *   - **A wait is also where time passes**, so it is where an interrupt is
 *     delivered (src/hle/intrman.c) and where other threads get the CPU. An
 *     unbounded wait is retried for at most WAIT_TICKS_MAX ticks: a thread
 *     parked on a flag
 *     that nothing will ever set re-checks it every tick instead of deadlocking
 *     the whole run. That is a deliberate approximation -- the faithful
 *     alternative is a run that stops with no evidence in it.
 *
 * ## The clock
 *
 * Time is virtual: `park_us()` advances it by the amount the guest asked for,
 * and a park of an unbounded duration advances it a tick at a time. Real time
 * is capped (`REAL_TICK_US`) so that a game asking for a 100 ms delay does not
 * make a 30 s bring-up run take minutes. 1 ms of guest time therefore costs at
 * most 400 us per host condition wait on POSIX (rounded up to milliseconds
 * on Windows), excluding CPU handover and host scheduling latency. This is a
 * choice about which resource is scarce during bring-up.
 *
 * ## Host stacks
 *
 * Each guest thread gets a host stack of HOST_STACK_BYTES, not the guest stack
 * size: a recompiled call is a C call, so guest call depth is *host* stack
 * depth, and a MIPS frame of 32 bytes becomes a host frame of a few hundred.
 * The guest stack allocation stays exactly what the firmware would have handed
 * the thread, reachable through $sp, so a guest stack overflow still lands
 * where the guest put it.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"

#include <errno.h>
#include <stdlib.h>
#if PSPRECOMP_REAL_THREADS
#include "host_thread.h"
#else
#define HOST_TLS
#endif
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_THREADS 128
#define MAX_SEMAS   128
#define MAX_FLAGS   128
#define MAX_CBS     64
#define UID_BASE    0x00040000u

/* Virtual microseconds in one scheduler tick, and the most real time a park
 * will ever spend (see the clock note above). */
#define TICK_US      1000ull
#define REAL_TICK_US 400ull
#define FRAME_US     16667ull      /* 59.94 Hz, the vblank a frame wait means */

#define HOST_STACK_BYTES (16u * 1024u * 1024u)

/* Real-time mode: a wait sleeps until what it waits for, instead of taking the CPU
 * every tick to look.
 *
 * Ticking is what makes virtual time deterministic, and it is expensive: with a
 * dozen threads each asleep for 100 ms, every one takes the CPU a hundred times to
 * find out it is still asleep. A game that waits for the GE many times a frame then
 * joins that queue at every wait, and spends its frame there.
 *
 * WAIT_STEP_US only bounds how long a wait can miss its wakeup. */
#define WAIT_STEP_US 16000ull

/* How long a wait watches the GE before sleeping. Enough for a typical list, and
 * short enough that nothing else minds the CPU being held that long. */
#define GE_SPIN_US 300u

/* Windows wakes a sleeper on a 1 ms tick, and often a tick late. A delay that has to
 * hit a deadline, such as the next vblank, sleeps most of the way and then watches
 * the clock for this long, holding no CPU a guest could use. */
#define PRECISE_US 2000ull
static int g_realtime;
static HOST_TLS uint64_t g_tls_real_us;   /* how long the next park really sleeps */

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_SUSPENDED };

/* What a parked thread is waiting for. A wake is targeted: signalling a
 * semaphore should not wake a thread waiting on a thread to exit. */
enum { W_NONE = 0, W_SLEEP, W_DELAY, W_SEMA, W_FLAG, W_THREAD_END, W_VPL };

static const char *wait_name(int w) {
    switch (w) {
    case W_SLEEP:      return "sleep";
    case W_DELAY:      return "delay";
    case W_SEMA:       return "sema";
    case W_FLAG:       return "event flag";
    case W_THREAD_END: return "thread end";
    case W_VPL:        return "vpl";
    default:           return "-";
    }
}

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t entry;
    uint32_t priority;
    /* Real time spent asleep, by what it was waiting for, and then queueing for
     * the CPU once ready. What is left of a thread's life is running. */
    uint64_t us_waiting[16];
    uint64_t us_queueing;
    uint64_t us_started;
    uint32_t stack_size;
    uint32_t stack_base;   /* low address of the allocation */
    uint32_t attr;
    uint32_t arglen;       /* sceKernelStartThread arguments, held for the host */
    uint32_t argp;
    uint32_t gp;           /* the creator's $gp, which the thread starts with */
    int      state;
    uint32_t exit_status;
    int      used;

    /* Started, spawned, and has never had the CPU. A park hands the CPU over to
     * it rather than racing the host's scheduler for it -- see the handover in
     * park_us(). Cleared by the thread itself on its first acquisition, so it
     * cannot outlive the thing it describes. */
    int      cpu_pending;

    /* Where this thread's registers live while it does not hold the CPU. */
    psp_cpu_state ctx;

#if PSPRECOMP_REAL_THREADS
    host_thread host;        /* the host thread carrying it, once started */
    int       host_live;

    /* Parked state. `wlock` guards the two fields below; a waker takes it for
     * as long as it takes to set `woken`, so a wakeup cannot land between a
     * thread deciding to park and actually parking. */
    host_mutex wlock;
    host_cond  wcv;
    int             wlock_ready;
    int             woken;        /* consumed by the park */
    int             host_joinable;
#endif
    int             waiting;      /* 1 while parked */
    int             wait_kind;

    uint64_t wakeups;             /* sceKernelWakeupThread, not yet consumed */
    uint64_t parks;
    uint32_t last_wait_kind;

    int terminated;               /* sceKernelTerminateThread asked for it */
    int delete_after;             /* sceKernelExitDeleteThread: free on the way out */
    int exited;
    int end_waiters;               /* pins a record while WaitThreadEnd parks */
    struct end_ctx *end_waits;     /* nested waits, unwound with the guest */

    jmp_buf  unwind;       /* where sceKernelExitThread returns to */
    int      unwind_set;
} psp_thread;

struct end_ctx { psp_thread *t; struct end_ctx *prev; };

typedef struct {
    uint32_t uid;
    char     name[32];
    int32_t  count;
    int32_t  max_count;
    int      used;
} psp_sema;

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t pattern;
    int      used;
} psp_evflag;

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t func;
    uint32_t arg;          /* what CreateCallback was given: the callback's first argument */
    /* A callback belongs to the thread that created it, and runs on that thread
     * -- which is why the owner is part of the object rather than a detail of
     * delivery: a notify from anywhere else leaves it pending until its own
     * thread reaches a check point. `0` is the boot thread. */
    uint32_t owner;
    uint32_t notify_arg;   /* the argument of the last notification */
    int      notify_count; /* notifications not yet delivered */
    int32_t  delivery_status;  /* what the callback returned, as sceKernelReferCallbackStatus reports */
    int      used;
} psp_callback;

/* Variable-size pools (see the VPL section below) and the blocks handed out of
 * them. The blocks are tracked because FreeVpl has to give the right one back
 * and ReferVplStatus has to report a free size that is true. */
#define MAX_VPLS       64
#define MAX_VPL_BLOCKS 512

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t attr;
    uint32_t pool_size;
    uint32_t free_size;
    int      used;
} psp_vpl;

typedef struct {
    uint32_t vpl, addr, size;
    int      used;
} psp_vpl_block;

static psp_thread   g_thread[MAX_THREADS];
static psp_sema     g_sema[MAX_SEMAS];
static psp_evflag   g_flag[MAX_FLAGS];
static psp_callback g_cb[MAX_CBS];
static uint64_t     g_cb_deliveries;        /* callbacks that actually ran */
static uint64_t     g_cb_unresolved;        /* notifications dropped: not a recompiled function */
static uint64_t     g_cb_unresolved_logged;
static uint64_t     g_cb_deliver_deferred;  /* passes that hit the per-tick ceiling */
static int          g_cb_delivering;        /* re-entrance guard, see deliver_callbacks */
static psp_vpl       g_vpl[MAX_VPLS];
static psp_vpl_block g_vplblk[MAX_VPL_BLOCKS];
static uint32_t     g_next_uid;
static psp_thread  *g_current;      /* whose registers are in psp_cpu, or NULL */
/* The host thread's own record. It has to be separate from g_current: the
 * thread that is ending clears g_current, and the next thread to run is then
 * left with no idea which record is its own -- which silently turns every park
 * into a no-op instead of a block. Measured, on the second guest thread. */
static HOST_TLS psp_thread *g_tls_me;

/* ---- the baton ------------------------------------------------------------
 *
 * The right to execute recompiled code. `g_baton_owned` is who has it; a
 * thread that blocks gives it away and takes it back when it is woken, which
 * is what makes `psp_cpu` coherent without a copy on every register access.
 *
 * The boot thread (whoever calls psp_threadman_reset, normally the host's
 * main) owns it from the start: it is running the module entry before any
 * guest thread exists.
 */
#if PSPRECOMP_REAL_THREADS
static host_mutex g_baton_m  = HOST_MUTEX_INIT;
static host_cond  g_baton_cv = HOST_COND_INIT;
static int             g_baton_owned;

/* Host threads sitting in baton_acquire, i.e. wanting the CPU right now. A
 * parker reads it to decide whether it may take the CPU back (see the handover
 * below); a thread that is waiting there has nowhere else to be, so it takes
 * the baton as soon as it is free -- the wait cannot be a deadlock. */
static int             g_baton_waiters;
static uint64_t        g_baton_takes;      /* every acquisition, by anyone */
#endif
static uint64_t        g_handovers;        /* parks that handed the CPU over */
static uint64_t        g_handovers_expired;

/* Per host thread, so a nested park (an interrupt handler that waits) restores
 * its outer nesting on the way back. */
static HOST_TLS int g_tls_depth;
static HOST_TLS int g_tls_intr_depth;
static uint64_t g_created, g_started;

static uint64_t g_switches;         /* baton handovers */
static uint64_t g_parks;
static uint64_t g_wait_gaveup;      /* unbounded waits that ran out of ticks */
static uint64_t g_vtime_us;         /* see the clock note above */
static uint64_t g_epoch_us;         /* wall clock at reset, so time never stops */

static uint64_t host_now_us(void) {
#if PSPRECOMP_REAL_THREADS
    return host_epoch_us();
#else
    return (uint64_t)time(NULL) * 1000000ull;
#endif
}

/* The handovers, one line each, on request: PSPRECOMP_TRACE_SCHED=1. Bounded,
 * and the bound announces itself -- a trace that goes quiet has to be
 * distinguishable from one that was silenced. */
#define SCHED_TRACE_MAX 2000
static int g_trace_sched;
#if PSPRECOMP_REAL_THREADS
static int g_trace_init;
static uint64_t g_trace_n;
#endif
#define TRACE_SCHED(...) do { \
    if (!g_trace_init) { g_trace_init = 1; const char *v_ = getenv("PSPRECOMP_TRACE_SCHED"); g_trace_sched = v_ && v_[0] == '1'; } \
    if (g_trace_sched && g_trace_n < SCHED_TRACE_MAX) { \
        g_trace_n++; \
        fprintf(stderr, "[sched %llu] ", (unsigned long long)g_trace_n); \
        fprintf(stderr, __VA_ARGS__); \
        if (g_trace_n == SCHED_TRACE_MAX) \
            fprintf(stderr, "[sched] %d lines is the limit; the rest is counted, not printed\n", SCHED_TRACE_MAX); \
    } \
} while (0)

#if PSPRECOMP_REAL_THREADS
static void baton_acquire(void) {
    if (g_tls_depth > 0) { g_tls_depth++; return; }   /* already ours */
    host_mutex_lock(&g_baton_m);
    g_baton_waiters++;
    while (g_baton_owned) host_cond_wait(&g_baton_cv, &g_baton_m);
    g_baton_waiters--;
    g_baton_owned = 1;
    g_baton_takes++;
    g_tls_depth = 1;
    g_switches++;
    /* Whoever is holding a park open for a handover has just got what it was
     * waiting for. */
    host_cond_broadcast(&g_baton_cv);
    TRACE_SCHED("acquired\n");
    host_mutex_unlock(&g_baton_m);
}

static void baton_release(void) {
    if (g_tls_depth == 0) return;
    if (--g_tls_depth) return;
    host_mutex_lock(&g_baton_m);
    g_baton_owned = 0;
    host_cond_broadcast(&g_baton_cv);
    TRACE_SCHED("release\n");
    host_mutex_unlock(&g_baton_m);
}

/* Is anyone waiting for the CPU? Two shapes, and both are certain to take it:
 * a thread that has been started and has never run, and a host thread already
 * sitting in baton_acquire. Neither is waiting for anything else, so a parker
 * that lets them go first cannot be left waiting for an event that never
 * comes. */
static int cpu_wanted_locked(void) {
    /* Guest records may change while another guest owns the CPU. Do not
     * inspect them until the baton is free (all mutations then happen-before
     * this lock acquisition). */
    if (g_baton_owned || g_baton_waiters > 0) return 1;
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_thread[i].used && g_thread[i].cpu_pending) return 1;
    return 0;
}

/* The handover: what "a park gives the CPU away" has to mean.
 *
 * Releasing the baton and taking it straight back makes the handover a race
 * with the host's own scheduler -- and the host's scheduler is free to run the
 * parking thread first. Measured, and it is why this exists: a thread started
 * just before a delay had not been scheduled yet when the delay ended (the park
 * waits REAL_TICK_US of *host* time for a wakeup that a delay never gets), so
 * the delay returned before the thread it was supposed to let through had run.
 * A guest never sees that: on one core a started thread of equal priority runs.
 * The starter's own test flaked about one run in ten on it.
 *
 * So the CPU is not taken back while someone wants it. Bounded anyway, and it
 * says so when the bound is reached: the alternative to a capped wait is a run
 * with nothing in it, and a wait that gives up has to be distinguishable from a
 * wait that was never needed. In practice this resolves in microseconds --
 * the thread it waits for is already blocked on the baton. */
#define HANDOVER_CAP_US 20000    /* 20 ms of host time */
static int handover_wait_locked(void) {
    uint64_t dl = host_deadline_us() + HANDOVER_CAP_US;

    while (cpu_wanted_locked()) {
        if (host_cond_until(&g_baton_cv, &g_baton_m, dl)) {
            return 1;
        }
    }
    return 0;
}

/* Give the CPU away and wait to be woken, or for `timeout_us` of *virtual*
 * time. A zero timeout waits for a wakeup only; every caller inside the guest
 * passes a tick, so no wait can be unbounded in practice.
 *
 * Returns 1 if a wakeup arrived, 0 if the timeout expired. */
static int park_us(uint64_t timeout_us) {
    psp_thread *t = g_tls_me;
    int depth = g_tls_depth;
    const uint64_t parked_at = host_deadline_us();
    const int waiting_for = t ? t->wait_kind : 0;

    if (t) {
        t->ctx = psp_cpu;               /* this IS the context switch */
        g_current = NULL;
        host_mutex_lock(&t->wlock);
        t->waiting = 1;
        t->woken = 0;
        host_mutex_unlock(&t->wlock);
    }

    TRACE_SCHED("park enter: depth=%d t=%s\n", depth, t ? t->name : "(none)");
    uint64_t takes0 = 0;
    if (depth) {                        /* give the CPU away, however nested */
        host_mutex_lock(&g_baton_m);
        takes0 = g_baton_takes;         /* to see, below, whether anyone ran */
        g_baton_owned = 0;
        host_cond_broadcast(&g_baton_cv);
        host_mutex_unlock(&g_baton_m);
        g_tls_depth = 0;
    }

    int woken = 0;
    if (t) {
        uint64_t real = timeout_us < REAL_TICK_US ? timeout_us : REAL_TICK_US;
        if (g_tls_real_us) real = g_tls_real_us;
        host_mutex_lock(&t->wlock);
        if (!t->woken) {
            const uint64_t dl = host_deadline_us() + real;
            const int precise = g_realtime && waiting_for == W_DELAY && real > PRECISE_US;
            const uint64_t coarse_dl = precise ? dl - PRECISE_US : dl;
            while (!t->woken) {
                if (host_cond_until(&t->wcv, &t->wlock, coarse_dl))
                    break;
            }
            while (precise && !t->woken && host_deadline_us() < dl) {
                host_mutex_unlock(&t->wlock);
                host_thread_yield();
                host_mutex_lock(&t->wlock);
            }
        }
        woken = t->woken;
        t->woken = 0;
        t->waiting = 0;
        t->wait_kind = W_NONE;
        host_mutex_unlock(&t->wlock);
    }

    const uint64_t ready_at = host_deadline_us();
    if (depth) {
        host_mutex_lock(&g_baton_m);
        /* The handover, before the CPU is taken back: a started thread that has
         * never run, or a host thread already queued for the baton, goes first.
         * Without this the re-acquire below is a race with the host's scheduler
         * -- see handover_wait_locked(). */
        int handed = (g_baton_takes != takes0);   /* someone ran while we were parked */
        int expired = 0;
        if (cpu_wanted_locked()) { expired = handover_wait_locked(); handed = 1; }
        while (g_baton_owned) host_cond_wait(&g_baton_cv, &g_baton_m);
        g_baton_owned = 1;
        g_switches++;
        host_mutex_unlock(&g_baton_m);
        g_tls_depth = depth;
        if (handed) g_handovers++;
        if (expired && g_handovers_expired++ == 0)
            fprintf(stderr, "psprecomp: CPU handover exceeded %d ms; further expiries are counted\n",
                    HANDOVER_CAP_US / 1000);
    }

    if (timeout_us) g_vtime_us += timeout_us;
    g_parks++;
    if (t) {
        const uint64_t resumed_at = host_deadline_us();
        t->us_waiting[waiting_for & 15] += ready_at - parked_at;
        t->us_queueing += resumed_at - ready_at;
        t->parks++;
        psp_cpu = t->ctx;
        g_current = t;
    }
    TRACE_SCHED("park done: %s woken=%d\n", t ? t->name : "(none)", woken);
    return woken;
}

#else
/* Inline mode has no host threads and never waits in real time. */
static int park_us(uint64_t timeout_us) {
    g_vtime_us += timeout_us;
    return 0;
}
#endif

static void trace_sched_enable(void) {
    const char *v = getenv("PSPRECOMP_TRACE_SCHED");
    g_trace_sched = v && v[0] == '1';
}

#if PSPRECOMP_REAL_THREADS
static void wake_thread(psp_thread *t) {
    if (!t->wlock_ready) return;
    host_mutex_lock(&t->wlock);
    t->woken = 1;
    host_cond_signal(&t->wcv);
    host_mutex_unlock(&t->wlock);
}

/* Wake everyone parked on `kind`. Called by whoever holds the baton, from the
 * call that changes the state they are waiting on. */
static void wake_waiters(int kind) {
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (!t->used || !t->wlock_ready) continue;
        host_mutex_lock(&t->wlock);
        int hit = t->waiting && t->wait_kind == kind;
        host_mutex_unlock(&t->wlock);
        if (hit) wake_thread(t);
    }
}

#else
static void wake_thread(psp_thread *t) { (void)t; }
static void wake_waiters(int kind) { (void)kind; }
#endif

/* Every park used by a wait goes through here, so that "time passes, another
 * thread runs, an interrupt may be delivered" is one thing rather than five
 * copies of it. */
static void check_terminated(void);

static void park_for(int kind, uint64_t timeout_us);

/* Park for this much real time, unless woken first. */
static void park_real(int kind, uint64_t us) {
    g_tls_real_us = us ? us : 1;
    park_for(kind, us);
    g_tls_real_us = 0;
}

static void park_for(int kind, uint64_t timeout_us) {
    check_terminated();
    /* Before giving the CPU away: the thread about to wait may be waiting for
     * exactly this. */
    psp_ge_deliver();
    psp_thread *t = g_tls_me;

#if PSPRECOMP_REAL_THREADS
    if (t) {
        host_mutex_lock(&t->wlock);
        t->wait_kind = kind;
        host_mutex_unlock(&t->wlock);
        if (t->parks < 2)
            fprintf(stderr, "thread %u parks  : %s (virtual time %llu us)\n",
                    t->uid, wait_name(kind), (unsigned long long)g_vtime_us);
        t->last_wait_kind = (uint32_t)kind;
    }
#else
    (void)t; (void)kind;
#endif
    park_us(timeout_us);
    check_terminated();
}

/* ================================ public ================================= */

static void boot_record(void);

void psp_threadman_reset(void) {
#if PSPRECOMP_REAL_THREADS
    /* Reset is a quiescent-host operation, not asynchronous cancellation.
     * Never overwrite a live thread's locks or free memory it can still use. */
    for (int i = 0; i < MAX_THREADS - 1; i++) {
        if (g_thread[i].host_live) {
            fputs("psprecomp: reset requires all guest threads to have ended\n", stderr);
            abort();
        }
        if (g_thread[i].host_joinable) host_thread_join(g_thread[i].host);
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_thread[i].wlock_ready) {
            host_cond_destroy(&g_thread[i].wcv);
            host_mutex_destroy(&g_thread[i].wlock);
        }
    }
#endif
    g_tls_me = NULL;
    g_current = NULL;
    memset(g_thread, 0, sizeof g_thread);
    memset(g_sema, 0, sizeof g_sema);
    memset(g_flag, 0, sizeof g_flag);
    memset(g_cb, 0, sizeof g_cb);
    g_cb_deliveries = 0;
    g_cb_unresolved = 0;
    g_cb_unresolved_logged = 0;
    g_cb_deliver_deferred = 0;
    g_cb_delivering = 0;
    memset(g_vpl, 0, sizeof g_vpl);
    memset(g_vplblk, 0, sizeof g_vplblk);
    g_next_uid = UID_BASE;
    g_switches = 0;
    g_parks = 0;
    g_wait_gaveup = 0;
    g_handovers = 0;
    g_handovers_expired = 0;
    g_vtime_us = 0;
    g_epoch_us = host_now_us();

#if PSPRECOMP_REAL_THREADS
    /* The caller is running the module entry: it holds the CPU from here. */
    host_mutex_lock(&g_baton_m);
    g_baton_owned = 1;
    host_mutex_unlock(&g_baton_m);
    g_baton_waiters = 0;
    g_baton_takes = 0;
#endif
    g_tls_depth = 1;
    g_created = g_started = 0;
    g_tls_intr_depth = 0;

    boot_record();
}

void psp_threadman_init(void) {
#if PSPRECOMP_REAL_THREADS
    host_timer_init();
#endif
    psp_threadman_reset();
    trace_sched_enable();
}

/* Delivers the deferred work every wait point owes the guest: an interrupt, and
 * the callbacks this thread has pending. Defined with the callbacks, below. */
static void deliver_tick(void);

/* A record for the host thread running the module entry, so that "who am I"
 * and "what is the CPU doing" have answers before any guest thread exists. */
static void boot_record(void) {
    psp_thread *t = &g_thread[MAX_THREADS - 1];
    t->uid = UID_BASE - 1;
    snprintf(t->name, sizeof t->name, "boot");
    t->state = TH_RUNNING;
    t->used = 1;

#if PSPRECOMP_REAL_THREADS
    host_mutex_init(&t->wlock);
    host_cond_init(&t->wcv);
    t->wlock_ready = 1;
#endif
    g_tls_me = t;
    g_current = t;
}

/* Hand the CPU over without blocking on anything. A frame wait is the usual
 * caller: it is the guest saying "let a frame pass", which on one core means
 * "let the others run". */
void psp_threadman_yield(void) { park_for(W_DELAY, TICK_US); }

void psp_threadman_set_realtime(int on) {
#if PSPRECOMP_REAL_THREADS
    g_realtime = on;
#else
    (void)on;
#endif
}

void psp_threadman_sleep_us(uint64_t us) {
    if (g_realtime) park_real(W_DELAY, us);
    else park_for(W_DELAY, TICK_US);
}

/* Called from the GE's thread. One waiter is enough: whichever wakes delivers the
 * pending callbacks, and those wake whoever the game had waiting on them. */
void psp_threadman_kick(void) {
#if PSPRECOMP_REAL_THREADS
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (!t->used || !t->wlock_ready) continue;
        host_mutex_lock(&t->wlock);
        const int hit = t->waiting && (t->wait_kind == W_SEMA || t->wait_kind == W_FLAG);
        host_mutex_unlock(&t->wlock);
        if (hit) { wake_thread(t); return; }
    }
#endif
}

int psp_threadman_times(const char *name, uint64_t *life_us, uint64_t *asleep_us,
                        uint64_t *sema_us, uint64_t *queue_us) {
#if PSPRECOMP_REAL_THREADS
    for (int i = 0; i < MAX_THREADS; i++) {
        const psp_thread *t = &g_thread[i];
        if (!t->used || !t->us_started || strcmp(t->name, name) != 0) continue;
        *life_us = host_deadline_us() - t->us_started;
        *asleep_us = 0;
        for (int k = 0; k < 16; k++) *asleep_us += t->us_waiting[k];
        *sema_us = t->us_waiting[W_SEMA];
        *queue_us = t->us_queueing;
        return 1;
    }
#else
    (void)name; (void)life_us; (void)asleep_us; (void)sema_us; (void)queue_us;
#endif
    return 0;
}

/* A short history of what threads waited for and when, to see the order of events
 * in a frame, which no total can show. Written only by whoever holds the CPU. */
#define SCHED_NOTES 512

static struct {
    uint64_t at_us;
    const char *who, *what, *name;
    uint32_t took_us;
} g_notes[SCHED_NOTES];
static uint32_t g_note_next;

void psp_sched_note(const char *what, const char *name, uint32_t took_us) {
    const uint32_t i = g_note_next++ % SCHED_NOTES;
    g_notes[i].at_us = host_now_us();
    g_notes[i].who = g_tls_me ? g_tls_me->name : "?";
    g_notes[i].what = what;
    g_notes[i].name = name;
    g_notes[i].took_us = took_us;
}

void psp_sched_dump(FILE *out, uint32_t count) {
    if (count > SCHED_NOTES) count = SCHED_NOTES;
    if (count > g_note_next) count = g_note_next;
    const uint64_t base = count ? g_notes[(g_note_next - count) % SCHED_NOTES].at_us : 0;
    fprintf(out, "last %u scheduling notes: ms since first, thread, event, how long in us\n", count);
    for (uint32_t k = g_note_next - count; k != g_note_next; k++) {
        const uint32_t i = k % SCHED_NOTES;
        fprintf(out, "  %8.2f %-18s %-18s %-22s %u\n", (double)(g_notes[i].at_us - base) / 1000.0,
                g_notes[i].who, g_notes[i].what, g_notes[i].name ? g_notes[i].name : "", g_notes[i].took_us);
    }
}

void psp_threadman_dump_times(FILE *out) {
#if PSPRECOMP_REAL_THREADS
    const uint64_t now = host_deadline_us();
    fprintf(out, "thread time, %%: run  queue | sema  flag delay  end\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        const psp_thread *t = &g_thread[i];
        if (!t->used || !t->us_started || now <= t->us_started) continue;
        const double life = (double)(now - t->us_started);
        uint64_t asleep = 0;
        for (int k = 0; k < 16; k++) asleep += t->us_waiting[k];
        const double run = life - (double)asleep - (double)t->us_queueing;
        fprintf(out, "  %-20s %5.1f %5.1f | %5.1f %5.1f %5.1f %5.1f\n", t->name,
                100.0 * run / life, 100.0 * (double)t->us_queueing / life,
                100.0 * (double)t->us_waiting[W_SEMA] / life,
                100.0 * (double)t->us_waiting[W_FLAG] / life,
                100.0 * (double)t->us_waiting[W_DELAY] / life,
                100.0 * (double)t->us_waiting[W_THREAD_END] / life);
    }
#else
    (void)out;
#endif
}

uint32_t psp_threadman_current(void) { return g_tls_me ? g_tls_me->uid : 0xFFFFFFFFu; }

void psp_threadman_frame_wait(void) {
    deliver_tick();
    if (!g_realtime) park_for(W_DELAY, FRAME_US);
}

uint64_t psp_threadman_switches(void) { return g_switches; }
uint64_t psp_threadman_parks(void)    { return g_parks; }
uint64_t psp_threadman_vtime_us(void) { return g_vtime_us; }
/* Parks after which another thread had run -- either it took the CPU while this
 * one was parked, or the park waited for it before taking the CPU back -- and
 * the parks that gave up before the thread they waited for was scheduled. The
 * second number is the interesting one when it is not zero: it means the guest
 * did not get the handover it was promised. */
uint64_t psp_threadman_handovers(void)         { return g_handovers; }
uint64_t psp_threadman_handover_expired(void)  { return g_handovers_expired; }

void psp_threadman_dump(FILE *out) {
    fprintf(out, "CPU               : %s\n",
            g_current ? g_current->name : "(none)");
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (!t->used) continue;
#if PSPRECOMP_REAL_THREADS
        host_mutex_lock(&t->wlock);
#endif
        const char *wait = t->waiting ? wait_name(t->wait_kind) : "on CPU";
#if PSPRECOMP_REAL_THREADS
        host_mutex_unlock(&t->wlock);
#endif
        fprintf(out, "  thread %u %-16s %-9s %s, parks %llu, wakeups %llu%s\n",
                t->uid, t->name,
                t->state == TH_RUNNING ? "running" :
                t->state == TH_SUSPENDED ? "suspended" :
                t->state == TH_READY ? "ready" : "dormant",
                wait,
                (unsigned long long)t->parks,
                (unsigned long long)t->wakeups,
                t->exited ? ", exited" : "");
    }
}

/* ---- interrupt masking --------------------------------------------------- */

/* Per-thread, because a critical section belongs to the thread that opened it.
 * The cookie is the previous state, which is what the guest passes back to
 * ResumeIntr -- and which is why resuming a nested suspend does not enable
 * interrupts. */

int psp_threadman_intr_suspend(void) {
    int was = g_tls_intr_depth == 0;
    g_tls_intr_depth++;
    return was;
}

void psp_threadman_intr_resume(uint32_t cookie) {
    if (g_tls_intr_depth > 0) g_tls_intr_depth--;
    if (cookie) g_tls_intr_depth = 0;
}

int psp_threadman_intr_disabled(void) { return g_tls_intr_depth > 0; }

/* ---- threads ------------------------------------------------------------- */

/* Threads are few per run, so every one is named rather than counted silently:
 * "was the worker ever started?" is the question that decides whether a wait
 * can end at all, and a driver that only reports a total cannot answer it. */

static psp_thread *find_thread(uint32_t id) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_thread[i].used && g_thread[i].uid == id) return &g_thread[i];
    return NULL;
}

static void thread_unwind(psp_thread *t, uint32_t status) {
    if (!t || !t->unwind_set) return;      /* not a started thread: nothing to unwind */
    while (t->end_waits) {
        t->end_waits->t->end_waiters--;
        t->end_waits = t->end_waits->prev;
    }
    t->exit_status = status;
    longjmp(t->unwind, 1);
}

/* A thread another thread has terminated never returns to the guest: this is
 * the only place the kill can be noticed, because nothing preempts a running
 * body. Called after every park. */
static void check_terminated(void) {
    if (g_tls_me && g_tls_me->terminated)
        thread_unwind(g_tls_me, SCE_KERNEL_ERROR_THREAD_TERMINATED);
}

/* ---- the shape of every guest wait --------------------------------------- */

/* How long an *unbounded* guest wait waits before it is reported. The guest's
 * own timeout is honoured exactly. A wait with none never times out on
 * hardware, and code relies on that: WTF's sound thread waits forever on its
 * request queue and does not check the result, so a timeout here made it
 * process an empty request and open a file named NULL.
 *
 * So, under real threads, an unbounded wait keeps waiting while any other guest
 * thread is alive to satisfy it, and says so once after this many ticks. Only a
 * waiter with no other live thread -- nothing could ever wake it -- gives up
 * with a timeout, so that a run that cannot progress ends in a report naming
 * what it waited for. A deadlock between live threads is the host's watchdog's
 * to report. */
#define WAIT_TICKS_MAX 500

#if PSPRECOMP_REAL_THREADS
static int others_alive(void) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (&g_thread[i] != g_tls_me && g_thread[i].host_live && !g_thread[i].exited)
            return 1;
    return 0;
}
#endif

static uint64_t g_wait_long;          /* long waits reported, real threads */

static void warn_wait(const char *what, const char *name) {
#if !PSPRECOMP_REAL_THREADS
    if (g_wait_gaveup++ == 0)
        fprintf(stderr, "psprecomp: %s would block in inline mode; returning timeout\n", what);
    (void)name;
#else
    if (g_wait_gaveup < 8)
        fprintf(stderr,
                "psprecomp: %s%s%s waited %llu ticks with nothing to wake it,"
                " returning a timeout.\n",
                what, (name && name[0]) ? " '" : "", (name && name[0]) ? name : "",
                (unsigned long long)WAIT_TICKS_MAX);
    g_wait_gaveup++;
#endif
}

/* Re-check the condition, deliver an interrupt, park for a tick -- until the
 * condition holds, the guest's timeout expires, or the cap above is reached.
 * One copy, so that all three of those are true of every wait. */
static uint32_t wait_loop(int (*ready)(void *), void *ctx, uint64_t timeout_us,
                          int kind, const char *what, const char *name) {
    uint64_t deadline = timeout_us ? g_vtime_us + timeout_us : 0;
    const uint64_t real_deadline = timeout_us ? host_now_us() + timeout_us : 0;
    const uint64_t began = host_now_us();
    uint64_t ticks = 0;

    for (;;) {
        check_terminated();
        deliver_tick();
        if (ready(ctx)) {
            const uint64_t waited = host_now_us() - began;
            if (waited >= 200) psp_sched_note(what, name, (uint32_t)waited);
            return SCE_KERNEL_ERROR_OK;
        }
#if !PSPRECOMP_REAL_THREADS
        /* No other guest can make progress while this inline call is active. */
        warn_wait(what, name);
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
#endif
        check_terminated();
        if (g_realtime) {
            if (psp_ge_wait_briefly(GE_SPIN_US)) continue;
            uint64_t step = WAIT_STEP_US;
            if (real_deadline) {
                const uint64_t now = host_now_us();
                if (now >= real_deadline) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
                if (real_deadline - now < step) step = real_deadline - now;
            }
            park_real(kind, step);
            continue;
        }
        if (deadline && g_vtime_us >= deadline) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        if (!deadline && ++ticks > WAIT_TICKS_MAX) {
#if PSPRECOMP_REAL_THREADS
            if (others_alive()) {
                if (ticks == WAIT_TICKS_MAX + 1 && g_wait_long++ < 8)
                    fprintf(stderr, "psprecomp: %s%s%s has waited %llu ticks; still waiting,"
                            " other threads are alive\n", what,
                            (name && name[0]) ? " '" : "", (name && name[0]) ? name : "",
                            (unsigned long long)WAIT_TICKS_MAX);
            } else
#endif
            {
                warn_wait(what, name);
                return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
            }
        }
        uint64_t step = TICK_US;
        if (deadline && deadline - g_vtime_us < step) step = deadline - g_vtime_us;
        if (step == 0) step = 1;
        park_for(kind, step);
    }
}

/* The firmware keeps a block for each thread at the top of its stack and points
 * $k0 at it. Game libc stores its per-thread reent at k0+4, and with $k0 left at
 * zero that write lands at address 4. The thread's uid sits at k0+0xC0. */
#define THREAD_BLOCK_SIZE 256u

static void set_thread_stack(const psp_thread *t) {
    const uint32_t block = (t->stack_base + t->stack_size - THREAD_BLOCK_SIZE) & ~15u;
    for (uint32_t i = 0; i < THREAD_BLOCK_SIZE; i += 4) psp_write32(block + i, 0);
    psp_write32(block + 0xC0u, t->uid);
    psp_cpu.r[26] = block;                  /* $k0 */
    psp_cpu.r[PSP_REG_SP] = block;
}

#if PSPRECOMP_REAL_THREADS
static void *guest_thread_main(void *arg) {
    psp_thread *t = (psp_thread *)arg;

    baton_acquire();                        /* wait for our turn on the CPU */
    g_tls_me = t;
    g_current = t;
    t->us_started = host_deadline_us();
    memset(t->us_waiting, 0, sizeof t->us_waiting);
    t->us_queueing = 0;

    /* A fresh register file: a thread must not see the leftovers of whatever
     * ran before it. $sp and the o32 argument registers are the firmware's to
     * set, everything else starts at zero. */
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_GP] = t->gp;          /* as the kernel does: globals are $gp-relative */
    psp_cpu.r[PSP_REG_A0] = t->arglen;
    psp_cpu.r[PSP_REG_A1] = t->argp;
    /* Stack pointer starts at the top of the allocation, 16-byte aligned, with
     * a little headroom so a callee storing below $sp cannot run off the end. */
    set_thread_stack(t);
    psp_cpu.r[PSP_REG_RA] = 0;

    t->state = TH_RUNNING;
    /* We have the CPU: the flag that says "started, never run" is cleared by the
     * thread itself, and whoever was holding a park open for it is told. */
    host_mutex_lock(&g_baton_m);
    t->cpu_pending = 0;
    host_cond_broadcast(&g_baton_cv);
    host_mutex_unlock(&g_baton_m);
    g_started++;
    fprintf(stderr, "thread %u started : entry 0x%08X\n", t->uid, t->entry);

    if (setjmp(t->unwind) == 0) {
        t->unwind_set = 1;
        check_terminated(); /* A request can arrive before our first CPU turn. */
        psp_call_from_host(t->entry);
        t->exit_status = psp_cpu.r[PSP_REG_V0];
    }
    /* Landing here with a nonzero setjmp result means the thread was unwound --
     * by ExitThread, ExitDeleteThread or TerminateThread, each of which set
     * exit_status on the way in. */
    t->unwind_set = 0;
    t->exited = 1;
    t->state = TH_DORMANT;
    /* Where it ended, not just that it did. A thread body that returns early
     * leaves the flag it was supposed to arm unset, and the address is what
     * says whether it finished or simply fell out somewhere. */
    fprintf(stderr, "thread %u ended   : status 0x%08X, last fn 0x%08X\n",
            t->uid, t->exit_status, psp_trace_last());

    if (t->delete_after && t->stack_base) {
        psp_sysmem_release(t->stack_base);
        t->stack_base = 0;
    }
    g_current = NULL;
    g_tls_me = NULL;
    wake_waiters(W_THREAD_END);
    t->host_live = 0;

    /* Give the CPU away for the last time and end this host thread. */
    g_tls_depth = 1;
    baton_release();
    return NULL;
}

#endif

/* The departing host must never publish a reusable record: it still uses it
 * while releasing the baton. Reap only from another CPU owner, after joining
 * the host, and keep records pinned by a parked WaitThreadEnd caller. */
static void reap_deleted_threads(void) {
    for (int i = 0; i < MAX_THREADS - 1; i++) {
        psp_thread *t = &g_thread[i];
        if (!t->used || !t->delete_after || !t->exited || t->end_waiters) continue;
#if PSPRECOMP_REAL_THREADS
        if (t->host_live) continue;
        if (t->host_joinable) {
            host_thread_join(t->host);
            t->host_joinable = 0;
        }
        if (t->wlock_ready) {
            host_cond_destroy(&t->wcv);
            host_mutex_destroy(&t->wlock);
            t->wlock_ready = 0;
        }
#endif
        t->used = 0;
    }
}

static void hle_CreateThread(void) {
    /* (name, entry, priority, stackSize, attr, option) */
    reap_deleted_threads();
    psp_thread *t = NULL;
    for (int i = 0; i < MAX_THREADS - 1; i++)
        if (!g_thread[i].used) { t = &g_thread[i]; break; }
    if (!t) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(t, 0, sizeof *t);
    psp_str(psp_arg(0), t->name, sizeof t->name);
    t->entry      = psp_arg(1);
    t->priority   = psp_arg(2);
    t->stack_size = psp_arg(3);
    t->attr       = psp_arg(4);
    t->gp         = psp_cpu.r[PSP_REG_GP];

    if (t->stack_size < 0x1000) t->stack_size = 0x1000;
    /* Stacks grow down, so allocate from the top of the heap: a stack that
     * overflows then runs into free space rather than into another block. */
    t->stack_base = psp_sysmem_alloc(t->stack_size, 1);
    if (!t->stack_base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    t->uid = g_next_uid++;
    t->state = TH_DORMANT;
    t->used = 1;

#if PSPRECOMP_REAL_THREADS
    host_mutex_init(&t->wlock);
    host_cond_init(&t->wcv);
    t->wlock_ready = 1;
#endif
    g_created++;
    fprintf(stderr, "thread %u created : entry 0x%08X stack %u  (%s)\n",
            t->uid, t->entry, t->stack_size, t->name);
    psp_ret(t->uid);
}

static void hle_StartThread(void) {
    /* (thid, arglen, argp) */
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
#if PSPRECOMP_REAL_THREADS
    if (t->host_live) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t->host_joinable) {
        host_thread_join(t->host);
        t->host_joinable = 0;
    }
#else
    if (t->state == TH_RUNNING) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    psp_cpu_state saved = psp_cpu;
    psp_thread *prev = g_current;
    psp_thread *prev_me = g_tls_me;
    int prev_intr = g_tls_intr_depth;
    g_tls_intr_depth = 0;
    g_current = g_tls_me = t;
    psp_cpu.r[PSP_REG_A0] = psp_arg(1);
    psp_cpu.r[PSP_REG_A1] = psp_arg(2);
    set_thread_stack(t);
    psp_cpu.r[PSP_REG_RA] = 0;
    t->state = TH_RUNNING;
    t->exited = t->terminated = t->delete_after = 0;
    g_started++;
    if (setjmp(t->unwind) == 0) {
        t->unwind_set = 1;
        psp_call_from_host(t->entry);
        t->exit_status = psp_cpu.r[PSP_REG_V0];
    }
    t->unwind_set = 0;
    t->exited = 1;
    t->state = TH_DORMANT;
    if (t->delete_after && t->stack_base) {
        psp_sysmem_release(t->stack_base);
        t->stack_base = 0;
    }
    g_current = prev;
    g_tls_me = prev_me;
    g_tls_intr_depth = prev_intr;
    psp_cpu = saved;
    psp_ret(SCE_KERNEL_ERROR_OK);
    return;
#endif
#if PSPRECOMP_REAL_THREADS

    t->arglen = psp_arg(1);
    t->argp   = psp_arg(2);
    t->state  = TH_READY;
    /* Set before the host thread exists, so a park that happens between here and
     * the new thread's first acquisition hands the CPU over instead of racing
     * for it. That window is the whole reason this flag is not just `state`. */
    t->cpu_pending = 1;

    t->exited = t->terminated = t->delete_after = 0;
    int rc = host_thread_create(&t->host, HOST_STACK_BYTES, guest_thread_main, t);
    if (rc != 0) {
        t->state = TH_DORMANT;
        t->cpu_pending = 0;
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    t->host_joinable = 1;
    t->host_live = 1;

    /* The caller goes on running: the new thread waits for the CPU, and gets it
     * at the caller's next blocking call. Unless it outranks the caller, which on
     * hardware means it runs now. A caller may spin until it has, and nothing
     * else would ever take the CPU away. Lower numbers are higher priority. */
    if (g_current && t->priority < g_current->priority) park_for(W_DELAY, TICK_US);
    psp_ret(SCE_KERNEL_ERROR_OK);
#endif
}

static void hle_ExitThread(void) {
    uint32_t status = psp_arg(0);
    if (g_current && g_current->unwind_set) {
        thread_unwind(g_current, status);
    }
    /* Nothing to unwind to: the module called ExitThread from its entry rather
     * than from a started thread. Nothing further can run. */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ExitDeleteThread(void) {
    if (g_current) g_current->delete_after = 1;
    hle_ExitThread();
}

static void hle_TerminateThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t == g_current) {                      /* firmware refuses this too */
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID);
        return;
    }
    t->terminated = 1;
    fprintf(stderr, "thread %u terminated by %u\n", t->uid,
            g_current ? g_current->uid : 0);
    wake_thread(t);
    wake_waiters(W_THREAD_END);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_DeleteThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    /* A running thread keeps its stack: it is still using it. */
    if (t->state == TH_DORMANT && t->stack_base) { psp_sysmem_release(t->stack_base); t->stack_base = 0; }
#if PSPRECOMP_REAL_THREADS
    if (!t->host_live) {
        if (t->host_joinable) { host_thread_join(t->host); t->host_joinable = 0; }
        if (t->wlock_ready) {
            host_cond_destroy(&t->wcv);
            host_mutex_destroy(&t->wlock);
            t->wlock_ready = 0;
        }
        t->used = 0;
    }
#else
    if (t->state != TH_RUNNING) t->used = 0;
#endif
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Park until `timeout_us` of virtual time have passed, delivering one
 * interrupt per tick. A bounded park is what DelayThread is; a sleep is the
 * same shape with a wakeup as its exit. */
static void park_ticks(uint64_t us, int kind) {
    if (g_realtime) {
        const uint64_t end = host_now_us() + (us ? us : 1);
        for (uint64_t now = host_now_us(); now < end; now = host_now_us())
            park_real(kind, end - now);
        return;
    }
    uint64_t left = us ? us : TICK_US;
    while (left) {
        uint64_t step = left < TICK_US ? left : TICK_US;
        park_for(kind, step);
        left -= step;
    }
}

/* One wait, one delivery -- the same contract as every other wait point here,
 * so a handler's call count stays a readable number rather than depending on
 * how long the guest asked to sleep. */
void psp_threadman_delay(uint64_t us) { park_ticks(us, W_DELAY); }

static void hle_DelayThread(void) {
    park_ticks(psp_arg(0), W_DELAY);
    deliver_tick();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelSleepThread[CB]: park until sceKernelWakeupThread.
 *
 * Capped at a tick, and that cap is the difference between a game that
 * advances and one that stops: a sleep that nothing ever wakes, inside a
 * `while (!flag) sleep();` poll, is indistinguishable from a hang -- and a
 * hang produces no evidence at all. One tick per iteration means the other
 * threads get the CPU and any interrupt handler gets to run, so what the poll
 * is waiting for can still be produced. */
static void hle_SleepThread(void) {
    psp_thread *t = g_tls_me;
    int already_woken = t && t->wakeups > 0;

    if (!already_woken) park_for(W_SLEEP, TICK_US);
    if (t && t->wakeups) t->wakeups--;      /* consume it, pending or fresh */
    deliver_tick();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WakeupThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->wakeups++;
    wake_thread(t);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_CancelWakeupThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    uint64_t had = t->wakeups;
    t->wakeups = 0;
    psp_ret((uint32_t)had);
}

static int end_reached(void *p) {
    psp_thread *t = ((struct end_ctx *)p)->t;
    return t->exited;
}

static void wait_thread_end(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    uint32_t out = psp_arg(1), timeout = psp_arg(2);
    struct end_ctx c = { t, g_tls_me ? g_tls_me->end_waits : NULL };

    t->end_waiters++;
    if (g_tls_me) g_tls_me->end_waits = &c;
    uint32_t rc = wait_loop(end_reached, &c, timeout, W_THREAD_END,
                            "sceKernelWaitThreadEnd", t->name);
    if (out) psp_write32(out, t->exit_status);
    t->end_waiters--;
    if (g_tls_me) g_tls_me->end_waits = c.prev;
    psp_ret(rc);
}

static void hle_WaitThreadEnd(void) { wait_thread_end(); }

static void hle_GetThreadExitStatus(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    uint32_t out = psp_arg(1);
    if (out) psp_write32(out, t->exit_status);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadId(void) { psp_ret(g_current ? g_current->uid : 0); }

static void hle_SuspendThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->state = TH_SUSPENDED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ResumeThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->state = TH_READY;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ChangeThreadPriority(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->priority = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadCurrentPriority(void) {
    psp_ret(g_current ? g_current->priority : 0);
}

static void hle_ChangeCurrentThreadAttr(void) {
    if (g_current) g_current->attr = (g_current->attr & ~psp_arg(0)) | psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadStackFreeSize(void) {
    /* Real firmware walks the stack looking for the fill pattern. We do not
     * paint one, so report the whole stack: it is used for "am I close to
     * overflowing", and claiming plenty of room is the safe direction. */
    psp_thread *t = find_thread(psp_arg(0));
    psp_ret(t ? t->stack_size : (g_current ? g_current->stack_size : 0));
}

/* ---- the clock ----------------------------------------------------------- */

/* Kept ahead of the epoch the run started at, so a game that compares two
 * readings -- or against a value it stored at startup -- never sees time go
 * backwards, and so the value is in the same range a real clock would give. */
/* Under real threads the guest's clock is the real one. Virtual time only moves
 * at parks, so a thread polling it without waiting would watch it stand still,
 * and music keeps real time whatever the scheduler does. */
static uint64_t now_us(void) {
#if PSPRECOMP_REAL_THREADS
    return host_now_us();
#else
    return g_epoch_us + g_vtime_us;
#endif
}

static void hle_GetSystemTime(void) {
    uint32_t out = psp_arg(0);
    uint64_t us = now_us();
    if (out) {
        psp_write32(out, (uint32_t)us);
        psp_write32(out + 4, (uint32_t)(us >> 32));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetSystemTimeWide(void) {
    uint64_t us = now_us();
    psp_cpu.r[PSP_REG_V0] = (uint32_t)us;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(us >> 32);
}

static void hle_TerminateDeleteThread(void) {
    hle_TerminateThread();
    if (psp_cpu.r[PSP_REG_V0] == SCE_KERNEL_ERROR_OK) hle_DeleteThread();
}

/* ---- semaphores ---------------------------------------------------------- */

static psp_sema *find_sema(uint32_t id) {
    for (int i = 0; i < MAX_SEMAS; i++)
        if (g_sema[i].used && g_sema[i].uid == id) return &g_sema[i];
    return NULL;
}

static void hle_CreateSema(void) {
    /* (name, attr, initVal, maxVal, option) */
    psp_sema *s = NULL;
    for (int i = 0; i < MAX_SEMAS; i++) if (!g_sema[i].used) { s = &g_sema[i]; break; }
    if (!s) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(s, 0, sizeof *s);
    psp_str(psp_arg(0), s->name, sizeof s->name);
    s->count     = (int32_t)psp_arg(2);
    s->max_count = (int32_t)psp_arg(3);
    s->uid = g_next_uid++;
    s->used = 1;
    psp_ret(s->uid);
}

static void hle_DeleteSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    s->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SignalSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    s->count += (int32_t)psp_arg(1);
    if (s->max_count > 0 && s->count > s->max_count) s->count = s->max_count;
    wake_waiters(W_SEMA);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

struct sema_ctx { psp_sema *s; int32_t need; };
static int sema_ready(void *p) {
    struct sema_ctx *c = (struct sema_ctx *)p;
    return c->s->count >= c->need;
}

static uint32_t take_sema(uint32_t timeout) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) return SCE_KERNEL_ERROR_UNKNOWN_UID;
    struct sema_ctx c = { s, (int32_t)psp_arg(1) };
    if (c.need < 1) c.need = 1;

    uint32_t rc = wait_loop(sema_ready, &c, timeout, W_SEMA,
                            "sceKernelWaitSema", s->name);
    if (rc == SCE_KERNEL_ERROR_OK) s->count -= c.need;
    return rc;
}

static void hle_WaitSema(void)   { psp_ret(take_sema(psp_arg(2))); }

/* The poll variant never blocks: it is the whole difference, and a game that
 * switches to it must not be parked. */
static void hle_PollSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    int32_t need = (int32_t)psp_arg(1);
    if (need < 1) need = 1;
    if (s->count < need) { psp_ret(SCE_KERNEL_ERROR_SEMA_ZERO); return; }
    s->count -= need;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Cancelling a semaphore releases its waiters. They are not told they were
 * cancelled -- the count they were waiting for is what they re-test, which is
 * the part of the contract that decides whether they proceed. */
static void hle_CancelSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    s->count = (int32_t)psp_arg(1);
    uint32_t out = psp_arg(2);
    if (out) psp_write32(out, 0);
    wake_waiters(W_SEMA);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- event flags --------------------------------------------------------- */

#define PSP_EVENT_WAITAND   0x00
#define PSP_EVENT_WAITOR    0x01
#define PSP_EVENT_WAITCLEAR 0x20

static psp_evflag *find_flag(uint32_t id) {
    for (int i = 0; i < MAX_FLAGS; i++)
        if (g_flag[i].used && g_flag[i].uid == id) return &g_flag[i];
    return NULL;
}

static void hle_CreateEventFlag(void) {
    /* (name, attr, bits, option) */
    psp_evflag *f = NULL;
    for (int i = 0; i < MAX_FLAGS; i++) if (!g_flag[i].used) { f = &g_flag[i]; break; }
    if (!f) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(f, 0, sizeof *f);
    psp_str(psp_arg(0), f->name, sizeof f->name);
    f->pattern = psp_arg(2);
    f->uid = g_next_uid++;
    f->used = 1;
    psp_ret(f->uid);
}

static void hle_DeleteEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    f->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    f->pattern |= psp_arg(1);
    wake_waiters(W_FLAG);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ClearEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    /* The argument is a mask of bits to KEEP, not bits to clear. Getting this
     * backwards leaves a game waiting on a flag that never clears. */
    f->pattern &= psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

struct flag_ctx { psp_evflag *f; uint32_t bits, mode; };
static int flag_ready(void *p) {
    struct flag_ctx *c = (struct flag_ctx *)p;
    return (c->mode & PSP_EVENT_WAITOR) ? (c->f->pattern & c->bits) != 0
                                        : (c->f->pattern & c->bits) == c->bits;
}

static uint32_t wait_flag(uint32_t timeout) {
    /* (evfid, bits, wait mode, outBits, timeout) */
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) return SCE_KERNEL_ERROR_UNKNOWN_UID;

    struct flag_ctx c = { f, psp_arg(1), psp_arg(2) };
    uint32_t out = psp_arg(3);

    uint32_t rc = wait_loop(flag_ready, &c, timeout, W_FLAG,
                            "sceKernelWaitEventFlag", f->name);
    if (out) psp_write32(out, f->pattern);
    if (rc == SCE_KERNEL_ERROR_OK && (c.mode & PSP_EVENT_WAITCLEAR))
        f->pattern &= ~c.bits;
    return rc;
}

static void hle_WaitEventFlag(void) { psp_ret(wait_flag(psp_arg(4))); }

static void hle_PollEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    struct flag_ctx c = { f, psp_arg(1), psp_arg(2) };
    uint32_t out = psp_arg(3);
    if (out) psp_write32(out, f->pattern);
    if (!flag_ready(&c)) { psp_ret(SCE_KERNEL_ERROR_EVF_COND); return; }
    if (c.mode & PSP_EVENT_WAITCLEAR) f->pattern &= ~c.bits;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- variable-size pools -------------------------------------------------
 *
 * A VPL is a memory pool a game allocates blocks out of. This one is backed by
 * the raw user-memory allocator -- the same one the thread stacks come from --
 * and the pool size is accounting rather than a reservation: the host heap is
 * one flat allocator, and carving a second one out of it would only make a
 * failure harder to see.
 *
 * LocoRoco is what put this here. It creates a pool, asks it for a block, and
 * reads the result; with `sceKernelCreateVpl` unimplemented and therefore
 * returning zero, "create failed" and "pool 0" are the same value, so the game
 * walked a list rooted at a handle that does not exist. A zero that is also a
 * valid answer is the failure mode this whole layer has to avoid. */
static psp_vpl *find_vpl(uint32_t id) {
    for (int i = 0; i < MAX_VPLS; i++)
        if (g_vpl[i].used && g_vpl[i].uid == id) return &g_vpl[i];
    return NULL;
}

static void hle_CreateVpl(void) {
    /* (name, partitionid, attr, size, option) */
    psp_vpl *v = NULL;
    for (int i = 0; i < MAX_VPLS; i++) if (!g_vpl[i].used) { v = &g_vpl[i]; break; }
    if (!v) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(v, 0, sizeof *v);
    psp_str(psp_arg(0), v->name, sizeof v->name);
    v->attr      = psp_arg(2);
    v->pool_size = psp_arg(3);
    v->free_size = v->pool_size;
    v->uid  = g_next_uid++;
    v->used = 1;
    psp_ret(v->uid);
}

static void hle_DeleteVpl(void) {
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    for (int i = 0; i < MAX_VPL_BLOCKS; i++) {
        if (g_vplblk[i].used && g_vplblk[i].vpl == v->uid) {
            psp_sysmem_release(g_vplblk[i].addr);
            g_vplblk[i].used = 0;
        }
    }
    v->used = 0;
    wake_waiters(W_VPL);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_FreeVpl(void) {
    /* (vplid, addr) */
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    uint32_t addr = psp_arg(1);
    for (int i = 0; i < MAX_VPL_BLOCKS; i++) {
        if (g_vplblk[i].used && g_vplblk[i].vpl == v->uid && g_vplblk[i].addr == addr) {
            psp_sysmem_release(addr);
            v->free_size += g_vplblk[i].size;
            g_vplblk[i].used = 0;
            wake_waiters(W_VPL);
            psp_ret(SCE_KERNEL_ERROR_OK);
            return;
        }
    }
    /* A block the pool never handed out. Saying so is better than a silent
     * success: the caller's next allocation would then be accounted wrongly. */
    psp_ret(SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK);
}

struct vpl_ctx { psp_vpl *v; uint32_t size; };
static int vpl_room(void *p) {
    struct vpl_ctx *c = (struct vpl_ctx *)p;
    return c->v->free_size >= c->size;
}

static uint32_t take_vpl_block(int blocking) {
    /* (vplid, size, void **addr, unsigned *timeout) -- the timeout is a
     * pointer, as the firmware declares it, and a null one means "wait". */
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) return SCE_KERNEL_ERROR_UNKNOWN_UID;

    uint32_t size = psp_arg(1);
    uint32_t outp = psp_arg(2);
    uint32_t tptr = psp_arg(3);
    uint32_t timeout = tptr ? psp_read32(tptr) : 0;
    if (!size) size = 1;
    if (!outp) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    struct vpl_ctx c = { v, size };
    if (blocking) {
        uint32_t rc = wait_loop(vpl_room, &c, timeout, W_VPL,
                                "sceKernelAllocateVpl", v->name);
        if (rc != SCE_KERNEL_ERROR_OK) return rc;
    } else if (!vpl_room(&c)) {
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;   /* the try variant never blocks */
    }

    uint32_t addr = psp_sysmem_alloc(size, 0);
    if (!addr) return SCE_KERNEL_ERROR_NO_MEMORY;

    for (int i = 0; i < MAX_VPL_BLOCKS; i++) {
        if (!g_vplblk[i].used) {
            g_vplblk[i].vpl = v->uid;
            g_vplblk[i].addr = addr;
            g_vplblk[i].size = size;
            g_vplblk[i].used = 1;
            v->free_size -= size;
            psp_write32(outp, addr);
            return SCE_KERNEL_ERROR_OK;
        }
    }
    psp_sysmem_release(addr);
    return SCE_KERNEL_ERROR_NO_MEMORY;
}

static void hle_AllocateVpl(void)    { psp_ret(take_vpl_block(1)); }
static void hle_TryAllocateVpl(void) { psp_ret(take_vpl_block(0)); }

static void hle_ReferVplStatus(void) {
    /* (vplid, SceKernelVplInfo *) -- size, name[32], attr, poolSize, freeSize,
     * numWaitThreads. The first field is the caller's own struct size and is
     * left alone, as the kernel leaves it. */
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    uint32_t info = psp_arg(1);
    if (!info) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }

    for (int i = 0; i < 32; i++) psp_write8(info + 4 + (uint32_t)i, (uint8_t)v->name[i]);
    psp_write32(info + 36, v->attr);
    psp_write32(info + 40, v->pool_size);
    psp_write32(info + 44, v->free_size);
    psp_write32(info + 48, 0);                 /* threads waiting on this pool */
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_CancelVpl(void) {
    psp_vpl *v = find_vpl(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    uint32_t pnum = psp_arg(1);
    if (pnum) psp_write32(pnum, 0);
    wake_waiters(W_VPL);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- callbacks -----------------------------------------------------------
 *
 * A callback is deferred work a thread has asked to run on itself: the guest
 * creates it, something notifies it, and it runs when its *own* thread next
 * reaches a point where it can be disturbed -- a wait, or an explicit
 * sceKernelCheckCallback. That ownership is the whole shape of the mechanism,
 * and it is why a callback is not an interrupt handler: an interrupt is
 * delivered to every enabled handler regardless of which thread is running,
 * while a callback waits for the thread that created it.
 *
 * It is also what stopped the game: `sceKernelCheckCallback` came back
 * unimplemented, so a load loop that checks callbacks between attempts had
 * nothing to check -- the notification was never delivered because there was no
 * delivery. LocoRoco is where this was measured: the function is reached from a
 * failed sceIoOpen, six times over. */

static psp_callback *find_callback(uint32_t id) {
    for (int i = 0; i < MAX_CBS; i++)
        if (g_cb[i].used && g_cb[i].uid == id) return &g_cb[i];
    return NULL;
}

/* Whether an id names a callback the guest has actually created. A subsystem
 * that takes a callback id (scePower's slots, and sceUmdUser's drive callback)
 * has to ask: recording an id that names nothing is how a registration becomes
 * a lie, and the lie surfaces much later, as a callback that never runs. */
int psp_callback_valid(uint32_t uid) { return find_callback(uid) != NULL; }

/* One delivery pass over the calling thread's pending callbacks. Returns how
 * many ran, which is what sceKernelCheckCallback reports as 1 or 0 -- the two
 * answers a guest uses to decide whether to keep polling.
 *
 * Guarded against re-entrance: a callback that notifies another callback (or
 * itself) must not recurse, or a game with a self-notifying callback would eat
 * the host stack instead of running. Not delivered inside a critical section
 * either -- the guest has said what it is doing cannot be disturbed, and this is
 * the same rule the interrupt path follows. */
#define CB_DELIVER_MAX 16
static int deliver_callbacks(void) {
    if (g_cb_delivering) return 0;
    if (psp_threadman_intr_disabled()) return 0;

    uint32_t me = g_tls_me ? g_tls_me->uid : 0;
    int ran = 0;
    g_cb_delivering = 1;

    for (int i = 0; i < MAX_CBS && ran < CB_DELIVER_MAX; i++) {
        psp_callback *c = &g_cb[i];
        if (!c->used || !c->notify_count || c->owner != me) continue;

        /* The function pointer is a guest address: a callback the discovery
         * never found cannot be called, and saying so is the difference between
         * a missing function and a delivery that silently did nothing. */
        if (!psp_lookup(c->func)) {
            g_cb_unresolved++;
            if (g_cb_unresolved_logged < 4) {
                g_cb_unresolved_logged++;
                fprintf(stderr,
                        "psprecomp: callback '%s' (uid %u) points at 0x%08X, which is not a"
                        " recompiled function -- notification dropped\n",
                        c->name, c->uid, c->func);
            }
            c->notify_count = 0;
            continue;
        }

        uint32_t arg = c->notify_arg;
        c->notify_count--;
        c->notify_arg = 0;

        /* A real callback does not disturb what it interrupted, so the whole
         * register file is saved and restored around it -- the same contract as
         * an interrupt handler, and the same one a thread body gets on entry. */
        psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_A0] = c->arg;      /* what the guest passed at create time */
        psp_cpu.r[PSP_REG_A1] = arg;         /* and what the notification carried */
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_call_from_host(c->func);
        c->delivery_status = (int32_t)psp_cpu.r[PSP_REG_V0];
        psp_cpu = saved;

        g_cb_deliveries++;
        ran++;
    }

    /* Anything left over is a notification we could not deliver in one pass:
     * counted, so a callback that keeps re-arming itself is visible rather than
     * silently dropped. */
    if (ran == CB_DELIVER_MAX) g_cb_deliver_deferred++;

    g_cb_delivering = 0;
    return ran;
}

/* Called at every point where the guest waits, next to the interrupt delivery:
 * "time passes, another thread runs, deferred work may run" is one thing, and
 * the five wait points must not drift apart on it. */
static void deliver_tick(void) {
    check_terminated();
    /* In real time, interrupts come from the host's own vblank, not from waits. */
    if (!g_realtime) psp_intr_tick();
    psp_ge_deliver();
    deliver_callbacks();
}

static void hle_CreateCallback(void) {
    psp_callback *c = NULL;
    for (int i = 0; i < MAX_CBS; i++) if (!g_cb[i].used) { c = &g_cb[i]; break; }
    if (!c) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(c, 0, sizeof *c);
    psp_str(psp_arg(0), c->name, sizeof c->name);
    c->func  = psp_arg(1);
    c->arg   = psp_arg(2);
    c->owner = g_tls_me ? g_tls_me->uid : 0;
    c->uid   = g_next_uid++;
    c->used  = 1;
    psp_ret(c->uid);
}

static void hle_DeleteCallback(void) {
    psp_callback *c = find_callback(psp_arg(0));
    if (!c) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    c->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_NotifyCallback(void) {
    /* (cbid, arg2). Queues work on the callback's own thread; it does not run
     * here, because "here" may be another thread entirely. */
    psp_callback *c = find_callback(psp_arg(0));
    if (!c) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    c->notify_arg = psp_arg(1);
    if (c->notify_count < 0x7FFFFFFF) c->notify_count++;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_CheckCallback(void) {
    /* The explicit check point, and the one a game uses to decide whether to
     * keep polling: 1 if a callback ran, 0 if none was pending. */
    psp_ret(deliver_callbacks() ? 1u : 0u);
}

uint64_t psp_threadman_callback_deliveries(void) { return g_cb_deliveries; }
uint64_t psp_threadman_callback_unresolved(void) { return g_cb_unresolved; }
uint64_t psp_threadman_callback_deferred(void) { return g_cb_deliver_deferred; }

uint64_t psp_threadman_created(void) { return g_created; }
uint64_t psp_threadman_started(void) { return g_started; }

void psp_threadman_register(void) {
    /* NIDs are SHA-1(name)[0:4] little-endian; tests/test_hle.c verifies every
     * pair below. */
    psp_hle_register(0x446D8DE6, "ThreadManForUser", "sceKernelCreateThread",            hle_CreateThread);
    psp_hle_register(0xF475845D, "ThreadManForUser", "sceKernelStartThread",             hle_StartThread);
    psp_hle_register(0xAA73C935, "ThreadManForUser", "sceKernelExitThread",              hle_ExitThread);
    psp_hle_register(0x809CE29B, "ThreadManForUser", "sceKernelExitDeleteThread",        hle_ExitDeleteThread);
    psp_hle_register(0x616403BA, "ThreadManForUser", "sceKernelTerminateThread",         hle_TerminateThread);
    psp_hle_register(0x9FA03CD3, "ThreadManForUser", "sceKernelDeleteThread",            hle_DeleteThread);
    psp_hle_register(0xCEADEB47, "ThreadManForUser", "sceKernelDelayThread",             hle_DelayThread);
    psp_hle_register(0x68DA9E36, "ThreadManForUser", "sceKernelDelayThreadCB",           hle_DelayThread);
    psp_hle_register(0x9ACE131E, "ThreadManForUser", "sceKernelSleepThread",             hle_SleepThread);
    psp_hle_register(0x82826F70, "ThreadManForUser", "sceKernelSleepThreadCB",           hle_SleepThread);
    psp_hle_register(0xD59EAD2F, "ThreadManForUser", "sceKernelWakeupThread",            hle_WakeupThread);
    psp_hle_register(0xFCCFAD26, "ThreadManForUser", "sceKernelCancelWakeupThread",      hle_CancelWakeupThread);
    psp_hle_register(0x278C0DF5, "ThreadManForUser", "sceKernelWaitThreadEnd",           hle_WaitThreadEnd);
    psp_hle_register(0x840E8133, "ThreadManForUser", "sceKernelWaitThreadEndCB",         hle_WaitThreadEnd);
    psp_hle_register(0x3B183E26, "ThreadManForUser", "sceKernelGetThreadExitStatus",     hle_GetThreadExitStatus);
    psp_hle_register(0x293B45B8, "ThreadManForUser", "sceKernelGetThreadId",             hle_GetThreadId);
    psp_hle_register(0x9944F31F, "ThreadManForUser", "sceKernelSuspendThread",           hle_SuspendThread);
    psp_hle_register(0x75156E8F, "ThreadManForUser", "sceKernelResumeThread",            hle_ResumeThread);
    psp_hle_register(0x71BC9871, "ThreadManForUser", "sceKernelChangeThreadPriority",    hle_ChangeThreadPriority);
    psp_hle_register(0x94AA61EE, "ThreadManForUser", "sceKernelGetThreadCurrentPriority",hle_GetThreadCurrentPriority);
    psp_hle_register(0xEA748E31, "ThreadManForUser", "sceKernelChangeCurrentThreadAttr", hle_ChangeCurrentThreadAttr);
    psp_hle_register(0x52089CA1, "ThreadManForUser", "sceKernelGetThreadStackFreeSize",  hle_GetThreadStackFreeSize);

    psp_hle_register(0xDB738F35, "ThreadManForUser", "sceKernelGetSystemTime",           hle_GetSystemTime);
    psp_hle_register(0x369ED59D, "ThreadManForUser", "sceKernelGetSystemTimeLow",        hle_GetSystemTimeWide);
    psp_hle_register(0x383F7BCC, "ThreadManForUser", "sceKernelTerminateDeleteThread",   hle_TerminateDeleteThread);
    psp_hle_register(0x82BC5777, "ThreadManForUser", "sceKernelGetSystemTimeWide",       hle_GetSystemTimeWide);

    psp_hle_register(0xD6DA4BA1, "ThreadManForUser", "sceKernelCreateSema",              hle_CreateSema);
    psp_hle_register(0x28B6489C, "ThreadManForUser", "sceKernelDeleteSema",              hle_DeleteSema);
    psp_hle_register(0x3F53E640, "ThreadManForUser", "sceKernelSignalSema",              hle_SignalSema);
    psp_hle_register(0x4E3A1105, "ThreadManForUser", "sceKernelWaitSema",                hle_WaitSema);
    psp_hle_register(0x58B1F937, "ThreadManForUser", "sceKernelPollSema",                hle_PollSema);
    psp_hle_register(0x8FFDF9A2, "ThreadManForUser", "sceKernelCancelSema",              hle_CancelSema);

    psp_hle_register(0x55C20A00, "ThreadManForUser", "sceKernelCreateEventFlag",         hle_CreateEventFlag);
    psp_hle_register(0xEF9E4C70, "ThreadManForUser", "sceKernelDeleteEventFlag",         hle_DeleteEventFlag);
    psp_hle_register(0x1FB15A32, "ThreadManForUser", "sceKernelSetEventFlag",            hle_SetEventFlag);
    psp_hle_register(0x812346E4, "ThreadManForUser", "sceKernelClearEventFlag",          hle_ClearEventFlag);
    psp_hle_register(0x328C546A, "ThreadManForUser", "sceKernelWaitEventFlagCB",         hle_WaitEventFlag);
    psp_hle_register(0x402FCF22, "ThreadManForUser", "sceKernelWaitEventFlag",           hle_WaitEventFlag);
    psp_hle_register(0x30FD48F0, "ThreadManForUser", "sceKernelPollEventFlag",           hle_PollEventFlag);

    psp_hle_register(0x56C039B5, "ThreadManForUser", "sceKernelCreateVpl",               hle_CreateVpl);
    psp_hle_register(0x89B3D48C, "ThreadManForUser", "sceKernelDeleteVpl",               hle_DeleteVpl);
    psp_hle_register(0xBED27435, "ThreadManForUser", "sceKernelAllocateVpl",             hle_AllocateVpl);
    psp_hle_register(0xEC0A693F, "ThreadManForUser", "sceKernelAllocateVplCB",           hle_AllocateVpl);
    psp_hle_register(0xAF36D708, "ThreadManForUser", "sceKernelTryAllocateVpl",          hle_TryAllocateVpl);
    psp_hle_register(0xB736E9FF, "ThreadManForUser", "sceKernelFreeVpl",                 hle_FreeVpl);
    psp_hle_register(0x39810265, "ThreadManForUser", "sceKernelReferVplStatus",          hle_ReferVplStatus);
    psp_hle_register(0x1D371B8A, "ThreadManForUser", "sceKernelCancelVpl",               hle_CancelVpl);

    psp_hle_register(0xE81CAF8F, "ThreadManForUser", "sceKernelCreateCallback",          hle_CreateCallback);
    psp_hle_register(0xEDBA5844, "ThreadManForUser", "sceKernelDeleteCallback",          hle_DeleteCallback);
    psp_hle_register(0xC11BA8C4, "ThreadManForUser", "sceKernelNotifyCallback",          hle_NotifyCallback);
    psp_hle_register(0x349D6D6C, "ThreadManForUser", "sceKernelCheckCallback",           hle_CheckCallback);
}
