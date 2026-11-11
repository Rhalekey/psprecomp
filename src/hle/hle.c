/* psprecomp — HLE dispatch. See include/psprecomp/hle.h. */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLE_MAX 512

static psp_hle_entry g_entry[HLE_MAX];
static psp_hle_fn    g_fn[HLE_MAX];
static int           g_count;
static uint64_t      g_calls[HLE_MAX];      /* per registered entry */

/* Unimplemented NIDs, counted individually: "9 so far" says nothing about
 * whether one call is being made a billion times. */
#define UNIMPL_DISTINCT 64
static struct { uint32_t nid; uint64_t n; } g_unimpl[UNIMPL_DISTINCT];
static int g_unimpl_n;

void psp_hle_register(uint32_t nid, const char *lib, const char *name, psp_hle_fn fn) {
    /* Re-registering replaces, so a game repo can override one function
     * without forking the table. */
    for (int i = 0; i < g_count; i++) {
        if (g_entry[i].nid == nid) { g_fn[i] = fn; g_entry[i].name = name; return; }
    }
    if (g_count >= HLE_MAX) return;
    g_entry[g_count].nid = nid;
    g_entry[g_count].lib = lib;
    g_entry[g_count].name = name;
    g_fn[g_count] = fn;
    g_count++;
}

/* Register a NID whose name is genuinely unknown.
 *
 * Every named entry is verified against SHA-1 of its own name, which is what
 * makes the table self-checking -- and which correctly refuses a made-up name.
 * But a firmware entry point can be *observed* without being *identified*: the
 * game calls it, the NID is exact, and no plausible name hashes to it.
 *
 * The wrong response is to invent a name so the entry can be registered. That
 * produces a table which runs, looks right, and lies about what it implements.
 * The right response is to make "unidentified" representable: registered and
 * callable, marked as unnamed, and skipped by the NID check by construction
 * rather than by exception. */
void psp_hle_register_unnamed(uint32_t nid, const char *lib, psp_hle_fn fn) {
    psp_hle_register(nid, lib, NULL, fn);
}

int psp_hle_is_named(int index) {
    return index >= 0 && index < g_count && g_entry[index].name != NULL;
}

const char *psp_hle_name(uint32_t nid) {
    for (int i = 0; i < g_count; i++)
        if (g_entry[i].nid == nid) return g_entry[i].name;
    return NULL;
}

int psp_hle_count(void) { return g_count; }

const psp_hle_entry *psp_hle_entries(int *count) {
    if (count) *count = g_count;
    return g_entry;
}

/* A short history of firmware calls that returned zero.
 *
 * Zero is the value a game most often treats as an address, so a stubbed or
 * failing call returning it tends to surface far away as a wild pointer --
 * typically a table walk starting near address 0. When that happens the useful
 * question is "which call handed the game a null?", and by then the call is
 * long gone. Keeping the last few makes the answer immediate instead of
 * requiring a second run with different instrumentation. */
#define ZERO_HISTORY 16
static struct { uint32_t nid; const char *name; } g_zero[ZERO_HISTORY];
/* Every zero-returning call, not just the last few. A game that spins on a
 * stubbed call reaches billions of these, so this has to be wider than int:
 * an `int` here wraps, and the wrap then travels into the dump, which prints a
 * negative count and indexes the ring with a negative offset. Measured on
 * LocoRoco: `last -1396532617 firmware calls`. */
static uint64_t g_zero_n;

static void note_zero(uint32_t nid, const char *name) {
    g_zero[g_zero_n % ZERO_HISTORY].nid = nid;
    g_zero[g_zero_n % ZERO_HISTORY].name = name;
    g_zero_n++;
}

void psp_hle_dump_recent(FILE *out) {
    if (!g_zero_n) {
        fprintf(out, "  (no firmware call returned zero)\n");
        return;
    }
    unsigned n = (unsigned)(g_zero_n < ZERO_HISTORY ? g_zero_n : ZERO_HISTORY);
    /* Read this list with care: SCE_KERNEL_ERROR_OK is also zero, so a
     * successful call that returns nothing appears here exactly like a
     * handle-returning call that failed. The entries worth suspecting are the
     * ones whose name implies an address or an id -- GetBlockHeadAddr,
     * GetModuleId, the Create/Alloc family. The rest are noise.
     *
     * That ambiguity is a limitation of this instrument, not of the runtime:
     * distinguishing the two needs per-function knowledge of what the return
     * value means, which the table does not currently carry. */
    fprintf(out, "  last %u firmware calls that returned zero (%llu in total),"
                 " newest first\n"
                 "  (note: success is also zero -- see the comment in hle.c):\n",
            n, (unsigned long long)g_zero_n);
    for (unsigned i = 1; i <= n; i++) {
        unsigned k = (unsigned)((g_zero_n - i) % ZERO_HISTORY);
        fprintf(out, "    0x%08X  %s\n", g_zero[k].nid,
                g_zero[k].name ? g_zero[k].name : "(unimplemented)");
    }
}

/* How many unimplemented calls to name before falling silent.
 *
 * The message used to print once per call, which is fine for a game that makes
 * a handful and ruinous for one that spins: a single 30-second run of LocoRoco
 * waiting on one stubbed call wrote 26 926 971 lines -- 1.37 GB of log, for one
 * call worth naming. Bring-up produces that shape constantly, so the ceiling is
 * not cosmetic, and neither is the total: it is what keeps a silenced message
 * distinguishable from a message that stopped happening. */
