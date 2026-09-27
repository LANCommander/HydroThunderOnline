/*
 * NETWORK LOBBY: an operator menu page (next to PC SETTINGS) for lobbies (lobby_client.c).
 * Host an internet lobby on the master server or a LAN lobby on this PC, find and join lobbies (or
 * DIRECT CONNECT to a PC's lobby by address), then wait in the LOBBY page with its players and chat. Joining takes effect when the operator picks START THE GAME:
 * gameinit reads the operator settings again then, and operator_ModuleInit (game/operator.c)
 * applies the lobby's unit.
 *
 * Text (names, password, master address) is typed on a prompt page: the keyboard types, a pad
 * spells with the d-pad (up/down letter, right next letter, left delete, A accept, B cancel).
 */
#include "hook.h"
#include "host.h"
#include "input.h"
#include "lobby_client.h"
#include "settings.h"

#include "hydro_syms.h"
#include "menu_fw.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define STATUS_ROW 41

/* ---------------------------------------------------------------- text entry */

typedef void (*TextDone)(const char *text);

static struct {
    const char *title, *prompt;
    char buf[128];
    int max;
    int secret;
    TextDone done;
    void (*cancel)(void);
    void (*extra)(void); /* paints more below the prompt (the chat) */
    unsigned (*extra_ver)(void); /* changes when `extra` needs a repaint */
    unsigned extra_seen;
    int finished, ok;
    MessHandler parent;
} g_txt;

static const char k_charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:!?";

static int allowed(int c)
{
    return c >= 0x20 && c < 0x7f && c != '\\' && c != '"' && c != ';';
}

static int __cdecl text_screen(int msg, int p)
{
    char line[160];
    switch (msg) {
    case MSG_ENTER:
        g_txt.parent = (MessHandler)p;
        g_txt.finished = g_txt.ok = 0;
        input_set_capture(1);
        break;
    case MSG_PAINT: {
        paint_background();
        menu_title(g_txt.title);
        clear_line_range(11, 40);
        vtext_set_string(4, 12, C_GREEN, 0, g_txt.prompt);
        size_t n = strlen(g_txt.buf);
        if (g_txt.secret) {
            memset(line, '*', n);
            line[n] = 0;
        } else {
            snprintf(line, sizeof line, "%s", g_txt.buf);
        }
        if (!g_txt.finished && n < (size_t)g_txt.max)
            strcat(line, "_");
        vtext_set_string(4, 15, C_WHITE, 0, line);
        vtext_set_string(4, 19, C_GREY, 0, "Keyboard: type, ENTER accepts, ESC cancels");
        vtext_set_string(4, 21, C_GREY, 0, "Pad: up/down letter, right next, left delete,");
        vtext_set_string(4, 22, C_GREY, 0, "     A accepts, B cancels");
        if (g_txt.extra)
            g_txt.extra();
        menu_message("");
        break;
    }
    case MSG_TICK: {
        if (g_txt.finished) {
            if (!input_any_held()) {
                input_set_capture(0);
                if (g_txt.ok && g_txt.done)
                    g_txt.done(g_txt.buf);
                else if (!g_txt.ok && g_txt.cancel)
                    g_txt.cancel();
                mess_sethandler(g_txt.parent);
            }
            break;
        }
        int changed = 0;
        size_t n = strlen(g_txt.buf);
        for (int c; (c = input_text_char()) != 0;) {
            if (c == '\r' || c == 27) {
                g_txt.finished = 1;
                g_txt.ok = c == '\r';
            } else if (c == '\b') {
                if (n)
                    g_txt.buf[--n] = 0;
            } else if (allowed(c) && n < (size_t)g_txt.max) {
                g_txt.buf[n++] = (char)c;
                g_txt.buf[n] = 0;
            }
            changed = 1;
        }
        int b = input_capture_pad();
        if (b != PAD_NONE) {
            changed = 1;
            if (b == PAD_A || b == PAD_START) {
                g_txt.finished = g_txt.ok = 1;
            } else if (b == PAD_B) {
                g_txt.finished = 1;
            } else if (b == PAD_DPAD_LEFT) {
                if (n)
                    g_txt.buf[--n] = 0;
            } else if (b == PAD_DPAD_RIGHT && n < (size_t)g_txt.max) {
                g_txt.buf[n++] = 'A';
                g_txt.buf[n] = 0;
            } else if (b == PAD_DPAD_UP || b == PAD_DPAD_DOWN) {
                if (!n) {
                    g_txt.buf[n++] = 'A';
                    g_txt.buf[n] = 0;
                } else {
                    const char *at = strchr(k_charset, g_txt.buf[n - 1]);
                    int i = at && g_txt.buf[n - 1] ? (int)(at - k_charset) : 0;
                    int count = (int)sizeof k_charset - 1;
                    i = (i + (b == PAD_DPAD_UP ? 1 : count - 1)) % count;
                    g_txt.buf[n - 1] = k_charset[i];
                }
            }
        }
        if (g_txt.extra_ver && g_txt.extra_ver() != g_txt.extra_seen) {
            g_txt.extra_seen = g_txt.extra_ver();
            changed = 1;
        }
        if (changed)
            mess_queue_message(MSG_PAINT, 0);
        break;
    }
    case MSG_LEAVE:
        input_set_capture(0);
        g_txt.cancel = NULL; /* set by text_extras before the next text_open, if at all */
        g_txt.extra = NULL;
        g_txt.extra_ver = NULL;
        menu_title("");
        menu_message("");
        clear_line_range(11, 40);
        break;
    }
    return 0; /* keys are swallowed: the prompt reads the host keyboard / pad itself */
}

/* Before text_open: called on cancel; paints more below the prompt, repainted when ver() changes. */
static void text_extras(void (*cancel)(void), void (*extra)(void), unsigned (*ver)(void))
{
    g_txt.cancel = cancel;
    g_txt.extra = extra;
    g_txt.extra_ver = ver;
}

