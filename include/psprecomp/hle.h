/* psprecomp — high-level emulation of the PSP firmware.
 *
 * A PSP game does not touch hardware. It calls firmware entry points through
 * an import table, and every one of those calls is identified by a **NID** —
 * the first four bytes of SHA-1(function name), little-endian. That is a
 * verifiable fact rather than a convention, and `tests/test_hle.c` checks it
 * for every function registered here: a mistyped NID or a wrong name cannot
 * survive the test.
 *
 * Because the surface is a library rather than hardware, it is *implemented*,
 * not emulated. What a game needs is exactly its import table and nothing
 * else, which `allegrexrecomp funcs` reports — so the work is bounded and
 * knowable in advance.
 *
 * Calling convention is MIPS o32: arguments in $a0-$a3 then the stack, return
 * value in $v0. Handlers take no C arguments and use psp_arg()/psp_ret().
 */
#ifndef PSPRECOMP_HLE_H
#define PSPRECOMP_HLE_H

#include "cpu.h"
#include "mem.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psp_hle_fn)(void);

/* Register one firmware function. `name` is kept for diagnostics and is what
 * the NID is verified against. */
void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn);
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn);
int psp_hle_is_named(int index);

/* Call a firmware function by NID. An unregistered NID reports itself by name
 * where possible and by number otherwise, rather than failing silently. */
void psp_hle_call(uint32_t nid);

/* Look up what is registered, for reporting. Returns NULL if absent. */
const char *psp_hle_name(uint32_t nid);
int         psp_hle_count(void);
/* The message naming an unimplemented call is printed a bounded number of
 * times; these are how much was kept back. `calls` is the number that tells a
 * silenced message from one that stopped happening -- see the comment on
 * UNIMPL_LOGGED in hle.c. */
uint64_t psp_hle_unimplemented_calls(void);
uint64_t psp_hle_unimplemented_logged(void);
void     psp_hle_unimplemented_reset(void);

/* Every zero-returning call ever made, not just the ZERO_HISTORY ring. */
uint64_t psp_hle_zero_calls(void);

/* Call counts per firmware function, busiest first, up to `limit` rows.
 * Unimplemented NIDs are counted individually too. */
void psp_hle_dump_counts(FILE *out, int limit);

/* Every registered entry, for the coverage report a game repo wants: which of
 * a module's imports actually exist yet. */
typedef struct {
    uint32_t    nid;
    const char *lib;
    const char *name;
} psp_hle_entry;

const psp_hle_entry *psp_hle_entries(int *count);

/* Print the recent firmware calls that returned zero. Zero is what a game most
 * often mistakes for an address, so this is the first thing to consult when a
 * wild pointer shows up far from its cause. */
void psp_hle_dump_recent(FILE *out);

/* Register everything the toolkit implements. Call once at startup. */
void psp_hle_init(void);

/* ---- o32 argument access ------------------------------------------------- */

/* Arguments 0-3 arrive in $a0-$a3; 4 and beyond are on the stack, at $sp+16
 * onward. The stack slots for the register arguments exist but are not
 * written by the caller, which is why the split is at 4 and not at 0. */
/* Firmware calls take arguments 5-8 in $t0-$t3, not on the stack.
 *
 * Plain o32 spills the fifth argument onward to `sp+16`, and that is what this
 * used to read. PSP firmware stubs do not: they load $t0-$t3 and branch, which
 * is visible in the delay slot of every such call --
 *
 *     000002EC  addu  $a3, $s1, $zero
 *     000002F0  jal   0x00091F14        ; sceKernelAllocPartitionMemory
 *     000002F4  addiu $t0, $zero, 4096  ; argument 5: the alignment
 *
 * Reading `sp+16` instead returned whatever happened to be on the stack. For
 * that call it produced 0x3AC85C where a power-of-two alignment belonged, so
 * the allocator rejected a 15.9 MB request with ILLEGAL_ATTR -- and every
 * failure this bring-up chased descended from it.
 *
 * Beyond eight, arguments do go on the stack. */
static inline uint32_t psp_arg(int n) {
    if (n < 4) return psp_cpu.r[PSP_REG_A0 + n];
    if (n < 8) return psp_cpu.r[PSP_REG_T0 + (n - 4)];
    return psp_read32(psp_cpu.r[PSP_REG_SP] + (uint32_t)n * 4);
}

