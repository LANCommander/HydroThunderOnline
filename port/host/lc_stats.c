/*
 * LANCommander match statistics: every race is reported to LANCommander as a match, through the LANCommander
 * Interposer's match API (LANCommander.Interposer.h). The Interposer is embedded in hydro_lancommander.dll with
 * only that feature (port/CMakeLists.txt says why it isn't in hydro.exe); hydro.exe delay-loads it, so without
 * the DLL nothing is reported and the game runs as before.
 *
 * Where reports go is the Interposer's configuration, not this file's (its MatchStatistics docs):
 *   - started by LANCommander's launcher: the launcher sends them, as the signed-in player;
 *   - .interposer\Config.yml next to hydro.exe with MatchStatistics ServerUrl, Token and GameId: the Interposer
 *     sends them itself (and keeps unsent ones in .interposer\Matches\ across runs);
 *   - otherwise they're only logged.
 *
 * A race is the time statemgr spends in its race state (5). lc_stats_frame runs after every frame's sim and
 * follows it: entering the state starts the match; while racing, each boat that crosses the finish line is
 * an event; leaving it for post-race (6, after statemgr_EndRace) ends the match with the standings, and leaving
 * it any other way (operator menu, link error, quitting) aborts it. Attract-mode demo races never enter state 5.
 *
 * Players. All 16 boats race, humans and AI, and all of them are in the standings:
 *   - a human in a lobby is name:<lobby name>; LANCommander links that to the user whose alias it is, so
 *     PLAYER NAME should be the player's LANCommander alias;
 *   - a human on a direct link (no lobby, so no names on the wire) is unit:<n>, named only when it's this PC's;
 *   - the human of a single-player race is name:<PLAYER NAME>;
 *   - an AI boat is bot:<slot>.
 *
 * Linked races are reported by the host alone: the lobby's owner (lobby_owner), or, when the owner isn't in
 * the race or there's no lobby (a direct link), the race's lowest unit. Every unit works this out the same way
 * from the race's seed mask, so exactly one of them reports; the lockstep sim gives it every boat's standings.
 * Its external id is a hash of the race seeds every unit exchanged before the start (net_link.c), the same on
 * every unit and different for every race, so even two units that both took themselves for the host would
 * still make one match. A host that was cut off from all the others mid-race raced on alone, so its standings
 * are its own: it ends the match as abandoned, without them.
 */
#include "lc_stats.h"

#include "host.h"
#include "lobby_client.h"
#include "settings.h"

#include "../common/sha256.h"
#include "hydro_syms.h"
#include "../game/net_link.h"

#include <windows.h>
#include <LANCommander.Interposer.h>
#include <stdio.h>
#include <string.h>

/* hydro_lancommander.dll (port/lancommander/hydro_lancommander.c) */
__declspec(dllimport) void __cdecl HydroLancommanderAttach(void);

/* ---------------------------------------------------------------- the game's state */

enum { ST_RACE = 5, ST_POSTRACE = 6 };
#define Statemgr_nState (**(int **)HY_VAR_Statemgr_pLocalData)
#define Player_nHuman HY_GLOBAL(uint32_t, Player_nHuman)
#define Player_nHumanCount HY_GLOBAL(uint32_t, Player_nHumanCount)
#define Player_nTotalCount HY_GLOBAL(uint32_t, Player_nTotalCount)
#define Player_apData ((uint8_t **)HY_VAR_Player_apData)
#define Player_fElapsedSecs HY_GLOBAL(float, Player_fElapsedSecs) /* race clock, zeroed by tracks_SetCurrentTrack */
#define Tracks_nCurrentTrack HY_GLOBAL(int, Tracks_nCurrentTrack)
#define Tracks_nNumberOfLaps HY_GLOBAL(int, Tracks_nNumberOfLaps)

#define PLAYER_FLAGS(p) (*(uint32_t *)(p)) /* bit 0 human, bit 1 local */
#define PLAYER_SLOT(p) (*(uint16_t *)((p) + 4))
#define PLAYER_BOAT(p) (*(uint16_t *)((p) + 6))
#define PLAYER_RANK(p) (*(uint32_t *)((p) + 0x14)) /* 1 = leading */
/* Seconds, to the hundredth, when the boat crossed the finish line (hud_TW_FinishLine); 0 until then. */
#define PLAYER_FINISH(p) (*(float *)((p) + 0x24))

