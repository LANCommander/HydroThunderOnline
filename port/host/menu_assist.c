/*
 * Digital navigation for the wheel-driven menus.
 *
 * The cabinet's menus map the absolute wheel angle to an item, which a keyboard or d-pad
 * (steer = full left / centre / full right) can't express. controls_UpdatePublicData is
 * replaced by a 1:1 copy; while a wheel menu is active, a left/right press steps the target
 * one item, and the public steer value is overwritten with an angle that the game's own
 * selection logic resolves to that item (and holds there while nothing is pressed).
 *
 *   track/boat select  wpr_select_Work    reads steer after dead zone (InputState +8)
 *   initials entry     wpr_hiscore_Work   reads raw steer (InputState +0)
 *
 * PC SETTINGS > CONTROLS > MENU ASSIST (hydro.ini) turns it off at runtime; HYDRO_MENU_ASSIST=0 doesn't
 * even install the hook (for a real wheel).
 */
#include "hook.h"
#include "host.h"
#include "input.h"
#include "settings.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define MEM(type, addr) (*(volatile type *)(uintptr_t)(addr))

/* controls: cached controls (controls_SampleControls) -> _Controls_InputState (public copy) */
#define CONTROLS_CACHE        0x005658bcu /* raw steer, raw throttle, steer, throttle, buttons */
#define CONTROLS_INPUTSTATE   0x006221a0u /* same + pressed latch (+0x14), released latch (+0x18) */

/* wpr_select_Work: stage 0/1 = track, 2/3 = boat. Each item owns a band [lo, hi] of steer. */
#define SELECT_ACTIVE         0x005bd758u
#define SELECT_ENABLED        0x005bd0bcu /* wpr_select_EnableSelectionChanges */
#define SELECT_STAGE          0x005bc294u
#define TRACK_ITEMS           0x005bd1a0u /* 10 x 0x4c: +0 byte selectable, +0xa index, +0x3c lo, +0x40 hi */
#define TRACK_CURRENT         0x005bd594u
#define TRACK_STRIDE          0x4cu
#define TRACK_COUNT           10
#define BOAT_ITEMS            0x005bbf60u /* 9 x 0x54: +0xa index, +0x44 lo, +0x48 hi */
#define BOAT_CURRENT          0x005bd550u
#define BOAT_STRIDE           0x54u
#define BOAT_COUNT            9
#define BOAT_ADVANCED_OK      0x005c0144u /* wpr_select_CanWeSelectAnAdvancedBoat: else boats 6..8 refused */
#define BOAT_FIRST_ADVANCED   6

/* wpr_hiscore_Work: state 1 = entering initials (wpr_hiscore_AreWeStillEnteringInitials) */
#define HISCORE_STATE         0x005bbbb4u
#define HISCORE_BONUSKEYS     0x005bbdecu /* nonzero while bonuskeys_Work owns the screen */
#define HISCORE_LETTER        0x005bbbbcu /* 0..0x28 over "0-9A-Z?!><=" */
#define HISCORE_COUNT         0x005bbbc0u /* letters entered */
#define HISCORE_LETTERS       41
#define HISCORE_DELETE        0x27
#define HISCORE_END           0x28

enum { CTX_NONE, CTX_TRACK, CTX_BOAT, CTX_INITIALS };
static const char *const ctx_name[] = {"none", "track", "boat", "letter"};

static int g_ctx, g_target, g_held, g_count;
static ULONGLONG g_next_repeat, g_target_time;

static int context(void)
{
    if (MEM(int32_t, SELECT_ACTIVE) && MEM(int32_t, SELECT_ENABLED)) {
        uint32_t stage = MEM(uint32_t, SELECT_STAGE);
        if (stage <= 1)
            return CTX_TRACK;
        if (stage <= 3)
            return CTX_BOAT;
    }
    if (MEM(int32_t, HISCORE_STATE) == 1 && !MEM(int32_t, HISCORE_BONUSKEYS))
        return CTX_INITIALS;
    return CTX_NONE;
}

static uint32_t track_item(int i) { return TRACK_ITEMS + (uint32_t)i * TRACK_STRIDE; }
static uint32_t boat_item(int i) { return BOAT_ITEMS + (uint32_t)i * BOAT_STRIDE; }

static int current(int ctx)
{
    switch (ctx) {
    case CTX_TRACK: return MEM(uint16_t, MEM(uint32_t, TRACK_CURRENT) + 0xa);
    case CTX_BOAT: return MEM(uint16_t, MEM(uint32_t, BOAT_CURRENT) + 0xa);
    case CTX_INITIALS: return MEM(int32_t, HISCORE_LETTER);
    }
    return 0;
}