static void text_open(const char *title, const char *prompt, const char *init, int max, int secret, TextDone done)
{
    g_txt.title = title;
    g_txt.prompt = prompt;
    snprintf(g_txt.buf, sizeof g_txt.buf, "%s", init ? init : "");
    g_txt.max = max < (int)sizeof g_txt.buf - 1 ? max : (int)sizeof g_txt.buf - 1;
    g_txt.buf[g_txt.max] = 0;
    g_txt.secret = secret;
    g_txt.done = done;
    g_txt.extra_seen = g_txt.extra_ver ? g_txt.extra_ver() : 0;
    MessHandler prev = mess_sethandler_raw(text_screen);
    text_screen(MSG_ENTER, (int)(intptr_t)prev);
    text_screen(MSG_PAINT, 0);
}

/* ---------------------------------------------------------------- status line */

static char g_status[96], g_status_painted[96];

static void status_text(char *out, size_t cap)
{
    int unit;
    if (lobby_active(&unit)) {
        char names[HTNET_MAX_UNITS][HTNET_NAME_LEN];
        unsigned mask = lobby_members(names);
        int n = 0;
        for (int u = 0; u < HTNET_MAX_UNITS; u++)
            n += (mask >> u) & 1;
        snprintf(out, cap, "%s lobby %s: player %d%s, %d/%d in",
                 lobby_is_direct() ? "DIRECT" : lobby_is_lan() ? "LAN" : "NET", lobby_name(),
                 unit + 1, lobby_is_owner() ? " (host)" : "", n, lobby_max());
    } else if (lobby_state() != LOBBY_IDLE && lobby_message()[0]) {
        snprintf(out, cap, "%s", lobby_message());
    } else {
        snprintf(out, cap, "Not in a lobby");
    }
}

static void status_paint(void)
{
    status_text(g_status, sizeof g_status);
    snprintf(g_status_painted, sizeof g_status_painted, "%s", g_status);
    clear_line(STATUS_ROW);
    vtext_set_string(4, STATUS_ROW, lobby_state() == LOBBY_FAILED ? C_YELLOW : C_GREEN, 0, g_status);
    vtext_set_string(4, k_note_row, C_GREY, 0, "Lobby changes apply at START THE GAME");
}

/* Repaint when the lobby's state changed (joins finish, members come and go). */
static int status_changed(void)
{
    status_text(g_status, sizeof g_status);
    return strcmp(g_status, g_status_painted) != 0;
}

static void status_clear(void)
{
    clear_line(STATUS_ROW);
    clear_line(k_note_row);
    g_status_painted[0] = 0;
}

/* ---------------------------------------------------------------- LOBBY (players and chat) */

/* Where a lobby's members wait: who's in, the chat, and START GAME for the lobby's owner. Hosting,
 * joining from FIND LOBBIES and DIRECT CONNECT all come here once the lobby has taken us. */

#define ROOM_PLAYERS_ROW 19 /* heading; the players on the rows after it (two columns past 8) */
#define ROOM_OPTS_ROW 28
#define ROOM_CHAT_ROW 29 /* heading; the chat on the rows after it, up to the field's */
#define ROOM_SAY_ROW 40  /* the chat's text field, just above the status line */
#define CHAT_COLS 58

static char g_room_title[48], g_room_players[48];
static int g_room_pending; /* a create/join is under way: enter the room when it succeeds */
static int g_room_exit;    /* LEAVE LOBBY: back to NETWORK LOBBY on the next tick */
static unsigned g_room_chat_seen;
static char g_room_sig[512];

static int __cdecl room_menu(int msg, int p);

/* START GAME: the menu leaves like START THE GAME, and every other member's follows (menus()) */
static int __cdecl cmd_start(int one, MenuItem *it)
{
    (void)one, (void)it;
    if (!lobby_start_game())
        return 0;
    mess_queue_message(10, 0); /* what START THE GAME queues: menus_Pump returns on it */
    menusnd_play(0, KEY_TEST);
    return 0;
}

static int chat_format(const LobbyChatLine *c, char *out, size_t cap)
{
    return c->unit < 0 ? snprintf(out, cap, "* %s", c->text) : snprintf(out, cap, "%s: %s", c->name, c->text);
}

/* The chat's last lines, word-wrapped, on rows first..last. */
static void chat_paint(int first, int last)
{
    const LobbyChatLine *lines;
    int n = lobby_chat(&lines);
    int self = -1;
    lobby_active(&self);
    struct {
        const LobbyChatLine *c;
        int from, len; /* the slice of chat_format's text */
    } rows[48];
    int nrows = 0, want = last - first + 1;
    /* back from the newest line until the rows are full */
    for (int i = n - 1; i >= 0 && nrows < want; i--) {
        char full[128];
        int len = chat_format(&lines[i], full, sizeof full);
        if (len >= (int)sizeof full)
            len = (int)sizeof full - 1;
        int cuts[8], ncut = 0, at = 0;
        while (at < len && ncut < 8) {
            int width = ncut ? CHAT_COLS - 2 : CHAT_COLS, take = len - at;
            if (take > width) {
                take = width;
                for (int k = width; k > width / 2; k--)
                    if (full[at + k] == ' ') {
                        take = k;
                        break;
                    }
            }
            cuts[ncut++] = at;
            at += take;
            while (at < len && full[at] == ' ')
                at++;
        }
        for (int k = ncut - 1; k >= 0 && nrows < want; k--) {
            int end = k + 1 < ncut ? cuts[k + 1] : len;
            while (end > cuts[k] && full[end - 1] == ' ')
                end--;
            rows[nrows].c = &lines[i];
            rows[nrows].from = cuts[k];
            rows[nrows].len = end - cuts[k];
            nrows++;
        }
    }
    clear_line_range(first, last);
    for (int r = 0; r < nrows; r++) {
        int row = first + nrows - 1 - r; /* rows[] runs newest first */
        const LobbyChatLine *c = rows[r].c;
        char full[128], piece[80];
        chat_format(c, full, sizeof full);
        snprintf(piece, sizeof piece, "%.*s", rows[r].len, full + rows[r].from);
        int x = rows[r].from ? 6 : 4;
        if (c->unit < 0) {
            vtext_set_string(x, row, C_GREY, 0, piece);
        } else if (rows[r].from == 0) {
            /* the name in its color (yellow = you), the text in white */
            int nl = (int)strlen(c->name) + 1;
            char name[HTNET_NAME_LEN + 2];
            snprintf(name, sizeof name, "%s:", c->name);
            vtext_set_string(x, row, c->unit == self ? C_YELLOW : C_GREEN, 0, name);
            if ((int)strlen(piece) > nl)
                vtext_set_string(x + nl, row, C_WHITE, 0, piece + nl);
        } else {
            vtext_set_string(x, row, C_WHITE, 0, piece);
        }
    }
}

