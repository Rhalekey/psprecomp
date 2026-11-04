/* Function discovery. See analyze.h. */

#include "analyze.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a growable uint32 list ---------------------------------------------- */

typedef struct { uint32_t *v; int n, cap; } u32list;

static int u32_push(u32list *l, uint32_t x) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        uint32_t *v = (uint32_t *)realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = x;
    return 0;
}

static int u32_contains(const u32list *l, uint32_t x) {
    for (int i = 0; i < l->n; i++) if (l->v[i] == x) return 1;
    return 0;
}

/* ---- per-word bookkeeping ------------------------------------------------ */

#define SEEN_CODE  0x01   /* decoded as an instruction */
#define SEEN_ENTRY 0x02   /* known function entry */
#define SEEN_SLOT  0x04   /* visited only as a delay slot; what follows it was not walked */

static uint32_t word_index(const a_analysis *an, uint32_t addr) {
    return (addr - an->base) >> 2;
}

int a_in_range(const a_analysis *an, uint32_t addr) {
    return addr >= an->base && addr < an->base + an->size && (addr & 3) == 0;
}

static int is_import_stub(const a_analysis *an, uint32_t addr) {
    return an->stub_size && addr >= an->stub_addr &&
           addr < an->stub_addr + an->stub_size;
}

static uint32_t fetch(const a_analysis *an, uint32_t addr) {
    const uint8_t *p = an->code + (addr - an->base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- the walk ------------------------------------------------------------ */

typedef struct {
    a_analysis *an;
    uint8_t    *seen;
    /* Every address known to be a function entry, marked before any walking
     * starts. Knowing the full entry set up front is what lets a walk tell a
     * tail call from a local jump, and lets it stop when it runs off the end
     * of one function into the start of the next. */
    uint8_t    *entry_map;
    /* Ownership of each word, and the entry of the function currently being
     * traced. Recorded as the walk proceeds so the emitter knows exactly which
     * instructions belong to which function. */
    uint32_t   *owner;
    uint32_t    cur_owner;
    u32list    *func_queue;
    u32list    *imports;
    u32list    *indirects;
    /* Branch targets already claimed by an earlier walk; promoted to entries
     * between rounds so they become reachable through dispatch. */
    u32list    *cross;
} walk_ctx;

static int is_known_entry(const walk_ctx *c, uint32_t addr) {
    if (!a_in_range(c->an, addr)) return 0;
    return c->entry_map[word_index(c->an, addr)] != 0;
}

static int resolve_jump_table(a_analysis *an, uint32_t site, u32list *out);

/* Follow a control transfer to `target`, a block of the function being walked.
 *
 * A word this same walk already reached is not a shared block: loop back-edges
 * and branches that join after an if/else arrive here all the time. Only a word
 * owned by another function is promoted, since the emitter cannot `goto` into a
 * different C function. Promoting our own blocks too splits every loop and join
 * into its own function, which still runs but hides where functions really end.
 *
 * A delay slot is marked as seen without being walked. Branching to one means
 * execution carries on past it, so that continuation still has to be walked. */
static void follow_block(walk_ctx *c, u32list *blocks, uint32_t target) {
    uint32_t ti = word_index(c->an, target);
    if (!(c->seen[ti] & SEEN_CODE) || c->entry_map[ti]) {
        u32_push(blocks, target);
    } else if (c->owner[ti] != c->cur_owner) {
        u32_push(c->cross, target);
    } else if (c->seen[ti] & SEEN_SLOT) {
        if (a_in_range(c->an, target + 4)) u32_push(blocks, target + 4);
    }
}

/* Record a call target. Import thunks are boundaries, not functions to walk
 * into: they get patched at load time and their contents in the file are
 * meaningless. Everything else is a function entry. */
static void note_call(walk_ctx *c, uint32_t target) {
    if (is_import_stub(c->an, target)) {
        if (!u32_contains(c->imports, target)) u32_push(c->imports, target);
        return;
    }
    if (!a_in_range(c->an, target)) return;
    /* A target only reached by walking (a call from outside the scanned range)
     * is not in the entry set yet, and a re-walk starts from that set alone. */
    if (c->an->scan_calls && !c->entry_map[word_index(c->an, target)])
        u32_push(c->cross, target);
    u32_push(c->func_queue, target);
}

/* Walk one function from `entry`, filling in `out`. Returns 0 if anything was
 * decoded, -1 if the entry was unusable. */
static int trace_function(walk_ctx *c, uint32_t entry, a_func *out) {
    a_analysis *an = c->an;
    if (!a_in_range(an, entry)) return -1;

    u32list blocks = { 0 };
    if (u32_push(&blocks, entry) != 0) return -1;

    uint32_t end = entry;
    uint32_t lo  = entry;
    uint32_t insns = 0;
    unsigned has_return = 0, has_indirect = 0, has_vfpu = 0;

    /* Instructions visited by *this* function, so a block shared with an
     * earlier function does not get walked twice here but is still counted
     * once globally. */
    while (blocks.n) {
        uint32_t a = blocks.v[--blocks.n];

        while (a_in_range(an, a)) {
            uint32_t idx = word_index(an, a);
            if (c->seen[idx] & SEEN_CODE) {         /* already walked */
                /* Running into another function's block needs the same promotion
                 * as branching to it, or the emitter has nowhere to fall to. */
                if (c->owner[idx] != c->cur_owner) {
                    if (!c->entry_map[idx]) u32_push(c->cross, a);
                } else if ((c->seen[idx] & SEEN_SLOT) && a_in_range(an, a + 4)) {
                    u32_push(&blocks, a + 4);
                }
                break;
            }

            /* Arriving at a different function's entry means we walked off the
             * end of this one — a tail call, or a function that ends without a
             * visible return. Stop; that address is walked as its own function.
             * Without this a single trace swallows every function that follows
             * it in address order. */
            if (a != entry && is_known_entry(c, a)) break;

            a_insn in;
            a_decode(fetch(an, a), a, &in);
            c->seen[idx] |= SEEN_CODE;
            c->owner[idx] = c->cur_owner;
            insns++;
            an->insns++;
            if (a + 4 > end) end = a + 4;
            if (a < lo) lo = a;

            if (in.op == A_INVALID) {
                /* Almost always data reached by a bad path. Stop this trace
                 * rather than manufacturing instructions out of it. */
                an->invalid++;
                break;
            }
            if (in.op == A_VFPU_UNKNOWN) { an->vfpu++; has_vfpu = 1; }

            /* Every branch and jump has a delay slot that executes before
             * control transfers, so it is always part of this function and is
             * always visited — including on paths that leave here. */
            if (in.has_delay_slot) {
                uint32_t d = a + 4;
                if (a_in_range(an, d)) {
                    uint32_t didx = word_index(an, d);
                    if (!(c->seen[didx] & SEEN_CODE)) {
                        a_insn din;
                        a_decode(fetch(an, d), d, &din);
                        c->seen[didx] |= SEEN_CODE | SEEN_SLOT;
                        c->owner[didx] = c->cur_owner;
                        insns++;
                        an->insns++;
                        if (din.op == A_VFPU_UNKNOWN) { an->vfpu++; has_vfpu = 1; }
                        if (din.op == A_INVALID) an->invalid++;
                    }
                    if (d + 4 > end) end = d + 4;
                    if (d < lo) lo = d;
                }
            }

            /* A branch or tail-jump into the import thunks is a call into
             * firmware by another name. It cannot be followed — the thunks are
             * patched at load time and their file contents are meaningless —
             * but it must be *recorded*, because the emitter decides what is an
             * import purely by address range. If the walk does not record it,
             * the generated code references psp_import_<addr> and the imports
             * file never defines it: a link error, and one that only shows up
             * on a module whose imports are not all reached by `jal`.
             *
             * The stub region sits immediately after `.text`, so it is outside
             * the analysed range and the branch/jump paths below skip it
             * silently. Hence the explicit check here. */
            if (in.has_target && !in.is_indirect && is_import_stub(an, in.target)) {
                if (!u32_contains(c->imports, in.target))
                    u32_push(c->imports, in.target);
            }

            if (in.is_call) {
                /* jal / jalr / the *al REGIMM forms. Direct calls give us a
                 * new function; indirect ones we cannot resolve statically.
                 * A get-PC `jal .+8` gives nothing: its target is where this
                 * walk resumes anyway. */
                if (in.has_target && !in.is_indirect && !in.is_get_pc) note_call(c, in.target);
                else if (in.is_indirect) has_indirect = 1;
                a += 8;                     /* past the delay slot; calls return */
                continue;
            }

            if (in.is_return) { has_return = 1; break; }

            if (in.is_indirect) {
                /* `jr $rN` — a computed jump, usually a switch table. We
                 * cannot follow it without resolving the table, so record the
                 * site and stop. These are the concrete targets for jump-table
                 * analysis; they are counted rather than silently dropped.
                 *
                 * When the tables were resolved up front, the cases are blocks of
                 * this function, and only a site with no table stays recorded. */
                has_indirect = 1;
                int ncases = 0;
                if (an->case_map) {
                    u32list cases = { 0 };
                    ncases = resolve_jump_table(an, a, &cases);
                    for (int i = 0; i < cases.n; i++) follow_block(c, &blocks, cases.v[i]);
                    free(cases.v);
                }
                if (ncases <= 0 && !u32_contains(c->indirects, a)) u32_push(c->indirects, a);
                break;
            }

            if (in.is_branch) {
                /* Conditional: the target is another block of this function,
                 * and execution also falls through past the delay slot. */
                if (in.has_target && a_in_range(an, in.target)) {
                    /* If another walk already claimed the target, two functions
                     * share this block, which is a walk-order artefact rather
                     * than a property of the code.
                     *
                     * The emitter cannot express a branch into another C
                     * function's middle; it degrades to a dispatch, which then
                     * misses because the address is not an entry. Promoting it
                     * to one makes it reachable. Over-split is the safe
                     * direction; unreachable is not. */
                    follow_block(c, &blocks, in.target);
                }
                a += 8;
                continue;
            }

            if (in.is_jump) {
                /* Unconditional `j`. MIPS compilers emit `b` — which is
                 * `beq $zero, $zero` and decodes as a branch — for local
                 * unconditional flow inside a function. A `j` is therefore
                 * almost always either a tail call or a loop back-edge, and
                 * the direction tells them apart:
                 *
                 *   backward -> a loop back-edge, a block of this function
                 *   forward  -> a tail call, the entry of another function
                 *
                 * Observed in this module as two-instruction thunks:
                 *     j 0x00091E38 ; addiu $a0, $zero, 2
                 * Treating that forward `j` as local flow pulls the callee
                 * into the thunk and, transitively, swallows most of .text
                 * into one 121 KB "function".
                 *
                 * Over-splitting is the safe direction to err: a wrongly split
                 * function is still correct code reached through the dispatch
                 * table, whereas a wrongly merged one has bogus boundaries. */
                if (in.has_target && a_in_range(an, in.target)) {
                    uint32_t bi = word_index(an, in.target);
                    int is_case = an->case_map && an->case_map[bi] && !c->entry_map[bi];
                    /* With a whole module's entries known, a forward `j` that
                     * crosses none of them stays inside this function. Compilers
                     * that use `j` for local flow would otherwise split here. */
                    int is_local = 0;
                    if (an->case_map && in.target > a && !c->entry_map[bi]) {
                        is_local = 1;
                        for (uint32_t t = a + 4; t < in.target; t += 4)
                            if (c->entry_map[word_index(an, t)]) { is_local = 0; break; }
                    }
                    if (is_case || is_local ||
                        (in.target <= a && a != entry && !is_known_entry(c, in.target))) {
                        /* Backward `j` -- a loop back-edge into this function's
                         * own blocks, unless another walk already claimed the
                         * target. Then the two functions share a block, exactly
                         * as for a conditional branch, and for the same reason:
                         * walk order, not a property of the code.
                         *
                         * The emitter cannot express a jump into another C
                         * function's middle. It degrades to a dispatch, which
                         * misses because the address was never made an entry --
                         * observed as `j 0x0000E588` inside the allocator,
                         * emitted as a dispatch to an undiscovered address
                         * and missing at run time.
                         *
                         * The branch path above has always promoted these. The
                         * jump path did not, so a whole class of shared block
                         * stayed unreachable.
                         *
                         * A forward `j` to a case label is local flow too, not a
                         * tail call: the label belongs to this switch. */
                        follow_block(c, &blocks, in.target);
                    } else {
                        uint32_t ti = word_index(an, in.target);
                        if (!c->entry_map[ti]) {
                            c->entry_map[ti] = 1;
                            u32_push(c->func_queue, in.target); /* tail call */
                        }
                    }
                }
                break;
            }

            a += 4;
        }
    }

    free(blocks.v);

    if (!insns) return -1;

    memset(out, 0, sizeof *out);
    out->addr = entry;
    out->start = lo;
    out->end = end;
    out->insns = insns;
    out->has_return = has_return;
    out->has_indirect = has_indirect;
    out->has_vfpu = has_vfpu;
    return 0;
}

/* ---- jump tables --------------------------------------------------------- */

/* MIPS compiles a `switch` into a bounds check, a scaled index, a table load
 * and an indirect jump:
 *
 *     sltiu $at, $key, N          bounds check -- N is the entry count
 *     beqz  $at, default
 *     sll   $t0, $key, 2          scale to a word index
 *     lui   $t1, %hi(table)
 *     addu  $t0, $t0, $t1
 *     lw    $t0, %lo(table)($t0)  load the target
 *     jr    $t0
 *
 * Recovering it means walking backward from the `jr` for the `lw` that fed it,
 * then for the lui/addiu pair that formed the address, then for the sltiu that
 * bounded it. The instructions may be interleaved with unrelated ones, so the
 * search is by register rather than by position.
 *
 * The result is cheap to *verify*, which is what makes this safe: every entry
 * must land inside .text, be instruction-aligned, and decode as a valid
 * instruction. A misidentified table fails those and is rejected wholesale
 * rather than seeding garbage.
 */

static uint32_t image_read32(const a_analysis *an, uint32_t addr) {
    if (!an->image) return 0;
    if (addr < an->image_base || addr + 4 > an->image_base + an->image_size) return 0;
    const uint8_t *p = an->image + (addr - an->image_base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* How far back to look. A compiler keeps the whole idiom close together; a
 * wider window mostly finds unrelated instructions that happen to match. */
#define JT_WINDOW 24
#define JT_MAX_ENTRIES 512

static int resolve_jump_table(a_analysis *an, uint32_t site, u32list *out) {
    if (!an->image) return 0;

    a_insn jr;
    a_decode(fetch(an, site), site, &jr);
    const uint8_t target_reg = jr.rs;

    /* Walk back for `lw target_reg, lo(base)`. */
    uint32_t lw_addr = 0;
    a_insn lw;
    memset(&lw, 0, sizeof lw);
    for (int i = 1; i <= JT_WINDOW; i++) {
        uint32_t a = site - (uint32_t)i * 4;
        if (!a_in_range(an, a)) break;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.op == A_LW && in.rt == target_reg) { lw = in; lw_addr = a; break; }
        /* If something else wrote the register first, this is not the idiom. */
        if (in.op == A_ADDIU && in.rt == target_reg) return 0;
    }
    if (!lw_addr) return 0;

    /* The lw's base is `index + tablebase`, formed by an `addu`. One operand
     * is the scaled index (from `sll`), the other is the table address. The
     * address itself is built as lui+addiu -- NOT as the lw's displacement,
     * which is typically zero:
     *
     *     lui   $t3, %hi(table)
     *     sll   $t0, $key, 2
     *     addiu $t2, $t3, %lo(table)
     *     addu  $t0, $t0, $t2
     *     lw    $rN, 0($t0)
     *
     * An earlier attempt looked for the low half in the lw displacement, which
     * is the other common form, and so matched nothing here. */
    uint8_t cand[2];
    int ncand = 0;
    for (int i = 1; i <= JT_WINDOW && ncand == 0; i++) {
        uint32_t a = lw_addr - (uint32_t)i * 4;
        if (!a_in_range(an, a)) break;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.op == A_ADDU && in.rd == lw.rs) {
            cand[0] = in.rs;
            cand[1] = in.rt;
            ncand = 2;
        }
    }
    if (!ncand) return 0;

    /* Whichever operand traces back to an lui+addiu pair is the table base;
     * the other is the index. */
    uint32_t table = 0;
    int found = 0;
    for (int k = 0; k < 2 && !found; k++) {
        uint8_t reg = cand[k];
        for (int i = 1; i <= JT_WINDOW && !found; i++) {
            uint32_t a = lw_addr - (uint32_t)i * 4;
            if (!a_in_range(an, a)) break;
            a_insn ai;
            a_decode(fetch(an, a), a, &ai);
            if (ai.op != A_ADDIU || ai.rt != reg) continue;

            /* Found the low half; now the lui that set its source. */
            for (int j = 1; j <= JT_WINDOW; j++) {
                uint32_t b = a - (uint32_t)j * 4;
                if (!a_in_range(an, b)) break;
                a_insn li;
                a_decode(fetch(an, b), b, &li);
                if (li.op == A_LUI && li.rt == ai.rs) {
                    table = ((uint32_t)li.imm << 16) + (uint32_t)ai.imm
                          + (uint32_t)lw.imm;
                    found = 1;
                    break;
                }
            }
        }
    }
    if (!found) return 0;

    /* Bound the entry count with the sltiu, if we can find it. Without one,
     * fall back to reading until an entry stops looking like code -- which the
     * per-entry validation below makes safe. */
    uint32_t limit = JT_MAX_ENTRIES;
    for (int i = 1; i <= JT_WINDOW * 2; i++) {
        uint32_t a = site - (uint32_t)i * 4;
        if (!a_in_range(an, a)) break;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.op == A_SLTIU && in.imm > 0 && (uint32_t)in.imm <= JT_MAX_ENTRIES) {
            limit = (uint32_t)in.imm;
            break;
        }
    }

    /* Read and validate. Every entry must be code; the first that is not ends
     * the table. A table that yields nothing valid is rejected entirely. */
    int taken = 0;
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t entry = image_read32(an, table + i * 4);
        if (!a_in_range(an, entry)) break;

        a_insn probe;
        if (!a_decode(fetch(an, entry), entry, &probe) || probe.op == A_INVALID) break;

        u32_push(out, entry);
        taken++;
    }

    /* One entry is not evidence of a table -- a single plausible word turns up
     * by chance. Two consecutive valid code pointers is a much stronger
     * signal. */
    if (taken < 2) {
        out->n -= taken;
        return 0;
    }
    return taken;
}

