/* psprecomp -- mutexes and message boxes.
 *
 * The scheduler hands the CPU over only at waits, so a lock never needs to be
 * atomic. It still has to exclude: a thread can park while holding one, and the
 * next thread to run must not walk in. Contended locks therefore yield until
 * the owner lets go. Timeouts are not modelled, so a lock that is never released
 * shows up as a thread stuck here rather than as a returned error.
 */

#include "psprecomp/hle.h"

#include <string.h>

#define MAX_MUTEX 128
#define MAX_MBX   32
#define MBX_DEPTH 256

#define ERROR_UNKNOWN_MUTEX 0x800201C3u
#define ERROR_UNKNOWN_MBX   0x800201C9u
#define ERROR_MBX_NO_MESSAGE 0x800201B2u
#define ERROR_NO_MEMORY     0x80020190u

typedef struct { int used; uint32_t owner; uint32_t count; } mutex_t_;

typedef struct {
    int used;
    uint32_t msg[MBX_DEPTH];
    int head, n;
} mbx_t_;

static mutex_t_ g_mutex[MAX_MUTEX];
static mbx_t_   g_mbx[MAX_MBX];

void psp_sync_init(void) {
    memset(g_mutex, 0, sizeof g_mutex);
    memset(g_mbx, 0, sizeof g_mbx);
}

/* ---- sceKernel*Mutex ----------------------------------------------------- */

#define MUTEX_UID(i) (0x00070000u + (uint32_t)(i))

static mutex_t_ *find_mutex(uint32_t uid) {
    uint32_t i = uid - MUTEX_UID(0);
    return (i < MAX_MUTEX && g_mutex[i].used) ? &g_mutex[i] : NULL;
}

/* (name, attr, initialCount, options) */
static void hle_CreateMutex(void) {
    for (int i = 0; i < MAX_MUTEX; i++) {
        if (g_mutex[i].used) continue;
        g_mutex[i].used = 1;
        g_mutex[i].count = psp_arg(2);
        g_mutex[i].owner = g_mutex[i].count ? psp_threadman_current() : 0;
        psp_ret(MUTEX_UID(i));
        return;
    }
    psp_ret(ERROR_NO_MEMORY);
}