/* Tracks_Info's names in the game's numbering. 9 and 12 have internal names there (TEST, LOOP3). */
static const char *const k_tracks[] = {
    "Ship Graveyard", "Lost Island",  "Venice Canals",  "Lake Powell", "Arctic Circle",
    "Nile Adventure", "New York Disaster", "Greek Isles", "Far East",  "Armageddon",
    "Thunder Park",   "Hydro Speedway",    "Catacombs",
};
/* Boats_Info's names in the game's numbering (PLAYER_BOAT). */
static const char *const k_boats[] = {
    "Banshee", "Tidal Blade", "Rad Hazard", "Miss Behave", "Damn the Torpedoes", "Cutthroat",  "Razorback",
    "Thresher", "Midway",     "Chumdinger", "Armed Response", "Blowfish",        "Tinytanic",
};
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static const char *boat_name(unsigned boat)
{
    return boat < (unsigned)COUNT(k_boats) ? k_boats[boat] : "Unknown";
}

/* ---------------------------------------------------------------- the race being reported */

static int g_on;      /* hydro_lancommander.dll is loaded */
static int g_racing;  /* statemgr is in a race */
static DWORD g_match; /* the race's match handle; 0 between races, or if the Interposer refused the start */

static struct {
    int linked;          /* more than one human: a linked race, reported by every unit */
    uint32_t start_group; /* the link's group size at the start */
    int cut_off;          /* linked, and this unit lost every other unit mid-race */
    unsigned boats;       /* how many of Player_apData race */
    uint8_t *player[16];  /* Player_apData at the start */
    char key[16][64];     /* per Player_apData entry */
    char name[16][48];
    int finished[16];     /* its finish line event was sent */
} g_race;

static long long race_ms(void)
{
    float s = Player_fElapsedSecs;
    return s > 0.0f ? (long long)(s * 1000.0f + 0.5f) : 0;
}

static long long finish_ms(const uint8_t *p)
{
    return (long long)(PLAYER_FINISH(p) * 1000.0f + 0.5f);
}

/* Names go to LANCommander as UTF-8; the game's are ASCII, so anything else becomes '?'. */
static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; src && *src && n + 1 < cap; src++)
        dst[n++] = (*src >= 0x20 && *src < 0x7f) ? *src : '?';
    while (n && dst[n - 1] == ' ')
        n--;
    dst[n] = 0;
}

/* The same id on every unit of a linked race: the seeds they all exchanged, which differ from race to race. */
static void linked_race_id(char out[40])
{
    const SeedData *s = &g_net_seed;
    uint32_t mask = Net_Link_SeedHeader.seed_mask;
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, "HydroThunder race", 17);
    sha256_update(&h, &mask, sizeof mask);
    sha256_update(&h, &s->track, sizeof s->track);
    for (int u = 0; u < NET_UNITS; u++)
        if (mask & (1u << u)) {
            sha256_update(&h, &u, sizeof u);
            sha256_update(&h, &s->u_random[u], sizeof s->u_random[u]);
            sha256_update(&h, &s->u_start_secs[u], sizeof s->u_start_secs[u]);
            sha256_update(&h, &s->u_boat[u], sizeof s->u_boat[u]);
            sha256_update(&h, &s->u_order[u], sizeof s->u_order[u]);
        }
    uint8_t d[32];
    sha256_final(&h, d);
    int n = snprintf(out, 40, "ht-");
    for (int i = 0; i < 16; i++)
        n += snprintf(out + n, 40 - n, "%02x", d[i]);
}