#define UNIMPL_LOGGED 8
static uint64_t g_unimpl_total;
static uint64_t g_unimpl_logged;

void psp_hle_call(uint32_t nid) {
    if (nid == 0x237DBD4Fu)
        fprintf(stderr, "psp_hle_call ENTERED for 0x237DBD4F (%d entries registered)\n", g_count);
    for (int i = 0; i < g_count; i++) {
        if (g_entry[i].nid == nid) {
            g_calls[i]++;
            g_fn[i]();
            if (psp_cpu.r[PSP_REG_V0] == 0) note_zero(nid, g_entry[i].name);
            return;
        }
    }
    note_zero(nid, NULL);

    /* Unimplemented. Naming the function is the whole point — bringing a game
     * up is largely the process of watching this message stop appearing, and
     * "0x237DBD4F" is far less use than "sceKernelAllocPartitionMemory".
     *
     * Returning 0 rather than aborting is deliberate: many firmware calls are
     * advisory (version reporting, profiling hooks) and a game will run past
     * them happily. One that genuinely needed the result will fail visibly
     * soon after, with this line already in the log. */
    g_unimpl_total++;
    {
        int k = 0;
        while (k < g_unimpl_n && g_unimpl[k].nid != nid) k++;
        if (k == g_unimpl_n && g_unimpl_n < UNIMPL_DISTINCT) g_unimpl[g_unimpl_n++].nid = nid;
        if (k < g_unimpl_n) g_unimpl[k].n++;
    }
    if (g_unimpl_logged < UNIMPL_LOGGED) {
        g_unimpl_logged++;
        fprintf(stderr, "psprecomp: unimplemented firmware call 0x%08X\n", nid);
    } else if (g_unimpl_logged == UNIMPL_LOGGED) {
        /* Say so, once. A message that stopped appearing because it was silenced
         * must not read as a message that stopped happening: "bringing a game up
         * is largely the process of watching those messages stop appearing" is
         * how progress is measured here, so the instrument has to keep the two
         * apart. The total below the message is what makes them apart. */
        g_unimpl_logged++;
        fprintf(stderr,
                "psprecomp: further unimplemented firmware calls are counted, not"
                " printed -- %llu so far, see psp_hle_unimplemented_calls()\n",
                (unsigned long long)g_unimpl_total);
    }
    psp_ret(0);
}

/* Total unimplemented calls, including the ones past the ceiling above: the
 * number that says whether a silenced message is one call or a billion. */
uint64_t psp_hle_unimplemented_calls(void)  { return g_unimpl_total; }
uint64_t psp_hle_unimplemented_logged(void) { return g_unimpl_logged; }

void psp_hle_unimplemented_reset(void) {
    g_unimpl_total = 0;
    g_unimpl_logged = 0;
}

uint64_t psp_hle_zero_calls(void) { return g_zero_n; }

/* Every firmware function called, busiest first. A game that is slow rather
 * than stuck shows here what it is spending its calls on. */
void psp_hle_dump_counts(FILE *out, int limit) {
    typedef struct { uint64_t n; uint32_t nid; const char *name; } count_row;
    count_row row[HLE_MAX + UNIMPL_DISTINCT];
    int rows = 0;
    for (int i = 0; i < g_count; i++)
        if (g_calls[i]) { row[rows].n = g_calls[i]; row[rows].nid = g_entry[i].nid; row[rows].name = g_entry[i].name; rows++; }
    for (int k = 0; k < g_unimpl_n; k++) { row[rows].n = g_unimpl[k].n; row[rows].nid = g_unimpl[k].nid; row[rows].name = NULL; rows++; }
    for (int i = 1; i < rows; i++)          /* insertion sort: tens of rows */
        for (int j = i; j > 0 && row[j].n > row[j - 1].n; j--) {
            count_row t = row[j]; row[j] = row[j - 1]; row[j - 1] = t;
        }
    fprintf(out, "firmware calls, busiest first:\n");
    for (int i = 0; i < rows && i < limit; i++)
        fprintf(out, "  %12llu  0x%08X  %s\n", (unsigned long long)row[i].n, row[i].nid,
                row[i].name ? row[i].name : "(unimplemented)");
}

const char *psp_str(uint32_t addr, char *dst, size_t cap) {
    size_t n = 0;
    if (cap == 0) return dst;
    while (n + 1 < cap) {
        uint8_t c = psp_read8(addr + (uint32_t)n);
        if (!c) break;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return dst;
}

static void miss_context(void) { psp_trace_dump(); psp_hle_dump_recent(stderr); }

void psp_hle_init(void) {
    psp_set_miss_context(miss_context);
    psp_sysmem_init();
    psp_sysmem_register();
    psp_threadman_init();
    psp_threadman_register();
    psp_intrman_init();
    psp_intrman_register();
    psp_display_init();
    psp_display_register();
    psp_ge_init();
    psp_ge_register();
    psp_sas_init();
    psp_sas_register();
    psp_io_init();
    psp_io_register();
    psp_misc_init();
    psp_misc_register();
    psp_sync_init();
    psp_sync_register();
    psp_utility_init();
    psp_utility_register();
    psp_power_init();
    psp_power_register();
    psp_mpeg_init();
    psp_mpeg_register();
    psp_netadhoc_register();
}
