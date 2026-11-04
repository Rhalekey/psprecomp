/* HLE tests — the firmware layer, with no game data involved.
 *
 * The headline check is the first one: **every registered NID is verified
 * against the SHA-1 of its own declared name.** A PSP NID is defined as the
 * first four bytes of SHA-1(name), so this is not a convention we are choosing
 * to follow — it is the identity the hardware uses, and it makes the whole
 * table self-verifying. A mistyped NID or a wrong function name cannot get
 * past this, which matters because such a mistake produces a game that runs
 * and misbehaves in a way indistinguishable from a codegen bug.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "crypto/sha1.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

/* What the GE callbacks saw. */
static int g_ge_finish_calls, g_ge_signal_calls;
static uint32_t g_ge_cb_a0, g_ge_cb_a1;

static void fake_ge_finish(void) {
    g_ge_finish_calls++;
    g_ge_cb_a0 = psp_cpu.r[PSP_REG_A0];
    g_ge_cb_a1 = psp_cpu.r[PSP_REG_A1];
    psp_cpu.r[PSP_REG_V0] = 0;
}

static void fake_ge_signal(void) {
    g_ge_signal_calls++;
    psp_cpu.r[PSP_REG_V0] = 0;
}

/* Invoke a registered firmware call with o32 arguments. */
static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t a3) {
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_A3] = a3;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

/* Stack arguments live at $sp+16 onward. */
static uint32_t call5(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                      uint32_t a3, uint32_t a4) {
    /* The PSP passes arguments 5-8 in $t0-$t3, not on the stack as o32 does;
     * psp_arg(4) reads $t0. Writing the stack here meant every fifth argument
     * arrived as 0. */
    psp_cpu.r[PSP_REG_T0] = a4;
    return call(nid, a0, a1, a2, a3);
}

static void test_sha1_vectors(void) {
    /* FIPS 180-4 examples, so the hash itself is trusted before anything is
     * built on it. */
    uint8_t d[20];
    char hex[41];

    sha1("abc", 3, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
          "SHA-1(\"abc\"): got %s", hex);

    sha1("", 0, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0,
          "SHA-1(\"\"): got %s", hex);

    sha1("The quick brown fox jumps over the lazy dog", 43, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12") == 0,
          "SHA-1(\"The quick brown fox...\"): got %s", hex);

    /* 56 bytes is the boundary case: the 0x80 terminator fits in the first
     * block but the 8-byte length does not, so padding spills into a second
     * block. An implementation that gets this wrong passes every shorter
     * vector. */
    const char *m = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(strlen(m) == 56, "the two-block padding case is 56 bytes");
    sha1(m, 56, d);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", d[i]);
    CHECK(strcmp(hex, "c2db330f6083854c99d4b5bfb6e8f29f201be699") == 0,
          "SHA-1 of 56 'a's (two-block padding): got %s", hex);
}

/* THE important test: the registered table describes itself correctly. */
static void test_nids_match_names(void) {
    int n = 0;
    const psp_hle_entry *e = psp_hle_entries(&n);
    CHECK(n > 0, "something is registered");

    int checked = 0, unnamed = 0;
    for (int i = 0; i < n; i++) {
        /* A NID may be observed without being identified: the game calls it,
         * the NID is exact, and no plausible name hashes to it. Those are
         * registered unnamed rather than under an invented name, and are
         * skipped here by construction. Inventing a name to satisfy this check
         * would defeat the only thing that makes the table trustworthy. */
        if (!e[i].name) { unnamed++; continue; }        uint32_t want = psp_nid(e[i].name);
        if (want != e[i].nid) {
            printf("FAIL %s::%s\n  registered 0x%08X but SHA-1(name) gives 0x%08X\n",
                   e[i].lib, e[i].name, e[i].nid, want);
            failures++;
        }
        checked++;
    }
    printf("  verified %d NIDs against SHA-1 of their names\n", checked);
}

