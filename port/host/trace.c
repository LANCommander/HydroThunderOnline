/*
 * Call tracer for original functions:  set HYDRO_TRACE=gameinit_ModuleInit,audio_Init
 *
 * Each traced entry gets an INT3. On hit the VEH logs the caller and first stack arguments,
 * plants a one-shot INT3 on the return address to log EAX, restores the original byte and
 * single-steps it before re-arming. Debug aid only: slow, but needs no instruction decoder.
 */
#include "host.h"
#include "symtab.h"
#include "trace.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BP 256

typedef struct Bp {
    uint32_t addr;
    uint8_t orig;
    uint8_t is_return; /* one-shot return breakpoint */
    const char *name;  /* traced function (for returns: the callee) */
    int hits;
} Bp;

static Bp g_bp[MAX_BP];
static int g_nbp;
static CRITICAL_SECTION g_lock;
static DWORD g_rearm_tls; /* per thread: index+1 of the entry bp to re-arm after single-step */
static int g_limit = 50;  /* log at most this many calls per function */

static Bp *bp_at(uint32_t a)
{
    for (int i = 0; i < g_nbp; i++)
        if (g_bp[i].addr == a)
            return &g_bp[i];
    return NULL;
}

static Bp *bp_add(uint32_t a, const char *name, int is_return)
{
    Bp *b = bp_at(a);
    if (b)
        return b->is_return == is_return ? b : NULL;
    b = bp_at(0); /* a freed return slot */
    if (!b) {
        if (g_nbp == MAX_BP)
            return NULL;
        b = &g_bp[g_nbp++];
    }
    b->addr = a;
    b->orig = *(uint8_t *)(uintptr_t)a;
    b->is_return = (uint8_t)is_return;
    b->name = name;
    b->hits = 0;
    *(uint8_t *)(uintptr_t)a = 0xCC;
    return b;
}

int trace_handle(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!g_nbp)
        return 0;

    if (code == EXCEPTION_SINGLE_STEP) {
        int idx = (int)(intptr_t)TlsGetValue(g_rearm_tls);
        if (!idx)
            return 0;
        TlsSetValue(g_rearm_tls, 0);
        EnterCriticalSection(&g_lock);
        *(uint8_t *)(uintptr_t)g_bp[idx - 1].addr = 0xCC;
        LeaveCriticalSection(&g_lock);
        return 1;
    }
    if (code != EXCEPTION_BREAKPOINT)
        return 0;

    EnterCriticalSection(&g_lock);
    Bp *b = bp_at(c->Eip);
    if (!b) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    uint32_t *sp = (uint32_t *)(uintptr_t)c->Esp;
    char who[160];
    if (b->is_return) {
        hy_log("trace: %s returned %08lx (%ld)", b->name, c->Eax, (long)c->Eax);
        *(uint8_t *)(uintptr_t)b->addr = b->orig;
        b->addr = 0; /* free the slot */
        b->is_return = 0;
    } else {
        if (b->hits++ < g_limit) {
            hy_log("trace: %s(%08x, %08x, %08x, %08x) from %s", b->name, sp[1], sp[2], sp[3], sp[4],
                   sym_describe(sp[0], who, sizeof who));
            if (!bp_at(sp[0]))
                bp_add(sp[0], b->name, 1);
        }
        /* execute the original first byte, then re-arm */
        *(uint8_t *)(uintptr_t)b->addr = b->orig;
        TlsSetValue(g_rearm_tls, (void *)(intptr_t)(b - g_bp + 1));
        c->EFlags |= 0x100; /* TF */
    }
    LeaveCriticalSection(&g_lock);
    return 1;
}

void trace_install(void)
{
    const char *env = getenv("HYDRO_TRACE");
    if (!env || !*env)
        return;
    InitializeCriticalSection(&g_lock);
    g_rearm_tls = TlsAlloc();
    const char *lim = getenv("HYDRO_TRACE_LIMIT");
    if (lim)
        g_limit = atoi(lim);

    char *list = _strdup(env), *ctx = NULL;
    for (char *tok = strtok_s(list, ",; ", &ctx); tok; tok = strtok_s(NULL, ",; ", &ctx)) {
        const HySym *s = sym_find(tok);
        if (!s) {
            hy_log("trace: unknown symbol %s", tok);
            continue;
        }
        if (*(uint8_t *)(uintptr_t)s->addr == 0xE9)
            hy_log("trace: %s is hooked by the host; tracing its jmp", tok);
        bp_add(s->addr, s->name, 0);
        hy_log("trace: armed %s at %08x", s->name, s->addr);
    }
}