static inline void psp_ret(uint32_t v) { psp_cpu.r[PSP_REG_V0] = v; }

/* Read a NUL-terminated string out of guest memory into a host buffer.
 * Always terminates; returns `dst`. */
const char *psp_str(uint32_t addr, char *dst, size_t cap);

/* ---- error codes --------------------------------------------------------- */
/* Only the ones the implemented functions can actually return. Games branch on
 * these, so returning a plausible-looking wrong value is worse than failing. */
#define SCE_KERNEL_ERROR_OK              0
#define SCE_KERNEL_ERROR_ERROR           0x80020001
#define SCE_KERNEL_ERROR_NOTIMPLEMENTED  0x80020002
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR    0x80020005
#define SCE_KERNEL_ERROR_NO_MEMORY       0x80020190
#define SCE_KERNEL_ERROR_ILLEGAL_ATTR    0x80020191
#define SCE_KERNEL_ERROR_UNKNOWN_UID     0x800201A2
#define SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK 0x800201A9
#define SCE_KERNEL_ERROR_ILLEGAL_THID    0x80020197
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT    0x800201A8
#define SCE_KERNEL_ERROR_THREAD_TERMINATED 0x800201AC
#define SCE_KERNEL_ERROR_SEMA_ZERO       0x800201AD
#define SCE_KERNEL_ERROR_EVF_COND        0x800201AF
/* Added for the power callback slots; the neighbouring codes are corrected on
 * the UMD/RTC branch (their values arrived from the same pspsdk source). */
#define SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT 0x800200D2

/* ---- the subsystems ------------------------------------------------------ */

void psp_sysmem_init(void);
void psp_sysmem_register(void);
void psp_sysmem_reset(void);
/* The memory the allocator may hand out. A module's .bss can run past the
 * default range, and a host that knows its layout should narrow it. */
void psp_sysmem_set_range(uint32_t lo, uint32_t hi);

void psp_display_init(void);
void psp_display_register(void);
void psp_display_reset(void);
/* Hold vblank waits to this many per second, changeable at any time. 0, the
 * default, does not hold them at all. */
void psp_display_set_refresh_rate(double hz);
/* Sleep until the next refresh, on the one clock every vblank wait uses. A vblank
 * interrupt must come from here too. On a clock of its own, however close its rate,
 * it drifts against the waits and every so often makes a game miss a refresh. */
void psp_display_wait_vblank(void);
/* Times the game has switched to a new front buffer: its real frame rate, which
 * a thread waiting for vblanks on its own says nothing about. */
uint64_t psp_display_flips(void);
void     psp_display_dump_frames(FILE *out);
int      psp_display_capture(const char *path);
/* The front buffer as 480x272 pixels of 0xAABBGGRR; -1 before the game has
 * set a framebuffer. */
int      psp_display_read(uint32_t *out);
uint64_t psp_display_vblanks(void);
uint32_t psp_display_framebuffer(void);

void psp_ge_init(void);
void psp_ge_register(void);
void psp_ge_reset(void);
void psp_ge_dump_stats(FILE *out);
uint64_t psp_ge_command_count(void);
uint64_t psp_ge_vertex_count(void);
uint64_t psp_ge_pixels(void);
/* Run display lists on a host thread of their own, as hardware runs them beside
 * the CPU. Needs real threads; without them, or by default, they run inside the
 * enqueue. Turn it on before any list is queued. */
void     psp_ge_set_async(int on);
/* The game has switched to showing the buffer at `addr`. */
void     psp_ge_flip(uint32_t addr, uint32_t stride, int fmt);
int      psp_ge_wait_briefly(uint32_t us);
/* Host time spent running display lists: the cost of software rendering. */
uint64_t psp_ge_busy_us(void);
/* Run the callbacks finished lists have raised. Called at every wait. */
void     psp_ge_deliver(void);
uint64_t psp_ge_callbacks_delivered(void);

void psp_sas_init(void);
void psp_sas_register(void);
void psp_sas_reset(void);
uint64_t psp_sas_frames(void);
uint64_t psp_sas_nonzero(void);

