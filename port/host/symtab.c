#include "symtab.h"
#include "host.h"

#include <stdio.h>
#include <string.h>

static int name_matches(const char *raw, const char *want)
{
    if (strcmp(raw, want) == 0)
        return 1;
    /* C name: strip one leading '_' and any "@N" stdcall suffix */
    if (raw[0] != '_')
        return 0;
    size_t n = strlen(want);
    return strncmp(raw + 1, want, n) == 0 && (raw[1 + n] == 0 || raw[1 + n] == '@');
}

const HySym *sym_find(const char *name)
{
    for (unsigned i = 0; i < hy_symtab_count; i++)
        if (name_matches(hy_symtab[i].name, name))
            return &hy_symtab[i];
    return NULL;
}

uint32_t sym_addr(const char *name)
{
    const HySym *s = sym_find(name);
    if (!s)
        hy_fatal("symbol not found: %s", name);
    return s->addr;
}

const HySym *sym_lookup(uint32_t addr, uint32_t *offset)
{
    if (addr < HY_IMAGE_BASE || addr >= HY_CODE_END)
        return NULL;
    unsigned lo = 0, hi = hy_symtab_count;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (hy_symtab[mid].addr <= addr)
            lo = mid;
        else
            hi = mid;
    }
    if (hy_symtab[lo].addr > addr || hy_symtab[lo].section != HY_SEC_CODE)
        return NULL;
    if (offset)
        *offset = addr - hy_symtab[lo].addr;
    return &hy_symtab[lo];
}

const char *sym_describe(uint32_t addr, char *buf, unsigned size)
{
    uint32_t off;
    const HySym *s = sym_lookup(addr, &off);
    if (s && off)
        snprintf(buf, size, "%s+0x%x (%08x)", s->name, off, addr);
    else if (s)
        snprintf(buf, size, "%s (%08x)", s->name, addr);
    else
        snprintf(buf, size, "%08x", addr);
    return buf;
}
