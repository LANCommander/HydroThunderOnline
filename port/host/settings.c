/* Port settings: hydro.ini <-> g_settings, and the tables the PC SETTINGS menu is built from. */
#include "settings.h"
#include "host.h"

#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;

/* [Lobby Options]: the game options a hosted lobby gets (lobby_create). */
static const struct {
    const char *key;
    size_t off;
} k_lobby_opt_keys[] = {
    {"free_play", offsetof(HtGameOpts, freeplay)},
    {"enable_all_boats", offsetof(HtGameOpts, allboats)},
    {"unlock_all_tracks", offsetof(HtGameOpts, alltracks)},
    {"track_select_time", offsetof(HtGameOpts, t_track)},
    {"boat_select_time", offsetof(HtGameOpts, t_boat)},
    {"high_score_time", offsetof(HtGameOpts, t_hiscore)},
    {"continue_time", offsetof(HtGameOpts, t_continue)},
    {"free_race_limiter", offsetof(HtGameOpts, limiter)},
    {"limit_free_races_to", offsetof(HtGameOpts, limit_pct)},
};

/* Per track, in the game's track order: "40,40,..." */
static void load_track_list(const char *key, int *v, const char *ini)
{
    char buf[128];
    GetPrivateProfileStringA("Lobby Options", key, "", buf, sizeof buf, ini);
    char *p = buf;
    for (int t = 0; t < HTNET_TRACKS && *p; t++) {
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p)
            break;
        v[t] = (int)x;
        p = *end == ',' ? end + 1 : end;
    }
}

static void save_track_list(const char *key, const int *v, const char *ini)
{
    char buf[128];
    size_t len = 0;
    for (int t = 0; t < HTNET_TRACKS; t++)
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%d", t ? "," : "", v[t]);
    WritePrivateProfileStringA("Lobby Options", key, buf, ini);
}

static void load_lobby_opts(HtGameOpts *o, const char *ini)
{
    ht_gameopts_defaults(o);
    for (size_t i = 0; i < sizeof k_lobby_opt_keys / sizeof *k_lobby_opt_keys; i++) {
        int *v = (int *)((char *)o + k_lobby_opt_keys[i].off);
        *v = GetPrivateProfileIntA("Lobby Options", k_lobby_opt_keys[i].key, *v, ini);
    }
    load_track_list("track_difficulty", o->track_diff, ini);
    load_track_list("ai_difficulty", o->ai_diff, ini);
    ht_gameopts_clamp(o);
}

static void save_lobby_opts(const HtGameOpts *o, const char *ini)
{
    char buf[16];
    for (size_t i = 0; i < sizeof k_lobby_opt_keys / sizeof *k_lobby_opt_keys; i++) {
        snprintf(buf, sizeof buf, "%d", *(const int *)((const char *)o + k_lobby_opt_keys[i].off));
        WritePrivateProfileStringA("Lobby Options", k_lobby_opt_keys[i].key, buf, ini);
    }
    save_track_list("track_difficulty", o->track_diff, ini);
    save_track_list("ai_difficulty", o->ai_diff, ini);
}

/* ---------------------------------------------------------------- option tables */

static const char *const k_onoff[] = {"OFF", "ON"};
static const char *const k_display[] = {"FULLSCREEN", "WINDOWED"};
static const char *const k_res[DGV_RES_COUNT] = {"GAME 640X480", "2X", "3X", "4X", "5X", "6X",
                                                 "MAX", "MAX INTEGER", "DESKTOP"};
static const char *const k_scale[DGV_SCALE_COUNT] = {"STRETCHED", "KEEP ASPECT", "4:3",
                                                     "CENTERED", "CENTER+ASPECT", "4:3 CRT-LIKE"};
static const char *const k_msaa[] = {"OFF", "2X", "4X", "8X"};
static const int k_msaa_val[] = {0, 2, 4, 8};
static const char *const k_filter[] = {"GAME", "POINT", "BILINEAR"};
static const char *const k_resample[] = {"POINT", "BILINEAR", "BICUBIC", "LANCZOS-2", "LANCZOS-3"};
static const char *const k_stick[] = {"LEFT STICK", "RIGHT STICK"};
static const char *const k_throttle[] = {"TRIGGERS", "RIGHT STICK", "LEFT STICK", "BUTTONS"};
static const char *const k_keysteer[] = {"INSTANT", "FAST", "MEDIUM", "SLOW"};
static const char *const k_audio_out[] = {"MIXED", "CABINET L/R"};
static const char *const k_crossover[] = {"OFF", "80 HZ", "120 HZ", "200 HZ"};
static const int k_crossover_val[] = {0, 80, 120, 200};