void psp_io_init(void);
void psp_io_register(void);
void psp_io_reset(void);
void psp_io_set_root(const char *root);
/* Where disc0:/ lives, when it is not <root>/disc. */
void psp_io_set_disc_root(const char *path);
/* A file with no file behind it, or none the game should see as it is. */
typedef struct {
    uint64_t size;
    void *ctx;
    size_t (*read)(void *ctx, uint64_t offset, void *dst, size_t n);
    void (*close)(void *ctx);
} psp_io_virtual;
/* Asked before a file is opened for reading. Returns 1 and fills `out` to provide
 * it. This is what lets a host replace a game's files without changing them. */
typedef int (*psp_io_open_hook)(const char *guest, const char *host, psp_io_virtual *out);
void psp_io_set_open_hook(psp_io_open_hook hook);
/* Where a guest path such as ms0:/PSP/... lands on the host. */
void psp_io_host_path(const char *guest, char *out, size_t cap);
/* The host path of the last Memory Stick file opened for reading. */
void psp_io_last_ms_file(char *out, size_t cap);
uint64_t psp_io_bytes_read(void);

/* What a savedata request did, for a host to tell the player: the system's own
 * screens for this are not drawn. Runs on a guest thread, so keep it short. */
enum {
    PSP_SAVEDATA_SAVED, PSP_SAVEDATA_LOADED, PSP_SAVEDATA_NO_DATA,
    PSP_SAVEDATA_DELETED, PSP_SAVEDATA_FAILED, PSP_SAVEDATA_UNREADABLE
};
typedef void (*psp_savedata_event_fn)(int event);
void psp_savedata_set_event_handler(psp_savedata_event_fn fn);
/* Whether the system's save screen is open, during which a game may draw nothing. */
int psp_utility_savedata_open(void);
/* What the system's screenshot dialog does when a game opens it: the host takes the picture and
 * keeps it. With none set, the dialog only walks through its states. */
void psp_utility_set_screenshot_handler(void (*take)(void));

/* A save slot as the system's list screen shows it: its name, whether it holds data, and the
 * title, detail and time of what it holds. Texts are UTF-8, from the save's SFO. */
typedef struct {
    char name[21];
    int used;
    char title[129];
    char detail[1025];
    long long time;        /* seconds since 1970, 0 if empty */
} psp_save_slot;
enum { PSP_SAVE_CHOOSE_LOAD, PSP_SAVE_CHOOSE_SAVE, PSP_SAVE_CHOOSE_DELETE };
/* Where a player chooses a slot from a list of more than one, for `purpose`; what is to be saved is
 * described by new_title and new_detail. Gives back the index chosen, or -1 if the player backed
 * out, or PSP_SAVE_CHOOSE_DEFAULT for no list, as for a list a game names but where it keeps one
 * save. It runs on the game's thread, which waits for it, as a game waits on the system's screen.
 * With none set, or for the default, a load takes the first slot with data and a save that slot or
 * the first. */
#define PSP_SAVE_CHOOSE_DEFAULT (-2)
/* Or PSP_SAVE_CHOOSE_AGAIN, when the host has changed what the slots hold: they are read again and
 * the host asked again. */
#define PSP_SAVE_CHOOSE_AGAIN (-3)
typedef int (*psp_savedata_chooser_fn)(int purpose, const psp_save_slot *slots, int count, const char *new_title,
                                       const char *new_detail);
void psp_savedata_set_chooser(psp_savedata_chooser_fn fn);

/* The system's on-screen keyboard: its description and the text it starts with, UTF-16, and at
 * most `limit` characters. Writes the text entered into `out` (limit + 1 UTF-16 units, zero
 * ended) and gives back 1, or 0 if the player backed out. Runs as the chooser does. With none
 * set, the keyboard closes with the text unchanged. */
typedef int (*psp_osk_fn)(const uint16_t *description, const uint16_t *initial, int limit, uint16_t *out);
void psp_utility_set_osk_handler(psp_osk_fn fn);

/* The system's message: an error by its number (`error`, with `text` empty) or a text, UTF-8, with
 * PSP_MSG_YESNO or PSP_MSG_OK in `options` for its buttons. Gives back PSP_MSG_YES, PSP_MSG_NO or
 * PSP_MSG_BACK. Runs as the chooser does. With none set, a message answers yes unseen. */
