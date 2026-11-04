/* psprecomp -- guest memory access for code that does it per pixel.
 *
 * psp_read32 and friends are out-of-line calls that also feed the bring-up
 * instruments. A rasterizer makes several per fragment and spends its time there.
 * These resolve RAM and VRAM inline and leave everything else, including unmapped
 * addresses and their reporting, to the ordinary functions. */
#ifndef PSPRECOMP_MEM_FAST_H
#define PSPRECOMP_MEM_FAST_H

#include "mem.h"

#include <string.h>

static inline uint8_t *psp_fast_ptr(uint32_t addr, uint32_t size) {
    const uint32_t a = addr & PSP_ADDR_MASK;
    uint32_t off = a - PSP_VRAM_BASE;
    if (off <= PSP_VRAM_SIZE - size) return psp_mem.vram + off;
    off = a - PSP_RAM_BASE;
    if (off <= PSP_RAM_SIZE - size) return psp_mem.ram + off;
    return NULL;
}

static inline uint8_t psp_fast_read8(uint32_t addr) {
    const uint8_t *p = psp_fast_ptr(addr, 1);
    return p ? *p : psp_read8(addr);
}

static inline uint16_t psp_fast_read16(uint32_t addr) {
    const uint8_t *p = psp_fast_ptr(addr, 2);
    if (!p) return psp_read16(addr);
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static inline uint32_t psp_fast_read32(uint32_t addr) {
    const uint8_t *p = psp_fast_ptr(addr, 4);
    if (!p) return psp_read32(addr);
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static inline float psp_fast_read_f32(uint32_t addr) {
    const uint8_t *p = psp_fast_ptr(addr, 4);
    if (!p) return psp_read_f32(addr);
    float v;
    memcpy(&v, p, sizeof v);
    return v;
}

static inline void psp_fast_write16(uint32_t addr, uint16_t v) {
    uint8_t *p = psp_fast_ptr(addr, 2);
    if (p) memcpy(p, &v, sizeof v);
    else psp_write16(addr, v);
}

static inline void psp_fast_write32(uint32_t addr, uint32_t v) {
    uint8_t *p = psp_fast_ptr(addr, 4);
    if (p) memcpy(p, &v, sizeof v);
    else psp_write32(addr, v);
}

#endif