static void hle_DeleteMutex(void) {
    mutex_t_ *m = find_mutex(psp_arg(0));
    if (!m) { psp_ret(ERROR_UNKNOWN_MUTEX); return; }
    m->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (uid, count, timeout) */
static void hle_LockMutex(void) {
    const uint32_t uid = psp_arg(0), count = psp_arg(1), me = psp_threadman_current();
    mutex_t_ *m = find_mutex(uid);
    if (!m) { psp_ret(ERROR_UNKNOWN_MUTEX); return; }
    while (m->used && m->count && m->owner != me) psp_threadman_yield();
    if (!m->used) { psp_ret(ERROR_UNKNOWN_MUTEX); return; }
    m->owner = me;
    m->count += count;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (uid, count) */
static void hle_UnlockMutex(void) {
    mutex_t_ *m = find_mutex(psp_arg(0));
    const uint32_t count = psp_arg(1);
    if (!m) { psp_ret(ERROR_UNKNOWN_MUTEX); return; }
    m->count = m->count > count ? m->count - count : 0;
    if (!m->count) m->owner = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceKernel*LwMutex --------------------------------------------------- *
 *
 * A lightweight mutex keeps its state in a work area the game owns:
 *   +0 count, +4 owning thread, +8 attr, +12 waiters, +16 uid */

/* (workarea, name, attr, initialCount, options) */
static void hle_CreateLwMutex(void) {
    const uint32_t w = psp_arg(0), count = psp_arg(3);
    psp_write32(w + 0, count);
    psp_write32(w + 4, count ? psp_threadman_current() : 0);
    psp_write32(w + 8, psp_arg(2));
    psp_write32(w + 12, 0);
    psp_write32(w + 16, w);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_DeleteLwMutex(void) {
    psp_write32(psp_arg(0) + 16, 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (workarea, count, timeout) */
static void hle_LockLwMutex(void) {
    const uint32_t w = psp_arg(0), count = psp_arg(1), me = psp_threadman_current();
    while (psp_read32(w) && psp_read32(w + 4) != me) psp_threadman_yield();
    psp_write32(w, psp_read32(w) + count);
    psp_write32(w + 4, me);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (workarea, count) */
static void hle_UnlockLwMutex(void) {
    const uint32_t w = psp_arg(0), count = psp_arg(1), now = psp_read32(w);
    const uint32_t left = now > count ? now - count : 0;
    psp_write32(w, left);
    if (!left) psp_write32(w + 4, 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceKernel*Mbx ------------------------------------------------------- *
 *
 * On hardware a message box links the game's message headers together. Keeping
 * the pointers here instead is invisible to a game that only sends and polls. */

#define MBX_UID(i) (0x00080000u + (uint32_t)(i))

static mbx_t_ *find_mbx(uint32_t uid) {
    uint32_t i = uid - MBX_UID(0);
    return (i < MAX_MBX && g_mbx[i].used) ? &g_mbx[i] : NULL;
}

static void hle_CreateMbx(void) {
    for (int i = 0; i < MAX_MBX; i++) {
        if (g_mbx[i].used) continue;
        memset(&g_mbx[i], 0, sizeof g_mbx[i]);
        g_mbx[i].used = 1;
        psp_ret(MBX_UID(i));
        return;
    }
    psp_ret(ERROR_NO_MEMORY);
}

static void hle_DeleteMbx(void) {
    mbx_t_ *b = find_mbx(psp_arg(0));
    if (!b) { psp_ret(ERROR_UNKNOWN_MBX); return; }
    b->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (uid, message) */
static void hle_SendMbx(void) {
    mbx_t_ *b = find_mbx(psp_arg(0));
    if (!b) { psp_ret(ERROR_UNKNOWN_MBX); return; }
    if (b->n == MBX_DEPTH) { psp_ret(ERROR_NO_MEMORY); return; }
    b->msg[(b->head + b->n++) % MBX_DEPTH] = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (uid, message *out) */
static void hle_PollMbx(void) {
    mbx_t_ *b = find_mbx(psp_arg(0));
    if (!b) { psp_ret(ERROR_UNKNOWN_MBX); return; }
    if (!b->n) { psp_ret(ERROR_MBX_NO_MESSAGE); return; }
    psp_write32(psp_arg(1), b->msg[b->head]);
    b->head = (b->head + 1) % MBX_DEPTH;
    b->n--;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_sync_register(void) {
    psp_hle_register(0xB7D098C6, "ThreadManForUser", "sceKernelCreateMutex",   hle_CreateMutex);
    psp_hle_register(0xF8170FBE, "ThreadManForUser", "sceKernelDeleteMutex",   hle_DeleteMutex);
    psp_hle_register(0xB011B11F, "ThreadManForUser", "sceKernelLockMutex",     hle_LockMutex);
    psp_hle_register(0x5BF4DD27, "ThreadManForUser", "sceKernelLockMutexCB",   hle_LockMutex);
    psp_hle_register(0x6B30100F, "ThreadManForUser", "sceKernelUnlockMutex",   hle_UnlockMutex);

    psp_hle_register(0x19CFF145, "ThreadManForUser", "sceKernelCreateLwMutex", hle_CreateLwMutex);
    psp_hle_register(0x60107536, "ThreadManForUser", "sceKernelDeleteLwMutex", hle_DeleteLwMutex);
    psp_hle_register(0xBEA46419, "Kernel_Library",   "sceKernelLockLwMutex",   hle_LockLwMutex);
    psp_hle_register(0x1FC64E09, "Kernel_Library",   "sceKernelLockLwMutexCB", hle_LockLwMutex);
    psp_hle_register(0x15B6446B, "Kernel_Library",   "sceKernelUnlockLwMutex", hle_UnlockLwMutex);

    psp_hle_register(0x8125221D, "ThreadManForUser", "sceKernelCreateMbx", hle_CreateMbx);
    psp_hle_register(0x86255ADA, "ThreadManForUser", "sceKernelDeleteMbx", hle_DeleteMbx);
    psp_hle_register(0xE9B3061E, "ThreadManForUser", "sceKernelSendMbx",   hle_SendMbx);
    psp_hle_register(0x0D81716A, "ThreadManForUser", "sceKernelPollMbx",   hle_PollMbx);
}