#define PSP_MSG_YESNO 0x10u
#define PSP_MSG_OK 0x20u
#define PSP_MSG_NOCANCEL 0x80u
#define PSP_MSG_DEFAULT_NO 0x100u
enum { PSP_MSG_YES = 1, PSP_MSG_NO = 2, PSP_MSG_BACK = 3 };
typedef int (*psp_message_fn)(uint32_t error, const char *text, uint32_t options);
void psp_utility_set_message_handler(psp_message_fn fn);

void psp_utility_init(void);
void psp_utility_register(void);

void psp_sync_init(void);
void psp_sync_register(void);

/* Told of each sceDmacMemcpy, (destination, size), as a renderer must be: a buffer it drew may
 * have been filled with a copy since. Set by the renderer. */
extern void (*psp_on_dmac_copy)(uint32_t dst, uint32_t size);

void psp_mpeg_init(void);
void psp_mpeg_register(void);
/* Whether movies can play: FFmpeg's decoders were found beside the executable. */
int psp_mpeg_available(void);
void psp_misc_init(void);
void psp_misc_register(void);
void psp_misc_reset(void);
int  psp_exit_requested(void);
void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay);
/* How long before the game's last read of the buttons the newest press it brought really came, in
 * microseconds; 0 if it brought none. A press is seen once a frame: this says when in that frame. */
uint32_t psp_ctrl_press_age_us(void);
void psp_ctrl_force(uint32_t buttons);
/* Sees the buttons as the game reads them, and can change them. NULL for none. */
void psp_ctrl_set_filter(uint32_t (*filter)(uint32_t buttons));
uint64_t psp_audio_blocks(void);
/* Receives every block of PCM the game outputs, on whichever thread sent it. The
 * samples are only valid during the call; volumes are 0x8000 at full. */
typedef void (*psp_audio_sink_fn)(int slot, const int16_t *samples, uint32_t frames,
                                  int stereo, uint32_t vl, uint32_t vr);
void psp_audio_set_sink(psp_audio_sink_fn sink);
/* How far ahead of real time a blocking output may run before it holds the caller.
 * Hardware has a queue of its own, and a sound card fed through the host's needs
 * more slack than one buffer: any delay in the feeding thread is then a gap. */
void psp_audio_set_lead(uint32_t us);

void psp_threadman_init(void);
void psp_threadman_register(void);
/* ThreadMan reset is a quiescent-host operation: all guest threads must have
 * ended first. It reclaims host handles/locks, not guest stack allocations. */
void psp_threadman_reset(void);

/* ---- the scheduler -------------------------------------------------------
 *
 * Default execution is inline. PSPRECOMP_REAL_THREADS=ON opts into one CPU,
 * handed over explicitly: the boot thread (whoever calls
 * psp_threadman_reset) runs the module entry, and a thread that blocks gives
 * the CPU away until it is woken. These are the points where that happens,
 * for use by the rest of the HLE layer. See src/hle/threadman.c. */

/* Give the CPU away without blocking on anything of the guest's. A frame wait
 * is the usual caller: one core means "let a frame pass" is "let the others
 * run". */
void psp_threadman_yield(void);
/* Waits sleep in real time instead of ticking, and the host must then call
 * psp_intr_tick at each vblank. Needs real threads. Set it before the game starts. */
void psp_threadman_set_realtime(int on);
/* Give the CPU away for this long in real time, or for a tick outside that mode. */
void psp_threadman_sleep_us(uint64_t us);
/* Wake a thread waiting on a semaphore or event flag, so that it runs pending
 * callbacks. Safe from a host thread that is not running guest code. */
void psp_threadman_kick(void);
/* The running thread's uid, nonzero even on the boot thread. */
uint32_t psp_threadman_current(void);
/* Park for `us` of virtual time, as sceKernelDelayThread does. For HLE calls
 * that block on hardware for a known time, such as blocking audio output. */
void psp_threadman_delay(uint64_t us);
void psp_threadman_frame_wait(void);

/* Interrupt masking is per thread, and the cookie is the previous state --
 * which is why resuming a nested suspend does not enable interrupts. */
