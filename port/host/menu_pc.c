/*
 * PC SETTINGS: a page added to the original operator menu for the port's own settings
 * (graphics via dgVoodoo, controls, keyboard and gamepad bindings). Saved to hydro.ini.
 *
 * It's built from the menu framework in the image (MESS/MENUUTIL/FIELDS); the tables and
 * handlers just live in the host. The framework, all __cdecl:
 *   handler(msg, param): 1 enter (param = parent when pushed), 2 paint, 3 leave, 4 key (param =
 *     key: 6 Test, 0xe boost = select, 7 service = back, 9/10 vol-/vol+ = up/down), 6 repeat,
 *     7 frame tick. mess_pushhandler enters a child, mess_sethandler(parent) returns.
 *   Menu {title, items, cur, parent}; MenuItem (32 bytes) {type, text, handler, help, arg x3,
 *     selected}, type 1 submenu, 2 back, 3 field (fields_int/fields_stringlist: arg10 desc,
 *     arg14 int *value, arg18 int *default), 4 command (handler(1, item)), 6 heading, 7 blank.
 *   menuutil_auto_message(menu, msg, param) runs a list menu; the screen is 64x48 8x8 cells,
 *   items on rows 11..40, a note on row 43 and help on rows 45..46.
 *
 * menu_operator (the root) is replaced so its table can carry the extra item; otherwise it's
 * the original (0x1bca40).
 *
 * The port boots into this menu (unless --skip-menu): _main's loop is "run the game; menus(2)", and
 * its game call is repointed at boot_game, which shows the menu once before the first run.
 */
#include "hook.h"
#include "host.h"
#include "input.h"
#include "lobby_client.h"
#include "settings.h"
#include "window.h"

#include "hydro_syms.h"
#include "menu_fw.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>


/* ---------------------------------------------------------------- root (menu_operator) */

#define ROOT_ORIG ((const MenuItem *)0x0030d5f0u) /* 13 items + terminator */
#define ROOT_START 18                             /* START THE GAME, kept last (service jumps there) */

static int __cdecl pc_menu(int msg, int p);

/* QUIT: save what leaving the menu would save (operator settings, audits, ...), then exit. */
static int __cdecl cmd_quit(int one, MenuItem *it)
{
    (void)one, (void)it;
    hy_log("operator menu: QUIT");
    lobby_leave();
    FN(void(__cdecl *)(void), menus_disksettings_write)();
    window_quit();
    return 0;
}

static MenuItem g_root_items[ROOT_START + 2];
static Menu g_root = {NULL, g_root_items, 999, NULL};

/* The original's table with NETWORK LOBBY, PC SETTINGS and QUIT inserted above START THE GAME. */
static void root_build(void)
{
    memcpy(g_root_items, ROOT_ORIG, 12 * sizeof(MenuItem));
    g_root_items[12] = (MenuItem){IT_SUBMENU, "NETWORK LOBBY", lobby_menu, "Host or join a LAN or internet lobby"};
    g_root_items[13] = (MenuItem){IT_BLANK};
    g_root_items[14] = (MenuItem){IT_SUBMENU, "PC SETTINGS", pc_menu, STR(Mt_enter_this_menu)};
    g_root_items[15] = (MenuItem){IT_BLANK};
    g_root_items[16] = (MenuItem){IT_COMMAND, "QUIT", cmd_quit, "Press TEST to save and quit Hydro Thunder"};
    g_root_items[17] = (MenuItem){IT_BLANK};
    g_root_items[ROOT_START] = ROOT_ORIG[12];
    g_root_items[ROOT_START + 1] = (MenuItem){IT_END};
}

static int __cdecl my_menu_operator(int msg, int p)
{
    if (msg == MSG_ENTER) {
        g_root_items[0].text = STR(Mt_diag_title);
        g_root_items[0].handler = (const void *)HY_FN_menu_diag;
        g_root.title = STR(Mt_operator_title);
        /* DIP switch 3 on: START THE GAME turns into a grey heading */
        g_root_items[ROOT_START].type = (dip_switches() & 4) | IT_BACK;
    } else if (msg == MSG_PAINT) {
        paint_background();
        FN(void(__cdecl *)(int), menuutil_show_versions)(0x1e);
        if (dip_switches() & 4)
            vtext_set_string(4, k_note_row, C_YELLOW, 0, "*** DIP switch 3 must be OFF to run game ***");
    } else if (msg == MSG_KEY && p == KEY_SERVICE) {
        set_current_line(&g_root, 999);
        mess_queue_message(MSG_PAINT, 0);
    }
    Menus_nOperatorCalls++;
    auto_message(&g_root, msg, p);
    if (msg == MSG_LEAVE)
        clear_line(k_note_row);
    return 0;
}

