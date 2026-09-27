/*
 * Shared header for decompiled game modules (port/game/<module>.c, one per .\PRODUCTION\*.OBJ).
 *
 * A ported function keeps its CodeView name and signature, and registers itself with
 *
 *     float xmath_LimitRange(float v, float lo, float hi) { ... }
 *     HY_PORT(xmath_LimitRange)
 *
 * At startup the host patches the original entry point to jump to the C version, so every
 * remaining original caller runs the port. HYDRO_UNHOOK=name1,name2 (or "all") keeps the
 * original instead: that's the A/B switch for verifying a port against the binary.
 *
 * Until a module is fully ported, globals and unported callees are reached through the
 * generated addresses in hydro_syms.h:
 *     #define Gameloop_bKill HY_GLOBAL(int, Gameloop_bKill)
 *     HY_CALL(void (__cdecl *)(void), gameloop_Start)();
 */
#pragma once
#include <stdint.h>

#include "hydro_syms.h"

typedef struct HyPort {
    uint32_t addr;
    const void *fn;
    const char *name;
} HyPort;

#pragma section(".hyport$a", read)
#pragma section(".hyport$m", read)
#pragma section(".hyport$z", read)

/* Only pointer-sized entries go in the section: the linker may pad between contributions,
 * so the collector walks it one pointer at a time and skips zeros. */
#define HY_PORT(fn)                                                                                    \
    static const HyPort hyport_##fn = {HY_FN_##fn, (const void *)fn, #fn};                             \
    __declspec(allocate(".hyport$m")) const HyPort *const hyport_ptr_##fn = &hyport_##fn;

/* The same for an unnamed static (no CodeView public): its address and our name for it. */
#define HY_PORT_AT(fn, address)                                                                        \
    static const HyPort hyport_##fn = {address, (const void *)fn, #fn};                                \
    __declspec(allocate(".hyport$m")) const HyPort *const hyport_ptr_##fn = &hyport_##fn;

/* Installs every HY_PORT (host side). */
void game_ports_install(void);
