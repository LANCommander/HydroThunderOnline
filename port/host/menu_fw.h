#pragma once
/*
 * The operator menu framework in the image (MESS/MENUUTIL/FIELDS), as the host's own pages use it
 * (menu_pc.c, menu_lobby.c). See menu_pc.c for how it works.
 */
#include "hydro_syms.h"

typedef int(__cdecl *MessHandler)(int msg, int param);

typedef struct MenuItem {
    int type;
    const char *text;
    const void *handler;
    const char *help;
    const void *arg10, *arg14, *arg18;
    int selected;
} MenuItem;

typedef struct Menu {
    const char *title;
    MenuItem *items;
    int cur; /* 999 = last */
    MessHandler parent;
} Menu;

typedef struct FieldStr {
    const char *text;
    int value;
} FieldStr;

typedef struct FieldIntDesc {
    int min, max, step, unused;
    const char *fmt;
    const void *draw;
} FieldIntDesc;

enum { IT_END, IT_SUBMENU, IT_BACK, IT_FIELD, IT_COMMAND, IT_HEADING = 6, IT_BLANK };
enum { MSG_ENTER = 1, MSG_PAINT, MSG_LEAVE, MSG_KEY, MSG_REPEAT = 6, MSG_TICK };
enum { KEY_TEST = 6, KEY_SERVICE = 7, KEY_UP = 9, KEY_DOWN = 10 };

#define C_WHITE 0xffff
#define C_GREEN 0x07e0
#define C_YELLOW 0xffe0
#define C_GREY 0x8410
#define C_DARK 0x4208

#define FN(type, name) ((type)HY_FN_##name)
#define STR(name) ((const char *)HY_VAR_##name)
#define Fields_col (*(int *)HY_VAR_Fields_col)
#define Vinput_bLowRes (*(int *)0x0063937cu)
#define Menus_nOperatorCalls (*(int *)0x00606174u) /* bumped by menu_operator on every message */

#define auto_message FN(int(__cdecl *)(Menu *, int, int), menuutil_auto_message)
#define set_current_line FN(void(__cdecl *)(Menu *, int), menuutil_set_current_line)
#define mess_sethandler FN(MessHandler(__cdecl *)(MessHandler), mess_sethandler)
#define mess_sethandler_raw FN(MessHandler(__cdecl *)(MessHandler), mess_sethandler_raw)
#define mess_queue_message FN(void(__cdecl *)(int, int), mess_queue_message)
#define paint_background FN(void(__cdecl *)(void), menu_main_paint_standard_background)
#define menu_title FN(void(__cdecl *)(const char *), menuutil_title)
#define menu_message FN(void(__cdecl *)(const char *), menuutil_message)
#define clear_line FN(void(__cdecl *)(int), menuutil_clear_line)
#define clear_line_range FN(void(__cdecl *)(int, int), menuutil_clear_line_range)
#define vtext_set_string FN(void(__cdecl *)(int, int, int, int, const char *), vtext_set_string)
#define menusnd_play FN(void(__cdecl *)(int, int), menusnd_play)
#define dip_switches FN(int(__cdecl *)(void), diegoio_comm_GetCachedDIPSwitches)

#define k_note_row 43

/* menu_lobby.c */
int __cdecl lobby_menu(int msg, int p);
int __cdecl lobby_cmd_master(int one, MenuItem *it); /* types g_settings.net.master */
void menu_lobby_install(void);