/* ---------------------------------------------------------------- boot into the menu */

#define MAIN_CALL_RUN_GAME 0x00147479u /* _main: call 0x1474a8 (init, gameloop_Start, close) */
#define RUN_GAME 0x001474a8u

static int __cdecl boot_game(int argc, char **argv, char **env)
{
    static int booted;
    /* The same as a cabinet with DIP switch 3 on, which only ever runs menus(2). _main's cold-boot
     * flag (0x57443c) is still set, so the first game run afterwards is a cold boot as usual. */
    if (!booted++ && !g_cfg.skip_menu)
        FN(int(__cdecl *)(int), menus)(2);
    return ((int(__cdecl *)(int, char **, char **))RUN_GAME)(argc, argv, env);
}

/* ---------------------------------------------------------------- setting fields */

#define MAX_FIELDS 32
static FieldStr g_lists[MAX_FIELDS][12];
static FieldIntDesc g_ranges[MAX_FIELDS];
static int g_defaults[MAX_FIELDS];

/* Items for every setting in `section`, then `extra` (each after a blank), a blank, RESET TO DEFAULTS, a blank
 * and RETURN. */
static int __cdecl cmd_reset_section(int one, MenuItem *it);

static void build_fields(MenuItem *items, const char *section, const char *help_reset, const MenuItem *extra,
                         int n_extra)
{
    int n = 0;
    for (int i = 0; i < g_setting_count && i < MAX_FIELDS; i++) {
        const SettingDesc *d = &g_setting_desc[i];
        if (strcmp(d->section, section))
            continue;
        if (n)
            items[n++] = (MenuItem){IT_BLANK}; /* the originals space their rows */
        g_defaults[i] = d->def;
        MenuItem *it = &items[n++];
        *it = (MenuItem){IT_FIELD, d->label, NULL, STR(Mt_enter_to_change), NULL, d->value, &g_defaults[i]};
        if (d->count) {
            for (int k = 0; k < d->count && k < 11; k++)
                g_lists[i][k] = (FieldStr){d->names[k], d->values ? d->values[k] : k};
            g_lists[i][d->count < 11 ? d->count : 11] = (FieldStr){NULL, 0};
            it->handler = (const void *)HY_FN_fields_stringlist;
            it->arg10 = g_lists[i];
        } else {
            g_ranges[i] = (FieldIntDesc){d->min, d->max, d->step, 0, "%i%%", NULL};
            it->handler = (const void *)HY_FN_fields_int;
            it->arg10 = &g_ranges[i];
        }
    }
    for (int e = 0; e < n_extra; e++) {
        items[n++] = (MenuItem){IT_BLANK};
        items[n++] = extra[e];
    }
    items[n++] = (MenuItem){IT_BLANK};
    items[n++] = (MenuItem){IT_COMMAND, "RESET TO DEFAULTS", cmd_reset_section, help_reset, section};
    items[n++] = (MenuItem){IT_BLANK};
    items[n++] = (MenuItem){IT_BACK, STR(Mt_return_to_previous), NULL, STR(Mt_test_to_return)};
    items[n] = (MenuItem){IT_END};
}