int  psp_threadman_intr_suspend(void);
void psp_threadman_intr_resume(uint32_t cookie);
int  psp_threadman_intr_disabled(void);

/* What the scheduler did, for a host's report. `switches` counts CPU
 * handovers and `parks` counts blocked calls: a run with threads and no
 * switches has not actually run them. */
uint64_t psp_threadman_switches(void);
uint64_t psp_threadman_parks(void);
uint64_t psp_threadman_vtime_us(void);
/* Parks after which another thread had run (so the CPU really was handed over),
 * and the ones that gave up before the thread they waited for was scheduled.
 * See the handover in src/hle/threadman.c. */
uint64_t psp_threadman_handovers(void);
uint64_t psp_threadman_handover_expired(void);
void     psp_threadman_dump(FILE *out);
/* Where each thread's real time has gone: running, queueing for the CPU, asleep. */
void     psp_threadman_dump_times(FILE *out);
void     psp_sched_note(const char *what, const char *name, uint32_t took_us);
void     psp_sched_dump(FILE *out, uint32_t count);
/* The same for one thread by name, running totals in microseconds. 0 if none. */
int      psp_threadman_times(const char *name, uint64_t *life_us, uint64_t *asleep_us,
                             uint64_t *sema_us, uint64_t *queue_us);

/* ---- callbacks -----------------------------------------------------------
 *
 * A callback runs on the thread that created it, at the points where that
 * thread waits or explicitly asks (sceKernelCheckCallback) -- not at the point
 * of the notification, which may be another thread entirely. See the callbacks
 * section of src/hle/threadman.c. */

/* Whether an id names a callback the guest has actually created. A subsystem
 * that records a callback id (scePower's slots) has to ask first. */
int psp_callback_valid(uint32_t uid);

/* What delivery did, for a host's report: callbacks that ran, notifications
 * dropped because the target is not a recompiled function, and passes that hit
 * the per-tick ceiling. */
uint64_t psp_threadman_callback_deliveries(void);
uint64_t psp_threadman_callback_unresolved(void);
uint64_t psp_threadman_callback_deferred(void);

/* scePower: the power callback slots. No power event exists in this HLE, so a
 * registered callback is never fired; what is real is the slot handed back to
 * the caller. See the header of src/hle/power.c. */
void     psp_power_init(void);
void     psp_power_register(void);
void     psp_power_reset(void);
uint64_t psp_power_registered(void);

/* InterruptManager: sub-interrupt handlers, and the points where an interrupt
 * is delivered. See the header of src/hle/intrman.c for why a game can be
 * stuck in a loop that no amount of implementing `sleep` gets it out of. */
void psp_intrman_init(void);
void psp_intrman_register(void);
void psp_intrman_reset(void);

/* Deliver one interrupt to every enabled handler. Called from the places where
 * the guest explicitly waits for time to pass -- SleepThread, DelayThread and
 * the vblank wait -- since with no timer there is nothing else to hook. Safe to
 * call from inside a handler: delivery does not re-enter. */
void psp_intr_tick(void);

/* 0 until a game registers a handler, and the first number worth looking at
 * when a wait never ends. */
int      psp_intr_handlers(void);      /* registered */
int      psp_intr_enabled(void);       /* registered *and* enabled */
uint64_t psp_intr_deliveries(void);
uint64_t psp_intr_unresolved(void);    /* handler address that is not recompiled */

/* How many threads were created and how many were actually started. The gap
 * between the two is the first thing to look at when a game waits on a flag
 * nothing ever arms: a created-but-never-started worker explains it exactly,
 * and no other instrument shows the difference. */
uint64_t psp_threadman_created(void);
uint64_t psp_threadman_started(void);

/* Bytes of user memory still available — the cheapest end-to-end check that
 * the allocator is behaving. */
uint32_t psp_sysmem_free(void);

/* Raw allocation for use by other HLE subsystems (thread stacks, mostly).
 * Returns 0 on failure. These bypass the UID table because nothing in the
 * guest ever refers to them. */
uint32_t psp_sysmem_alloc(uint32_t size, int from_high);
void     psp_sysmem_release(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_HLE_H */