#define LIST(names) names, NULL, (int)(sizeof names / sizeof names[0]), 0, 0, 0
#define LISTV(names, vals) names, vals, (int)(sizeof names / sizeof names[0]), 0, 0, 0
#define RANGE(lo, hi, st) NULL, NULL, 0, lo, hi, st

const SettingDesc g_setting_desc[] = {
    {"Graphics", "display", "DISPLAY", &g_settings.gfx.windowed, LIST(k_display), 0},
    {"Graphics", "resolution", "RESOLUTION", &g_settings.gfx.resolution, LIST(k_res), DGV_RES_DESKTOP},
    {"Graphics", "scaling", "SCALING", &g_settings.gfx.scaling, LIST(k_scale), DGV_SCALE_CENTERED_ASPECT},
    {"Graphics", "antialiasing", "ANTI-ALIASING", &g_settings.gfx.msaa, LISTV(k_msaa, k_msaa_val), 0},
    {"Graphics", "texture_filter", "TEXTURE FILTER", &g_settings.gfx.filter, LIST(k_filter), 0},
    {"Graphics", "resampling", "RESAMPLING", &g_settings.gfx.resampling, LIST(k_resample), 1},
    {"Graphics", "vsync", "VSYNC", &g_settings.gfx.vsync, LIST(k_onoff), 1},
    {"Graphics", "brightness", "BRIGHTNESS", &g_settings.gfx.brightness, RANGE(50, 200, 10), 100},
    {"Graphics", "contrast", "CONTRAST", &g_settings.gfx.contrast, RANGE(50, 200, 10), 100},
    {"Graphics", "color", "COLOR", &g_settings.gfx.color, RANGE(0, 200, 10), 100},

    {"Audio", "output", "OUTPUT", &g_settings.audio.output, LIST(k_audio_out), 0},
    {"Audio", "headrest", "HEADREST LEVEL", &g_settings.audio.headrest, RANGE(0, 150, 10), 100},
    {"Audio", "subwoofer", "SUBWOOFER LEVEL", &g_settings.audio.subwoofer, RANGE(0, 150, 10), 40},
    {"Audio", "crossover", "SUBWOOFER FILTER", &g_settings.audio.crossover, LISTV(k_crossover, k_crossover_val), 0},

    {"Controls", "gamepad", "GAMEPAD", &g_settings.ctl.gamepad, LIST(k_onoff), 1},
    {"Controls", "steer_stick", "PAD STEERING", &g_settings.ctl.steer_stick, LIST(k_stick), 0},
    {"Controls", "throttle", "PAD THROTTLE", &g_settings.ctl.throttle_source, LIST(k_throttle), 0},
    {"Controls", "deadzone", "STICK DEAD ZONE", &g_settings.ctl.deadzone, RANGE(0, 50, 2), 24},
    {"Controls", "sensitivity", "STICK SENSITIVITY", &g_settings.ctl.sensitivity, RANGE(50, 200, 10), 100},
    {"Controls", "key_steering", "KEYBOARD STEERING", &g_settings.ctl.key_steer, LIST(k_keysteer), 0},
    {"Controls", "menu_assist", "MENU ASSIST", &g_settings.ctl.menu_assist, LIST(k_onoff), 1},

    {"Network", "lan_broadcast", "LAN BROADCAST", &g_settings.net.lan_broadcast, LIST(k_onoff), 1},
    {"Network", "local_instances", "SAME-PC UNITS", &g_settings.net.local_instances, LIST(k_onoff), 1},
};
const int g_setting_count = sizeof g_setting_desc / sizeof g_setting_desc[0];

/* Index into the list for the stored value (lists with explicit values map back through them). */
int setting_index(const SettingDesc *d)
{
    int v = *d->value;
    if (!d->count)
        return v;
    if (d->values) {
        for (int i = 0; i < d->count; i++)
            if (d->values[i] == v)
                return i;
        return 0;
    }
    return v >= 0 && v < d->count ? v : 0;
}

void setting_set_index(const SettingDesc *d, int i)
{
    if (!d->count) {
        *d->value = i < d->min ? d->min : i > d->max ? d->max : i;
        return;
    }
    i = i < 0 ? 0 : i >= d->count ? d->count - 1 : i;
    *d->value = d->values ? d->values[i] : i;
}