/* Does control never fall through this instruction? */
static int ends_flow(const a_insn *in) {
    if (in->is_return) return 1;
    if (in->is_call) return 0;
    if (in->is_jump || in->is_indirect) return 1;
    return in->is_branch && in->op == A_BEQ && in->rs == 0 && in->rt == 0;
}

/* Promote what no walk has reached in the scanned range.
 *
 * Whatever seeds are used, some function is always only reachable through an
 * address built in a way no scan recognises, and a call to it ends the program.
 * Every word of `.text` decodes, so an unreached stretch is code rather than
 * data, and it costs nothing to have it available. A stretch begins where the
 * code before it stopped: after a walked word, or after an unconditional transfer
 * and its delay slot, past any nop padding. */
static void sweep_gaps(a_analysis *an, const uint8_t *seen, u32list *out) {
    const uint32_t sbase = an->scan_size ? an->scan_base : an->base;
    const uint32_t ssize = an->scan_size ? an->scan_size : an->size;
    for (uint32_t off = 0; off + 4 <= ssize; off += 4) {
        const uint32_t a = sbase + off;
        if (!a_in_range(an, a)) continue;
        const uint32_t i = word_index(an, a);
        if ((seen[i] & SEEN_CODE) || fetch(an, a) == 0) continue;
        if (an->case_map && an->case_map[i]) continue;   /* reached from its `jr` */

        uint32_t p = a;
        while (p > sbase && !(seen[word_index(an, p - 4)] & SEEN_CODE) && fetch(an, p - 4) == 0) p -= 4;
        int starts = p == sbase || (seen[word_index(an, p - 4)] & SEEN_CODE);
        a_insn in;
        if (!starts) {
            a_decode(fetch(an, p - 4), p - 4, &in);
            starts = ends_flow(&in);
        }
        if (!starts && p - 8 >= sbase) {
            a_decode(fetch(an, p - 8), p - 8, &in);
            starts = ends_flow(&in);
        }
        if (starts) u32_push(out, a);
    }
}

