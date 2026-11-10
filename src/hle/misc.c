/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. The mask belongs to the *thread* that opened the critical
 * section, not to the machine: with several guest threads on one CPU, a global
 * flag would let one thread's suspend silence interrupts inside another
 * thread's unrelated bracket. The scheduler owns that state, because it is
 * also what decides whether an interrupt may be delivered at a park. */

static void hle_CpuSuspendIntr(void) {
    psp_ret((uint32_t)psp_threadman_intr_suspend());   /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    psp_threadman_intr_resume(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_LibcTime(void) {
    time_t t = time(NULL);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, (uint32_t)t);
    psp_ret((uint32_t)t);
}

static void hle_LibcClock(void) {
    /* Microseconds since start. A game that uses this for frame pacing needs
     * it to advance, so it is derived from the host clock rather than being a
     * constant -- a frozen clock makes a game either spin or run at infinite
     * speed, both of which look like a hang. */
    psp_ret((uint32_t)((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC));
}

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        time_t t = time(NULL);
        psp_write32(tv, (uint32_t)t);
        psp_write32(tv + 4, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected. */
static void hle_GetGPI(void) { psp_ret(0); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- sceCtrl ------------------------------------------------------------- */

static uint32_t g_buttons;
static uint8_t  g_analog_x = 128, g_analog_y = 128;
static uint32_t g_ctrl_frame;

/* Once something else is playing, the window no longer has a say. */
static volatile int g_ctrl_forced;

/* All that has been down since the game last read, so that a press between two of its reads is
 * still there for the second. */
static volatile uint32_t g_ctrl_seen;

/* When each button last went down, in microseconds, and how long before the game's last read the
 * newest press it was given came: a press is seen once a frame, and this says when in the frame. */
static volatile uint64_t g_ctrl_down_us[32];
static uint32_t g_ctrl_last_raw, g_ctrl_last_read;
static volatile uint32_t g_ctrl_press_age_us;

static uint64_t ctrl_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000u + (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

uint32_t psp_ctrl_press_age_us(void) { return g_ctrl_press_age_us; }

void psp_ctrl_force(uint32_t buttons) {
    g_ctrl_forced = 1;
    g_buttons = buttons;
}

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    if (g_ctrl_forced) return;
    const uint32_t went_down = buttons & ~g_ctrl_last_raw;
    g_ctrl_last_raw = buttons;
    if (went_down) {
        const uint64_t now = ctrl_now_us();
        for (int bit = 0; bit < 32; bit++)
            if (went_down & (1u << bit)) g_ctrl_down_us[bit] = now;
    }
    g_buttons = buttons;
    __atomic_fetch_or(&g_ctrl_seen, buttons, __ATOMIC_RELAXED);
    g_analog_x = ax;
    g_analog_y = ay;
}

static uint32_t (*g_ctrl_filter)(uint32_t);

void psp_ctrl_set_filter(uint32_t (*filter)(uint32_t buttons)) {
    g_ctrl_filter = filter;
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    const uint32_t down = g_buttons | __atomic_exchange_n(&g_ctrl_seen, 0, __ATOMIC_RELAXED);
    /* The newest press this read brings, and how long ago it came. */
    const uint32_t fresh = down & ~g_ctrl_last_read;
    g_ctrl_last_read = down;
    uint64_t newest = 0;
    for (int bit = 0; bit < 32; bit++)
        if ((fresh & (1u << bit)) && g_ctrl_down_us[bit] > newest) newest = g_ctrl_down_us[bit];
    if (newest) {
        const uint64_t now = ctrl_now_us();
        g_ctrl_press_age_us = now > newest ? (uint32_t)(now - newest) : 0u;
    } else {
        g_ctrl_press_age_us = 0;
    }
    const uint32_t buttons = g_ctrl_filter ? g_ctrl_filter(down) : down;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, g_ctrl_frame++);
        psp_write32(at + 4, buttons);
        psp_write8(at + 8, g_analog_x);
        psp_write8(at + 9, g_analog_y);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
static uint64_t g_audio_blocks;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }

static void hle_ChReserve(void) {
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: ChReserve ch %d samples %u format %u\n",
                (int)psp_arg(0), psp_arg(1), psp_arg(2));
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static psp_audio_sink_fn g_audio_sink;
static uint32_t g_audio_lead_us;

void psp_audio_set_lead(uint32_t us) { g_audio_lead_us = us; }

void psp_audio_set_sink(psp_audio_sink_fn sink) { g_audio_sink = sink; }

#define AUDIO_FORMAT_MONO 0x10u

/* Pass a block to the host. The block count is what tells you audio is flowing
 * whether or not anything is listening. */
static void submit_block(int slot, uint32_t buf, uint32_t frames, int stereo, uint32_t vl, uint32_t vr) {
    g_audio_blocks++;
    if (!g_audio_sink || !buf || !frames) return;
    const int16_t *pcm = (const int16_t *)psp_mem_ptr(buf, frames * (stereo ? 4u : 2u));
    if (pcm) g_audio_sink(slot, pcm, frames, stereo, vl, vr);
}

static void submit_channel(uint32_t ch, uint32_t vl, uint32_t vr, uint32_t buf) {
    if (ch < AUDIO_CHANNELS)
        submit_block((int)ch, buf, g_audio[ch].samples, g_audio[ch].format != AUDIO_FORMAT_MONO, vl, vr);
}

/* (channel, left, right, buf) */
static void hle_OutputPanned(void) {
    const uint32_t ch = psp_arg(0);
    submit_channel(ch, psp_arg(1), psp_arg(2), psp_arg(3));
    psp_ret(ch < AUDIO_CHANNELS ? g_audio[ch].samples : 0);
}

/* The blocking forms hold the caller while the hardware drains a buffer, and
 * that is what paces a game's audio thread. Returning at once lets a mixer
 * loop spin forever and starve every other guest thread.
 *
 * Under real threads the hardware queue is modelled on the host clock: a
 * buffer is due one play time after the last, and the call only waits while
 * the queue is more than a buffer ahead. Waiting a fixed play time per call
 * did not work there. The scheduler is cooperative, each tick of the wait
 * handed the CPU to the game's main thread for a whole frame, and WTF's audio
 * ran at about 10 buffers a second instead of 172; its title waits for a
 * jingle that, by its own sample count, never finished. A thread that falls
 * behind now catches up without waiting, as it would against a real queue.
 * Inline mode keeps the virtual-time wait. */
#if PSPRECOMP_REAL_THREADS
static uint64_t host_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* The two extra slots are sceAudioOutput2 and sceVaudio. */
static uint64_t g_audio_due_us[AUDIO_CHANNELS + 2];

/* Hold the caller until a block due at `due` is within the lead of real time. */
static void wait_audio_due(uint64_t due, uint64_t play_us) {
    for (;;) {
        const uint64_t ready_at = host_now_us() + play_us + g_audio_lead_us;
        if (ready_at >= due) return;
        psp_threadman_sleep_us(due - ready_at);
    }
}
#endif

static void output_blocking(uint32_t ch, uint32_t vl, uint32_t vr, uint32_t buf) {
    uint32_t samples = ch < AUDIO_CHANNELS ? g_audio[ch].samples : 0;
    static int traced;
    if (getenv("PSPRECOMP_TRACE_SAS") && traced++ < 4)
        fprintf(stderr, "audio: OutputBlocking ch %u samples %u buf 0x%08X\n", ch, samples, psp_arg(2));
    submit_channel(ch, vl, vr, buf);
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    if (ch < AUDIO_CHANNELS) {
        uint64_t now = host_now_us();
        if (g_audio_due_us[ch] < now) g_audio_due_us[ch] = now;   /* underrun */
        g_audio_due_us[ch] += play_us;
        wait_audio_due(g_audio_due_us[ch], play_us);
    }
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}

/* (channel, volume, buf) */
static void hle_OutputBlocking(void) { output_blocking(psp_arg(0), psp_arg(1), psp_arg(1), psp_arg(2)); }
/* (channel, left, right, buf) */
static void hle_OutputPannedBlocking(void) { output_blocking(psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3)); }

/* (channel, format) */
static void hle_ChangeChannelConfig(void) {
    const uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }
    g_audio[ch].format = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceAudioOutput2 and sceVaudio each drive one stream, and their blocking calls
 * pace the thread feeding them just as sceAudioOutputBlocking does. */
static uint32_t g_output2_samples, g_vaudio_samples;

static void pace_stream(int slot, uint32_t samples) {
    /* (volume, buf) */
    submit_block(AUDIO_CHANNELS + slot, psp_arg(1), samples, 1, psp_arg(0), psp_arg(0));
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    const int i = AUDIO_CHANNELS + slot;
    uint64_t now = host_now_us();
    if (g_audio_due_us[i] < now) g_audio_due_us[i] = now;
    g_audio_due_us[i] += play_us;
    wait_audio_due(g_audio_due_us[i], play_us);
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}

static void hle_Output2Reserve(void) { g_output2_samples = psp_arg(0); psp_ret(0); }
static void hle_Output2Release(void) { g_output2_samples = 0; psp_ret(0); }
static void hle_Output2Blocking(void) { pace_stream(0, g_output2_samples); }
static void hle_VaudioReserve(void) { g_vaudio_samples = psp_arg(0); psp_ret(0); }
static void hle_VaudioRelease(void) { g_vaudio_samples = 0; psp_ret(0); }
static void hle_VaudioBlocking(void) { pace_stream(1, g_vaudio_samples); }

/* ---- sceDmac: (dst, src, size), a copy the console makes with its DMA. Done at once here. ---- */
void (*psp_on_dmac_copy)(uint32_t dst, uint32_t size);

static void hle_DmacMemcpy(void) {
    const uint32_t dst = psp_arg(0), src = psp_arg(1), size = psp_arg(2);
    void *to = size ? psp_mem_ptr(dst, size) : NULL;
    const void *from = size ? psp_mem_ptr(src, size) : NULL;
    if (!size) { psp_ret(0); return; }
    if (!to || !from) { psp_ret(0x80000023u); return; }   /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
    memmove(to, from, size);
    if (psp_on_dmac_copy) psp_on_dmac_copy(dst, size);
    psp_ret(0);
}

/* ---- sceRtc -------------------------------------------------------------- *
 * Real time. These are what a game polls in a loop while another thread does
 * something, and nothing preempts a thread here, so every so often a poll gives
 * the CPU away. Without that the thread it waits for never gets to run. */
#define RTC_YIELD_EVERY 32u
#define RTC_TICKS_TO_UNIX 62135596800000000ull   /* microseconds, year 1 to 1970 */

static uint64_t unix_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void hle_RtcGetAccumulativeTime(void) {
    static uint32_t polls;
    if (++polls % RTC_YIELD_EVERY == 0) psp_threadman_yield();
    const uint64_t ticks = unix_now_us() + RTC_TICKS_TO_UNIX;
    psp_cpu.r[PSP_REG_V0] = (uint32_t)ticks;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(ticks >> 32);
}

/* pspTime: u16 year, month, day, hour, minute, second, then u32 microsecond. */
static void hle_RtcGetCurrentClockLocalTime(void) {
    const uint32_t out = psp_arg(0);
    const uint64_t now = unix_now_us();
    const time_t secs = (time_t)(now / 1000000u);
    const struct tm *tm = localtime(&secs);
    if (!tm) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    psp_write16(out + 0, (uint16_t)(tm->tm_year + 1900));
    psp_write16(out + 2, (uint16_t)(tm->tm_mon + 1));
    psp_write16(out + 4, (uint16_t)tm->tm_mday);
    psp_write16(out + 6, (uint16_t)tm->tm_hour);
    psp_write16(out + 8, (uint16_t)tm->tm_min);
    psp_write16(out + 10, (uint16_t)tm->tm_sec);
    psp_write32(out + 12, (uint32_t)(now % 1000000u));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Days since 1970-01-01 in the proleptic Gregorian calendar. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* (const pspTime *date, u64 *seconds since 1970) */
static void hle_RtcGetTime64_t(void) {
    const uint32_t date = psp_arg(0), out = psp_arg(1);
    const int64_t days = days_from_civil(psp_read16(date), psp_read16(date + 2), psp_read16(date + 4));
    const uint64_t secs = (uint64_t)(days * 86400 + psp_read16(date + 6) * 3600 +
                                     psp_read16(date + 8) * 60 + psp_read16(date + 10));
    psp_write32(out, (uint32_t)secs);
    psp_write32(out + 4, (uint32_t)(secs >> 32));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceUmdUser ---------------------------------------------------------- *
 * The disc is a host directory: always inserted, spun up and readable. */
#define UMD_STAT_READY 0x32u   /* present | ready | readable */
static void hle_UmdCheckMedium(void) { psp_ret(1); }
static void hle_UmdGetDriveStat(void) { psp_ret(UMD_STAT_READY); }

/* ---- sceKernelVolatileMem ------------------------------------------------ *
 * The 4 MB the system keeps for itself at 0x08400000 and lends out on request. */
static void hle_VolatileLock(void) {
    if (psp_arg(1)) psp_write32(psp_arg(1), 0x08400000u);
    if (psp_arg(2)) psp_write32(psp_arg(2), 0x00400000u);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The sample count a channel outputs per call. It is set at reserve time and
 * changed here; ignoring the change left WTF's channel at its reserved length,
 * so each blocking output waited for 4,096 samples while the game mixed 256,
 * and its audio ran 16 times slow. */
static void hle_SetChannelDataLen(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }   /* invalid channel */
    g_audio[ch].samples = psp_arg(1);
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: SetChannelDataLen ch %u samples %u\n", ch, psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

void psp_misc_reset(void) {
    /* The interrupt mask is per thread and lives in the scheduler; a reset
     * here cannot reach another thread's bracket, and should not. */
    g_exit_requested = 0;
    g_buttons = 0;
    g_ctrl_seen = 0;
    g_analog_x = g_analog_y = 128;
    g_ctrl_frame = 0;
    memset(g_audio, 0, sizeof g_audio);
#if PSPRECOMP_REAL_THREADS
    memset(g_audio_due_us, 0, sizeof g_audio_due_us);
#endif
    g_audio_blocks = 0;
    g_output2_samples = g_vaudio_samples = 0;
}

void psp_misc_init(void) { psp_misc_reset(); }

void psp_misc_register(void) {
    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_CpuResumeIntr);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",   hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock", hle_ok);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",   hle_ok);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);
    psp_hle_register(0x3A622550, "sceCtrl", "sceCtrlPeekBufferPositive",  hle_ReadBufferPositive);

    psp_hle_register(0x3EE30821, "UtilsForUser", "sceKernelDcacheWritebackRange", hle_CacheOp);
    psp_hle_register(0x3E0271D3, "sceSuspendForUser", "sceKernelVolatileMemLock",   hle_VolatileLock);
    psp_hle_register(0xA569E425, "sceSuspendForUser", "sceKernelVolatileMemUnlock", hle_ok);

    psp_hle_register(0x011F03C1, "sceRtc", "sceRtcGetAccumulativeTime",      hle_RtcGetAccumulativeTime);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime", hle_RtcGetCurrentClockLocalTime);
    psp_hle_register(0xE1C93E47, "sceRtc", "sceRtcGetTime64_t",              hle_RtcGetTime64_t);

    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",               hle_ok);
    psp_hle_register(0x46EBB729, "sceUmdUser", "sceUmdCheckMedium",            hle_UmdCheckMedium);
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat",           hle_UmdGetDriveStat);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",          hle_ok);
    psp_hle_register(0x56202973, "sceUmdUser", "sceUmdWaitDriveStatWithTimer", hle_ok);
    psp_hle_register(0x6AF9B50A, "sceUmdUser", "sceUmdCancelWaitDriveStat",    hle_ok);
    psp_hle_register(0xAEE7404D, "sceUmdUser", "sceUmdRegisterUMDCallBack",    hle_ok);

    psp_hle_register(0x01562BA3, "sceAudio", "sceAudioOutput2Reserve",        hle_Output2Reserve);
    psp_hle_register(0x43196845, "sceAudio", "sceAudioOutput2Release",        hle_Output2Release);
    psp_hle_register(0x2D53F36E, "sceAudio", "sceAudioOutput2OutputBlocking", hle_Output2Blocking);
    psp_hle_register(0x617F3FE6, "sceDmac", "sceDmacMemcpy",              hle_DmacMemcpy);
    psp_hle_register(0x03B6807D, "sceVaudio", "sceVaudioChReserve",      hle_VaudioReserve);
    psp_hle_register(0x67585DFD, "sceVaudio", "sceVaudioChRelease",      hle_VaudioRelease);
    psp_hle_register(0x8986295E, "sceVaudio", "sceVaudioOutputBlocking", hle_VaudioBlocking);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ChangeChannelConfig);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);
}