static void chat_prompt_paint(void)
{
    vtext_set_string(4, 25, C_GREEN, 0, "CHAT");
    chat_paint(26, 40);
}

/* The CHAT line is a text field: with the cursor on it the keyboard types there (input_set_typing)
 * and ENTER sends, while Up/Down still move. A pad's A opens the prompt page to spell a message. */
#define SAY_SHOWN 52 /* the field shows the message's end */

static char g_say[HTNET_CHAT_LEN], g_say_line[80];
static int g_say_typing;

static void say_send(const char *t)
{
    if (*t && !lobby_say(t))
        menusnd_play(0, KEY_SERVICE);
}

static void on_say(const char *t)
{
    say_send(t);
    g_say[0] = 0;
}

static int __cdecl cmd_say(int one, MenuItem *it)
{
    (void)one, (void)it;
    g_say_typing = 0;
    input_set_typing(0);
    text_extras(NULL, chat_prompt_paint, lobby_chat_version);
    text_open("LOBBY CHAT", "Your message (ENTER sends)", g_say, HTNET_CHAT_LEN - 1, 0, on_say);
    return 0;
}

/* Typed characters into the field; 1 if it changed. */
static int say_keys(void)
{
    int changed = 0;
    size_t n = strlen(g_say);
    for (int c; (c = input_text_char()) != 0;) {
        if (c == '\r') {
            say_send(g_say);
            g_say[n = 0] = 0;
        } else if (c == '\b') {
            if (n)
                g_say[--n] = 0;
        } else if (allowed(c) && n < sizeof g_say - 1) {
            g_say[n++] = (char)c;
            g_say[n] = 0;
        } else {
            continue; /* Esc (the menu's back) and the like */
        }
        changed = 1;
    }
    return changed;
}

static int __cdecl cmd_room_leave(int one, MenuItem *it)
{
    (void)one, (void)it;
    lobby_leave();
    menusnd_play(0, KEY_TEST);
    g_room_exit = 1; /* switching handlers from inside auto_message isn't safe: the tick does it */
    return 0;
}

/* Item i is drawn on row 11 + i: blanks (filled in by menu_lobby_install) carry the chat's text
 * field down to ROOM_SAY_ROW, under the players and the chat. */
enum { RI_START = 0, RI_LEAVE = 2, RI_BLANKS = 5, RI_SAY = ROOM_SAY_ROW - 11 };
static MenuItem g_room_items[RI_SAY + 2] = {
    {IT_COMMAND, "START GAME", cmd_start, "Press TEST to start the game on every unit in the lobby"},
    {IT_BLANK},
    {IT_COMMAND, "LEAVE LOBBY", cmd_room_leave, "Press TEST to leave (NETWORK ADJUSTMENTS apply again)"},
    {IT_BLANK},
    {IT_BACK, NULL, NULL, NULL},
};
static Menu g_room = {g_room_title, g_room_items, 0, NULL};

/* Everything the page shows apart from the chat, for spotting changes. */
static void room_signature(char *out, size_t cap)
{
    char names[HTNET_MAX_UNITS][HTNET_NAME_LEN];
    unsigned mask = lobby_members(names);
    size_t used = (size_t)snprintf(out, cap, "%d;%d;%d;%x;", lobby_state(), lobby_owner(), lobby_max(), mask);
    for (int u = 0; u < HTNET_MAX_UNITS && used < cap; u++)
        if ((mask >> u) & 1)
            used += (size_t)snprintf(out + used, cap - used, "%s;", names[u]);
    if (used < cap)
        status_text(out + used, cap - used);
}

static void room_items(void)
{
    int in = lobby_active(NULL);
    int owner = lobby_is_owner();
    g_room_items[RI_SAY].type = in ? IT_COMMAND : IT_HEADING;
    /* START GAME is the owner's, and needs DIP switch 3 off like START THE GAME */
    g_room_items[RI_START].type = owner && !(dip_switches() & 4) ? IT_COMMAND : IT_HEADING;
    g_room_items[RI_START].text = !in || owner ? "START GAME" : "START GAME (the lobby's host starts it)";
    g_room_items[RI_LEAVE].type = in ? IT_COMMAND : IT_HEADING;
    size_t n = strlen(g_say);
    const char *shown = n > SAY_SHOWN ? g_say + n - SAY_SHOWN : g_say;
    snprintf(g_say_line, sizeof g_say_line, "> %s%s", shown, g_say_typing && n < sizeof g_say - 1 ? "_" : "");
    snprintf(g_room_title, sizeof g_room_title, "LOBBY %s", in ? lobby_name() : "");
}

