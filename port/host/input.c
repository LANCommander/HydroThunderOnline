/*
 * Keyboard / gamepad (SDL, else XInput) -> cabinet inputs, as the DiegoIO board reports them.
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
 * The pad is the first SDL game controller (Xbox, PlayStation, Switch and anything else SDL has a mapping for;
 * gamecontrollerdb.txt next to hydro.exe or SDL_GAMECONTROLLERCONFIG adds more), read into an XINPUT_GAMEPAD so
 * bindings keep the XInput button numbering. If SDL can't start, XInput pad 0 is read directly.
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

#define SDL_MAIN_HANDLED
#include <SDL.h>

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

/* Runs fn once, whichever thread gets here first (InitOnceExecuteOnce is Vista+). */
static void run_once(volatile LONG *state, void (*fn)(void))
{
    if (*state == 2)
        return;
    if (InterlockedCompareExchange(state, 1, 0) == 0) {
        fn();
        InterlockedExchange(state, 2);
        return;
    }
    while (*state != 2)
        Sleep(0);
}

/* ---------------------------------------------------------------- gamepad: SDL, else XInput (loaded at runtime) */

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE *);
static PFN_XInputGetState g_xinput_get;
static int g_sdl; /* SDL's game controller subsystem is up */
static SDL_GameController *g_sdl_pad;
static volatile LONG g_pad_once;
static volatile LONG g_pad_retry; /* GetTickCount of the next probe while no XInput pad is connected */

static void xinput_load(void)
{
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
}

static void pad_load(void)
{
    /* Focus is checked here (focused()): SDL has no window to track it by. SDL's own thread watches for pads. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        hy_log("input: SDL unavailable (%s), trying XInput", SDL_GetError());
        xinput_load();
        return;
    }
    g_sdl = 1;
    SDL_JoystickEventState(SDL_IGNORE); /* polled: don't queue events nobody reads */
    SDL_GameControllerEventState(SDL_IGNORE);
    SDL_version v;
    SDL_GetVersion(&v);
    hy_log("input: SDL %d.%d.%d game controllers", v.major, v.minor, v.patch);
    char db[MAX_PATH]; /* next to hydro.exe */
    DWORD len = GetModuleFileNameA(NULL, db, sizeof db);
    char *slash = len < sizeof db ? strrchr(db, '\\') : NULL;
    if (slash && snprintf(slash + 1, sizeof db - (size_t)(slash + 1 - db), "gamecontrollerdb.txt") > 0 &&
        GetFileAttributesA(db) != INVALID_FILE_ATTRIBUTES) {
        int n = SDL_GameControllerAddMappingsFromFile(db);
        if (n < 0)
            hy_log("input: %s: %s", db, SDL_GetError());
        else
            hy_log("input: %s: %d mappings", db, n);
    }
}

/* SDL's buttons as XINPUT_GAMEPAD wButtons bits (the PAD_* numbering). */
static const struct {
    int pad;
    SDL_GameControllerButton sdl;
} k_sdl_buttons[] = {
    {PAD_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP},     {PAD_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {PAD_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {PAD_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {PAD_START, SDL_CONTROLLER_BUTTON_START},         {PAD_BACK, SDL_CONTROLLER_BUTTON_BACK},
    {PAD_LS, SDL_CONTROLLER_BUTTON_LEFTSTICK},        {PAD_RS, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {PAD_LB, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},     {PAD_RB, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {PAD_A, SDL_CONTROLLER_BUTTON_A},                 {PAD_B, SDL_CONTROLLER_BUTTON_B},
    {PAD_X, SDL_CONTROLLER_BUTTON_X},                 {PAD_Y, SDL_CONTROLLER_BUTTON_Y},
};

/* The first game controller SDL has, as an XInput pad (SDL's Y axes point down, XInput's up). */
static int sdl_pad_read(XINPUT_GAMEPAD *out)
{
    SDL_GameControllerUpdate();
    if (g_sdl_pad && !SDL_GameControllerGetAttached(g_sdl_pad)) {
        hy_log("input: gamepad disconnected");
        SDL_GameControllerClose(g_sdl_pad);
        g_sdl_pad = NULL;
    }
    for (int i = 0; !g_sdl_pad && i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i) && (g_sdl_pad = SDL_GameControllerOpen(i)) != NULL)
            hy_log("input: gamepad connected: %s", SDL_GameControllerName(g_sdl_pad));
    }
    if (!g_sdl_pad)
        return 0;
    SDL_GameController *c = g_sdl_pad;
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < sizeof k_sdl_buttons / sizeof k_sdl_buttons[0]; i++)
        if (SDL_GameControllerGetButton(c, k_sdl_buttons[i].sdl))
            out->wButtons |= (WORD)(1u << k_sdl_buttons[i].pad);
    out->bLeftTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 255 / 32767);
    out->bRightTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 255 / 32767);
    out->sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    out->sThumbLY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    out->sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    out->sThumbRY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    return 1;
}

/* The pad if enabled, connected and the window has focus. Probing a missing XInput pad is slow, so that's retried
 * once a second. */
static int pad_read(XINPUT_GAMEPAD *out)
{
    static int connected = -1;
    if (!g_settings.ctl.gamepad)
        return 0;
    run_once(&g_pad_once, pad_load);
    if (!focused())
        return 0;
    if (g_sdl)
        return sdl_pad_read(out);
    if (!g_xinput_get)
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
static volatile LONG g_script_once;

static DWORD ms_since_launch(void)
{
    FILETIME created, x, now;
    GetProcessTimes(GetCurrentProcess(), &created, &x, &x, &x);
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a = {.LowPart = created.dwLowDateTime, .HighPart = created.dwHighDateTime};
    ULARGE_INTEGER b = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
    return (DWORD)((b.QuadPart - a.QuadPart) / 10000);
}

static void script_parse(void)
{
    const char *env = getenv("HYDRO_INPUT_SCRIPT");
    if (!env)
        return;
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
}

static int script_held(int act)
{
    run_once(&g_script_once, script_parse);
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
