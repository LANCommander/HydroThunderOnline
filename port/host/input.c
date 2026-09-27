/*
 * Keyboard / XInput pad -> cabinet inputs, as the DiegoIO board reports them.
 *
 * controldriver (CONTROL_DRIVER.OBJ) reads:
 *   analog 0 = ADC 1 (reply byte 3): steering, 0..255, centre 0x80, dead zone +-7.5%
 *   analog 1 = ADC 0 (reply byte 4): throttle lever, 0..255, centre 0x80 = idle, dead zone +-17.5%
 *   buttons  = ~switch byte (active low): 0x01 boost/start, 0x02 0x04 0x08 view buttons, 0x10 service
 *              credit, 0x20 volume down, 0x40 volume up, 0x80 Test switch (ConsoleEscKeyHit: leaves
 *              the game loop for the operator menu)
 *
 * Keys and pad buttons are bound per cabinet input in g_settings.ctl (settings.c; edited in the
 * operator menu's PC SETTINGS, saved to hydro.ini). Defaults: arrows steer/throttle, Space boost,
 * 1/2/3 views, 5 coin, F1 service credit, F2 Test, -/= volume; pad d-pad, A/Start, B/X/Y, Back.
 * The pad's stick steers and its triggers (or a stick) drive the throttle. Esc closes the window,
 * except in the operator menu (which has a QUIT item).
 *
 * The operator menu (vinput in the image) navigates with the cabinet's buttons: vol-/vol+ move
 * the cursor, Test or boost selects, service credit goes back. While it runs, Up/Down (keys or
 * d-pad) also press vol-/vol+, Enter presses Test, and Backspace / Esc / pad B press service credit.
 *
 * The in-game menus read the wheel as an absolute angle; menu_assist.c turns input_menu_dir()
 * into one-item steps there.
 *
 * HYDRO_INPUT_SCRIPT holds inputs at set times, with or without focus (testing, e.g. several linked
 * instances): "20:coin,21:coin,23:boost,40-95:throttle_up". Seconds since launch; a single time
 * taps for 150 ms; names are the hydro.ini [Keyboard] keys.
 */
#include "input.h"
#include "host.h"
#include "settings.h"
#include "window.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xinput.h>

static uint8_t g_coin[5];
static int g_coin_down;
static volatile int g_operator_menu;
static volatile int g_capture;
static volatile int g_typing; /* inline text entry in a menu: keys type instead of pressing buttons */
static uint8_t g_suppress; /* switches held when the operator menu opened: ignored until released */
static int g_suppress_arm;

static const uint8_t k_switch_bit[ACT_COUNT] = {
    [ACT_BOOST] = 0x01,   [ACT_VIEW1] = 0x02,    [ACT_VIEW2] = 0x04,  [ACT_VIEW3] = 0x08,
    [ACT_SERVICE] = 0x10, [ACT_VOL_DOWN] = 0x20, [ACT_VOL_UP] = 0x40, [ACT_TEST] = 0x80,
};

int input_operator_menu_open(void)
{
    return g_operator_menu;
}

void input_set_operator_menu(int on)
{
    g_operator_menu = on;
    g_suppress_arm = on;
    if (!on)
        g_typing = 0; /* the menu can leave from a page that was typing (START GAME) */
    hy_log("input: operator menu %s", on ? "open" : "closed");
}

static int focused(void)
{
    return GetForegroundWindow() == window_get();
}

static int key(int vk)
{
    return vk && focused() && (GetAsyncKeyState(vk) & 0x8000);
}

/* ---------------------------------------------------------------- XInput (loaded at runtime) */

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE *);
static PFN_XInputGetState g_xinput_get;
static INIT_ONCE g_xinput_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_pad_retry; /* GetTickCount of the next probe while no pad is connected */

static BOOL CALLBACK xinput_load(INIT_ONCE *once, void *param, void **ctx)
{
    (void)once, (void)param, (void)ctx;
    static const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !g_xinput_get; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m)
            g_xinput_get = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
        if (g_xinput_get)
            hy_log("input: %s loaded", dlls[i]);
    }
    if (!g_xinput_get)
        hy_log("input: XInput not available, keyboard only");
    return TRUE;
}