/* ---------------------------------------------------------------- names */

static const char *const k_actions[ACT_COUNT] = {
    "STEER LEFT", "STEER RIGHT", "THROTTLE UP", "THROTTLE DOWN", "BOOST / START", "PILOT VIEW",
    "LOW VIEW", "HIGH VIEW", "COIN", "SERVICE CREDIT", "TEST (MENU)", "VOLUME DOWN", "VOLUME UP",
};
static const char *const k_action_keys[ACT_COUNT] = {
    "steer_left", "steer_right", "throttle_up", "throttle_down", "boost", "view_pilot", "view_low",
    "view_high", "coin", "service", "test", "volume_down", "volume_up",
};

const char *action_name(int act)
{
    return act >= 0 && act < ACT_COUNT ? k_actions[act] : "?";
}

int action_from_key(const char *ini_key)
{
    for (int a = 0; a < ACT_COUNT; a++)
        if (!_stricmp(k_action_keys[a], ini_key))
            return a;
    return -1;
}

static const char *const k_pad[PAD_BUTTON_COUNT] = {
    "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT", "START", "BACK", "LS", "RS",
    "LB", "RB", NULL, NULL, "A", "B", "X", "Y", "LT", "RT",
};

const char *pad_name(int btn)
{
    return btn >= 0 && btn < PAD_BUTTON_COUNT && k_pad[btn] ? k_pad[btn] : "";
}

static const struct {
    int vk;
    const char *name;
} k_keys[] = {
    {VK_BACK, "BACKSPACE"}, {VK_TAB, "TAB"},         {VK_RETURN, "ENTER"},      {VK_PAUSE, "PAUSE"},
    {VK_CAPITAL, "CAPS"},   {VK_SPACE, "SPACE"},     {VK_PRIOR, "PAGE UP"},     {VK_NEXT, "PAGE DOWN"},
    {VK_END, "END"},        {VK_HOME, "HOME"},       {VK_LEFT, "LEFT"},         {VK_UP, "UP"},
    {VK_RIGHT, "RIGHT"},    {VK_DOWN, "DOWN"},       {VK_INSERT, "INSERT"},     {VK_DELETE, "DELETE"},
    {VK_LSHIFT, "LSHIFT"},  {VK_RSHIFT, "RSHIFT"},   {VK_LCONTROL, "LCTRL"},    {VK_RCONTROL, "RCTRL"},
    {VK_LMENU, "LALT"},     {VK_RMENU, "RALT"},      {VK_MULTIPLY, "NUM *"},    {VK_ADD, "NUM +"},
    {VK_SUBTRACT, "NUM -"}, {VK_DECIMAL, "NUM ."},   {VK_DIVIDE, "NUM /"},      {VK_OEM_1, ";"},
    {VK_OEM_PLUS, "="},     {VK_OEM_COMMA, ","},     {VK_OEM_MINUS, "-"},       {VK_OEM_PERIOD, "."},
    {VK_OEM_2, "/"},        {VK_OEM_3, "`"},         {VK_OEM_4, "["},           {VK_OEM_5, "\\"},
    {VK_OEM_6, "]"},        {VK_OEM_7, "'"},
};

const char *key_name(int vk)
{
    static char buf[8][12];
    static int n;
    if (!vk)
        return "";
    for (unsigned i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].vk == vk)
            return k_keys[i].name;
    char *b = buf[n++ & 7];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        snprintf(b, 12, "%c", vk);
    else if (vk >= VK_F1 && vk <= VK_F24)
        snprintf(b, 12, "F%d", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        snprintf(b, 12, "NUM %d", vk - VK_NUMPAD0);
    else
        snprintf(b, 12, "KEY %02X", vk);
    return b;
}

static int key_from_name(const char *s)
{
    for (int vk = 1; vk < 256; vk++) {
        const char *n = key_name(vk);
        if (*n && !_stricmp(n, s) && strncmp(n, "KEY ", 4))
            return vk;
    }
    unsigned v;
    if (!_strnicmp(s, "KEY ", 4) && sscanf(s + 4, "%x", &v) == 1 && v < 256)
        return (int)v;
    return 0;
}

static int pad_from_name(const char *s)
{
    for (int b = 0; b < PAD_BUTTON_COUNT; b++)
        if (k_pad[b] && !_stricmp(k_pad[b], s))
            return b;
    return PAD_NONE;
}

