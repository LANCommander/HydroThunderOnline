/*
 * ETS ships its own implementation of a Win32 subset (every `_Name@N` symbol in the image).
 * The signatures are the real Win32 ones, so most are forwarded straight to the real DLLs.
 * The overrides here virtualize what differs on a PC:
 *   - drive letters: the cabinet's C:/D:/E: FAT volumes are folders under the data root
 *   - the program's own identity (command line, module file name)
 *   - PAGE_NOCACHE allocations (fine on a Voodoo host, slow on a PC)
 */
#include "hook.h"
#include "host.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- paths */

static char g_vcwd[MAX_PATH] = "C:\\"; /* virtual current directory as the game sees it */

static int is_drive_letter(char c)
{
    c = (char)(c & ~0x20);
    return c >= 'C' && c <= 'E';
}

/* Map a game path to a host path. Relative paths stay relative: the real cwd tracks g_vcwd. */
static const char *map_path(const char *in, char *out, size_t size)
{
    if (!in)
        return NULL;
    if (in[0] && in[1] == ':' && is_drive_letter(in[0])) {
        const char *rest = in + 2;
        while (*rest == '\\' || *rest == '/')
            rest++;
        snprintf(out, size, "%s\\%c\\%s", g_cfg.data_root, in[0] & ~0x20, rest);
    } else if (in[0] == '\\' || in[0] == '/') {
        snprintf(out, size, "%s\\%c\\%s", g_cfg.data_root, g_vcwd[0], in + 1);
    } else {
        snprintf(out, size, "%s", in);
    }
    return out;
}

static HANDLE WINAPI my_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disp, DWORD flags, HANDLE tmpl)
{
    char p[MAX_PATH * 2];
    HANDLE h = CreateFileA(map_path(name, p, sizeof p), access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    hy_log("CreateFileA(\"%s\" -> \"%s\", acc=%lx, disp=%lu) = %p%s", name, p, access, disp, h,
           h == INVALID_HANDLE_VALUE ? " FAILED" : "");
    SetLastError(err);
    return h;
}

static BOOL WINAPI my_DeleteFileA(LPCSTR name)
{
    char p[MAX_PATH * 2];
    hy_log("DeleteFileA(\"%s\")", name);
    return DeleteFileA(map_path(name, p, sizeof p));
}

static UINT WINAPI my_GetDriveTypeA(LPCSTR root)
{
    char c = root ? root[0] : g_vcwd[0];
    return is_drive_letter(c) ? DRIVE_FIXED : DRIVE_NO_ROOT_DIR;
}

static BOOL WINAPI my_SetCurrentDirectoryA(LPCSTR dir)
{
    char p[MAX_PATH * 2];
    BOOL ok = SetCurrentDirectoryA(map_path(dir, p, sizeof p));
    hy_log("SetCurrentDirectoryA(\"%s\" -> \"%s\") = %d", dir, p, ok);
    if (ok) {
        if (dir[0] && dir[1] == ':')
            snprintf(g_vcwd, sizeof g_vcwd, "%s", dir);
        else if (dir[0] == '\\')
            snprintf(g_vcwd, sizeof g_vcwd, "%c:%s", g_vcwd[0], dir);
        else
            hy_log("  (relative chdir: virtual cwd stays %s)", g_vcwd);
        g_vcwd[0] = (char)(g_vcwd[0] & ~0x20);
    }
    return ok;
}

static DWORD WINAPI my_GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    DWORD n = (DWORD)strlen(g_vcwd);
    if (!buf || size <= n)
        return n + 1;
    memcpy(buf, g_vcwd, n + 1);
    return n;
}

/* ---------------------------------------------------------------- identity */

static const char g_game_exe[] = "C:\\HYDRO.EXE";
static char g_cmdline[1024];

static LPSTR WINAPI my_GetCommandLineA(void)
{
    return g_cmdline;
}

static DWORD WINAPI my_GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD size)
{
    if (mod && mod != GetModuleHandleA(NULL))
        return GetModuleFileNameA(mod, buf, size);
    DWORD n = (DWORD)strlen(g_game_exe);
    if (size == 0)
        return 0;
    lstrcpynA(buf, g_game_exe, (int)size);
    return n < size ? n : size;
}