/* Pad 0 if enabled, connected and the window has focus. Probing a missing pad is slow, so retry once a second. */
static int pad_read(XINPUT_GAMEPAD *out)
{
    static int connected = -1;
    if (!g_settings.ctl.gamepad)
        return 0;
    InitOnceExecuteOnce(&g_xinput_once, xinput_load, NULL, NULL);
    if (!g_xinput_get || !focused())
        return 0;
    DWORD now = GetTickCount();
    if (connected == 0 && (LONG)(now - (DWORD)g_pad_retry) < 0)
        return 0;
    XINPUT_STATE st;
    int ok = g_xinput_get(0, &st) == ERROR_SUCCESS;
    if (ok != connected) {
        hy_log("input: gamepad %s", ok ? "connected" : "not connected");
        connected = ok;
    }
    if (!ok) {
        g_pad_retry = (LONG)(now + 1000);
        return 0;
    }
    *out = st.Gamepad;
    return 1;
}

static int pad_button(const XINPUT_GAMEPAD *p, int b)
{
    if (b == PAD_LT)
        return p->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    if (b == PAD_RT)
        return p->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    return b >= 0 && b < 16 && (p->wButtons & (1u << b));
}

/* One stick axis in -1..1, with the configured radial dead zone and sensitivity. */
static float pad_axis(const XINPUT_GAMEPAD *p, int right_stick, int y_axis)
{
    float x = (float)(right_stick ? p->sThumbRX : p->sThumbLX);
    float y = (float)(right_stick ? p->sThumbRY : p->sThumbLY);
    float mag = sqrtf(x * x + y * y);
    float dz = 32767.0f * (float)g_settings.ctl.deadzone / 100.0f;
    if (mag <= dz || mag < 1.0f)
        return 0.0f;
    float scaled = ((mag > 32767.0f ? 32767.0f : mag) - dz) / (32767.0f - dz + 1.0f);
    float v = (y_axis ? y : x) / mag * scaled * (float)g_settings.ctl.sensitivity / 100.0f;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

/* ---------------------------------------------------------------- actions */

/* ---------------------------------------------------------------- scripted input */

static struct {
    int act;
    DWORD from, to; /* ms since launch */
} g_script[64];
static int g_script_n;
static INIT_ONCE g_script_once = INIT_ONCE_STATIC_INIT;

static DWORD ms_since_launch(void)
{
    FILETIME created, x, now;
    GetProcessTimes(GetCurrentProcess(), &created, &x, &x, &x);
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a = {.LowPart = created.dwLowDateTime, .HighPart = created.dwHighDateTime};
    ULARGE_INTEGER b = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
    return (DWORD)((b.QuadPart - a.QuadPart) / 10000);
}

static BOOL CALLBACK script_parse(INIT_ONCE *once, void *param, void **ctx)
{
    (void)once, (void)param, (void)ctx;
    const char *env = getenv("HYDRO_INPUT_SCRIPT");
    if (!env)
        return TRUE;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", env);
    char *save, *tok = strtok_s(buf, ",", &save);
    for (; tok && g_script_n < (int)(sizeof g_script / sizeof g_script[0]); tok = strtok_s(NULL, ",", &save)) {
        double from, to;
        char name[32];
        if (sscanf(tok, "%lf-%lf:%31s", &from, &to, name) != 3) {
            if (sscanf(tok, "%lf:%31s", &from, name) != 2) {
                hy_log("input: script entry '%s' ignored", tok);
                continue;
            }
            to = from + 0.15;
        }
        int act = action_from_key(name);
        if (act < 0) {
            hy_log("input: script input '%s' unknown", name);
            continue;
        }
        g_script[g_script_n].act = act;
        g_script[g_script_n].from = (DWORD)(from * 1000);
        g_script[g_script_n++].to = (DWORD)(to * 1000);
    }
    hy_log("input: %d scripted inputs", g_script_n);
    return TRUE;
}

static int script_held(int act)
{
    InitOnceExecuteOnce(&g_script_once, script_parse, NULL, NULL);
    if (!g_script_n)
        return 0;
    DWORD t = ms_since_launch();
    for (int i = 0; i < g_script_n; i++)
        if (g_script[i].act == act && t >= g_script[i].from && t < g_script[i].to)
            return 1;
    return 0;
}

static int action_held(int act, const XINPUT_GAMEPAD *p)
{
    const Controls *c = &g_settings.ctl;
    if (script_held(act))
        return 1;
    for (int s = 0; s < BIND_SLOTS; s++) {
        if (!g_typing && key(c->key[act][s]))
            return 1;
        if (p && c->pad[act][s] != PAD_NONE && pad_button(p, c->pad[act][s]))
            return 1;
    }
    return 0;
}

/* Keyboard steering, -1..1: instant, or ramped towards the target at the configured speed. */
static float key_steer(int dir)
{
    static float pos;
    static LARGE_INTEGER last, freq;
    static const float full_lock_s[] = {0.0f, 0.12f, 0.25f, 0.45f};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
    last = now;
    int speed = g_settings.ctl.key_steer;
    if (speed <= 0 || speed > 3) {
        pos = (float)dir;
        return pos;
    }
    float step = dt / full_lock_s[speed];
    if (!dir || (dir > 0) != (pos > 0))
        step *= 2.0f; /* recentre / reverse faster than turning in */
    float target = (float)dir;
    pos = pos < target ? fminf(pos + step, target) : fmaxf(pos - step, target);
    return pos;
}

static uint8_t to_adc(float v)
{
    int a = 0x80 + (int)lroundf(v * (v < 0 ? 128.0f : 127.0f));
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
}

/* ---------------------------------------------------------------- polling */

int input_menu_dir(void)
{
    XINPUT_GAMEPAD p;
    int pad = pad_read(&p);
    int dir = action_held(ACT_STEER_RIGHT, pad ? &p : NULL) - action_held(ACT_STEER_LEFT, pad ? &p : NULL);
    if (!dir && pad) {
        float x = pad_axis(&p, g_settings.ctl.steer_stick, 0);
        dir = x > 0.5f ? 1 : x < -0.5f ? -1 : 0;
    }
    return dir;
}

void input_poll(InputState *out)
{
    for (int i = 0; i < 5; i++)
        out->coin[i] = g_coin[i];
    if (g_capture) { /* a binding screen owns the keyboard: the cabinet sees nothing pressed */
        out->adc[0] = out->adc[1] = 0x80;
        out->switches = 0xff;
        return;
    }

    XINPUT_GAMEPAD p;
    const XINPUT_GAMEPAD *pp = pad_read(&p) ? &p : NULL;
    const Controls *c = &g_settings.ctl;

    int kdir = action_held(ACT_STEER_RIGHT, pp) - action_held(ACT_STEER_LEFT, pp);
    float steer = key_steer(kdir);
    if (!kdir && pp)
        steer = pad_axis(pp, c->steer_stick, 0);

    float throttle = (float)(action_held(ACT_THROTTLE_UP, pp) - action_held(ACT_THROTTLE_DOWN, pp));
    if (throttle == 0.0f && pp) {
        switch (c->throttle_source) {
        case 0:
            throttle = ((float)pp->bRightTrigger - (float)pp->bLeftTrigger) / 255.0f;
            break;
        case 1:
            throttle = pad_axis(pp, 1, 1);
            break;
        case 2:
            throttle = pad_axis(pp, 0, 1);
            break;
        }
    }
    out->adc[0] = to_adc(throttle);
    out->adc[1] = to_adc(steer);

    uint8_t buttons = 0;
    for (int a = 0; a < ACT_COUNT; a++)
        if (k_switch_bit[a] && action_held(a, pp))
            buttons |= k_switch_bit[a];
    static int test_logged;
    if (buttons & 0x80) {
        if (!test_logged++ && !g_operator_menu)
            hy_log("input: Test switch pressed (game will leave its loop)");
    } else {
        test_logged = 0;
    }
    if (g_operator_menu) {
        WORD pb = pp ? pp->wButtons : 0;
        if (key(VK_UP) || (pb & XINPUT_GAMEPAD_DPAD_UP))
            buttons |= 0x20;
        if (key(VK_DOWN) || (pb & XINPUT_GAMEPAD_DPAD_DOWN))
            buttons |= 0x40;
        if (key(VK_RETURN) && !g_typing)
            buttons |= 0x80;
        if ((key(VK_BACK) && !g_typing) || key(VK_ESCAPE) || (pb & XINPUT_GAMEPAD_B))
            buttons |= 0x10;
        if (g_suppress_arm) {
            g_suppress_arm = 0;
            g_suppress = buttons;
        }
        g_suppress &= buttons;
        buttons &= (uint8_t)~g_suppress;
    }
    out->switches = (uint8_t)~buttons;

    int coin = action_held(ACT_COIN, pp);
    if (coin && !g_coin_down)
        g_coin[0] = (uint8_t)(g_coin[0] % 7 + 1);
    g_coin_down = coin;
    out->coin[0] = g_coin[0];
}

/* ---------------------------------------------------------------- binding capture */

static volatile LONG g_char_head, g_char_tail;
static int g_chars[64];
static uint8_t g_cap_keys[256];
static WORD g_cap_pad;

/* wButtons with the triggers folded into its two unused bits (0x400 LT, 0x800 RT). */
static WORD pad_bits(const XINPUT_GAMEPAD *p)
{
    return p->wButtons | (p->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x400 : 0) |
           (p->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x800 : 0);
}

void input_set_capture(int on)
{
    g_capture = on;
    g_char_tail = g_char_head; /* typing starts with an empty queue */
    /* Whatever is held right now (the key that opened the prompt) doesn't count as a new press. */
    for (int vk = 0; vk < 256; vk++)
        g_cap_keys[vk] = key(vk) ? 1 : 0;
    XINPUT_GAMEPAD p;
    g_cap_pad = pad_read(&p) ? pad_bits(&p) : 0;
}

int input_capture_key(void)
{
    for (int vk = 8; vk < 256; vk++) {
        /* generic modifiers: report the left/right variants instead; skip mouse buttons */
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
            continue;
        int down = key(vk);
        if (down && !g_cap_keys[vk]) {
            g_cap_keys[vk] = 1;
            return vk;
        }
        if (!down)
            g_cap_keys[vk] = 0;
    }
    return 0;
}

int input_capture_pad(void)
{
    XINPUT_GAMEPAD p;
    if (!pad_read(&p))
        return PAD_NONE;
    WORD now = pad_bits(&p);
    WORD fresh = now & ~g_cap_pad;
    g_cap_pad = now;
    for (int b = 0; b < 16; b++) {
        if (!(fresh & (1u << b)))
            continue;
        if (b == 10)
            return PAD_LT;
        if (b == 11)
            return PAD_RT;
        return b;
    }
    return PAD_NONE;
}

int input_any_held(void)
{
    for (int vk = 8; vk < 256; vk++)
        if (key(vk))
            return 1;
    XINPUT_GAMEPAD p;
    return pad_read(&p) && pad_bits(&p);
}

int input_capturing(void)
{
    return g_capture;
}

void input_set_typing(int on)
{
    if (on && !g_typing)
        g_char_tail = g_char_head; /* typing starts with an empty queue */
    g_typing = on;
}

/* ---------------------------------------------------------------- text entry (WM_CHAR) */


void input_push_char(int c)
{
    if (!g_capture && !g_typing)
        return;
    LONG next = (g_char_head + 1) % 64;
    if (next == g_char_tail)
        return;
    g_chars[g_char_head] = c;
    InterlockedExchange(&g_char_head, next);
}

int input_text_char(void)
{
    if (g_char_tail == g_char_head)
        return 0;
    int c = g_chars[g_char_tail];
    InterlockedExchange(&g_char_tail, (g_char_tail + 1) % 64);
    return c;
}
