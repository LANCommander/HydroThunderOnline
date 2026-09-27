/*
 * Vectored exception handler:
 *  - emulates CLI/STI and IN/OUT left inline in game code (coins, banker, the odd driver),
 *    which fault in user mode with EXCEPTION_PRIV_INSTRUCTION;
 *  - reports crashes symbolized with the CodeView names, plus a heuristic call stack.
 */
#include "hle_hw.h"
#include "host.h"
#include "symtab.h"
#include "trace.h"

#include <windows.h>
#include <stdio.h>

static int emulate_priv(CONTEXT *c)
{
    uint8_t *ip = (uint8_t *)(uintptr_t)c->Eip;
    int opsize = 4, len = 0;
    if (ip[0] == 0x66)
        opsize = 2, len = 1;
    uint8_t op = ip[len];
    uint32_t port;

    switch (op) {
    case 0xFA: hw_set_interrupt_flag(0); c->Eip += 1; return 1; /* cli */
    case 0xFB: hw_set_interrupt_flag(1); c->Eip += 1; return 1; /* sti */
    case 0xE4: case 0xE5: case 0xEC: case 0xED: {                /* in */
        int imm = op == 0xE4 || op == 0xE5;
        int size = (op & 1) ? opsize : 1;
        port = imm ? ip[len + 1] : (c->Edx & 0xffff);
        uint32_t v = hw_port_in(port, size);
        uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
        c->Eax = (c->Eax & ~mask) | (v & mask);
        c->Eip += len + 1 + imm;
        return 1;
    }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: {                /* out */
        int imm = op == 0xE6 || op == 0xE7;
        int size = (op & 1) ? opsize : 1;
        port = imm ? ip[len + 1] : (c->Edx & 0xffff);
        uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
        hw_port_out(port, c->Eax & mask, size);
        c->Eip += len + 1 + imm;
        return 1;
    }
    }
    return 0;
}

static void report(EXCEPTION_POINTERS *ep, const char *what)
{
    CONTEXT *c = ep->ContextRecord;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    char buf[200];
    hy_log("=== %s: code %08lx at %s", what, r->ExceptionCode, sym_describe(c->Eip, buf, sizeof buf));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        hy_log("    %s address %08lx", r->ExceptionInformation[0] ? "writing" : "reading",
               (unsigned long)r->ExceptionInformation[1]);
    hy_log("    eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Eax, c->Ebx,
           c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    /* Heuristic stack: every dword on the stack that points into the image's code. */
    uint32_t *sp = (uint32_t *)(uintptr_t)c->Esp;
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 24; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (((uintptr_t)(sp + i) & 0xfff) == 0 &&
            (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT))
            break;
        uint32_t v = sp[i];
        if (v >= HY_IMAGE_BASE && v < HY_CODE_END) {
            hy_log("    [esp+%03x] %s", i * 4, sym_describe(v, buf, sizeof buf));
            shown++;
        }
    }
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (trace_handle(ep))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (code == EXCEPTION_PRIV_INSTRUCTION && emulate_priv(ep->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_BREAKPOINT: {
        uint32_t eip = ep->ContextRecord->Eip;
        if (eip >= HY_IMAGE_BASE && eip < HY_CODE_END)
            report(ep, "exception in game image (first chance)");
        break;
    }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep)
{
    report(ep, "UNHANDLED EXCEPTION");
    hy_fatal("crashed; see hydro.log");
}

void veh_install(void)
{
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(unhandled);
}