static LPVOID WINAPI my_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    prot &= ~(PAGE_NOCACHE | PAGE_WRITECOMBINE);
    return VirtualAlloc(addr, size, type, prot);
}

static HMODULE WINAPI my_LoadLibraryA(LPCSTR name)
{
    hy_log("LoadLibraryA(\"%s\")", name);
    return LoadLibraryA(name);
}

static void WINAPI my_OutputDebugStringA(LPCSTR s)
{
    hy_log("ODS: %s", s);
}

static int WINAPI my_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT type)
{
    hy_log("MessageBoxA(\"%s\", \"%s\")", cap ? cap : "", text ? text : "");
    return MessageBoxA(w, text, cap, type);
}

/* The operator menu's SET TIME AND DATE would otherwise set the host's clock. */
static BOOL WINAPI my_SetSystemTime(const SYSTEMTIME *st)
{
    hy_log("SetSystemTime(%04u-%02u-%02u %02u:%02u:%02u) ignored", st->wYear, st->wMonth, st->wDay, st->wHour,
           st->wMinute, st->wSecond);
    return TRUE;
}

static void WINAPI my_ExitProcess(UINT code)
{
    hy_log("ExitProcess(%u)", code);
    ExitProcess(code);
}

/* ---------------------------------------------------------------- install */

static const struct {
    const char *name;
    const void *fn;
} g_overrides[] = {
    {"CreateFileA", my_CreateFileA},
    {"DeleteFileA", my_DeleteFileA},
    {"GetDriveTypeA", my_GetDriveTypeA},
    {"SetCurrentDirectoryA", my_SetCurrentDirectoryA},
    {"GetCurrentDirectoryA", my_GetCurrentDirectoryA},
    {"GetCommandLineA", my_GetCommandLineA},
    {"GetModuleFileNameA", my_GetModuleFileNameA},
    {"VirtualAlloc", my_VirtualAlloc},
    {"LoadLibraryA", my_LoadLibraryA},
    {"OutputDebugStringA", my_OutputDebugStringA},
    {"MessageBoxA", my_MessageBoxA},
    {"SetSystemTime", my_SetSystemTime},
    {"ExitProcess", my_ExitProcess},
};

void hle_win32_install(void)
{
    snprintf(g_cmdline, sizeof g_cmdline, "%s", g_game_exe);

    HMODULE dlls[] = {GetModuleHandleA("kernel32.dll"), LoadLibraryA("ws2_32.dll"),
                      LoadLibraryA("user32.dll")};
    unsigned forwarded = 0, overridden = 0, missing = 0;

    for (unsigned i = 0; i < hy_symtab_count; i++) {
        const HySym *s = &hy_symtab[i];
        const char *raw = s->name;
        const char *at = strrchr(raw, '@');
        /* `_Name@N` only; skip `__imp__X@N` pointer slots and C++ names */
        if (s->section != HY_SEC_CODE || raw[0] != '_' || raw[1] == '_' || !at || at == raw + 1)
            continue;
        char name[128];
        size_t n = (size_t)(at - (raw + 1));
        if (n >= sizeof name)
            continue;
        memcpy(name, raw + 1, n);
        name[n] = 0;

        const void *target = NULL;
        for (unsigned k = 0; k < sizeof g_overrides / sizeof g_overrides[0]; k++)
            if (!strcmp(g_overrides[k].name, name)) {
                target = g_overrides[k].fn;
                overridden++;
            }
        for (unsigned k = 0; !target && k < sizeof dlls / sizeof dlls[0]; k++)
            if (dlls[k] && (target = (const void *)GetProcAddress(dlls[k], name)) != NULL)
                forwarded++;
        if (!target) {
            hy_log("win32: no host implementation for %s", raw);
            missing++;
            continue;
        }
        hook_jmp(s->addr, target);
    }
    hy_log("win32: %u forwarded, %u overridden, %u unresolved", forwarded, overridden, missing);

    char p[MAX_PATH * 2];
    if (!SetCurrentDirectoryA(map_path("C:\\", p, sizeof p)))
        hy_fatal("data root %s has no C\\ folder (extract the disk image first)", g_cfg.data_root);
}