/* Player keys and names for every boat in the race (see the top of the file). */
static void name_players(void)
{
    char lobby_names[HTNET_MAX_UNITS][HTNET_NAME_LEN];
    unsigned lobby_mask = 0;
    int unit;
    if (lobby_active(&unit))
        lobby_mask = lobby_members(lobby_names);
    char own[48];
    copy_name(own, sizeof own, g_settings.net.player_name);
    if (!own[0])
        snprintf(own, sizeof own, "PLAYER");

    for (unsigned i = 0; i < g_race.boats; i++) {
        const uint8_t *p = g_race.player[i];
        unsigned slot = PLAYER_SLOT(p);
        char *key = g_race.key[i], *name = g_race.name[i];
        name[0] = 0;
        if (!(PLAYER_FLAGS(p) & 1)) {
            snprintf(key, 64, "bot:%u", slot + 1);
            snprintf(name, 48, "%s (CPU)", boat_name(PLAYER_BOAT(p)));
            continue;
        }
        if (slot < HTNET_MAX_UNITS && (lobby_mask & (1u << slot)))
            copy_name(name, 48, lobby_names[slot]);
        if (name[0]) {
            snprintf(key, 64, "name:%s", name);
        } else if (g_race.linked) {
            snprintf(key, 64, "unit:%u", slot + 1);
            if (slot == Player_nHuman)
                snprintf(name, 48, "%s", own);
        } else {
            snprintf(name, 48, "%s", own);
            snprintf(key, 64, "name:%s", name);
        }
        /* Two humans with the same name: the later slots get their own keys (the same on every unit). */
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(g_race.key[j], key)) {
                size_t n = strlen(key);
                snprintf(key + n, 64 - n, "#%u", slot + 1);
                break;
            }
    }
}

/* The unit that reports a linked race (see the top of the file): the same answer on every unit of the race. */
static int race_host(void)
{
    uint32_t mask = Net_Link_SeedHeader.seed_mask;
    int owner = lobby_active(NULL) ? lobby_owner() : -1;
    if (owner >= 0 && owner < NET_UNITS && (mask & (1u << owner)))
        return owner;
    for (int u = 0; u < NET_UNITS; u++)
        if (mask & (1u << u))
            return u;
    return -1;
}

static void race_start(void)
{
    memset(&g_race, 0, sizeof g_race);
    uint32_t total = Player_nTotalCount;
    uint32_t humans = Player_nHumanCount;
    g_race.linked = humans > 1;
    if (g_race.linked) {
        int host = race_host();
        if (host != (int)Player_nHuman) {
            hy_log("lancommander: linked race; unit %d hosts it and reports it", host + 1);
            return;
        }
    }
    g_race.boats = total < 16 ? total : 16;
    for (unsigned i = 0; i < g_race.boats; i++)
        g_race.player[i] = Player_apData[i];
    g_race.start_group = net_link_GetCurrentGroupSize();
    name_players();

    int track = Tracks_nCurrentTrack;
    char map[32], id[40], settings[160], lobby[HTNET_LOBBY_LEN];
    if (track >= 0 && track < COUNT(k_tracks))
        snprintf(map, sizeof map, "%s", k_tracks[track]);
    else
        snprintf(map, sizeof map, "Track %d", track);
    if (g_race.linked)
        linked_race_id(id);
    snprintf(settings, sizeof settings, "{\"track\":%d,\"laps\":%d,\"humans\":%u,\"ai\":%u,\"linked\":%s}", track,
             Tracks_nNumberOfLaps, humans, total > humans ? total - humans : 0, g_race.linked ? "true" : "false");

    InterposerMatchInfo info = {sizeof info};
    info.externalId = g_race.linked ? id : NULL; /* NULL: a new id for a single-player race */
    info.map = map;
    info.mode = "race";
    info.gameVersion = HYDRO_VERSION;
    copy_name(lobby, sizeof lobby, lobby_active(NULL) ? lobby_name() : NULL);
    info.serverName = lobby[0] ? lobby : NULL;
    info.settingsJson = settings;
    g_match = InterposerMatchStart(&info);
    if (!g_match) {
        hy_log("lancommander: the race on %s was not recorded", map);
        return;
    }
    for (unsigned i = 0; i < g_race.boats; i++)
        InterposerMatchPlayerJoined(g_match, g_race.key[i], g_race.name[i][0] ? g_race.name[i] : NULL, NULL, 0);
    hy_log("lancommander: race on %s started (%u humans, %u boats%s%s)", map, humans, g_race.boats,
           g_race.linked ? ", linked race " : "", g_race.linked ? id : "");
}