static int cmp_func(const void *a, const void *b) {
    uint32_t x = ((const a_func *)a)->addr, y = ((const a_func *)b)->addr;
    return (x > y) - (x < y);
}

int a_discover(a_analysis *an, const uint32_t *seeds, int nseeds) {
    an->funcs = NULL; an->nfuncs = 0;
    an->imports = NULL; an->nimports = 0;
    an->indirects = NULL; an->nindirects = 0;
    an->insns = an->vfpu = an->invalid = 0;
    an->ntables = an->ntable_targets = 0;
    an->bytes_reached = 0;
    an->case_map = NULL;
    /* The import stubs sit at the end of .text, so their extent is the best
     * available executable bound when no section header gives one. */
    if (!an->text_size && an->stub_addr && an->stub_size)
        an->text_size = an->stub_addr + an->stub_size - an->base;

    const uint32_t nwords = an->size >> 2;
    uint8_t *seen = (uint8_t *)calloc(nwords ? nwords : 1, 1);
    if (!seen) return -1;

    uint8_t *entry_map = (uint8_t *)calloc(nwords ? nwords : 1, 1);
    if (!entry_map) { free(seen); return -1; }

    uint32_t *owner = (uint32_t *)malloc((nwords ? nwords : 1) * sizeof *owner);
    if (!owner) { free(seen); free(entry_map); return -1; }
    for (uint32_t i = 0; i < nwords; i++) owner[i] = A_NO_OWNER;

    u32list queue = { 0 }, imports = { 0 }, indirects = { 0 }, cross = { 0 };
    walk_ctx ctx = { an, seen, entry_map, owner, A_NO_OWNER,
                     &queue, &imports, &indirects, &cross };

    /* Find the switch tables before choosing any entries.
     *
     * A table's entries are code addresses stored in data, so the pointer scan
     * reports them as functions and every case gets split off from its switch.
     * Knowing them first keeps them out of the seeds; the walk then reaches them
     * from their `jr` as ordinary blocks. A real `jal` target stays an entry.
     *
     * Only with `scan_calls`, which is how a caller says it has a whole module
     * to look at. A table the resolver misses is no worse off than before: its
     * cases are still seeded and split. */
    if (an->scan_calls && an->image) {
        an->case_map = (uint8_t *)calloc(nwords ? nwords : 1, 1);
        if (an->case_map) {
            uint32_t sbase = an->scan_size ? an->scan_base : an->base;
            uint32_t ssize = an->scan_size ? an->scan_size : an->size;
            for (uint32_t off = 0; off + 4 <= ssize; off += 4) {
                uint32_t a = sbase + off;
                if (!a_in_range(an, a)) continue;
                a_insn in;
                a_decode(fetch(an, a), a, &in);
                if (!in.is_indirect || in.is_call || in.is_return) continue;

                u32list cases = { 0 };
                int n = resolve_jump_table(an, a, &cases);
                if (n > 0) {
                    an->ntables++;
                    an->ntable_targets += n;
                    for (int i = 0; i < cases.n; i++)
                        an->case_map[word_index(an, cases.v[i])] = 1;
                }
                free(cases.v);
            }
        }
    }

    /* Pass 1: establish the complete set of function entries before walking
     * anything. Both the tail-call test and the ran-off-the-end test need to
     * ask "is this somebody's entry?", and neither can answer correctly if
     * entries are still being discovered as the walk proceeds. */
    for (int i = 0; i < nseeds; i++) {
        if (!a_in_range(an, seeds[i])) continue;
        if (an->case_map && an->case_map[word_index(an, seeds[i])]) continue;
        entry_map[word_index(an, seeds[i])] = 1;
        u32_push(&queue, seeds[i]);
    }

    /* Harvest `jal` targets by linear scan. See the note in analyze.h for why
     * this is required rather than a nicety on PSP. Duplicates are harmless —
     * the entry map makes the second sighting a no-op — so no membership test
     * is done here, which keeps this O(n) rather than O(n^2). */
    if (an->scan_calls) {
        /* Restricted to the scan range -- see the note in analyze.h. Falls back
         * to the whole extent if the caller did not set one. */
        uint32_t sbase = an->scan_size ? an->scan_base : an->base;
        uint32_t ssize = an->scan_size ? an->scan_size : an->size;
        for (uint32_t off = 0; off + 4 <= ssize; off += 4) {
            uint32_t a = sbase + off;
            if (!a_in_range(an, a)) continue;
            a_insn in;
            a_decode(fetch(an, a), a, &in);
            if (in.op != A_JAL || !in.has_target || in.is_get_pc) continue;
            if (!a_in_range(an, in.target) || is_import_stub(an, in.target)) continue;

            uint32_t ti = word_index(an, in.target);
            if (entry_map[ti]) continue;
            entry_map[ti] = 1;
            u32_push(&queue, in.target);
        }
    }

    /* Harvest lui+addiu/ori immediate-constructed code pointers too. A direct
     * `jal` is not the only way a function address reaches the program: the
     * common case this catches is sceKernelCreateThread's second argument --
     * the real entry point is built as a bare value right before the call
     * and handed off as data, so it is never a jal target itself (the call
     * target is the firmware import stub) and the scan above misses it.
     * Confirmed on this module: `lui $s1,0x882 / addiu $a1,$s1,0x182C` right
     * before `jal sceKernelCreateThread`, address 0x0882182C never reached
     * by anything else -- not a relocation, not a jal target, not in any
     * data table. Any lui whose paired addiu/ori yields an address that is
     * in range and decodes as valid code is promoted to an entry, same
     * over-split-is-safe reasoning as everywhere else in this file. */
    if (an->scan_calls) {
        uint32_t sbase = an->scan_size ? an->scan_base : an->base;
        uint32_t ssize = an->scan_size ? an->scan_size : an->size;
        for (uint32_t off = 0; off + 4 <= ssize; off += 4) {
            uint32_t a = sbase + off;
            if (!a_in_range(an, a)) continue;
            a_insn in;
            a_decode(fetch(an, a), a, &in);
            if (in.op != A_LUI) continue;

            for (int k = 1; k <= 4; k++) {
                uint32_t b = a + (uint32_t)k * 4;
                if (!a_in_range(an, b)) break;
                a_insn in2;
                a_decode(fetch(an, b), b, &in2);
                if (in2.op == A_LUI && in2.rt == in.rt) break; /* overwritten first */
                if ((in2.op == A_ADDIU || in2.op == A_ORI) && in2.rs == in.rt) {
                    uint32_t addr = (in2.op == A_ORI)
                        ? (((uint32_t)in.imm << 16) | (uint32_t)in2.imm)
                        : (((uint32_t)in.imm << 16) + (uint32_t)in2.imm);
                    /* The target must be in the scanned range too, not just
                     * the image: lui+addiu builds far more data pointers than
                     * code pointers, and nearly any data word decodes. */
                    if (a_in_range(an, addr) && !is_import_stub(an, addr) &&
                        addr - sbase < ssize) {
                        a_insn probe;
                        if (a_decode(fetch(an, addr), addr, &probe) && probe.op != A_INVALID) {
                            uint32_t ti = word_index(an, addr);
                            if (!entry_map[ti] && !(an->case_map && an->case_map[ti])) {
                                entry_map[ti] = 1;
                                u32_push(&queue, addr);
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    a_func *funcs = NULL;
    int nfuncs = 0, cap = 0;

    /* Two rounds. The first walks everything reachable by control flow; the
     * second walks whatever the jump tables reveal, which can only be resolved
     * once the `jr` sites have been found. Tables discovered in round two are
     * not chased further -- one level covers the switch statements a compiler
     * actually emits, and unbounded rounds would need a fixpoint check for
     * very little gain. */
    /* Iterate to a fixpoint. Each re-walk can expose cross-function branches that
 * the previous walk order hid, so a fixed two rounds leaves some unpromoted --
 * which shows up as a dispatch miss at run time. The cap is a termination
 * guarantee, not an expected limit: entries only ever accumulate, so this
 * converges in a handful of rounds. */
    for (int round = 0; round < 16; round++) {
    if (round > 0) {
        if (!an->image && !cross.n && !indirects.n) break;
        u32list targets = { 0 };
        /* Every round re-resolves the same sites, so count only the latest. */
        if (!an->case_map) an->ntables = an->ntable_targets = 0;
        for (int i = 0; i < indirects.n; i++) {
            int n = resolve_jump_table(an, indirects.v[i], &targets);
            if (n > 0) { an->ntables++; an->ntable_targets += n; }
        }
        for (int i = 0; i < cross.n; i++) u32_push(&targets, cross.v[i]);
        cross.n = 0;
        if (an->scan_calls) sweep_gaps(an, seen, &targets);
        int added = 0;
        for (int i = 0; i < targets.n; i++) {
            uint32_t t = targets.v[i];
            if (!a_in_range(an, t)) continue;
            uint32_t ti = word_index(an, t);
            if (entry_map[ti]) continue;
            entry_map[ti] = 1;
            added++;
        }
        free(targets.v);
        if (!added) break;
        indirects.n = 0;   /* kept until here so a final round still reports them */

        /* Re-walk everything from a clean slate rather than walking only the
         * new entries.
         *
         * A promoted address was, by definition, already claimed in round one
         * -- that is why it showed up as a cross-function target. Walking it
         * again in isolation does nothing: the very first instruction is
         * already marked as code, so the trace stops immediately and produces
         * an empty function. The owning function also still contains the code,
         * so simply stealing it would leave that function with a hole.
         *
         * Discarding the round-one result and re-walking with the complete
         * entry set produces correct, non-overlapping boundaries in one pass,
         * because every split point is now known before any walking starts --
         * which is the same reason the entry map is built up front to begin
         * with. One extra walk is cheap; reconciling overlapping ownership
         * afterwards is not. */
        memset(seen, 0, nwords);
        for (uint32_t i = 0; i < nwords; i++) owner[i] = A_NO_OWNER;
        an->insns = an->vfpu = an->invalid = 0;
        nfuncs = 0;

        for (uint32_t i = 0; i < nwords; i++)
            if (entry_map[i]) u32_push(&queue, an->base + i * 4);
        if (!queue.n) break;
    }

    /* The queue grows as calls are found; this terminates because every
     * function entry is marked and never walked twice, and there are finitely
     * many words. */
    while (queue.n) {
        uint32_t entry = queue.v[--queue.n];
        if (!a_in_range(an, entry)) continue;

        uint32_t idx = word_index(an, entry);
        if (seen[idx] & SEEN_ENTRY) continue;
        seen[idx] |= SEEN_ENTRY;

        a_func f;
        ctx.cur_owner = entry;
        if (trace_function(&ctx, entry, &f) != 0) continue;

        if (nfuncs == cap) {
            int ncap = cap ? cap * 2 : 512;
            a_func *nf = (a_func *)realloc(funcs, (size_t)ncap * sizeof *nf);
            if (!nf) break;
            funcs = nf;
            cap = ncap;
        }
        funcs[nfuncs++] = f;
    }
    }   /* rounds */

    for (uint32_t i = 0; i < nwords; i++)
        if (seen[i] & SEEN_CODE) an->bytes_reached += 4;

    qsort(funcs, (size_t)nfuncs, sizeof *funcs, cmp_func);

    an->funcs = funcs;
    an->nfuncs = nfuncs;
    an->imports = imports.v;
    an->nimports = imports.n;
    an->indirects = indirects.v;
    an->nindirects = indirects.n;

    an->owner = owner;
    an->nwords = nwords;

    free(cross.v);
    free(queue.v);
    free(seen);
    free(entry_map);
    return 0;
}

int a_scan_data_pointers(const a_analysis *an,
                         const uint8_t *region, uint32_t region_len,
                         uint32_t *out, int max) {
    int found = 0;

    for (uint32_t off = 0; off + 4 <= region_len; off += 4) {
        uint32_t v = (uint32_t)region[off] | ((uint32_t)region[off + 1] << 8) |
                     ((uint32_t)region[off + 2] << 16) | ((uint32_t)region[off + 3] << 24);

        if (!a_in_range(an, v)) continue;        /* inside the code extent */

        /* Point at something that decodes. A pointer into the middle of a data
         * table would usually fail this; a genuine function entry never does. */
        a_insn in;
        if (!a_decode(fetch(an, v), v, &in) || in.op == A_INVALID) continue;

        if (found < max) out[found] = v;
        found++;
    }
    return found;
}

void a_analysis_free(a_analysis *an) {
    free(an->funcs);
    free(an->imports);
    free(an->indirects);
    free(an->owner);
    free(an->case_map);
    an->case_map = NULL;
    an->funcs = NULL;
    an->imports = NULL;
    an->indirects = NULL;
    an->owner = NULL;
    an->nfuncs = an->nimports = an->nindirects = 0;
}
