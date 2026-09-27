/* OPERATOR.OBJ: the operator settings (the CMOS copy the game runs with). */
#include "port.h"

#include "../host/hook.h"
#include "../host/host.h"
#include "../host/lobby_client.h"

#include <string.h>

#define OP_COUNT 0x56
#define OperatorSettings ((int *)HY_VAR_OperatorSettings) /* 0x56 dwords, CMOS dword 5 on */
/* cmos_ModuleInit points this at OperatorSettings; cmos_CopyAllGlobalStructs2Cmos saves from it */
#define Cmos_pOperatorSettings (*(int **)0x0055d450u)

/* OperatorSettings indices (defaults table at 0x1ee4e0, menu tables at 0x30fbd0/0x30ff90/0x310410).
 * The port changes two factory defaults: FREE PLAY is ON and the volume is full (operator_install). */
#define OP_DEFAULTS 0x001ee4e0u /* cmos_LoadOpSettingsDefaults copies it */
enum {
    OP_TRACK_SELECT_TIME = 5, /* float seconds */
    OP_BOAT_SELECT_TIME = 6,
    OP_HIGH_SCORE_TIME = 7,
    OP_LIMIT_FREE_RACES = 9, /* percent */
    OP_CONTINUE_TIME = 12,
    OP_MASTER_VOLUME = 11, /* 0..255, -/= in game */
    OP_FREE_PLAY = 15,
    OP_UNLOCK_ALL_TRACKS = 17,
    OP_TRACK_DIFFICULTY = 20, /* 13 tracks */
    OP_AI_DIFFICULTY = 33,    /* 13 tracks */
    OP_FREE_RACE_LIMITER = 46,
    OP_ENABLE_ALL_BOATS = 47,
    OP_SAVED_CREDITS = 85,
};

/* Port-only: a lobby's game options, forced over this run's copy only. g_cab is what the CMOS
 * holds, g_forced marks the indices the lobby replaced. */
static int g_cab[OP_COUNT];
static unsigned char g_forced[OP_COUNT];
static int g_forcing;

static void force(int i, int value)
{
    OperatorSettings[i] = value;
    g_forced[i] = 1;
}

static void force_seconds(int i, int seconds)
{
    float f = (float)seconds;
    int bits;
    memcpy(&bits, &f, sizeof bits);
    force(i, bits);
}

static void apply_lobby_options(const HtGameOpts *o)
{
    memcpy(g_cab, OperatorSettings, sizeof g_cab);
    memset(g_forced, 0, sizeof g_forced);
    force(OP_FREE_PLAY, o->freeplay);
    force(OP_ENABLE_ALL_BOATS, o->allboats);
    force(OP_UNLOCK_ALL_TRACKS, o->alltracks);
    force_seconds(OP_TRACK_SELECT_TIME, o->t_track);
    force_seconds(OP_BOAT_SELECT_TIME, o->t_boat);
    force_seconds(OP_HIGH_SCORE_TIME, o->t_hiscore);
    force_seconds(OP_CONTINUE_TIME, o->t_continue);
    force(OP_FREE_RACE_LIMITER, o->limiter);
    force(OP_LIMIT_FREE_RACES, o->limit_pct);
    for (int t = 0; t < HTNET_TRACKS; t++) {
        force(OP_TRACK_DIFFICULTY + t, o->track_diff[t]);
        force(OP_AI_DIFFICULTY + t, o->ai_diff[t]);
    }
    /* Forced free play on a coin-op cabinet: wpr_banker_ModuleInit zeroes the saved credits, which
     * must not reach the CMOS either. */
    if (o->freeplay && !g_cab[OP_FREE_PLAY])
        g_forced[OP_SAVED_CREDITS] = 1;
    g_forcing = 1;
    hy_log("operator: lobby game options: free play %d, all boats %d, all tracks %d, times %d/%d/%d/%d, "
           "limiter %d %d%%",
           o->freeplay, o->allboats, o->alltracks, o->t_track, o->t_boat, o->t_hiscore, o->t_continue, o->limiter,
           o->limit_pct);
}

/* Runs at every gameinit, i.e. at boot and each time the operator menu starts the game again. */
int operator_ModuleInit(void)
{
    HY_CALL(void(__cdecl *)(int *), cmos_ReadOperatorSettings)(OperatorSettings);
    /* Port-only: in a lobby the unit is the one the lobby assigned, and networking is on (0 = on).
     * Only this runtime copy changes; the CMOS (and NETWORK ADJUSTMENTS) keep the operator's. */
    g_forcing = 0;
    int unit;
    if (lobby_active(&unit)) {
        OperatorSettings[0] = unit;
        OperatorSettings[1] = 0;
        const HtGameOpts *o = lobby_game_opts();
        if (o)
            apply_lobby_options(o);
    }
    HY_CALL(void(__cdecl *)(int), gutil_SetGammaCorrectionLevel)(OperatorSettings[8]);
    return 1;
}
HY_PORT(operator_ModuleInit)

/* The operator menu opens (menus): it reloads the settings from the CMOS, so nothing is forced
 * until the next gameinit. */
void operator_ClearLobbyOptions(void)
{
    g_forcing = 0;
}

/* Port-only: cmos_CopyAllGlobalStructs2Cmos (gameloop_Start and the end of every race) writes the
 * whole OperatorSettings array back to the CMOS. While a lobby's options are forced, it saves a
 * copy with the cabinet's own values in their place instead. */
static void __cdecl save_cmos(void)
{
    typedef void(__cdecl * VoidFn)(void);
    if (!g_forcing || Cmos_pOperatorSettings != OperatorSettings) {
        HY_CALL(VoidFn, cmos_CopyAllGlobalStructs2Cmos)();
        return;
    }
    static int merged[OP_COUNT];
    static int logged;
    if (!logged++)
        hy_log("operator: CMOS save keeps the cabinet's own adjustments (lobby options are not saved)");
    for (int i = 0; i < OP_COUNT; i++)
        merged[i] = g_forced[i] ? g_cab[i] : OperatorSettings[i];
    Cmos_pOperatorSettings = merged;
    HY_CALL(VoidFn, cmos_CopyAllGlobalStructs2Cmos)();
    Cmos_pOperatorSettings = OperatorSettings;
}

void operator_install(void)
{
    static const uint32_t k_calls[] = {
        0x0013d7d2u, /* gameloop_Start */
        0x0016d9b6u, /* statemgr_EndRace and the statics after it */
        0x0016da04u,
        0x0016db14u,
    };
    for (unsigned i = 0; i < sizeof k_calls / sizeof *k_calls; i++) {
        uint32_t at = k_calls[i]; /* call rel32 */
        hook_patch32(at + 1, HY_FN_cmos_CopyAllGlobalStructs2Cmos - (at + 5),
                     (uint32_t)(uintptr_t)save_cmos - (at + 5));
    }
    /* Port-only: a fresh CMOS (and RESTORE FACTORY SETTINGS) gets free play and full volume. */
    hook_patch32(OP_DEFAULTS + OP_FREE_PLAY * 4, 0, 1);
    hook_patch32(OP_DEFAULTS + OP_MASTER_VOLUME * 4, 105, 255);
}

/* Port-only: controls_SampleControls changes the volume (-/=) in OperatorSettings only, and the
 * CMOS gets it at the end of the next race. Quitting before that would lose it, so the host
 * writes it through on exit. */
int operator_master_volume(void)
{
    return OperatorSettings[OP_MASTER_VOLUME];
}
