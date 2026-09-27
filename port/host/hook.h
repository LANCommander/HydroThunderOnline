#pragma once
#include <stdint.h>

#include "symtab.h"

/* Overwrite the entry of the original function at `addr` with `jmp target`. */
void hook_jmp(uint32_t addr, const void *target);
/* Overwrite bytes in the image (code or data) after checking they are what we expect. Fatal if not. */
void hook_patch(uint32_t addr, const void *expect, const void *bytes, unsigned n);
void hook_patch8(uint32_t addr, uint8_t expect, uint8_t value);
void hook_patch32(uint32_t addr, uint32_t expect, uint32_t value);
/* Same, by CodeView name ("timer_driver_Init" or "_timer_driver_Init"). Fatal if unknown. */
void hook_name(const char *name, const void *target);
/* Like hook_name, but silently skips symbols the image doesn't have. Returns 1 if hooked. */
int hook_name_opt(const char *name, const void *target);
int hook_is_hooked(uint32_t addr);

/* Make the original function fatal-error on entry, naming itself and its caller. */
void hook_trap(const HySym *s);
/* Replace the original with a cdecl "return value" stub that logs its first call. */
void hook_stub(const char *name, uint32_t value);

/* Executable scratch memory for generated thunks. */
uint8_t *thunk_alloc(unsigned size);

/* Trap every HY_SYM_PRIV function that nothing has hooked yet. Call after all installs. */
void hook_trap_unhandled(void);
