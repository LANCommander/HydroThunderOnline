#pragma once
#include <stdint.h>

/* HYDRO.EXE has no relocations: it must live at its link address. */
#define HY_IMAGE_BASE 0x00100000u
#define HY_CODE_END   0x001eb6e8u /* end of .text (start of CONST data) */

typedef struct HostConfig {
    char exe_path[260];  /* original HYDRO.EXE */
    char data_root[260]; /* folder holding C\, D\, E\ extracted from the disk image */
    char save_dir[260];  /* cmos, eeprom, hydro.ini: <Saved Games>\Midway\Hydro Thunder */
    char glide_dll[260]; /* dgVoodoo glide2x.dll */
    char work_dir[260];  /* launch directory (the game chdirs into data\C, so keep this for our files) */
    int windowed;
    int log_calls;       /* HYDRO_TRACE: log every forwarded ETS/Win32 call */
    int net_unit;        /* HYDRO_NET_UNIT=1..16: linked-play test instance (own CMOS, log, title); 0 = off */
    int skip_menu;       /* --skip-menu: boot into the game, not the operator menu */
} HostConfig;

extern HostConfig g_cfg;

void hy_log(const char *fmt, ...);
__declspec(noreturn) void hy_fatal(const char *fmt, ...);

/* image.c */
uint32_t image_load(const char *exe_path); /* returns SizeOfImage */

/* hle_*.c: each installs its hooks */
void hle_win32_install(void);
void hle_glide_install(void);
void hle_hw_install(void);
void hle_audio_install(void);
void hle_net_install(void);
void hle_net_capture_checksum(void); /* before any hook patches CODE */
void menu_assist_install(void); /* menu_assist.c */
void menu_pc_install(void);     /* menu_pc.c: PC SETTINGS in the operator menu */
void game_ports_install(void); /* game/ports.c */
void r2file_install(void);     /* game/r2file.c: MARKERS.R2X overlay on HT.R2 */

/* veh.c */
void veh_install(void);