/* ---------------------------------------------------------------- defaults */

void settings_default_bindings(Controls *c)
{
    static const int keys[ACT_COUNT][BIND_SLOTS] = {
        {VK_LEFT}, {VK_RIGHT}, {VK_UP}, {VK_DOWN}, {VK_SPACE}, {'1'}, {'2'},
        {'3'}, {'5'}, {VK_F1}, {VK_F2}, {VK_OEM_MINUS}, {VK_OEM_PLUS},
    };
    static const int pads[ACT_COUNT][BIND_SLOTS] = {
        {PAD_DPAD_LEFT, PAD_NONE}, {PAD_DPAD_RIGHT, PAD_NONE}, {PAD_DPAD_UP, PAD_NONE},
        {PAD_DPAD_DOWN, PAD_NONE}, {PAD_A, PAD_START},         {PAD_B, PAD_NONE},
        {PAD_X, PAD_NONE},         {PAD_Y, PAD_NONE},          {PAD_BACK, PAD_NONE},
        {PAD_NONE, PAD_NONE},      {PAD_NONE, PAD_NONE},       {PAD_NONE, PAD_NONE},
        {PAD_NONE, PAD_NONE},
    };
    memcpy(c->key, keys, sizeof keys);
    memcpy(c->pad, pads, sizeof pads);
}

/* ---------------------------------------------------------------- ini */

static char g_ini[MAX_PATH];

static const char *ini_path(void)
{
    if (!g_ini[0])
        snprintf(g_ini, sizeof g_ini, "%s\\hydro.ini", g_cfg.save_dir);
    return g_ini;
}

void settings_load(void)
{
    const char *ini = ini_path();
    char buf[128];
    for (int i = 0; i < g_setting_count; i++) {
        const SettingDesc *d = &g_setting_desc[i];
        GetPrivateProfileStringA(d->section, d->key, "", buf, sizeof buf, ini);
        *d->value = d->def;
        if (!buf[0])
            continue;
        if (d->count) {
            for (int k = 0; k < d->count; k++)
                if (!_stricmp(d->names[k], buf))
                    setting_set_index(d, k);
        } else {
            setting_set_index(d, atoi(buf));
        }
    }

    Controls *c = &g_settings.ctl;
    settings_default_bindings(c);
    for (int a = 0; a < ACT_COUNT; a++) {
        if (GetPrivateProfileStringA("Keyboard", k_action_keys[a], "\x01", buf, sizeof buf, ini) && buf[0] != 1) {
            char *save, *tok = strtok_s(buf, ",", &save);
            for (int s = 0; s < BIND_SLOTS; s++, tok = tok ? strtok_s(NULL, ",", &save) : NULL) {
                while (tok && *tok == ' ')
                    tok++;
                c->key[a][s] = tok && *tok ? key_from_name(tok) : 0;
            }
        }
        if (GetPrivateProfileStringA("Gamepad", k_action_keys[a], "\x01", buf, sizeof buf, ini) && buf[0] != 1) {
            char *save, *tok = strtok_s(buf, ",", &save);
            for (int s = 0; s < BIND_SLOTS; s++, tok = tok ? strtok_s(NULL, ",", &save) : NULL) {
                while (tok && *tok == ' ')
                    tok++;
                c->pad[a][s] = tok && *tok ? pad_from_name(tok) : PAD_NONE;
            }
        }
    }

    Network *n = &g_settings.net;
    n->base_port = GetPrivateProfileIntA("Network", "base_port", 2537, ini);
    if (n->base_port < 1024 || n->base_port > 65535 - 15)
        n->base_port = 2537;
    GetPrivateProfileStringA("Network", "peers", "", n->peers, sizeof n->peers, ini);
    GetPrivateProfileStringA("Network", "master", "", n->master, sizeof n->master, ini);
    if (getenv("HYDRO_MASTER")) /* testing */
        snprintf(n->master, sizeof n->master, "%s", getenv("HYDRO_MASTER"));
    if (!n->master[0] || !_stricmp(n->master, NET_OLD_DEFAULT_MASTER))
        snprintf(n->master, sizeof n->master, "%s", NET_DEFAULT_MASTER);
    GetPrivateProfileStringA("Network", "direct", "", n->direct, sizeof n->direct, ini);
    GetPrivateProfileStringA("Network", "player_name", "PLAYER", n->player_name, sizeof n->player_name, ini);
    GetPrivateProfileStringA("Network", "lobby_name", "HYDRO LOBBY", n->lobby_name, sizeof n->lobby_name, ini);
    GetPrivateProfileStringA("Network", "lobby_password", "", n->lobby_password, sizeof n->lobby_password, ini);
    n->lobby_max = GetPrivateProfileIntA("Network", "lobby_max", 4, ini);
    if (n->lobby_max < 2 || n->lobby_max > 16)
        n->lobby_max = 4;
    n->lobby_delay = GetPrivateProfileIntA("Network", "lobby_delay", 0, ini);
    if (n->lobby_delay < 0 || n->lobby_delay > 6)
        n->lobby_delay = 0;
    n->input_delay = GetPrivateProfileIntA("Network", "input_delay", 1, ini);
    if (n->input_delay < 1 || n->input_delay > 6)
        n->input_delay = 1;
    n->lan_port = GetPrivateProfileIntA("Network", "lan_port", 27960, ini);
    if (n->lan_port < 1024 || n->lan_port > 65535 - 3)
        n->lan_port = 27960;
    load_lobby_opts(&n->lobby_opts, ini);
    hy_log("settings: loaded %s", ini);
}

