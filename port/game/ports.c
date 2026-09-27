/* Collects the HY_PORT records every game module drops into the .hyport$m section. */
#include "port.h"

#include "../host/hook.h"
#include "../host/host.h"

#include <stdlib.h>
#include <string.h>

__declspec(allocate(".hyport$a")) static const HyPort *const g_first = NULL;
__declspec(allocate(".hyport$z")) static const HyPort *const g_last = NULL;

static int listed(const char *list, const char *name)
{
    if (!list)
        return 0;
    if (!strcmp(list, "all"))
        return 1;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
            return 1;
    return 0;
}

void game_ports_install(void)
{
    const char *unhook = getenv("HYDRO_UNHOOK");
    unsigned ported = 0, kept = 0;
    /* The linker may pad between contributions: skip zeroed slots. */
    for (const HyPort *const *pp = &g_first + 1; pp < &g_last; pp++) {
        const HyPort *p = *pp;
        if (!p)
            continue;
        if (listed(unhook, p->name)) {
            kept++;
            continue;
        }
        hook_jmp(p->addr, p->fn);
        ported++;
    }
    hy_log("game: %u ported functions active%s", ported,
           kept ? " (some kept original via HYDRO_UNHOOK)" : "");
}