static void room_paint_players(void)
{
    char names[HTNET_MAX_UNITS][HTNET_NAME_LEN];
    unsigned mask = lobby_members(names);
    int max = lobby_max(), self = -1, n = 0, owner = lobby_owner();
    if (!lobby_active(&self))
        return;
    for (int u = 0; u < HTNET_MAX_UNITS; u++)
        n += (mask >> u) & 1;
    if (max < 1 || max > HTNET_MAX_UNITS)
        max = HTNET_MAX_UNITS;
    snprintf(g_room_players, sizeof g_room_players, "PLAYERS %d/%d", n, max);
    vtext_set_string(4, ROOM_PLAYERS_ROW, C_GREEN, 0, g_room_players);
    for (int u = 0; u < max; u++) {
        char line[48];
        int col = max > 8 && u >= 8, row = ROOM_PLAYERS_ROW + 1 + (col ? u - 8 : u);
        int here = (mask >> u) & 1;
        snprintf(line, sizeof line, "%2d %s%s%s", u + 1, here ? names[u] : "-", here && u == owner ? " (HOST)" : "",
                 u == self ? " (YOU)" : "");
        vtext_set_string(4 + col * 30, row, !here ? C_DARK : u == self ? C_YELLOW : C_WHITE, 0, line);
    }
    const HtGameOpts *o = lobby_game_opts();
    char line[64];
    if (o)
        snprintf(line, sizeof line, "FREE PLAY %s  ALL BOATS %s  ALL TRACKS %s  DELAY %d",
                 o->freeplay ? "ON" : "OFF", o->allboats ? "ON" : "OFF", o->alltracks ? "ON" : "OFF",
                 lobby_input_delay());
    else
        snprintf(line, sizeof line, "INPUT DELAY %d", lobby_input_delay());
    vtext_set_string(4, ROOM_OPTS_ROW, C_GREY, 0, line);
}

static int __cdecl room_menu(int msg, int p)
{
    if (msg == MSG_ENTER || msg == MSG_PAINT)
        room_items();
    if (msg == MSG_ENTER) {
        g_room_exit = 0;
        g_room_pending = 0;
        g_room.cur = RI_SAY; /* the page opens on the chat field */
    } else if (msg == MSG_PAINT) {
        paint_background();
    }
    auto_message(&g_room, msg, p);
    if (msg == MSG_PAINT) {
        clear_line_range(ROOM_PLAYERS_ROW, ROOM_SAY_ROW - 1);
        room_paint_players();
        vtext_set_string(4, ROOM_CHAT_ROW, C_GREEN, 0, "CHAT");
        chat_paint(ROOM_CHAT_ROW + 1, ROOM_SAY_ROW - 1);
        g_room_chat_seen = lobby_chat_version();
        room_signature(g_room_sig, sizeof g_room_sig);
        status_paint();
        char addr[64], line[96];
        if (lobby_host_address(addr, sizeof addr)) {
            snprintf(line, sizeof line, "Others can DIRECT CONNECT to %s", addr);
            clear_line(k_note_row);
            vtext_set_string(4, k_note_row, C_GREY, 0, line);
        }
    } else if (msg == MSG_TICK) {
        char sig[sizeof g_room_sig];
        if (g_room_exit) {
            g_room_exit = 0;
            mess_sethandler(g_room.parent ? g_room.parent : lobby_menu);
            return 0;
        }
        int typing = lobby_active(NULL) && g_room.cur == RI_SAY, changed = 0;
        if (typing != g_say_typing) {
            g_say_typing = typing;
            input_set_typing(typing);
            changed = 1;
        }
        if (typing)
            changed |= say_keys();
        room_signature(sig, sizeof sig);
        if (changed || strcmp(sig, g_room_sig) || lobby_chat_version() != g_room_chat_seen)
            mess_queue_message(MSG_PAINT, 0);
    } else if (msg == MSG_LEAVE) {
        g_say_typing = 0;
        input_set_typing(0);
        status_clear();
        clear_line_range(ROOM_PLAYERS_ROW, 40);
    }
    return 0;
}

/* A page that started a create or join: once the lobby has taken us, show the room (its BACK goes
 * to NETWORK LOBBY). 1 if it switched. */
static int room_check(void)
{
    if (!g_room_pending)
        return 0;
    int st = lobby_state();
    if (st == LOBBY_IN) {
        g_room_pending = 0;
        g_room.parent = lobby_menu;
        mess_sethandler(room_menu);
        return 1;
    }
    if (st != LOBBY_BUSY && st != LOBBY_NEEDPASS)
        g_room_pending = 0;
    return 0;
}

/* ---------------------------------------------------------------- GAME OPTIONS (hosting) */

/* The operator adjustments a hosted lobby forces on every member, laid out like the originals
 * (GENERAL, SELECTION TIME and DIFFICULTY ADJUSTMENTS). They live in g_settings.net.lobby_opts
 * ([Lobby Options]) and go out with CREATE LOBBY; game/operator.c applies them at gameinit. */

#define OPTS_NOTE_ROW 42
#define OPTS (&g_settings.net.lobby_opts)

static HtGameOpts g_opts_def;
static FieldStr g_offon[] = {{"OFF", 0}, {"ON", 1}, {NULL, 0}};
static FieldIntDesc g_time_desc = {10, 60, 1, 0, "%-2i", NULL};
static FieldIntDesc g_chkpnt_desc = {1, 100, 1, -1, "%-2i", NULL}; /* -1: AI shares its row */
static FieldIntDesc g_ai_desc = {1, 100, 1, 0, "%-2i", NULL};
static FieldIntDesc g_limit_desc = {5, 50, 5, 0, "%i%%", NULL};

/* The difficulty menu's tracks and labels, in its order */
static const struct {
    int track;
    const char *label;
} k_diff_tracks[] = {
    {0, "GRAVEYARD   CHKPNT"}, {2, "VENICE      CHKPNT"}, {4, "ARCTIC      CHKPNT"}, {6, "NY          CHKPNT"},
    {8, "CHINA       CHKPNT"},  {1, "AMAZON      CHKPNT"}, {3, "POWELL      CHKPNT"}, {5, "NILE        CHKPNT"},
    {7, "GREECE      CHKPNT"}, {10, "LOOP1       CHKPNT"}, {11, "LOOP2       CHKPNT"},
};
#define DIFF_TRACKS ((int)(sizeof k_diff_tracks / sizeof *k_diff_tracks))