static int __cdecl cmd_reset_section(int one, MenuItem *it)
{
    (void)one;
    for (int i = 0; i < g_setting_count; i++)
        if (!strcmp(g_setting_desc[i].section, (const char *)it->arg10))
            *g_setting_desc[i].value = g_setting_desc[i].def;
    if (!strcmp((const char *)it->arg10, "Network"))
        snprintf(g_settings.net.master, sizeof g_settings.net.master, "%s", NET_DEFAULT_MASTER);
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

/* ---------------------------------------------------------------- GRAPHICS */

static MenuItem g_gfx_items[2 * MAX_FIELDS + 5];
static Menu g_gfx = {"GRAPHICS", g_gfx_items, 0, NULL};
static DgvGraphics g_gfx_before;

static int __cdecl gfx_menu(int msg, int p)
{
    if (msg == MSG_ENTER) {
        if (p)
            g_gfx_before = g_settings.gfx;
        Fields_col = 22;
    } else if (msg == MSG_PAINT) {
        paint_background();
    }
    auto_message(&g_gfx, msg, p);
    if (msg == MSG_PAINT)
        vtext_set_string(4, k_note_row, C_GREY, 0, "Settings apply when you leave this menu");
    if (msg == MSG_LEAVE) {
        clear_line(k_note_row);
        Fields_col = 0x14;
        if (p == 0 && memcmp(&g_gfx_before, &g_settings.gfx, sizeof g_gfx_before)) {
            settings_save();
            settings_apply_graphics();
            /* dgVoodoo reads its config at grGlideInit: restart the menu's Glide session so the new
             * mode shows now. The parent's repaint redraws every cell. */
            FN(void(__cdecl *)(void), vgraphics_term)();
            FN(void(__cdecl *)(int), vgraphics_init)(!Vinput_bLowRes);
            g_gfx_before = g_settings.gfx;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- AUDIO */

/* hle_audio.c reads these every block, so a change is heard at once. */
static MenuItem g_audio_items[2 * MAX_FIELDS + 5];
static Menu g_audio = {"AUDIO", g_audio_items, 0, NULL};

static int __cdecl audio_menu(int msg, int p)
{
    if (msg == MSG_ENTER)
        Fields_col = 22;
    else if (msg == MSG_PAINT)
        paint_background();
    auto_message(&g_audio, msg, p);
    if (msg == MSG_PAINT)
        vtext_set_string(4, k_note_row, C_GREY, 0, "Headrest: voices, music. Subwoofer: rumble");
    if (msg == MSG_LEAVE) {
        clear_line(k_note_row);
        Fields_col = 0x14;
        if (p == 0)
            settings_save();
    }
    return 0;
}

/* ---------------------------------------------------------------- CONTROLS */

static MenuItem g_ctl_items[2 * MAX_FIELDS + 5];
static Menu g_ctl = {"CONTROLS", g_ctl_items, 0, NULL};

static int __cdecl ctl_menu(int msg, int p)
{
    if (msg == MSG_ENTER)
        Fields_col = 22;
    else if (msg == MSG_PAINT)
        paint_background();
    auto_message(&g_ctl, msg, p);
    if (msg == MSG_LEAVE) {
        Fields_col = 0x14;
        if (p == 0)
            settings_save();
    }
    return 0;
}

/* ---------------------------------------------------------------- NETWORK */

static MenuItem g_net_items[2 * MAX_FIELDS + 7];
static Menu g_net = {"NETWORK", g_net_items, 0, NULL};
static char g_txt_master[64];

static int __cdecl net_menu(int msg, int p)
{
    if (msg == MSG_ENTER || msg == MSG_PAINT)
        snprintf(g_txt_master, sizeof g_txt_master, "%-21s %s", "MASTER SERVER", g_settings.net.master);
    if (msg == MSG_ENTER)
        Fields_col = 22;
    else if (msg == MSG_PAINT)
        paint_background();
    auto_message(&g_net, msg, p);
    if (msg == MSG_PAINT)
        vtext_set_string(4, k_note_row, C_GREY, 0, "Lobbies: NETWORK LOBBY. Unit ID: NETWORK ADJUSTMENTS");
    if (msg == MSG_LEAVE) {
        clear_line(k_note_row);
        Fields_col = 0x14;
        if (p == 0)
            settings_save();
    }
    return 0;
}

/* ---------------------------------------------------------------- binding prompt */

static struct {
    int pad; /* 0 = keyboard, 1 = gamepad */
    int act;
    int step;           /* slot being asked for: 0, 1; 2 = done, waiting for release */
    int got[BIND_SLOTS];
    int changed;
    ULONGLONG t0;
    MessHandler parent;
} g_cap;

#define CAP_TIMEOUT_MS 6000

static const char *slot_text(int pad, int v)
{
    const char *s = pad ? pad_name(v) : key_name(v);
    return *s ? s : "---";
}

static int __cdecl capture_screen(int msg, int p)
{
    char line[80];
    switch (msg) {
    case MSG_ENTER:
        g_cap.parent = (MessHandler)p;
        g_cap.step = 0;
        g_cap.changed = 0;
        g_cap.t0 = GetTickCount64();
        input_set_capture(1);
        break;
    case MSG_PAINT:
        paint_background();
        menu_title(g_cap.pad ? "GAMEPAD BINDINGS" : "KEYBOARD BINDINGS");
        clear_line_range(11, 40);
        snprintf(line, sizeof line, "%s", action_name(g_cap.act));
        vtext_set_string(4, 12, C_GREEN, 0, line);
        if (g_cap.step == 0)
            snprintf(line, sizeof line, "Press a %s for this input", g_cap.pad ? "pad button" : "key");
        else if (g_cap.step == 1)
            snprintf(line, sizeof line, "Press a second %s, or %s for none", g_cap.pad ? "button" : "key",
                     g_cap.pad ? "wait / ESC" : "ESC");
        else
            snprintf(line, sizeof line, "Release all %s", g_cap.pad ? "buttons" : "keys");
        vtext_set_string(4, 15, C_WHITE, 0, line);
        if (g_cap.step >= 1) {
            snprintf(line, sizeof line, "First:  %s", slot_text(g_cap.pad, g_cap.got[0]));
            vtext_set_string(4, 18, C_YELLOW, 0, line);
        }
        if (g_cap.step >= 2) {
            snprintf(line, sizeof line, "Second: %s", slot_text(g_cap.pad, g_cap.got[1]));
            vtext_set_string(4, 19, C_YELLOW, 0, line);
        }
        menu_message(g_cap.step == 0 ? (g_cap.pad ? "ESC (keyboard) or waiting 6 seconds cancels"
                                                  : "ESC cancels and keeps the current keys")
                                     : "");
        break;
    case MSG_TICK: {
        int esc = 0, got = g_cap.pad ? PAD_NONE : 0;
        if (g_cap.step < 2) {
            int k = input_capture_key();
            if (k == VK_ESCAPE)
                esc = 1;
            else if (!g_cap.pad && k)
                got = k;
            if (g_cap.pad) {
                got = input_capture_pad();
                if (GetTickCount64() - g_cap.t0 > CAP_TIMEOUT_MS)
                    esc = 1;
            }
        }
        int have = g_cap.pad ? got != PAD_NONE : got != 0;
        if (g_cap.step == 0 && (esc || have)) {
            if (esc) {
                g_cap.step = 2; /* cancelled: nothing changes */
            } else {
                g_cap.got[0] = got;
                g_cap.step = 1;
                g_cap.t0 = GetTickCount64();
            }
            mess_queue_message(MSG_PAINT, 0);
        } else if (g_cap.step == 1 && (esc || have)) {
            g_cap.got[1] = esc || got == g_cap.got[0] ? (g_cap.pad ? PAD_NONE : 0) : got;
            int *dst = g_cap.pad ? g_settings.ctl.pad[g_cap.act] : g_settings.ctl.key[g_cap.act];
            dst[0] = g_cap.got[0];
            dst[1] = g_cap.got[1];
            g_cap.changed = 1;
            g_cap.step = 2;
            menusnd_play(0, KEY_TEST);
            mess_queue_message(MSG_PAINT, 0);
        } else if (g_cap.step == 2 && !input_any_held()) {
            input_set_capture(0);
            mess_sethandler(g_cap.parent);
        }
        break;
    }
    case MSG_LEAVE:
        input_set_capture(0);
        menu_title("");
        menu_message("");
        clear_line_range(11, 40);
        if (g_cap.changed)
            settings_save();
        break;
    }
    return 0; /* keys (msg 4/6) are swallowed: the prompt reads the host keyboard itself */
}

/* ---------------------------------------------------------------- KEYBOARD / GAMEPAD BINDINGS */

static char g_bind_text[2][ACT_COUNT][64];
static MenuItem g_bind_items[2][2 * ACT_COUNT + 5];
static Menu g_bind[2] = {{"KEYBOARD BINDINGS", g_bind_items[0], 0, NULL},
                         {"GAMEPAD BINDINGS", g_bind_items[1], 0, NULL}};

static int __cdecl cmd_bind(int one, MenuItem *it)
{
    (void)one;
    g_cap.pad = it->arg14 != NULL;
    g_cap.act = (int)(intptr_t)it->arg10;
    MessHandler prev = mess_sethandler_raw(capture_screen);
    capture_screen(MSG_ENTER, (int)(intptr_t)prev);
    capture_screen(MSG_PAINT, 0);
    return 0;
}

static int __cdecl cmd_bind_defaults(int one, MenuItem *it)
{
    (void)one;
    Controls d;
    settings_default_bindings(&d);
    if (it->arg14)
        memcpy(g_settings.ctl.pad, d.pad, sizeof d.pad);
    else
        memcpy(g_settings.ctl.key, d.key, sizeof d.key);
    settings_save();
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

static void bind_text_refresh(int pad)
{
    for (int a = 0; a < ACT_COUNT; a++) {
        const int *v = pad ? g_settings.ctl.pad[a] : g_settings.ctl.key[a];
        int none = pad ? PAD_NONE : 0;
        const char *s0 = slot_text(pad, v[0]);
        if (v[1] != none)
            snprintf(g_bind_text[pad][a], sizeof g_bind_text[pad][a], "%-16s %s / %s", action_name(a), s0,
                     slot_text(pad, v[1]));
        else
            snprintf(g_bind_text[pad][a], sizeof g_bind_text[pad][a], "%-16s %s", action_name(a), s0);
    }
}

static void build_bind(int pad)
{
    MenuItem *items = g_bind_items[pad];
    int n = 0;
    for (int a = 0; a < ACT_COUNT; a++) {
        if (n)
            items[n++] = (MenuItem){IT_BLANK};
        items[n++] = (MenuItem){IT_COMMAND, g_bind_text[pad][a], cmd_bind,
                                pad ? "Press TEST, then press the pad button(s) for this input"
                                    : "Press TEST, then press the key(s) for this input",
                                (const void *)(intptr_t)a, pad ? (const void *)1 : NULL};
    }
    items[n++] = (MenuItem){IT_BLANK};
    items[n++] = (MenuItem){IT_COMMAND, "RESET TO DEFAULTS", cmd_bind_defaults, STR(Mt_run_command), NULL,
                            pad ? (const void *)1 : NULL};
    items[n++] = (MenuItem){IT_BLANK};
    items[n++] = (MenuItem){IT_BACK, STR(Mt_return_to_previous), NULL, STR(Mt_test_to_return)};
    items[n] = (MenuItem){IT_END};
}

static int bind_menu(int pad, int msg, int p)
{
    if (msg == MSG_PAINT) {
        paint_background();
        bind_text_refresh(pad);
    }
    auto_message(&g_bind[pad], msg, p);
    if (msg == MSG_PAINT && pad)
        vtext_set_string(4, k_note_row, C_GREY, 0, "Steering and throttle sticks: see CONTROLS");
    if (msg == MSG_LEAVE)
        clear_line(k_note_row);
    return 0;
}

static int __cdecl keys_menu(int msg, int p) { return bind_menu(0, msg, p); }
static int __cdecl pad_menu(int msg, int p) { return bind_menu(1, msg, p); }

/* ---------------------------------------------------------------- PC SETTINGS */

static MenuItem g_pc_items[] = {
    {IT_SUBMENU, "GRAPHICS", gfx_menu, "Display mode, resolution and filtering (dgVoodoo)"},
    {IT_BLANK},
    {IT_SUBMENU, "AUDIO", audio_menu, "Headrest speaker and subwoofer mix"},
    {IT_BLANK},
    {IT_SUBMENU, "CONTROLS", ctl_menu, "Gamepad sticks, dead zone, keyboard steering"},
    {IT_BLANK},
    {IT_SUBMENU, "KEYBOARD BINDINGS", keys_menu, "Keys for each cabinet input"},
    {IT_BLANK},
    {IT_SUBMENU, "GAMEPAD BINDINGS", pad_menu, "Gamepad buttons for each cabinet input"},
    {IT_BLANK},
    {IT_SUBMENU, "NETWORK", net_menu, "Linked units and the lobby master server"},
    {IT_BLANK},
    {IT_BACK, NULL, NULL, NULL},
    {IT_END},
};
static Menu g_pc = {"PC SETTINGS", g_pc_items, 0, NULL};

static int __cdecl pc_menu(int msg, int p)
{
    if (msg == MSG_PAINT)
        paint_background();
    auto_message(&g_pc, msg, p);
    return 0;
}

void menu_pc_install(void)
{
    root_build();
    for (MenuItem *it = g_pc_items; it->type != IT_END; it++)
        if (it->type == IT_BACK) {
            it->text = STR(Mt_return_to_previous);
            it->help = STR(Mt_test_to_return);
        }
    static const MenuItem net_extra[] = {
        {IT_COMMAND, g_txt_master, lobby_cmd_master, "Press TEST to type the lobby master server's address"},
    };
    build_fields(g_gfx_items, "Graphics", "Press TEST to restore the default graphics settings", NULL, 0);
    build_fields(g_audio_items, "Audio", "Press TEST to restore the default audio settings", NULL, 0);
    build_fields(g_ctl_items, "Controls", "Press TEST to restore the default control settings", NULL, 0);
    build_fields(g_net_items, "Network", "Press TEST to restore the default network settings", net_extra, 1);
    build_bind(0);
    build_bind(1);
    menu_lobby_install();
    hook_name("menu_operator", my_menu_operator);
    hook_patch32(MAIN_CALL_RUN_GAME + 1, RUN_GAME - (MAIN_CALL_RUN_GAME + 5),
                 (uint32_t)(uintptr_t)boot_game - (MAIN_CALL_RUN_GAME + 5));
}
