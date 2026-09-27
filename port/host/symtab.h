#pragma once
#include <stdint.h>

enum { HY_SEC_CODE, HY_SEC_DATA, HY_SEC_BSS };

enum {
    HY_SYM_PRIV = 1,   /* executes privileged instructions: trap on entry unless hooked */
    HY_SYM_CLISTI = 2, /* only CLI/STI: runs natively, the VEH emulates those */
};

typedef struct HySym {
    uint32_t addr;
    uint8_t section;
    uint8_t flags;
    const char *name; /* raw CodeView name, e.g. "_gameloop_Start", "_Sleep@4" */
} HySym;

extern const HySym hy_symtab[];
extern const unsigned hy_symtab_count;

/* Exact lookup by raw name ("_timer_driver_Init") or by C name ("timer_driver_Init"). */
const HySym *sym_find(const char *name);
uint32_t sym_addr(const char *name); /* fatal if missing */
/* Nearest code symbol at or below addr; returns NULL outside the image. */
const HySym *sym_lookup(uint32_t addr, uint32_t *offset);
/* "name+0x12" or "0x12345678" into buf. */
const char *sym_describe(uint32_t addr, char *buf, unsigned size);
