#include "hook.h"
#include "host.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_hooked[(HY_CODE_END - HY_IMAGE_BASE + 7) / 8];

static uint8_t *g_arena, *g_arena_end;

uint8_t *thunk_alloc(unsigned size)
{
    size = (size + 15) & ~15u;
    if (!g_arena || g_arena + size > g_arena_end) {
        g_arena = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!g_arena)
            hy_fatal("thunk arena allocation failed");
        g_arena_end = g_arena + (1 << 20);
    }
    uint8_t *p = g_arena;
    g_arena += size;
    return p;
}

int hook_is_hooked(uint32_t addr)
{
    uint32_t i = addr - HY_IMAGE_BASE;
    return addr >= HY_IMAGE_BASE && addr < HY_CODE_END && (g_hooked[i / 8] >> (i % 8) & 1);
}

void hook_jmp(uint32_t addr, const void *target)
{
    if (addr < HY_IMAGE_BASE || addr >= HY_CODE_END)
        hy_fatal("hook_jmp: %08x is outside CODE", addr);
    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)target - (addr + 5));
    uint32_t i = addr - HY_IMAGE_BASE;
    g_hooked[i / 8] |= 1 << (i % 8);
}

void hook_patch(uint32_t addr, const void *expect, const void *bytes, unsigned n)
{
    if (addr < HY_IMAGE_BASE || memcmp((const void *)(uintptr_t)addr, expect, n))
        hy_fatal("hook_patch: unexpected bytes at %08x", addr);
    memcpy((void *)(uintptr_t)addr, bytes, n);
}

void hook_patch8(uint32_t addr, uint8_t expect, uint8_t value)
{
    hook_patch(addr, &expect, &value, 1);
}

void hook_patch32(uint32_t addr, uint32_t expect, uint32_t value)
{
    hook_patch(addr, &expect, &value, 4);
}

void hook_name(const char *name, const void *target)
{
    hook_jmp(sym_addr(name), target);
}

int hook_name_opt(const char *name, const void *target)
{
    const HySym *s = sym_find(name);
    if (!s)
        return 0;
    hook_jmp(s->addr, target);
    return 1;
}

/* ---------------------------------------------------------------- traps */

static void __cdecl trap_hit(const HySym *s, uint32_t ret)
{
    char buf[160];
    hy_fatal("unimplemented hardware/ETS function %s called from %s", s->name,
             sym_describe(ret, buf, sizeof buf));
}

/* Trampoline body: push <HySym*>; call trap_common. The caller's return address is above. */
static __declspec(naked) void trap_common(void)
{
    __asm {
        pop eax            ; our own return into the trampoline (unused)
        pop eax            ; HySym*
        push dword ptr [esp] ; caller's return address
        push eax
        call trap_hit
        int 3
    }
}

void hook_trap(const HySym *s)
{
    uint8_t *t = thunk_alloc(16);
    t[0] = 0x68; /* push imm32 */
    *(uint32_t *)(t + 1) = (uint32_t)(uintptr_t)s;
    t[5] = 0xE8; /* call trap_common */
    *(int32_t *)(t + 6) = (int32_t)((uintptr_t)trap_common - ((uintptr_t)t + 10));
    hook_jmp(s->addr, t);
}

void hook_trap_unhandled(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < hy_symtab_count; i++) {
        const HySym *s = &hy_symtab[i];
        if ((s->flags & HY_SYM_PRIV) && !hook_is_hooked(s->addr)) {
            hook_trap(s);
            n++;
        }
    }
    hy_log("hooks: %u privileged functions trap on entry", n);
}

/* ---------------------------------------------------------------- stubs */

typedef struct Stub {
    const char *name;
    uint32_t value;
    volatile LONG seen;
} Stub;

static uint32_t __cdecl stub_hit(Stub *st)
{
    if (!InterlockedExchange(&st->seen, 1))
        hy_log("stub: %s -> %u (logged once)", st->name, st->value);
    return st->value;
}

void hook_stub(const char *name, uint32_t value)
{
    Stub *st = calloc(1, sizeof *st);
    st->name = name;
    st->value = value;
    /* push st; call stub_hit; add esp,4; ret */
    uint8_t *t = thunk_alloc(16);
    t[0] = 0x68;
    *(uint32_t *)(t + 1) = (uint32_t)(uintptr_t)st;
    t[5] = 0xE8;
    *(int32_t *)(t + 6) = (int32_t)((uintptr_t)stub_hit - ((uintptr_t)t + 10));
    t[10] = 0x83, t[11] = 0xC4, t[12] = 0x04; /* add esp, 4 */
    t[13] = 0xC3;                             /* ret */
    hook_name(name, t);
}