static MenuItem g_gen_items[8], g_time_items[12], g_diff_items[2 * DIFF_TRACKS + 9];

static MenuItem field_list(const char *label, int *value, int *def)
{
    return (MenuItem){IT_FIELD, label, (const void *)HY_FN_fields_stringlist, STR(Mt_enter_to_change),
                      g_offon, value, def};
}

static MenuItem field_int(const char *label, FieldIntDesc *desc, int *value, int *def)
{
    return (MenuItem){IT_FIELD, label, (const void *)HY_FN_fields_int, STR(Mt_enter_to_change), desc, value, def};
}

static MenuItem item_back(void)
{
    return (MenuItem){IT_BACK, STR(Mt_return_to_previous), NULL, STR(Mt_test_to_return)};
}

static int __cdecl cmd_reset_times(int one, MenuItem *it)
{
    (void)one, (void)it;
    OPTS->t_track = g_opts_def.t_track;
    OPTS->t_boat = g_opts_def.t_boat;
    OPTS->t_hiscore = g_opts_def.t_hiscore;
    OPTS->t_continue = g_opts_def.t_continue;
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

/* arg10: 0 = track (checkpoint) difficulties, 1 = AI */
static int __cdecl cmd_reset_diff(int one, MenuItem *it)
{
    (void)one;
    if (it->arg10)
        memcpy(OPTS->ai_diff, g_opts_def.ai_diff, sizeof OPTS->ai_diff);
    else
        memcpy(OPTS->track_diff, g_opts_def.track_diff, sizeof OPTS->track_diff);
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

static void opts_build(void)
{
    ht_gameopts_defaults(&g_opts_def);
    int n = 0;
    g_gen_items[n++] = field_list("FREE PLAY", &OPTS->freeplay, &g_opts_def.freeplay);
    g_gen_items[n++] = (MenuItem){IT_BLANK};
    g_gen_items[n++] = field_list("ENABLE ALL BOATS", &OPTS->allboats, &g_opts_def.allboats);
    g_gen_items[n++] = (MenuItem){IT_BLANK};
    g_gen_items[n++] = field_list("UNLOCK ALL TRACKS", &OPTS->alltracks, &g_opts_def.alltracks);
    g_gen_items[n++] = (MenuItem){IT_BLANK};
    g_gen_items[n++] = item_back();
    g_gen_items[n] = (MenuItem){IT_END};

    n = 0;
    g_time_items[n++] = field_int("TRACK SELECT TIME", &g_time_desc, &OPTS->t_track, &g_opts_def.t_track);
    g_time_items[n++] = (MenuItem){IT_BLANK};
    g_time_items[n++] = field_int("BOAT SELECT TIME", &g_time_desc, &OPTS->t_boat, &g_opts_def.t_boat);
    g_time_items[n++] = (MenuItem){IT_BLANK};
    g_time_items[n++] = field_int("HIGH SCORE TIME", &g_time_desc, &OPTS->t_hiscore, &g_opts_def.t_hiscore);
    g_time_items[n++] = (MenuItem){IT_BLANK};
    g_time_items[n++] = field_int("CONTINUE TIME", &g_time_desc, &OPTS->t_continue, &g_opts_def.t_continue);
    g_time_items[n++] = (MenuItem){IT_BLANK};
    g_time_items[n++] = (MenuItem){IT_COMMAND, "RESET TIMES TO DEFAULTS", cmd_reset_times, STR(Mt_enter_to_execute)};
    g_time_items[n++] = (MenuItem){IT_BLANK};
    g_time_items[n++] = item_back();
    g_time_items[n] = (MenuItem){IT_END};

    n = 0;
    g_diff_items[n++] = (MenuItem){IT_HEADING, "                 TRACK DIFFICULTY    AI DIFFICULTY"};
    for (int i = 0; i < DIFF_TRACKS; i++) {
        int t = k_diff_tracks[i].track;
        g_diff_items[n++] = field_int(k_diff_tracks[i].label, &g_chkpnt_desc, &OPTS->track_diff[t], &g_opts_def.track_diff[t]);
        g_diff_items[n++] = field_int("                AI", &g_ai_desc, &OPTS->ai_diff[t], &g_opts_def.ai_diff[t]);
    }
    g_diff_items[n++] = field_list("FREE RACE LIMITER", &OPTS->limiter, &g_opts_def.limiter);
    g_diff_items[n++] = field_int("LIMIT FREE RACES TO ", &g_limit_desc, &OPTS->limit_pct, &g_opts_def.limit_pct);
    g_diff_items[n++] = (MenuItem){IT_COMMAND, "RESET TRACKS TO DEFAULTS", cmd_reset_diff, STR(Mt_enter_to_execute), (void *)0};
    g_diff_items[n++] = (MenuItem){IT_COMMAND, "RESET AI TO DEFAULTS", cmd_reset_diff, STR(Mt_enter_to_execute), (void *)1};
    g_diff_items[n++] = (MenuItem){IT_BLANK};
    g_diff_items[n++] = item_back();
    g_diff_items[n] = (MenuItem){IT_END};
}

static Menu g_gen = {"GENERAL ADJUSTMENTS", g_gen_items, 0, NULL};
static Menu g_times = {"SELECTION TIME ADJUSTMENTS", g_time_items, 0, NULL};
static Menu g_diff = {"DIFFICULTY ADJUSTMENTS", g_diff_items, 0, NULL};

/* One options page: `col` is where values are drawn, `hint` goes on the note row. */
static int opts_page(Menu *m, int col, const char *hint, int msg, int p)
{
    if (msg == MSG_ENTER)
        Fields_col = col;
    else if (msg == MSG_PAINT)
        paint_background();
    auto_message(m, msg, p);
    if (msg == MSG_PAINT) {
        vtext_set_string(4, OPTS_NOTE_ROW, C_GREY, 0, "Every player in the lobby races with these;");
        vtext_set_string(4, OPTS_NOTE_ROW + 1, C_GREY, 0, "no one's own adjustments change");
        if (hint)
            vtext_set_string(4, OPTS_NOTE_ROW + 2, C_YELLOW, 0, hint);
    } else if (msg == MSG_LEAVE) {
        clear_line_range(OPTS_NOTE_ROW, OPTS_NOTE_ROW + 2);
        Fields_col = 0x14;
        if (p == 0) {
            ht_gameopts_clamp(OPTS);
            settings_save();
        }
    }
    return 0;
}

static int __cdecl gen_menu(int msg, int p) { return opts_page(&g_gen, 22, NULL, msg, p); }
static int __cdecl times_menu(int msg, int p) { return opts_page(&g_times, 22, "Times in seconds", msg, p); }
static int __cdecl diff_menu(int msg, int p)
{
    return opts_page(&g_diff, 0x18, "Difficulties: 1 = EASIEST, 100 = HARDEST", msg, p);
}
static MenuItem g_opts_items[] = {
    {IT_SUBMENU, "GENERAL", gen_menu, "Free play, all boats, all tracks"},
    {IT_BLANK},
    {IT_SUBMENU, "SELECTION TIMES", times_menu, "Track select, boat select, high score and continue times"},
    {IT_BLANK},
    {IT_SUBMENU, "DIFFICULTY", diff_menu, "Checkpoint and AI difficulty per track, free race limiter"},
    {IT_BLANK},
    {IT_BACK, NULL, NULL, NULL},
    {IT_END},
};
static Menu g_opts = {"LOBBY GAME OPTIONS", g_opts_items, 0, NULL};

static int __cdecl opts_menu(int msg, int p) { return opts_page(&g_opts, 22, NULL, msg, p); }

/* ---------------------------------------------------------------- HOST (internet / LAN) */

static int g_host_lan;
static char g_txt_lname[64], g_txt_pw[64];
static FieldIntDesc g_max_desc = {2, LOBBY_GAME_UNITS, 1, 0, "%i", NULL};
static int g_max_def = 4;
static FieldStr g_delay_list[] = {{"AUTO", 0}, {"1 FRAME", 1}, {"2 FRAMES", 2}, {"3 FRAMES", 3},
                                  {"4 FRAMES", 4}, {"5 FRAMES", 5}, {"6 FRAMES", 6}, {NULL, 0}};
static int g_delay_def = 0;

static void on_lobby_name(const char *t)
{
    if (*t) {
        snprintf(g_settings.net.lobby_name, sizeof g_settings.net.lobby_name, "%s", t);
        settings_save();
    }
}

static void on_password(const char *t)
{
    snprintf(g_settings.net.lobby_password, sizeof g_settings.net.lobby_password, "%s", t);
    settings_save();
}

static int __cdecl cmd_lobby_name(int one, MenuItem *it)
{
    (void)one, (void)it;
    text_open("LOBBY NAME", "Name of the lobby", g_settings.net.lobby_name, HTNET_LOBBY_LEN - 1, 0, on_lobby_name);
    return 0;
}

static int __cdecl cmd_password(int one, MenuItem *it)
{
    (void)one, (void)it;
    text_open("LOBBY PASSWORD", "Password to join (leave empty for none)", g_settings.net.lobby_password,
              sizeof g_settings.net.lobby_password - 1, 0, on_password);
    return 0;
}

static int __cdecl cmd_create(int one, MenuItem *it)
{
    (void)one, (void)it;
    if (g_settings.net.lobby_max > LOBBY_GAME_UNITS)
        g_settings.net.lobby_max = LOBBY_GAME_UNITS;
    settings_save();
    lobby_create(g_host_lan, g_settings.net.lobby_name, g_settings.net.lobby_max, g_settings.net.lobby_password,
                 g_settings.net.lobby_delay);
    g_room_pending = 1;
    menusnd_play(0, KEY_TEST);
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

static MenuItem g_host_items[] = {
    {IT_COMMAND, g_txt_lname, cmd_lobby_name, "Press TEST to type the lobby's name"},
    {IT_BLANK},
    {IT_FIELD, "MAX PLAYERS", NULL, "Most players the lobby lets in", &g_max_desc, &g_settings.net.lobby_max,
     &g_max_def},
    {IT_BLANK},
    {IT_COMMAND, g_txt_pw, cmd_password, "Press TEST to set a password (empty = open lobby)"},
    {IT_BLANK},
    {IT_FIELD, "INPUT DELAY", NULL, "More frames hide more latency. AUTO: 1 on a LAN, 3 online", g_delay_list,
     &g_settings.net.lobby_delay, &g_delay_def},
    {IT_BLANK},
    {IT_SUBMENU, "GAME OPTIONS", opts_menu, "Free play, unlocks, selection times and difficulty for every player"},
    {IT_BLANK},
    {IT_COMMAND, "CREATE LOBBY", cmd_create, "Press TEST to create the lobby and join it as player 1"},
    {IT_BLANK},
    {IT_BACK, NULL, NULL, NULL},
    {IT_END},
};
static Menu g_host = {"HOST LOBBY", g_host_items, 0, NULL};

static int host_menu(int lan, int msg, int p)
{
    if (msg == MSG_ENTER) {
        g_host_lan = lan;
        g_host.title = lan ? "HOST LAN LOBBY" : "HOST INTERNET LOBBY";
        Fields_col = 22;
    } else if (msg == MSG_PAINT) {
        snprintf(g_txt_lname, sizeof g_txt_lname, "LOBBY NAME      %s", g_settings.net.lobby_name);
        snprintf(g_txt_pw, sizeof g_txt_pw, "PASSWORD        %s", g_settings.net.lobby_password[0] ? "(set)" : "(none)");
        paint_background();
    }
    auto_message(&g_host, msg, p);
    if (msg == MSG_PAINT) {
        status_paint();
        if (!lan && !g_settings.net.master[0])
            vtext_set_string(4, STATUS_ROW + 1, C_YELLOW, 0, "Set the MASTER SERVER first (PC SETTINGS > NETWORK)");
    } else if (msg == MSG_TICK) {
        if (room_check())
            return 0;
        if (status_changed())
            mess_queue_message(MSG_PAINT, 0);
    } else if (msg == MSG_LEAVE) {
        status_clear();
        clear_line(STATUS_ROW + 1);
        Fields_col = 0x14;
        settings_save();
    }
    return 0;
}

static int __cdecl host_net_menu(int msg, int p) { return host_menu(0, msg, p); }
static int __cdecl host_lan_menu(int msg, int p) { return host_menu(1, msg, p); }

/* ---------------------------------------------------------------- FIND LOBBIES */

#define BROWSE_SHOWN 16

static char g_browse_text[BROWSE_SHOWN][64];
static MenuItem g_browse_items[BROWSE_SHOWN + 8];
static Menu g_browse = {"FIND LOBBIES", g_browse_items, 0, NULL};
static char g_browse_sig[512];
static LobbyEntry g_join_entry;
static ULONGLONG g_browse_next;

static void on_join_password(const char *t)
{
    lobby_join(&g_join_entry, t);
    g_room_pending = 1;
}

static int __cdecl cmd_join(int one, MenuItem *it)
{
    (void)one;
    const LobbyEntry *list;
    int n = lobby_list(&list);
    int i = (int)(intptr_t)it->arg10;
    if (i >= n)
        return 0;
    g_join_entry = list[i];
    if (g_join_entry.needpass) {
        text_open("JOIN LOBBY", "This lobby needs a password", "", 31, 1, on_join_password);
    } else {
        lobby_join(&g_join_entry, "");
        g_room_pending = 1;
        menusnd_play(0, KEY_TEST);
        mess_queue_message(MSG_PAINT, 0);
    }
    return 0;
}

static int __cdecl cmd_refresh(int one, MenuItem *it)
{
    (void)one, (void)it;
    lobby_browse(1);
    g_browse_next = GetTickCount64() + 3000;
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

/* The list's text, for spotting changes. */
static void browse_signature(char *out, size_t cap)
{
    const LobbyEntry *list;
    int n = lobby_list(&list);
    size_t used = (size_t)snprintf(out, cap, "%d;", n);
    for (int i = 0; i < n && i < BROWSE_SHOWN && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%u,%d,%d;", list[i].id, list[i].clients, list[i].max);
}

static void browse_build(void)
{
    const LobbyEntry *list;
    int n = lobby_list(&list);
    int k = 0;
    g_browse_items[k++] = (MenuItem){IT_COMMAND, "REFRESH", cmd_refresh, "Press TEST to look for lobbies again"};
    g_browse_items[k++] = (MenuItem){IT_BLANK};
    for (int i = 0; i < n && i < BROWSE_SHOWN; i++) {
        const LobbyEntry *e = &list[i];
        snprintf(g_browse_text[i], sizeof g_browse_text[i], "%-23s %d/%d %s%s", e->name, e->clients, e->max,
                 e->lan ? "LAN" : "NET", e->needpass ? " PASSWORD" : "");
        g_browse_items[k++] = (MenuItem){IT_COMMAND, g_browse_text[i], cmd_join,
                                         e->clients >= e->max ? "That lobby is full" : "Press TEST to join",
                                         (const void *)(intptr_t)i};
    }
    if (!n)
        g_browse_items[k++] = (MenuItem){IT_HEADING, "No lobbies found (yet)"};
    g_browse_items[k++] = (MenuItem){IT_BLANK};
    g_browse_items[k++] = (MenuItem){IT_BACK, STR(Mt_return_to_previous), NULL, STR(Mt_test_to_return)};
    g_browse_items[k] = (MenuItem){IT_END};
    browse_signature(g_browse_sig, sizeof g_browse_sig);
}

static int __cdecl browse_menu(int msg, int p)
{
    if (msg == MSG_ENTER) {
        if (p) {
            lobby_browse(1);
            g_browse.cur = 0;
        }
        g_browse_next = GetTickCount64() + 3000;
        Fields_col = 22;
        browse_build();
    } else if (msg == MSG_PAINT) {
        browse_build();
        paint_background();
    }
    auto_message(&g_browse, msg, p);
    if (msg == MSG_PAINT) {
        status_paint();
    } else if (msg == MSG_TICK) {
        char sig[sizeof g_browse_sig];
        if (room_check())
            return 0;
        if (GetTickCount64() >= g_browse_next) {
            lobby_browse(0); /* ask again; answers update the list in place */
            g_browse_next = GetTickCount64() + 3000;
        }
        browse_signature(sig, sizeof sig);
        if (strcmp(sig, g_browse_sig) || status_changed())
            mess_queue_message(MSG_PAINT, 0);
    } else if (msg == MSG_LEAVE) {
        status_clear();
        Fields_col = 0x14;
    }
    return 0;
}

/* ---------------------------------------------------------------- NETWORK LOBBY */

static char g_txt_player[64];

static void on_player_name(const char *t)
{
    if (*t) {
        snprintf(g_settings.net.player_name, sizeof g_settings.net.player_name, "%s", t);
        settings_save();
        lobby_name_changed();
    }
}

static void on_master(const char *t)
{
    /* empty: back to the default */
    snprintf(g_settings.net.master, sizeof g_settings.net.master, "%s", *t ? t : NET_DEFAULT_MASTER);
    settings_save();
}

static int __cdecl cmd_player_name(int one, MenuItem *it)
{
    (void)one, (void)it;
    text_open("PLAYER NAME", "Your name in lobbies", g_settings.net.player_name, HTNET_NAME_LEN - 1, 0,
              on_player_name);
    return 0;
}

/* PC SETTINGS > NETWORK's MASTER SERVER item (menu_pc.c): the prompt lives here. */
int __cdecl lobby_cmd_master(int one, MenuItem *it)
{
    (void)one, (void)it;
    text_open("MASTER SERVER", "Server address, optionally with :port; empty = default",
              g_settings.net.master, sizeof g_settings.net.master - 1, 0, on_master);
    return 0;
}

/* DIRECT CONNECT: to a PC hosting a lobby (HOST LAN LOBBY), by address */
static void on_direct(const char *t)
{
    if (!*t)
        return;
    snprintf(g_settings.net.direct, sizeof g_settings.net.direct, "%s", t);
    settings_save();
    lobby_connect(t);
    g_room_pending = 1;
}

static int __cdecl cmd_direct(int one, MenuItem *it)
{
    (void)one, (void)it;
    text_open("DIRECT CONNECT", "Address of the hosting PC, optionally with :port", g_settings.net.direct,
              sizeof g_settings.net.direct - 1, 0, on_direct);
    return 0;
}

static void on_direct_password(const char *t)
{
    LobbyEntry e = *lobby_found();
    lobby_join(&e, t);
    g_room_pending = 1;
}

static void on_direct_cancel(void)
{
    lobby_leave();
}

static int __cdecl cmd_leave(int one, MenuItem *it)
{
    (void)one, (void)it;
    lobby_leave();
    menusnd_play(0, KEY_TEST);
    mess_queue_message(MSG_PAINT, 0);
    return 0;
}

enum { LI_ROOM = 10, LI_LEAVE = 12 };
static MenuItem g_lobby_items[] = {
    {IT_COMMAND, g_txt_player, cmd_player_name, "Press TEST to type your name"},
    {IT_BLANK},
    {IT_SUBMENU, "HOST INTERNET LOBBY", host_net_menu, "Create a lobby on the master server"},
    {IT_BLANK},
    {IT_SUBMENU, "HOST LAN LOBBY", host_lan_menu, "Create a lobby on this PC (LAN, or DIRECT CONNECT)"},
    {IT_BLANK},
    {IT_SUBMENU, "FIND LOBBIES", browse_menu, "LAN and internet lobbies to join"},
    {IT_BLANK},
    {IT_COMMAND, "DIRECT CONNECT", cmd_direct, "Press TEST to join a PC's lobby by address (UDP 27960)"},
    {IT_BLANK},
    {IT_SUBMENU, "LOBBY", room_menu, "Your lobby: players, chat, START GAME"},
    {IT_BLANK},
    {IT_COMMAND, "LEAVE LOBBY", cmd_leave, "Press TEST to leave (NETWORK ADJUSTMENTS apply again)"},
    {IT_BLANK},
    {IT_BACK, NULL, NULL, NULL},
    {IT_END},
};
static Menu g_lobby = {"NETWORK LOBBY", g_lobby_items, 0, NULL};

int __cdecl lobby_menu(int msg, int p)
{
    if (msg == MSG_ENTER || msg == MSG_PAINT) {
        snprintf(g_txt_player, sizeof g_txt_player, "PLAYER NAME     %s", g_settings.net.player_name);
        /* not in a lobby: LOBBY and LEAVE turn into grey headings */
        int in = lobby_active(NULL);
        g_lobby_items[LI_ROOM].type = in ? IT_SUBMENU : IT_HEADING;
        g_lobby_items[LI_LEAVE].type = in ? IT_COMMAND : IT_HEADING;
    }
    if (msg == MSG_PAINT)
        paint_background();
    auto_message(&g_lobby, msg, p);
    if (msg == MSG_PAINT) {
        status_paint();
    } else if (msg == MSG_TICK) {
        if (room_check())
            return 0;
        if (lobby_state() == LOBBY_NEEDPASS) {
            /* DIRECT CONNECT found a lobby with a password */
            text_extras(on_direct_cancel, NULL, NULL);
            text_open("DIRECT CONNECT", "This lobby needs a password", "", 31, 1, on_direct_password);
            return 0;
        }
        if (status_changed())
            mess_queue_message(MSG_PAINT, 0);
    } else if (msg == MSG_LEAVE) {
        status_clear();
    }
    return 0;
}

void menu_lobby_install(void)
{
    opts_build();
    for (MenuItem *it = g_opts_items; it->type != IT_END; it++)
        if (it->type == IT_BACK) {
            it->text = STR(Mt_return_to_previous);
            it->help = STR(Mt_test_to_return);
        }
    for (int i = RI_BLANKS; i < RI_SAY; i++)
        g_room_items[i] = (MenuItem){IT_BLANK};
    g_room_items[RI_SAY] =
        (MenuItem){IT_COMMAND, g_say_line, cmd_say, "Type a message, ENTER sends (pad: A to spell one)"};
    for (MenuItem *it = g_room_items; it->type != IT_END; it++)
        if (it->type == IT_BACK) {
            it->text = STR(Mt_return_to_previous);
            it->help = STR(Mt_test_to_return);
        }
    for (MenuItem *it = g_lobby_items; it->type != IT_END; it++)
        if (it->type == IT_BACK) {
            it->text = STR(Mt_return_to_previous);
            it->help = STR(Mt_test_to_return);
        }
    for (MenuItem *it = g_host_items; it->type != IT_END; it++) {
        if (it->type == IT_BACK) {
            it->text = STR(Mt_return_to_previous);
            it->help = STR(Mt_test_to_return);
        }
        if (it->type == IT_FIELD)
            it->handler = it->arg10 == g_delay_list ? (const void *)HY_FN_fields_stringlist
                                                    : (const void *)HY_FN_fields_int;
    }
}