/* While racing: the finish line, and whether the link still has anyone else. */
static void race_poll(void)
{
    for (unsigned i = 0; i < g_race.boats; i++) {
        const uint8_t *p = g_race.player[i];
        if (g_race.finished[i] || PLAYER_FINISH(p) == 0.0f)
            continue;
        g_race.finished[i] = 1;
        char payload[96];
        snprintf(payload, sizeof payload, "{\"place\":%u,\"time_ms\":%lld,\"boat\":\"%s\"}", PLAYER_RANK(p),
                 finish_ms(p), boat_name(PLAYER_BOAT(p)));
        InterposerMatchEvent(g_match, "finished", race_ms(), -1, g_race.key[i], NULL, payload);
    }
    if (g_race.linked && g_race.start_group > 2 && net_link_GetCurrentGroupSize() == 1 && !g_race.cut_off) {
        g_race.cut_off = 1;
        hy_log("lancommander: the host lost the link to every other unit; the race will be reported abandoned");
    }
}

static void race_end(int completed)
{
    if (!completed || g_race.cut_off) {
        InterposerMatchEnd(g_match, g_race.cut_off ? "link_lost" : "aborted", TRUE);
        hy_log("lancommander: race %s", g_race.cut_off ? "abandoned: the link was lost" : "aborted");
        g_match = 0;
        return;
    }
    race_poll(); /* a boat that finished in the race's last frame */
    for (unsigned i = 0; i < g_race.boats; i++) {
        const uint8_t *p = g_race.player[i];
        int finished = PLAYER_FINISH(p) != 0.0f;
        char payload[64];
        snprintf(payload, sizeof payload, "{\"boat\":\"%s\"}", boat_name(PLAYER_BOAT(p)));
        InterposerMatchResult r = {sizeof r};
        r.playerKey = g_race.key[i];
        r.rank = (int)PLAYER_RANK(p);
        r.status = finished ? INTERPOSER_MATCH_RESULT_FINISHED : INTERPOSER_MATCH_RESULT_DID_NOT_FINISH;
        r.timeMs = finished ? finish_ms(p) : -1;
        r.payloadJson = payload;
        InterposerMatchSetResult(g_match, &r);
        if (finished)
            InterposerMatchSetStatistic(g_match, g_race.key[i], NULL, "finish_time_ms", (double)finish_ms(p));
    }
    InterposerMatchEnd(g_match, "completed", FALSE);
    hy_log("lancommander: race completed");
    g_match = 0;
}

/* ---------------------------------------------------------------- the host's calls */

void lc_stats_init(void)
{
    /* By full path: the game changes directory into data\C. The delay-load helper then finds it loaded. */
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!n || n >= MAX_PATH || !slash || (size_t)(slash + 1 - path) + 24 >= MAX_PATH)
        return;
    wcscpy(slash + 1, L"hydro_lancommander.dll");
    if (!LoadLibraryW(path)) {
        hy_log("lancommander: hydro_lancommander.dll not loaded (error %lu): races aren't reported", GetLastError());
        return;
    }
    HydroLancommanderAttach();
    g_on = 1;
    hy_log("lancommander: reporting races (match statistics %s)",
           (InterposerGetFeatures() & INTERPOSER_FEATURE_FLAG_MATCHES) ? "built in" : "missing");
}

void lc_stats_frame(void)
{
    if (!g_on)
        return;
    int state = Statemgr_nState;
    if (!g_racing) {
        if (state == ST_RACE) {
            g_racing = 1;
            race_start();
        }
    } else if (state == ST_RACE) {
        if (g_match)
            race_poll();
    } else {
        g_racing = 0;
        if (g_match)
            race_end(state == ST_POSTRACE);
    }
}

void lc_stats_shutdown(void)
{
    if (!g_on || !g_match)
        return;
    DWORD match = g_match;
    g_racing = 0;
    race_end(0);
    InterposerMatchFlush(match, 3000); /* returns at once unless the Interposer sends it to a server itself */
}
