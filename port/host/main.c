/*
 * Hydro Thunder host: runs the original arcade program (a Phar Lap ETS image) as a Win32 process.
 *
 *   hydro.exe [--chd hydro.chd] [--data data] [--exe data\C\HYDRO.EXE] [--save dir] [--glide glide2x.dll]
 *             [--skip-menu] [-- game args]
 *
 * The game files come from the arcade hard disk: when <data> doesn't hold them, the first run finds
 * hydro.chd (or asks for it) and extracts it there (disk_setup.c). --chd re-extracts from the given file.
 * --save holds cmos.bin, eeprom.bin and hydro.ini (default <Saved Games>\Midway\Hydro Thunder).
 * The game starts in the operator menu (menu_pc.c); --skip-menu boots straight into attract mode, as the cabinet does.
 *
 * Boot: map HYDRO.EXE at 0x100000, install HLE hooks (Win32 subset, Glide, hardware), trap every
 * remaining privileged routine, then enter the image's own MSVC CRT startup (mainCRTStartup),
 * skipping EtsStart / EtsInitSubsystems (kernel, IDT, PIC, disk, network bring-up).
 */
#include "disk_setup.h"
#include "hook.h"
#include "host.h"
#include "lobby_client.h"
#include "settings.h"
#include "symtab.h"
#include "trace.h"
#include "window.h"

#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HostConfig g_cfg;

static CRITICAL_SECTION g_log_lock;
static FILE *g_log;
static DWORD g_t0;

void hy_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_log_lock);
    DWORD t = GetTickCount() - g_t0;
    fprintf(stderr, "[%6lu.%03lu] %s\n", t / 1000, t % 1000, line);
    if (g_log) {
        fprintf(g_log, "[%6lu.%03lu] %s\n", t / 1000, t % 1000, line);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_lock);
}

void hy_fatal(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    hy_log("FATAL: %s", line);
    MessageBoxA(NULL, line, "Hydro Thunder host", MB_ICONERROR);
    ExitProcess(1);
}

static void set_path(char *dst, const char *src, int absolute)
{
    if (absolute)
        GetFullPathNameA(src, 260, dst, NULL);
    else
        snprintf(dst, 260, "%s", src);
}

static void default_save_dir(char *dst)
{
    char base[MAX_PATH] = "";
    PWSTR w;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_SavedGames, KF_FLAG_CREATE, NULL, &w))) {
        WideCharToMultiByte(CP_ACP, 0, w, -1, base, sizeof base, NULL, NULL);
        CoTaskMemFree(w);
    }
    if (!base[0]) {
        const char *home = getenv("USERPROFILE");
        snprintf(base, sizeof base, "%s\\Saved Games", home ? home : ".");
    }
    snprintf(dst, 260, "%s\\Midway\\Hydro Thunder", base);
}

/* Saves used to live in the data folder: carry them over once, never overwriting. */
static void migrate_saves(void)
{
    static const char *const names[] = {"cmos.bin", "eeprom.bin", "hydro.ini"};
    char name[32], from[MAX_PATH], to[MAX_PATH];
    for (int i = 0; i < 3 + 16; i++) {
        if (i < 3)
            snprintf(name, sizeof name, "%s", names[i]);
        else
            snprintf(name, sizeof name, "cmos_unit%d.bin", i - 2);
        snprintf(from, sizeof from, "%s\\%s", g_cfg.data_root, name);
        snprintf(to, sizeof to, "%s\\%s", g_cfg.save_dir, name);
        if (CopyFileA(from, to, TRUE))
            hy_log("copied save %s to %s", from, to);
    }
}

static const char *g_chd_arg; /* --chd: extract the disk image from this file */

static void parse_args(int argc, char **argv)
{
    GetCurrentDirectoryA(sizeof g_cfg.work_dir, g_cfg.work_dir);
    set_path(g_cfg.data_root, "data", 1);
    default_save_dir(g_cfg.save_dir);
    /* Next to hydro.exe first, then the normal DLL search order. */
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    char *slash = strrchr(self, '\\');
    if (slash)
        strcpy(slash + 1, "glide2x.dll");
    set_path(g_cfg.glide_dll, GetFileAttributesA(self) != INVALID_FILE_ATTRIBUTES ? self : "glide2x.dll", 0);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chd") && i + 1 < argc)
            g_chd_arg = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc)
            set_path(g_cfg.exe_path, argv[++i], 1);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc)
            set_path(g_cfg.data_root, argv[++i], 1);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            set_path(g_cfg.save_dir, argv[++i], 1);
        else if (!strcmp(argv[i], "--glide") && i + 1 < argc)
            set_path(g_cfg.glide_dll, argv[++i], 1);
        else if (!strcmp(argv[i], "--skip-menu"))
            g_cfg.skip_menu = 1;
        else if (!strcmp(argv[i], "--"))
            break;
        else
            hy_fatal("unknown argument %s", argv[i]);
    }
    if (!g_cfg.exe_path[0])
        snprintf(g_cfg.exe_path, sizeof g_cfg.exe_path, "%s\\C\\HYDRO.EXE", g_cfg.data_root);
}

int main(int argc, char **argv)
{
    InitializeCriticalSection(&g_log_lock);
    g_t0 = GetTickCount();
    /* Several instances on one PC for linked play: each gets its own CMOS (hle_hw.c) and log. */
    const char *unit = getenv("HYDRO_NET_UNIT");
    g_cfg.net_unit = unit && atoi(unit) >= 1 && atoi(unit) <= 16 ? atoi(unit) : 0;
    char log_name[32] = "hydro.log";
    if (g_cfg.net_unit)
        snprintf(log_name, sizeof log_name, "hydro_unit%d.log", g_cfg.net_unit);
    g_log = fopen(log_name, "w");
    hy_log("Hydro Thunder for Windows %s", HYDRO_VERSION);

    /* Claim the image range before anything else can allocate there. */
    parse_args(argc, argv);
    disk_setup(g_chd_arg);
    image_load(g_cfg.exe_path);
    hle_net_capture_checksum();
    hy_log("exe %s, data %s, saves %s, glide %s", g_cfg.exe_path, g_cfg.data_root, g_cfg.save_dir, g_cfg.glide_dll);
    int err = SHCreateDirectoryExA(NULL, g_cfg.save_dir, NULL);
    if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS)
        hy_fatal("can't create save folder %s (error %d)", g_cfg.save_dir, err);
    migrate_saves();

    /* hydro.ini is the source of truth for the managed dgVoodoo settings: push them before Glide loads. */
    settings_load();
    settings_apply_graphics();
    lobby_client_init();
    lobby_autostart();

    veh_install();
    window_create(1024, 768);
    hle_win32_install();
    hle_glide_install();
    hle_hw_install();
    hle_audio_install();
    hle_net_install();
    menu_assist_install();
    menu_pc_install();
    hook_trap_unhandled();
    game_ports_install();
    r2file_install();
    trace_install();
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)HY_IMAGE_BASE, HY_CODE_END - HY_IMAGE_BASE);

    void(__cdecl * crt_start)(void) = (void(__cdecl *)(void))(uintptr_t)sym_addr("mainCRTStartup");
    hy_log("entering mainCRTStartup at %08x", (uint32_t)(uintptr_t)crt_start);
    crt_start(); /* ends in the image's exit() -> ExitProcess */
    return 0;
}
