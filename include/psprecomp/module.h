/* psprecomp -- loading and starting a recompiled module.
 *
 * A recompiled program still needs its own image in guest memory (its data,
 * its constants, the tables its code reads), and the few facts the loader
 * would have supplied: where module_start is, the $gp its globals are
 * addressed from, and the C++ constructors crt0 would have run. */
#ifndef PSPRECOMP_MODULE_H
#define PSPRECOMP_MODULE_H

#include <stdint.h>

typedef struct {
    uint32_t base;          /* link address of the image */
    uint32_t image_end;     /* end of the file-backed part; .bss follows */
    uint32_t mem_end;       /* end of .bss: nothing else may be placed below this */
    uint32_t stack_top;     /* module_start's stack; 0 uses a fixed default */
    uint32_t entry;         /* module_start */
    uint32_t gp;            /* from the module info */
    uint32_t ctors;         /* constructor table, 0 if none was found */
    int      nctors;
} psp_module;

/* Map `span` bytes at the module's link address, copy its PT_LOAD segment in,
 * and read entry, $gp and the constructor table. `span` covers .text, .data,
 * .bss and whatever a module linked at 0 expects above them. Needs
 * psp_mem_init() first. Returns 0, or -1 with a message on stderr. */
int psp_module_load(const char *path, uint32_t span, psp_module *m);

/* Set up the registers the loader would have, run the constructors and call
 * module_start on this host thread. Needs the HLE initialised and the
 * recompiled functions registered. Returns module_start's result. */
uint32_t psp_module_start(const psp_module *m);

#endif
