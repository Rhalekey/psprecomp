/* psprecomp -- loading and starting a recompiled module. See module.h. */

#include "psprecomp/module.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"
#include "psprecomp/ctors.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int psp_module_load(const char *path, uint32_t span, psp_module *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "psprecomp: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *b = (n > 52) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b);
        fprintf(stderr, "psprecomp: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    /* A decrypted PRX is an ELF with one PT_LOAD. Its p_paddr is not a
     * physical address: it holds the file offset of the module info. */
    const uint32_t phoff = rd32(b + 28);
    if (memcmp(b, "\x7F" "ELF", 4) != 0 || (size_t)phoff + 32 > (size_t)n) {
        free(b);
        fprintf(stderr, "psprecomp: %s is not a decrypted ELF module\n", path);
        return -1;
    }
    const uint8_t *ph = b + phoff;
    const uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), paddr = rd32(ph + 12);
    const uint32_t filesz = rd32(ph + 16), memsz = rd32(ph + 20);
    /* A module linked inside RAM is simply in RAM. Giving it a mapping of its
     * own would split guest memory at the end of `span`. */
    const int in_ram = vaddr >= PSP_RAM_BASE && vaddr < PSP_RAM_BASE + PSP_RAM_SIZE;
    if ((size_t)off + filesz > (size_t)n || filesz > span ||
        (!in_ram && psp_mem_map_module(vaddr, span) != 0) ||
        psp_mem_write_block(vaddr, b + off, filesz) != 0) {
        free(b);
        fprintf(stderr, "psprecomp: cannot place %s at 0x%08X\n", path, vaddr);
        return -1;
    }

    m->base = vaddr;
    m->image_end = vaddr + filesz;
    m->mem_end = vaddr + (memsz > filesz ? memsz : filesz);
    m->entry = rd32(b + 24);
    /* SceModuleInfo: u16 attr, u16 version, char name[28], then gp. */
    const uint32_t info = (paddr & 0x7FFFFFFFu) - off + vaddr;
    m->gp = psp_read32(info + 32);
    free(b);

    /* The constructor table and the constructors sit together at the end of
     * the image; a vtable elsewhere has the same shape, so only that region
     * is searched, and the entries must point into it too.
     * ponytail: right for WTF's modules (8 constructors just below .bss);
     * widen the window if another title keeps them elsewhere. */
    const uint32_t lo = m->image_end > vaddr + 0x10000u ? m->image_end - 0x10000u : vaddr;
    uint32_t t;
    int count;
    if (psp_ctors_find(lo, m->image_end, lo, m->image_end, &t, &count) == 0) {
        m->ctors = t;
        m->nctors = count;
    }
    return 0;
}

/* The loader's leftovers. $k0 is the thread pointer: libc reaches its
 * per-thread reent, and through it the heap, at *(k0 + 4). The stack is only
 * module_start's, which returns once it has started the game's threads.
 * ponytail: fixed addresses, proven on WTF; take them from sysmem if a title
 * minds. */
#define START_STACK 0x08F00000u
#define TCB_ADDR    0x08800000u
#define REENT_ADDR  0x08800100u

uint32_t psp_module_start(const psp_module *m) {
    psp_cpu.r[PSP_REG_GP] = m->gp;
    psp_cpu.r[PSP_REG_SP] = m->stack_top ? m->stack_top : START_STACK;
    psp_cpu.r[PSP_REG_A0] = 0;
    psp_cpu.r[PSP_REG_A1] = 0;
    psp_cpu.r[26] = TCB_ADDR;                   /* $k0 */
    psp_write32(TCB_ADDR + 4, REENT_ADDR);
    if (m->ctors) psp_ctors_run(m->ctors);
    psp_call_from_host(m->entry);
    return psp_cpu.r[PSP_REG_V0];
}