void settings_save(void)
{
    const char *ini = ini_path();
    char buf[128];
    for (int i = 0; i < g_setting_count; i++) {
        const SettingDesc *d = &g_setting_desc[i];
        if (d->count)
            snprintf(buf, sizeof buf, "%s", d->names[setting_index(d)]);
        else
            snprintf(buf, sizeof buf, "%d", *d->value);
        WritePrivateProfileStringA(d->section, d->key, buf, ini);
    }
    const Controls *c = &g_settings.ctl;
    for (int a = 0; a < ACT_COUNT; a++) {
        snprintf(buf, sizeof buf, "%s%s%s", key_name(c->key[a][0]), c->key[a][1] ? "," : "", key_name(c->key[a][1]));
        WritePrivateProfileStringA("Keyboard", k_action_keys[a], buf, ini);
        snprintf(buf, sizeof buf, "%s%s%s", pad_name(c->pad[a][0]), c->pad[a][1] != PAD_NONE ? "," : "",
                 pad_name(c->pad[a][1]));
        WritePrivateProfileStringA("Gamepad", k_action_keys[a], buf, ini);
    }
    snprintf(buf, sizeof buf, "%d", g_settings.net.base_port);
    WritePrivateProfileStringA("Network", "base_port", buf, ini);
    WritePrivateProfileStringA("Network", "peers", g_settings.net.peers, ini);
    WritePrivateProfileStringA("Network", "master", g_settings.net.master, ini);
    WritePrivateProfileStringA("Network", "direct", g_settings.net.direct, ini);
    WritePrivateProfileStringA("Network", "player_name", g_settings.net.player_name, ini);
    WritePrivateProfileStringA("Network", "lobby_name", g_settings.net.lobby_name, ini);
    WritePrivateProfileStringA("Network", "lobby_password", g_settings.net.lobby_password, ini);
    snprintf(buf, sizeof buf, "%d", g_settings.net.lobby_max);
    WritePrivateProfileStringA("Network", "lobby_max", buf, ini);
    snprintf(buf, sizeof buf, "%d", g_settings.net.lobby_delay);
    WritePrivateProfileStringA("Network", "lobby_delay", buf, ini);
    snprintf(buf, sizeof buf, "%d", g_settings.net.input_delay);
    WritePrivateProfileStringA("Network", "input_delay", buf, ini);
    snprintf(buf, sizeof buf, "%d", g_settings.net.lan_port);
    WritePrivateProfileStringA("Network", "lan_port", buf, ini);
    save_lobby_opts(&g_settings.net.lobby_opts, ini);
    hy_log("settings: saved %s", ini);
}

void settings_apply_graphics(void)
{
    char conf[MAX_PATH];
    snprintf(conf, sizeof conf, "%s", g_cfg.glide_dll);
    char *slash = strrchr(conf, '\\');
    if (!slash) {
        /* glide2x.dll from the search path: we can't tell which folder dgVoodoo reads. */
        hy_log("settings: Glide DLL path is not absolute; dgVoodoo.conf not written");
        return;
    }
    strcpy(slash + 1, "dgVoodoo.conf");
    dgv_write_conf(&g_settings.gfx, conf);
}