/* The item one press in `dir` away from t (t if there is none). */
static int step(int ctx, int t, int dir)
{
    switch (ctx) {
    case CTX_TRACK:
        /* the game skips unselectable tracks the same way */
        for (int i = t + dir; i >= 0 && i < TRACK_COUNT; i += dir)
            if (MEM(uint8_t, track_item(i)))
                return i;
        return t;
    case CTX_BOAT: {
        int last = MEM(int32_t, BOAT_ADVANCED_OK) ? BOAT_COUNT - 1 : BOAT_FIRST_ADVANCED - 1;
        int i = t + dir;
        return i < 0 || i > last ? t : i;
    }
    case CTX_INITIALS:
        if (MEM(uint32_t, HISCORE_COUNT) >= 3) /* only delete or end are left */
            return dir < 0 ? HISCORE_DELETE : HISCORE_END;
        return (t + dir + HISCORE_LETTERS) % HISCORE_LETTERS;
    }
    return t;
}

/* A steer value the game resolves to item t, coming from item cur. */
static float steer_for(int ctx, int t, int cur)
{
    switch (ctx) {
    case CTX_TRACK: return (MEM(float, track_item(t) + 0x3c) + MEM(float, track_item(t) + 0x40)) * 0.5f;
    case CTX_BOAT: return (MEM(float, boat_item(t) + 0x44) + MEM(float, boat_item(t) + 0x48)) * 0.5f;
    case CTX_INITIALS:
        if (MEM(uint32_t, HISCORE_COUNT) >= 3) /* raw > -0.1 = end, else delete */
            return t == HISCORE_END ? 0.5f : -0.5f;
        /* v = (raw+1)/2 picks floor(42v) when v < (cur+1)/42, else floor(42v)-1: t+0.5 lands on t
         * from above, t+1.5 from below and also holds t once there. */
        return 2.0f * ((float)t + (t < cur ? 0.5f : 1.5f)) / 42.0f - 1.0f;
    }
    return 0.0f;
}

static void menu_assist(void)
{
    int ctx = context();
    int cur = ctx ? current(ctx) : 0;
    int count = ctx == CTX_INITIALS ? (int)MEM(uint32_t, HISCORE_COUNT) : 0;
    ULONGLONG now = GetTickCount64();
    if (ctx != g_ctx || count != g_count) {
        g_ctx = ctx;
        g_count = count;
        g_target = cur;
    }
    if (!ctx)
        return;

    int dir = input_menu_dir(), press = 0;
    if (dir && dir != g_held) {
        press = 1;
        g_next_repeat = now + 350;
    } else if (dir && now >= g_next_repeat) {
        press = 1;
        g_next_repeat = now + 120;
    }
    g_held = dir;

    /* the game refused the target (or changed the selection itself): follow it */
    if (g_target != cur && now - g_target_time > 500)
        g_target = cur;
    if (press) {
        int t = step(ctx, g_target, dir);
        if (t != g_target) {
            hy_log("menu: %s %d -> %d", ctx_name[ctx], g_target, t);
            g_target = t;
            g_target_time = now;
        }
    }

    float s = steer_for(ctx, g_target, cur);
    MEM(float, CONTROLS_INPUTSTATE + 0x0) = s;
    MEM(float, CONTROLS_INPUTSTATE + 0x8) = s;
}

/* 1:1 with the original (0x136468), plus the assist. */
static void __cdecl my_controls_UpdatePublicData(void)
{
    uint32_t b = MEM(uint32_t, CONTROLS_CACHE + 0x10);
    for (uint32_t i = 0; i < 0x10; i += 4)
        MEM(uint32_t, CONTROLS_INPUTSTATE + i) = MEM(uint32_t, CONTROLS_CACHE + i);
    MEM(uint32_t, CONTROLS_INPUTSTATE + 0x10) = b;
    MEM(uint32_t, CONTROLS_INPUTSTATE + 0x14) |= b & MEM(uint32_t, CONTROLS_INPUTSTATE + 0x18);
    MEM(uint32_t, CONTROLS_INPUTSTATE + 0x18) |= ~b;
    if (g_settings.ctl.menu_assist)
        menu_assist();
}

void menu_assist_install(void)
{
    const char *env = getenv("HYDRO_MENU_ASSIST");
    if (env && !strcmp(env, "0")) {
        hy_log("menu assist off (HYDRO_MENU_ASSIST=0)");
        return;
    }
    hook_name("controls_UpdatePublicData", my_controls_UpdatePublicData);
}