static void test_sysmem(void) {
    psp_sysmem_reset();

    const uint32_t NID_ALLOC = psp_nid("sceKernelAllocPartitionMemory");
    const uint32_t NID_FREE  = psp_nid("sceKernelFreePartitionMemory");
    const uint32_t NID_HEAD  = psp_nid("sceKernelGetBlockHeadAddr");

    uint32_t before = psp_sysmem_free();

    /* Low placement. */
    uint32_t uid = call5(NID_ALLOC, 2, 0, 0 /*Low*/, 0x1000, 0);
    CHECK(uid >= 0x00010000u, "low alloc returns a UID, got 0x%08X", uid);
    uint32_t lo = call(NID_HEAD, uid, 0, 0, 0);
    CHECK(lo != 0, "block has an address");

    /* High placement must land above the low one -- games depend on the
     * distinction, so it is not enough that both merely succeed. */
    uint32_t uid2 = call5(NID_ALLOC, 2, 0, 1 /*High*/, 0x1000, 0);
    uint32_t hi = call(NID_HEAD, uid2, 0, 0, 0);
    CHECK(hi > lo, "high allocation sits above the low one (lo=0x%08X hi=0x%08X)", lo, hi);

    /* Blocks must not overlap. */
    CHECK(lo + 0x1000 <= hi, "blocks do not overlap");

    /* Sizes round up to the hardware's 256-byte granule. */
    uint32_t uid3 = call5(NID_ALLOC, 2, 0, 0, 100, 0);
    CHECK(uid3 >= 0x00010000u, "a 100-byte request succeeds");
    CHECK(psp_sysmem_free() % 0x100 == 0, "allocations are granule-rounded");

    /* Freeing returns the memory. */
    uint32_t mid = psp_sysmem_free();
    CHECK(call(NID_FREE, uid, 0, 0, 0) == 0, "free succeeds");
    CHECK(psp_sysmem_free() > mid, "freeing returns memory");

    CHECK(call(NID_FREE, 0xDEADBEEF, 0, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_UID,
          "freeing an unknown UID is refused");
    CHECK(call(NID_HEAD, 0xDEADBEEF, 0, 0, 0) == 0,
          "an unknown UID has no address");

    /* An impossible request fails rather than returning a bogus block. */
    uint32_t huge = call5(NID_ALLOC, 2, 0, 0, 0x7F000000u, 0);
    CHECK(huge == SCE_KERNEL_ERROR_NO_MEMORY, "an oversized request fails, got 0x%08X", huge);

    call(NID_FREE, uid2, 0, 0, 0);
    call(NID_FREE, uid3, 0, 0, 0);
    CHECK(psp_sysmem_free() == before, "everything freed restores the heap");
}

static void test_semaphores(void) {
    psp_threadman_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateSema");
    const uint32_t WAIT   = psp_nid("sceKernelWaitSema");
    const uint32_t SIGNAL = psp_nid("sceKernelSignalSema");
    const uint32_t DELETE = psp_nid("sceKernelDeleteSema");

    uint32_t sem = call5(CREATE, 0, 0, 2 /*init*/, 4 /*max*/, 0);
    CHECK(sem != 0, "semaphore created");

    CHECK(call(WAIT, sem, 1, 0, 0) == 0, "wait succeeds while the count allows");
    CHECK(call(WAIT, sem, 1, 0, 0) == 0, "and again, down to zero");
    CHECK(call(WAIT, sem, 1, 0, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "a wait that would block reports a timeout rather than hanging");

    CHECK(call(SIGNAL, sem, 1, 0, 0) == 0, "signal succeeds");
    CHECK(call(WAIT, sem, 1, 0, 0) == 0, "and the signalled count is available");

    /* The count must saturate at max, not run away. */
    call(SIGNAL, sem, 100, 0, 0);
    CHECK(call(WAIT, sem, 4, 0, 0) == 0, "count saturates at max (4 available)");
    CHECK(call(WAIT, sem, 1, 0, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "and no more than max");

    CHECK(call(DELETE, sem, 0, 0, 0) == 0, "delete succeeds");
    CHECK(call(WAIT, sem, 1, 0, 0) == SCE_KERNEL_ERROR_UNKNOWN_UID,
          "a deleted semaphore is gone");
}

static void test_event_flags(void) {
    psp_threadman_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateEventFlag");
    const uint32_t SET    = psp_nid("sceKernelSetEventFlag");
    const uint32_t CLEAR  = psp_nid("sceKernelClearEventFlag");
    const uint32_t WAIT   = psp_nid("sceKernelWaitEventFlag");

    uint32_t ef = call(CREATE, 0, 0, 0x0000, 0);
    CHECK(ef != 0, "event flag created");

    call(SET, ef, 0x0005, 0, 0);

    /* WAITOR is satisfied by any bit; WAITAND needs all of them. */
    CHECK(call5(WAIT, ef, 0x0004, 0x01 /*OR*/, 0, 0) == 0, "OR wait on a set bit");
    CHECK(call5(WAIT, ef, 0x0003, 0x00 /*AND*/, 0, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "AND wait fails when only some bits are set");
    CHECK(call5(WAIT, ef, 0x0005, 0x00 /*AND*/, 0, 0) == 0,
          "AND wait succeeds when all bits are set");

    /* clear takes a mask of bits to KEEP. Getting that backwards leaves a game
     * waiting on a flag that never clears, so it is pinned explicitly. */
    call(CLEAR, ef, ~0x0004u, 0, 0);
    CHECK(call5(WAIT, ef, 0x0004, 0x01, 0, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "the cleared bit is gone");
    CHECK(call5(WAIT, ef, 0x0001, 0x01, 0, 0) == 0,
          "the kept bit survives");
}

/* A recompiled thread entry: writes a marker so the test can prove it ran on
 * the stack the thread manager gave it. */
static uint32_t g_thread_sp;
static uint32_t g_thread_arg;
static int      g_thread_ran;

static void fake_thread_entry(void) {
    g_thread_ran++;
    g_thread_sp  = psp_cpu.r[PSP_REG_SP];
    g_thread_arg = psp_cpu.r[PSP_REG_A0];
    psp_cpu.r[PSP_REG_V0] = 0x1234;      /* exit status */
}

/* An entry that blocks, so the test drives the scheduler rather than only
 * observing that a body ran once. It records how far it got, and the only
 * thing that moves it past the sleep is a wakeup from another thread. */
static int g_sleeper_stage;
static void fake_sleeper_entry(void) {
    g_sleeper_stage = 1;
    call(psp_nid("sceKernelSleepThread"), 0, 0, 0, 0);
    g_sleeper_stage = 2;
}

/* And one that blocks on a semaphore, which is a different wake path. */
static uint32_t g_test_sema;
static int      g_waiter_stage;
static void fake_waiter_entry(void) {
    g_waiter_stage = 1;
    call(psp_nid("sceKernelWaitSema"), g_test_sema, 1, 0 /* no timeout */, 0);
    g_waiter_stage = 2;
}

static void test_threads(void) {
    psp_sysmem_reset();
    psp_threadman_reset();
    psp_dispatch_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateThread");
    const uint32_t START  = psp_nid("sceKernelStartThread");
    const uint32_t WAITEND= psp_nid("sceKernelWaitThreadEnd");
    const uint32_t DELETE = psp_nid("sceKernelDeleteThread");
    const uint32_t GETID  = psp_nid("sceKernelGetThreadId");
    const uint32_t DELAY  = psp_nid("sceKernelDelayThread");
    const uint32_t WAKEUP = psp_nid("sceKernelWakeupThread");
    const uint32_t CREATESEMA = psp_nid("sceKernelCreateSema");
    const uint32_t SIGNAL = psp_nid("sceKernelSignalSema");

    const uint32_t ENTRY = 0x08801000u;
    psp_register(ENTRY, fake_thread_entry);

    uint32_t thid = call5(CREATE, 0 /*name*/, ENTRY, 32 /*prio*/, 0x4000 /*stack*/, 0);
    CHECK(thid != 0, "thread created, got 0x%08X", thid);

    /* The caller's context must survive the thread running. */
    psp_cpu.r[PSP_REG_S0] = 0xC0FFEE;
    uint32_t caller_sp = psp_cpu.r[PSP_REG_SP];

    g_thread_ran = 0;
    uint32_t rc = call(START, thid, 7 /*arglen*/, 0xAAAA, 0);
    CHECK(rc == 0, "start succeeds");
    /* The contract that unblocks a game: StartThread returns with the body not
     * yet entered. A caller that runs the body inline never gets to post the
     * work that body is waiting for. */
#if PSPRECOMP_REAL_THREADS
    CHECK(g_thread_ran == 0, "StartThread returns before the body is entered");
#else
    CHECK(g_thread_ran == 1, "default StartThread completes the body inline");
#endif

    CHECK(psp_cpu.r[PSP_REG_S0] == 0xC0FFEE, "caller's registers restored");

    /* The exit status the thread returned is what a waiter sees -- and waiting
     * is also how the caller gives the CPU away, so the body runs here. */
    uint32_t out = 0x08802000u;
    CHECK(call(WAITEND, thid, out, 0, 0) == 0, "wait-for-end succeeds");
    CHECK(g_thread_ran == 1, "the thread entry ran once the caller gave up the CPU");
    CHECK(g_thread_arg == 7, "argument reached the thread, got %u", g_thread_arg);
    CHECK(g_thread_sp != caller_sp && g_thread_sp != 0,
          "the thread ran on its own stack (0x%08X vs caller 0x%08X)",
          g_thread_sp, caller_sp);
    CHECK((g_thread_sp & 15) == 0, "the thread stack pointer is 16-byte aligned");
    CHECK(psp_read32(out) == 0x1234, "exit status recorded, got 0x%X", psp_read32(out));
    CHECK(psp_cpu.r[PSP_REG_SP] == caller_sp, "caller's stack pointer restored");

    /* Deleting frees the stack. */
    uint32_t before_delete = psp_sysmem_free();
    CHECK(call(DELETE, thid, 0, 0, 0) == 0, "delete succeeds");
    CHECK(psp_sysmem_free() > before_delete, "deleting a thread frees its stack");

    /* Sleep yields for a tick in opt-in mode and returns immediately in inline
     * mode. It is deliberately not faithful indefinite sleep. */
    const uint32_t SLEEPER = 0x08801080u;
    psp_register(SLEEPER, fake_sleeper_entry);
    uint32_t sid = call5(CREATE, 0, SLEEPER, 32, 0x4000, 0);
    g_sleeper_stage = 0;
    CHECK(call(START, sid, 0, 0, 0) == 0, "second thread started");

    /* The delay is a *handover*, not a nap: the CPU is not taken back while a
     * thread that has been started has never run (nor while one is queued for
     * the baton). Asserted through the counter rather than hoped for, because
     * the assertion right after it is only deterministic if the handover
     * happened -- a park that races the host's own scheduler for the CPU gives
     * the OS the choice of whether the guest ever sees this thread run, which is
     * a flake, measured at about one run in ten. */
    uint64_t handovers = psp_threadman_handovers();
    uint64_t started = psp_threadman_started();
    call(DELAY, 1000, 0, 0, 0);        /* the caller parks: the sleeper gets in */
#if PSPRECOMP_REAL_THREADS
    CHECK(psp_threadman_handovers() > handovers,
          "the delay handed the CPU over to the thread that had not run yet");
#else
    CHECK(psp_threadman_handovers() == handovers && g_sleeper_stage == 2,
          "default delay does not schedule; the sleeper already completed inline");
#endif
    CHECK(psp_threadman_handover_expired() == 0,
          "and the handover did not fall back to its cap");
    /* The numbers are in the message on purpose: if this ever fails again, the
     * failing run says what it saw instead of needing a second one.
     *
     * And it is `>= 1`, not `== 1`: after the handover both threads are runnable,
     * which is what hardware does too, so the sleeper may legitimately have got
     * all the way through its sleep before the starter gets the CPU back. What
     * the delay has to guarantee is that the thread *ran* -- asserting where it
     * stopped would assert a race between two equal peers, and it was flaking on
     * exactly that (stage=2, one run in ten) after the handover was in place. */
    CHECK(g_sleeper_stage >= 1,
          "the sleeper ran and then blocked on SleepThread (stage=%d, started %llu->%llu,"
          " handovers %llu->%llu, expired=%llu)",
          g_sleeper_stage, (unsigned long long)started,
          (unsigned long long)psp_threadman_started(),
          (unsigned long long)handovers,
          (unsigned long long)psp_threadman_handovers(),
          (unsigned long long)psp_threadman_handover_expired());

    /* A sleep with nothing to wake it still ends, one tick later. That is the
     * approximation that keeps a `while (!flag) sleep();` poll alive: the
     * faithful alternative is a thread that never returns, and a run that
     * stops with no evidence in it. */
    call(DELAY, 1000, 0, 0, 0);
    CHECK(g_sleeper_stage == 2, "a sleep nothing wakes returns after its tick");

    /* A wakeup is not lost when it arrives before the sleep: a producer that
     * wakes a worker it has not started waiting yet must not have to guess. */
    uint32_t sid2 = call5(CREATE, 0, SLEEPER, 32, 0x4000, 0);
    g_sleeper_stage = 0;
    CHECK(call(START, sid2, 0, 0, 0) == 0, "third thread started");
    CHECK(call(WAKEUP, sid2, 0, 0, 0) == 0, "wakeup succeeds");
    CHECK(call(WAITEND, sid2, 0, 0, 0) == 0, "and that sleeper reaches its end");
    CHECK(g_sleeper_stage == 2, "the pending wakeup is consumed, not stranded");

    /* The same shape over a semaphore: a wait with no timeout blocks, and a
     * signal from another thread is what releases it. */
    const uint32_t WAITER = 0x08801090u;
    psp_register(WAITER, fake_waiter_entry);
    g_test_sema = call5(CREATESEMA, 0, 0, 0 /*init*/, 4 /*max*/, 0);
    CHECK(g_test_sema != 0, "semaphore created for the waiter");
    uint32_t wid = call5(CREATE, 0, WAITER, 32, 0x4000, 0);
    g_waiter_stage = 0;
    CHECK(call(START, wid, 0, 0, 0) == 0, "waiter started");
    call(DELAY, 1000, 0, 0, 0);
#if PSPRECOMP_REAL_THREADS
    CHECK(g_waiter_stage == 1, "the waiter blocked on an empty semaphore");
#else
    CHECK(g_waiter_stage == 2, "default unsatisfied semaphore wait returns inline");
#endif

    CHECK(call(SIGNAL, g_test_sema, 1, 0, 0) == 0, "signal succeeds");
    CHECK(call(WAITEND, wid, 0, 0, 0) == 0, "the waiter reaches its end");
    CHECK(g_waiter_stage == 2, "the signal released it");

    CHECK(call(START, 0xDEADBEEF, 0, 0, 0) == SCE_KERNEL_ERROR_ILLEGAL_THID,
          "starting an unknown thread is refused");
    /* The host thread running the module entry is a thread too: it is the one
     * whose call we are inside, so it has to have an identity of its own. */
    CHECK(call(GETID, 0, 0, 0, 0) != 0,
          "the thread running the module entry has an id");
}

/* Variable-size pools. The value that matters most here is the first one: a
 * created pool must have a handle that is not zero, because a stubbed create
 * returning zero is indistinguishable from a successful create of pool 0 --
 * and that is exactly what stopped LocoRoco in its own initialisation. */
static void test_vpl(void) {
    psp_sysmem_reset();
    psp_threadman_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateVpl");
    const uint32_t ALLOC  = psp_nid("sceKernelAllocateVplCB");
    const uint32_t TRYALLOC = psp_nid("sceKernelTryAllocateVpl");
    const uint32_t FREE   = psp_nid("sceKernelFreeVpl");
    const uint32_t REFER  = psp_nid("sceKernelReferVplStatus");
    const uint32_t DELETE = psp_nid("sceKernelDeleteVpl");
    const uint32_t MAXFREE= psp_nid("sceKernelMaxFreeMemSize");

    const uint32_t NAME = 0x08804000u;
    const char *nm = "sceVplName";
    for (uint32_t i = 0; i <= strlen(nm); i++) psp_write8(NAME + i, (uint8_t)nm[i]);

    uint32_t vpl = call5(CREATE, NAME, 2 /*partition*/, 0, 4096 /*size*/, 0);
    CHECK(vpl != 0, "a created pool has a handle, not zero");

    uint32_t out  = 0x08804100u;
    uint32_t to   = 0x08804110u;
    uint32_t info = 0x08804200u;

    psp_write32(out, 0);
    psp_write32(to, 1000);
    CHECK(call(ALLOC, vpl, 256, out, to) == 0, "a block is handed out");
    uint32_t blk = psp_read32(out);
    CHECK(blk != 0, "and its address is not zero");

    /* The block is real guest memory, not a plausible-looking number. */
    psp_write32(blk, 0xABCDEF01u);
    CHECK(psp_read32(blk) == 0xABCDEF01u, "the block is addressable guest memory");

    psp_write32(info, 52);
    CHECK(call(REFER, vpl, info, 0, 0) == 0, "status is reported");
    CHECK(psp_read32(info + 40) == 4096, "pool size is reported, got %u", psp_read32(info + 40));
    CHECK(psp_read32(info + 44) == 4096 - 256,
          "free size accounts for the allocation, got %u", psp_read32(info + 44));
    char back[32];
    for (int i = 0; i < 32; i++) back[i] = (char)psp_read8(info + 4 + (uint32_t)i);
    CHECK(strcmp(back, nm) == 0, "the name round-trips, got \"%s\"", back);

    /* A request bigger than the pool must not be answered with a block. */
    CHECK(call(ALLOC, vpl, 8192, out, to) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "a request larger than the pool times out rather than succeeding");
    CHECK(call(TRYALLOC, vpl, 8192, out, 0) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "and the try variant never blocks");

    /* Freeing gives the room back, and only a block this pool handed out. */
    CHECK(call(FREE, vpl, blk, 0, 0) == 0, "free succeeds");
    CHECK(call(REFER, vpl, info, 0, 0) == 0 && psp_read32(info + 44) == 4096,
          "freeing gives the room back");
    CHECK(call(FREE, vpl, 0x08805000u, 0, 0) == SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK,
          "a pointer the pool never handed out is refused");

    /* Memory the game can see is not reported as zero. */
    CHECK(call(MAXFREE, 0, 0, 0, 0) > 0, "free memory is reported, not zero");

    CHECK(call(DELETE, vpl, 0, 0, 0) == 0, "delete succeeds");
    CHECK(call(ALLOC, vpl, 16, out, to) == SCE_KERNEL_ERROR_UNKNOWN_UID,
          "a deleted pool is gone");
}

static void test_guest_strings(void) {
    /* Names come out of guest memory, so the reader has to terminate and must
     * not run past the buffer. */
    const uint32_t at = 0x08803000u;
    const char *s = "sceThreadName";
    for (uint32_t i = 0; i <= strlen(s); i++) psp_write8(at + i, (uint8_t)s[i]);

    char buf[32];
    CHECK(strcmp(psp_str(at, buf, sizeof buf), s) == 0, "string round-trips");

    char small[6];
    psp_str(at, small, sizeof small);
    CHECK(strlen(small) == 5 && strncmp(small, s, 5) == 0,
          "an over-long string is truncated, not overflowed: \"%s\"", small);
}

/* Build a display list in guest memory and check the walk follows control flow
 * rather than merely counting words. The list deliberately contains a PRIM
 * that a JUMP skips over: if it gets counted, the walk is not following jumps. */
static void test_ge_display_list(void) {
    psp_ge_reset();

    const uint32_t LIST = 0x08820000u;
    uint32_t w[16], n = 0;

    /* GE addresses are 24-bit; BASE supplies the high byte. Setting it here
     * also exercises that path -- a list whose BASE is ignored jumps to the
     * wrong place and silently executes garbage. */
    w[n++] = (0x10u << 24) | 0x080000;              /* BASE  -> 0x08000000 */
    w[n++] = (0x12u << 24) | 0x000123;              /* VTYPE */
    w[n++] = (0x04u << 24) | (3u << 16) | 6;        /* PRIM triangles, 6 verts */
    w[n++] = (0x08u << 24) | 0x820020;              /* JUMP -> LIST + 0x20 */
    w[n++] = (0x04u << 24) | (0u << 16) | 99;       /* PRIM points -- SKIPPED */
    w[n++] = (0x00u << 24);
    w[n++] = (0x00u << 24);
    w[n++] = (0x00u << 24);
    /* word 8 == LIST + 0x20 */
    w[n++] = (0x04u << 24) | (6u << 16) | 2;        /* PRIM sprites, 2 verts */
    w[n++] = (0x0Fu << 24);                          /* FINISH */
    w[n++] = (0x0Cu << 24);                          /* END */

    for (uint32_t i = 0; i < n; i++) psp_write32(LIST + i * 4, w[i]);

    uint64_t before = psp_ge_command_count();
    /* sceGeListEnQueue(list, stall=0, cbid, arg) */
    uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, 0, 0);
    CHECK(qid != 0, "list enqueued, got 0x%08X", qid);

    uint64_t executed = psp_ge_command_count() - before;
    CHECK(executed == 6, "walked BASE,VTYPE,PRIM,JUMP,PRIM,FINISH = 6, got %llu",
          (unsigned long long)executed);
    CHECK(psp_ge_vertex_count() == 8,
          "counted 6+2 vertices and skipped the jumped-over PRIM, got %llu",
          (unsigned long long)psp_ge_vertex_count());

    CHECK(call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0) == 0, "draw sync succeeds");

    /* A finished list frees its queue slot: a game enqueues one per frame,
     * and there are only a few slots. */
    uint64_t lists_before = psp_ge_command_count();
    int failed = 0;
    for (int f = 0; f < 100; f++) {
        uint32_t id = call(psp_nid("sceGeListEnQueue"), LIST, 0, 0, 0);
        if (id & 0x80000000u) failed++;
    }
    CHECK(failed == 0, "100 finished lists enqueue without running out of slots: %d failed", failed);
    CHECK(psp_ge_command_count() - lists_before == 600,
          "all 100 lists ran: %llu commands", (unsigned long long)(psp_ge_command_count() - lists_before));
    CHECK(call(psp_nid("sceGeListUpdateStallAddr"), qid, LIST, 0, 0) == 0,
          "moving the stall of a finished list is not an error");
    CHECK(call(psp_nid("sceGeEdramGetAddr"), 0, 0, 0, 0) == PSP_VRAM_BASE,
          "eDRAM is the VRAM window");
    CHECK(call(psp_nid("sceGeEdramGetSize"), 0, 0, 0, 0) == PSP_VRAM_SIZE,
          "eDRAM is 2 MB");

    /* The clock is real time, far past year 1, and dates convert to Unix time. */
    {
        const uint32_t DATE = LIST + 0x300, OUT = LIST + 0x320;
        call(psp_nid("sceRtcGetAccumulativeTime"), 0, 0, 0, 0);
        CHECK(psp_cpu.r[PSP_REG_V1] != 0, "accumulative time has its high word");
        psp_write16(DATE + 0, 1970);
        psp_write16(DATE + 2, 1);
        psp_write16(DATE + 4, 2);
        psp_write16(DATE + 6, 0);
        psp_write16(DATE + 8, 0);
        psp_write16(DATE + 10, 1);
        call(psp_nid("sceRtcGetTime64_t"), DATE, OUT, 0, 0);
        CHECK(psp_read32(OUT) == 86401u, "1970-01-02 00:00:01 is 86401, got %u", psp_read32(OUT));
    }

    /* Callbacks arrive after the enqueue returns, and a SIGNAL's END does not
     * end the list: what follows it still has to run. */
    {
        const uint32_t CB = LIST + 0x100, CB_LIST = LIST + 0x200;
        const uint32_t FINISH_FN = 0x08A00000u, SIGNAL_FN = 0x08A00010u;
        psp_register(FINISH_FN, fake_ge_finish);
        psp_register(SIGNAL_FN, fake_ge_signal);
        psp_write32(CB + 0, SIGNAL_FN);
        psp_write32(CB + 4, 0);
        psp_write32(CB + 8, FINISH_FN);
        psp_write32(CB + 12, 0x1234u);
        const uint32_t cbid = call(psp_nid("sceGeSetCallback"), CB, 0, 0, 0);
        CHECK(cbid < 16, "a callback id is issued, got 0x%08X", cbid);

        psp_write32(CB_LIST + 0, (0x0Eu << 24) | 7u);            /* SIGNAL */
        psp_write32(CB_LIST + 4, (0x0Cu << 24));                 /* its END */
        psp_write32(CB_LIST + 8, (0x00u << 24));                 /* NOP */
        psp_write32(CB_LIST + 12, (0x0Fu << 24) | 0x42u);        /* FINISH */
        psp_write32(CB_LIST + 16, (0x0Cu << 24));                /* END */

        uint64_t cb_before = psp_ge_command_count();
        call(psp_nid("sceGeListEnQueue"), CB_LIST, 0, cbid, 0);
        CHECK(psp_ge_command_count() - cb_before == 3, "the list ran past its SIGNAL, %llu commands",
              (unsigned long long)(psp_ge_command_count() - cb_before));
        CHECK(g_ge_finish_calls == 0, "the finish callback waits for the enqueue to return");

        call(psp_nid("sceGeDrawSync"), 0, 0, 0, 0);
        CHECK(g_ge_signal_calls == 1, "the signal callback ran once, got %d", g_ge_signal_calls);
        CHECK(g_ge_finish_calls == 1, "the finish callback ran once, got %d", g_ge_finish_calls);
        CHECK(g_ge_cb_a0 == 0x42u && g_ge_cb_a1 == 0x1234u, "finish(0x%X, 0x%X)", g_ge_cb_a0, g_ge_cb_a1);
        call(psp_nid("sceGeUnsetCallback"), cbid, 0, 0, 0);
    }
}

/* A list that jumps to itself must terminate rather than hang the host -- this
 * is a normal transient state while the CPU is still writing the list. */
static void test_ge_infinite_list(void) {
    psp_ge_reset();
    const uint32_t LIST = 0x08830000u;
    psp_write32(LIST + 0, (0x10u << 24) | 0x080000);   /* BASE */
    psp_write32(LIST + 4, (0x08u << 24) | 0x830000);   /* JUMP to itself */

    uint32_t qid = call(psp_nid("sceGeListEnQueue"), LIST, 0, 0, 0);
    CHECK(qid != 0, "a self-jumping list still returns");
}

static void test_sas_adpcm(void) {
    psp_sas_reset();

    /* One VAG block: shift 8, filter 0, then 28 nibbles. With filter 0 there is
     * no prediction, so each output sample is just the sign-extended nibble
     * scaled -- which makes a decode bug obvious rather than merely quieter. */
    const uint32_t VAG = 0x08840000u;
    psp_write8(VAG + 0, 0x08);        /* shift 8, filter 0 */
    psp_write8(VAG + 1, 0x00);        /* flags: not the end */
    for (uint32_t i = 0; i < 14; i++)
        psp_write8(VAG + 2 + i, 0x7Fu);   /* nibbles 0xF and 0x7 */

    CHECK(call5(psp_nid("__sceSasInit"), 0, 64 /*grain*/, 32, 0, 44100) == 0,
          "SAS init");

    /* sceSasSetVoice(core, voice, addr, size, loop) */
    CHECK(call5(psp_nid("__sceSasSetVoice"), 0, 0, VAG, 16, 0) == 0, "voice set");
    CHECK(call(psp_nid("__sceSasSetVolume"), 0, 0, 0x1000, 0x1000) == 0, "volume set");
    CHECK(call(psp_nid("__sceSasSetPitch"), 0, 0, 0x1000, 0) == 0, "pitch set");

    /* Before key-on nothing should be playing. */
    const uint32_t OUT = 0x08850000u;
    for (uint32_t i = 0; i < 64 * 4; i += 4) psp_write32(OUT + i, 0);
    call(psp_nid("__sceSasCore"), 0, OUT, 0, 0);
    CHECK(psp_sas_nonzero() == 0, "silence before key-on");

    CHECK(call(psp_nid("__sceSasSetKeyOn"), 0, 0, 0, 0) == 0, "key on");
    call(psp_nid("__sceSasCore"), 0, OUT, 0, 0);

    CHECK(psp_sas_frames() == 2, "two frames rendered, got %llu",
          (unsigned long long)psp_sas_frames());
    CHECK(psp_sas_nonzero() > 0, "audio was actually produced after key-on");

    /* The end flag is how a game knows a sound finished; a voice that never
     * reports ended is a common way for audio to stall after the first sound. */
    uint32_t ended = call(psp_nid("__sceSasGetEndFlag"), 0, 0, 0, 0);
    CHECK((ended & ~1u) != 0, "unused voices report ended, got 0x%08X", ended);

    /* The block flags decide whether a voice loops: loop start (4), then an
     * end (1), which repeats only with the repeat bit (2) and loop mode on.
     * Three blocks are 84 samples; at a grain of 64, a voice that ends is gone
     * after two frames, and one that loops is still playing after twenty. */
    static const struct { uint8_t end_flags; uint32_t loop; int want_ended; const char *what; } C[] = {
        { 0x01, 1, 1, "an end without repeat stops, even in loop mode" },
        { 0x03, 1, 0, "an end with repeat loops in loop mode" },
        { 0x03, 0, 1, "an end with repeat stops when loop mode is off" },
    };
    for (int k = 0; k < 3; k++) {
        const uint8_t flags[3] = { 0x04, 0x00, C[k].end_flags };
        for (uint32_t b = 0; b < 3; b++) {
            psp_write8(VAG + b * 16, 0x08);
            psp_write8(VAG + b * 16 + 1, flags[b]);
            for (uint32_t i = 0; i < 14; i++) psp_write8(VAG + b * 16 + 2 + i, 0x7Fu);
        }
        psp_write8(VAG + 48, 0x00);
        psp_write8(VAG + 49, 0x07);        /* terminator block */
        call5(psp_nid("__sceSasSetVoice"), 0, 1, VAG, 64, C[k].loop);
        call(psp_nid("__sceSasSetKeyOn"), 0, 1, 0, 0);
        for (int f = 0; f < 20; f++) call(psp_nid("__sceSasCore"), 0, OUT, 0, 0);
        uint32_t e = call(psp_nid("__sceSasGetEndFlag"), 0, 0, 0, 0);
        CHECK(((e >> 1) & 1u) == (uint32_t)C[k].want_ended, "%s (end flags 0x%08X)", C[k].what, e);
    }
}

static void test_display(void) {
    psp_display_reset();

    CHECK(call(psp_nid("sceDisplaySetMode"), 0, 480, 272, 0) == 0, "set mode");
    /* (topaddr, bufferwidth, pixelformat, sync) */
    CHECK(call(psp_nid("sceDisplaySetFrameBuf"), PSP_VRAM_BASE, 512, 3, 0) == 0,
          "set framebuffer");
    CHECK(psp_display_framebuffer() == PSP_VRAM_BASE, "framebuffer recorded");

    uint64_t v0 = psp_display_vblanks();
    call(psp_nid("sceDisplayWaitVblank"), 0, 0, 0, 0);
    call(psp_nid("sceDisplayWaitVblankStartCB"), 0, 0, 0, 0);
    CHECK(psp_display_vblanks() == v0 + 2,
          "vblank waits advance the frame counter (the bring-up heartbeat)");
}


/* The unsubscribe-from-a-stub case: a game that spins on a call nobody
 * implemented. It made 26 926 971 log lines -- 1.37 GB -- in one 30-second run,
 * for a single call worth naming. */
static void test_unimplemented_logging(void) {
    psp_hle_unimplemented_reset();
    CHECK(psp_hle_unimplemented_calls() == 0, "counter starts at zero");

    /* Relative, because the zero-return total is a running total since start-up,
     * not a per-test counter -- earlier tests have already made calls. */
    uint64_t zero_before = psp_hle_zero_calls();

    for (int i = 0; i < 100000; i++) psp_hle_call(0xDEAD0001u);   /* nobody registered this */

    CHECK(psp_hle_unimplemented_calls() == 100000,
          "every unimplemented call is counted");
    CHECK(psp_hle_unimplemented_logged() <= 9,
          "but only a bounded number are printed (was one per call: 1.37 GB)");
    CHECK(psp_cpu.r[PSP_REG_V0] == 0,
          "and the call still returns 0, as it did before");

    /* The dump keeps a 16-entry ring but counts all of them, and the count has
     * to survive passing the ring -- an `int` here wrapped and printed
     * `last -1396532617 firmware calls`. */
    CHECK(psp_hle_zero_calls() == zero_before + 100000,
          "the zero-return ring's total counts past its own size");

    FILE *f = tmpfile();
    CHECK(f != NULL, "temporary report file created");
    if (f) {
        psp_hle_dump_recent(f);
        rewind(f);
        char buf[1024] = {0};
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        char expect[64];
        snprintf(expect, sizeof expect, "%llu in total",
                 (unsigned long long)(zero_before + 100000));
        CHECK(got > 0 && strstr(buf, expect) != NULL,
              "the dump reports the true total: %s", buf);
        CHECK(strstr(buf, "last 16 firmware calls") != NULL,
              "and caps its listing at the ring size, with no negative count");
    }
}

/* The shape LocoRoco got stuck on: register a handler, enable it, then wait for
 * something the handler arms. The wait has to deliver, because the guest's exit
 * condition is set by the handler -- a wait that returns without delivering is
 * a loop that cannot end (2 900 000 000 iterations, measured). */
static int      g_handler_calls;
static uint32_t g_handler_sub, g_handler_arg;

static void fake_subintr_handler(void) {
    /* Reads its arguments the way a recompiled handler would. */
    g_handler_sub = psp_cpu.r[PSP_REG_A0];
    g_handler_arg = psp_cpu.r[PSP_REG_A1];
    g_handler_calls++;
}

static void fake_subintr_handler_that_waits(void) {
    g_handler_calls++;
    psp_intr_tick();   /* a handler that waits must not recurse */
}

static void test_sub_interrupts(void) {
    psp_intrman_reset();
    g_handler_calls = 0;
    g_handler_sub = g_handler_arg = 0;

    const uint32_t FN = 0x0000A000u;
    psp_register(FN, fake_subintr_handler);

    /* (intno, no, handler, arg) */
    CHECK(call(psp_nid("sceKernelRegisterSubIntrHandler"), 1, 2, FN, 0x1234) == 0,
          "register a sub-interrupt handler");
    CHECK(psp_intr_handlers() == 1, "handler recorded");
    CHECK(psp_intr_enabled() == 0, "and starts disabled, as it does on hardware");

    /* Registered but not enabled: a wait must not deliver. */
    call(psp_nid("sceKernelSleepThread"), 0, 0, 0, 0);
    CHECK(g_handler_calls == 0 && psp_intr_deliveries() == 0,
          "a disabled handler is not called");

    CHECK(call(psp_nid("sceKernelEnableSubIntr"), 1, 2, 0, 0) == 0,
          "enable returns the previous state (0 = was disabled)");
    CHECK(psp_intr_enabled() == 1, "handler enabled");

    call(psp_nid("sceKernelSleepThread"), 0, 0, 0, 0);
    CHECK(g_handler_calls == 1 && psp_intr_deliveries() == 1,
          "the wait delivers one interrupt");
    CHECK(g_handler_sub == 2 && g_handler_arg == 0x1234,
          "handler receives (subintr, arg)");
    CHECK(psp_cpu.r[PSP_REG_A0] == 0 && psp_cpu.r[PSP_REG_A1] == 0,
          "the interrupted context's registers survive the handler");

    /* A delay and a vblank wait are the other two delivery points. */
    call(psp_nid("sceKernelDelayThread"), 0, 0, 0, 0);
    call(psp_nid("sceDisplayWaitVblank"), 0, 0, 0, 0);
    CHECK(g_handler_calls == 3, "DelayThread and the vblank wait deliver too");

    /* Reentrancy: a handler that waits is serviced later, not recursively. */
    g_handler_calls = 0;
    psp_intrman_reset();
    psp_register(FN, fake_subintr_handler_that_waits);
    call(psp_nid("sceKernelRegisterSubIntrHandler"), 1, 2, FN, 0);
    call(psp_nid("sceKernelEnableSubIntr"), 1, 2, 0, 0);
    call(psp_nid("sceKernelSleepThread"), 0, 0, 0, 0);
    CHECK(g_handler_calls == 1, "a handler that waits does not re-enter delivery");

    /* A handler address that discovery never found is counted and named, not
     * silently skipped -- that failure would look like the bug this fixes. */
    psp_intrman_reset();
    call(psp_nid("sceKernelRegisterSubIntrHandler"), 1, 2, 0x0000BEEFu, 0);
    call(psp_nid("sceKernelEnableSubIntr"), 1, 2, 0, 0);
    call(psp_nid("sceKernelSleepThread"), 0, 0, 0, 0);
    CHECK(psp_intr_unresolved() == 1 && psp_intr_deliveries() == 0,
          "an unrecompiled handler address is counted as unresolved");

    /* Releasing stops delivery. */
    psp_intrman_reset();
    g_handler_calls = 0;
    psp_register(FN, fake_subintr_handler);
    call(psp_nid("sceKernelRegisterSubIntrHandler"), 1, 2, FN, 0);
    call(psp_nid("sceKernelEnableSubIntr"), 1, 2, 0, 0);
    CHECK(call(psp_nid("sceKernelReleaseSubIntrHandler"), 1, 2, 0, 0) == 0,
          "release");
    call(psp_nid("sceKernelSleepThread"), 0, 0, 0, 0);
    CHECK(g_handler_calls == 0 && psp_intr_handlers() == 0,
          "a released handler is not called");
}

/* A callback, as the guest sees one: it is called with two arguments, and it
 * must not disturb the thread it runs on. */
static uint32_t g_cb_arg, g_cb_notify;
static int      g_cb_calls;
static void fake_callback(void) {
    g_cb_calls++;
    g_cb_arg    = psp_cpu.r[PSP_REG_A0];
    g_cb_notify = psp_cpu.r[PSP_REG_A1];
    psp_cpu.r[PSP_REG_S3] = 0xDEAD;    /* a register the interrupted code owns */
    psp_cpu.r[PSP_REG_V0] = 0x42;      /* the delivery status the kernel keeps */
}

/* And a thread that is handed a callback id belonging to another thread: it
 * notifies it, and checks -- and must find nothing of its own to run. */
static uint32_t g_boot_cbid;      /* created by the test's own thread */
static uint32_t g_other_ran;      /* what the other thread's check reported */
static uint32_t g_other_delta;    /* deliveries its check caused */
static void fake_other_thread(void) {
    call(psp_nid("sceKernelNotifyCallback"), g_boot_cbid, 0x99, 0, 0);
    uint32_t before = (uint32_t)g_cb_calls;
    g_other_ran = call(psp_nid("sceKernelCheckCallback"), 0, 0, 0, 0);
    g_other_delta = (uint32_t)g_cb_calls - before;
}

static void test_callbacks(void) {
    psp_threadman_reset();
    psp_power_reset();
    psp_dispatch_reset();

    const uint32_t CREATE = psp_nid("sceKernelCreateCallback");
    const uint32_t NOTIFY = psp_nid("sceKernelNotifyCallback");
    const uint32_t CHECK  = psp_nid("sceKernelCheckCallback");
    const uint32_t DELETE = psp_nid("sceKernelDeleteCallback");
    const uint32_t DELAY  = psp_nid("sceKernelDelayThread");
    const uint32_t PWREG  = psp_nid("scePowerRegisterCallback");
    const uint32_t PWUNREG= psp_nid("scePowerUnregisterCallback");
    const uint32_t START  = psp_nid("sceKernelStartThread");
    const uint32_t END    = psp_nid("sceKernelWaitThreadEnd");
    const uint32_t CREATE_TH = psp_nid("sceKernelCreateThread");

    const uint32_t FN = 0x08803000u;
    psp_register(FN, fake_callback);

    /* Nothing pending is the answer a polling loop gets while it has no work:
     * 0, distinctly not an error. */
    CHECK(call(CHECK, 0, 0, 0, 0) == 0, "no callback pending -> CheckCallback reports 0");

    uint32_t cbid = call(CREATE, 0 /*name*/, FN, 0xAA /*arg*/, 0);
    CHECK(cbid != 0, "callback created, got 0x%08X", cbid);

    /* A notification queues, it does not run in place: the notify may well come
     * from another thread, and the callback belongs to its own. */
    g_cb_calls = 0;
    CHECK(call(NOTIFY, cbid, 0x77, 0, 0) == 0, "notify succeeds");
    CHECK(g_cb_calls == 0, "a notification does not run the callback in place");

    /* The check point runs it, with the argument from create time and the one
     * the notification carried, and reports 1 -- one callback ran. */
    psp_cpu.r[PSP_REG_S3] = 0x5A5A5A5A;
    CHECK(call(CHECK, 0, 0, 0, 0) == 1, "CheckCallback reports 1 when a callback ran");
    CHECK(g_cb_calls == 1, "the callback ran once, got %d", g_cb_calls);
    CHECK(g_cb_arg == 0xAA, "first argument is the one CreateCallback was given, got 0x%X", g_cb_arg);
    CHECK(g_cb_notify == 0x77, "second argument is the one the notification carried, got 0x%X", g_cb_notify);
    CHECK(psp_cpu.r[PSP_REG_S3] == 0x5A5A5A5A, "a callback does not disturb the thread it runs on");

    /* One delivery per notification. */
    CHECK(call(CHECK, 0, 0, 0, 0) == 0, "a delivered notification is not delivered twice");

    /* And the waits deliver too, next to the interrupt delivery: a game that
     * polls by sleeping rather than by checking must still see it. */
    g_cb_calls = 0;
    call(NOTIFY, cbid, 0x11, 0, 0);
    call(DELAY, 1000, 0, 0, 0);
    CHECK(g_cb_calls == 1, "a wait delivers the callback, got %d", g_cb_calls);

    /* An id that names nothing is refused rather than recorded: a registration
     * that accepts one is a callback that never runs for no visible reason. */
    CHECK(call(NOTIFY, 0xDEADBEEFu, 0, 0, 0) != 0, "notifying an unknown id fails");

    /* Ownership, both ways round: a callback runs only on the thread that
     * created it. The second thread notifies the first thread's callback and
     * then checks -- the notification queues, its own check finds nothing to
     * run, and the work is still there for the thread it belongs to. */
    g_cb_calls = 0;
    g_boot_cbid = call(CREATE, 0, FN, 0xBB, 0);
    call(NOTIFY, g_boot_cbid, 0x22, 0, 0);
    CHECK(call(CHECK, 0, 0, 0, 0) == 1, "owner consumes its first notification");
    const uint32_t OTHER = 0x08803100u;
    g_other_ran = 0xFFFFFFFFu;
    g_other_delta = 0xFFFFFFFFu;
    psp_register(OTHER, fake_other_thread);
    uint32_t oid = call5(CREATE_TH, 0, OTHER, 32, 0x4000, 0);
    CHECK(call(START, oid, 0, 0, 0) == 0, "notifying thread started");
    CHECK(call(END, oid, 0x08804000u, 0, 0) == 0, "notifying thread ended");
    CHECK(g_other_ran == 0, "another thread's check runs nothing of this thread's (got %u)",
          g_other_ran);
    CHECK(g_other_delta == 0, "and does not deliver it on the way (delta %u)", g_other_delta);
    /* The owner delivered -- once per notification it was given -- and the last
     * one carried the argument the *other* thread passed. */
    CHECK(g_cb_calls == 2, "the owner delivered both notifications, got %d", g_cb_calls);
    CHECK(g_cb_notify == 0x99, "the last one carried the other thread's argument, got 0x%X",
          g_cb_notify);
    CHECK(g_cb_arg == 0xBB, "and the callback still sees what create time gave it, got 0x%X",
          g_cb_arg);
    CHECK(call(CHECK, 0, 0, 0, 0) == 0, "and nothing is left pending");

    /* A deleted callback cannot be notified. */
    CHECK(call(DELETE, cbid, 0, 0, 0) == 0, "delete succeeds");
    CHECK(call(NOTIFY, cbid, 0, 0, 0) != 0, "a deleted callback cannot be notified");
    CHECK(call(DELETE, cbid, 0, 0, 0) != 0, "and deleting it twice fails");

    /* Power callback slots. No power event exists in this HLE, so what is real
     * is the slot: the caller passes -1, stores what comes back, and passes it
     * to unregister later. */
    CHECK(call(PWREG, (uint32_t)-1, 0xDEADBEEFu, 0, 0) != 0,
          "a slot is not given to an id that is not a callback");
    uint32_t cbid3 = call(CREATE, 0, FN, 0, 0);
    int32_t slot_a = (int32_t)call(PWREG, (uint32_t)-1, cbid3, 0, 0);
    CHECK(slot_a >= 0 && slot_a < 16, "auto slot is a real slot number, got %d", slot_a);
    int32_t slot_b = (int32_t)call(PWREG, (uint32_t)-1, cbid3, 0, 0);
    CHECK(slot_b >= 0 && slot_b != slot_a, "a second registration gets another slot, got %d", slot_b);
    CHECK(call(PWREG, (uint32_t)slot_a, cbid3, 0, 0) != 0, "a taken slot is refused, not overwritten");
    CHECK(call(PWREG, 99, cbid3, 0, 0) != 0, "a slot out of range is refused");
    CHECK(call(PWUNREG, (uint32_t)slot_a, 0, 0, 0) == 0, "unregister succeeds");
    CHECK((int32_t)call(PWREG, (uint32_t)-1, cbid3, 0, 0) == slot_a, "the freed slot is reused");
    CHECK(psp_power_registered() == 3, "three registrations succeeded, got %llu",
          (unsigned long long)psp_power_registered());
}

/* Run one savedata request through the states a game polls, as a game does. */
static uint32_t savedata_request(uint32_t param, uint32_t mode) {
    psp_write32(param + 0x30, mode);
    CHECK(call(psp_nid("sceUtilitySavedataInitStart"), param, 0, 0, 0) == 0, "savedata starts");
    CHECK(call(psp_nid("sceUtilitySavedataGetStatus"), 0, 0, 0, 0) == 1, "it initialises");
    CHECK(call(psp_nid("sceUtilitySavedataGetStatus"), 0, 0, 0, 0) == 2, "then runs");
    call(psp_nid("sceUtilitySavedataUpdate"), 1, 0, 0, 0);
    CHECK(call(psp_nid("sceUtilitySavedataGetStatus"), 0, 0, 0, 0) == 3, "then is done");
    call(psp_nid("sceUtilitySavedataShutdownStart"), 0, 0, 0, 0);
    CHECK(call(psp_nid("sceUtilitySavedataGetStatus"), 0, 0, 0, 0) == 4, "then shuts down");
    CHECK(call(psp_nid("sceUtilitySavedataGetStatus"), 0, 0, 0, 0) == 0, "and is gone");
    return psp_read32(param + 0x1C);
}

static void put_guest_string(uint32_t addr, const char *s) {
    do psp_write8(addr++, (uint8_t)*s); while (*s++);
}

/* What the game wrote comes back byte for byte, through a slot list as well. */
static void test_savedata(void) {
    const uint32_t PARAM = 0x08840000u, LIST = PARAM + 0x1000u, DATA = PARAM + 0x2000u;
    const uint32_t NO_DATA = 0x80110307u;
    for (uint32_t i = 0; i < 0x3000u; i += 4) psp_write32(PARAM + i, 0);
    put_guest_string(PARAM + 0x3C, "TESTGAME");
    put_guest_string(PARAM + 0x64, "DATA.BIN");
    put_guest_string(LIST, "SLOT0");
    put_guest_string(LIST + 20, "SLOT1");
    psp_write32(PARAM + 0x60, LIST);
    psp_write32(PARAM + 0x74, DATA);
    psp_write32(PARAM + 0x78, 64);
    psp_write32(DATA, 0xCAFEF00Du);
    psp_write32(DATA + 4, 0x12345678u);
    psp_write32(PARAM + 0x7C, 8);
    put_guest_string(PARAM + 0x80, "A title");

    /* Clear what an earlier run of this test left. */
    savedata_request(PARAM, 6);

    CHECK(savedata_request(PARAM, 4) == NO_DATA, "a list load with no saves reports no data");
    CHECK(savedata_request(PARAM, 5) == 0, "a list save succeeds");

    psp_write32(DATA, 0);
    psp_write32(DATA + 4, 0);
    psp_write32(PARAM + 0x7C, 0);
    psp_write8(PARAM + 0x80, 0);
    psp_write8(PARAM + 0x4C, 0);
    CHECK(savedata_request(PARAM, 4) == 0, "a list load finds it");
    CHECK(psp_read32(DATA) == 0xCAFEF00Du && psp_read32(DATA + 4) == 0x12345678u,
          "the data comes back, got 0x%08X", psp_read32(DATA));
    CHECK(psp_read32(PARAM + 0x7C) == 8, "with its size, got %u", psp_read32(PARAM + 0x7C));
    CHECK(psp_read8(PARAM + 0x80) == 'A', "and its description");
    CHECK(psp_read8(PARAM + 0x4C) == 'S', "and the slot chosen is named");

    CHECK(savedata_request(PARAM, 6) == 0, "a list delete removes it");
    CHECK(savedata_request(PARAM, 4) == NO_DATA, "and it is gone");
}

int main(void) {
    CHECK(psp_mem_init() == 0, "memory init");
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_SP] = 0x08810000u;   /* a stack for the "caller" */
    psp_hle_init();

    printf("registered %d firmware functions\n", psp_hle_count());

    test_sha1_vectors();
    test_nids_match_names();
    test_sysmem();
    test_semaphores();
    test_event_flags();
    test_threads();
    test_vpl();
    test_guest_strings();
    test_ge_display_list();
    test_ge_infinite_list();
    test_sas_adpcm();
    test_display();
    test_unimplemented_logging();
    test_sub_interrupts();
    test_callbacks();
    test_savedata();

    psp_mem_free();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all HLE checks passed\n");
    return 0;
}
