/*
 * STATEMGR.OBJ, the linked race setup (0x16e2d0, an unnamed static that pre-race runs once
 * every group member's seed has arrived). Every unit merges the same seeds the same way, so the
 * race is identical everywhere without a master: the lowest-priority track, the best split
 * times, the highest random seed, the AND of the secret codes. Ported for 16 units: it reads the
 * per-unit seed data from net_link.c and writes the human list to player.c's.
 */
#include "port.h"

#include "net_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/host.h"

typedef void(__cdecl *VoidFn)(void);

#define Hi_Score_Table ((HiScore *)HY_VAR_Hi_Score_Table)
#define Hud_SplitTimes ((float *)HY_VAR_Hud_SplitTimes)
#define Tracks_anTrackSelectionPriority ((const uint32_t *)HY_FN_Tracks_anTrackSelectionPriority)
#define Tracks_fCurrentStartSecs HY_GLOBAL(float, Tracks_fCurrentStartSecs)
#define Tracks_afCurrentChkPtSecs ((float *)HY_VAR_Tracks_afCurrentChkPtSecs)
#define Tracks_nCurrentTrack HY_GLOBAL(int, Tracks_nCurrentTrack)
#define Player_fCountdownSecs HY_GLOBAL(float, Player_fCountdownSecs)
#define Player_afHumanFrameTime ((float *)HY_VAR_Player_afHumanFrameTime) /* [4]: set, never read */
#define Player_nHumanCount HY_GLOBAL(uint32_t, Player_nHumanCount)
#define Player_nAiCount HY_GLOBAL(uint32_t, Player_nAiCount)
#define Player_nTotalCount HY_GLOBAL(uint32_t, Player_nTotalCount)
#define Player_apData ((uint8_t **)HY_VAR_Player_apData)
#define Temp_bNoTimers HY_GLOBAL(int, Temp_bNoTimers)

#define PLAYER_SIZE 0x32c
#define PLAYER(i) ((uint8_t *)(uintptr_t)(HY_VAR_Player_aData + (uint32_t)(i) * PLAYER_SIZE))
#define PLAYER_FLAGS(p) (*(uint32_t *)(p))        /* bit 0 human, bit 1 local */
#define PLAYER_INDEX(p) (*(uint16_t *)((p) + 4))
#define PLAYER_BOAT(p) (*(uint16_t *)((p) + 6))
#define PLAYER_RANK(p) (*(uint32_t *)((p) + 0x14))
#define PLAYER_POLE(p) (*(uint32_t *)((p) + 0x20))

#define hi_score_Compare2Scores HY_CALL(int(__cdecl *)(const HiScore *, const HiScore *), hi_score_Compare2Scores)
#define hi_score_CopyHiScoreStruct HY_CALL(void(__cdecl *)(HiScore *, const HiScore *), hi_score_CopyHiScoreStruct)
#define tracks_SetCurrentTrack HY_CALL(void(__cdecl *)(uint32_t, uint32_t), tracks_SetCurrentTrack)
#define player_ResetPlayersToDefaults HY_CALL(VoidFn, player_ResetPlayersToDefaults)
#define controls_ZeroCachedControls HY_CALL(VoidFn, controls_ZeroCachedControls)
#define audits_StartARace HY_CALL(VoidFn, audits_StartARace)
#define ai_rabbit_SetNetSyncedData                                                                     \
    HY_CALL(void(__cdecl *)(uint32_t, uint32_t, int, uint32_t, float, float, float, float), ai_rabbit_SetNetSyncedData)
#define race_SeedRandom HY_CALL(void(__cdecl *)(float), race_SeedRandom)
#define player_EnableHumanToHumanCheats HY_CALL(void(__cdecl *)(int), player_EnableHumanToHumanCheats)
#define player_SetCheckForFreeRaceVar HY_CALL(void(__cdecl *)(uint32_t, uint32_t), player_SetCheckForFreeRaceVar)

#define NO_SPLIT 0.0f /* _DAT_001f0e08 */
#define NO_TRACK 0x3039

/* Adds this unit's hiscores the merged table doesn't hold yet (as many copies as any unit has). */
static void merge_hiscores(SeedData *s, uint32_t mask, int unit)
{
    for (int i = 0; i < 10; i++) {
        const HiScore *score = &s->u_hiscores[unit][i];
        uint32_t most = 0;
        for (int u = 0; u < NET_UNITS; u++) {
            if (!(mask & (1u << u)))
                continue;
            uint32_t n = 0;
            for (int k = 0; k < 10; k++)
                if (hi_score_Compare2Scores(score, &s->u_hiscores[u][k]))
                    n++;
            if (most < n)
                most = n;
        }
        uint32_t have = 0;
        for (uint32_t k = 0; k < s->merged_hiscore_count; k++)
            if (hi_score_Compare2Scores(score, &s->merged_hiscores[k]))
                have++;
        if (have < most)
            hi_score_CopyHiScoreStruct(&s->merged_hiscores[s->merged_hiscore_count++], score);
    }
}

static void statemgr_SetupLinkedRace(void)
{
    SeedData *s = &g_net_seed;
    uint32_t mask = Net_Link_SeedHeader.seed_mask;

    s->merged_hiscore_count = 0;
    s->track = NO_TRACK;
    s->pole_count = 0;
    s->difficulty = NO_TRACK;
    s->no_timers = 0;
    s->codes = 3;
    s->random = 0.0f;
    s->start_secs = 0.0f;
    s->rabbit_b0 = s->rabbit_b1 = 0;
    s->rabbit_b2 = 0x7b;
    s->rabbit_b3 = 0;
    memset(s->rabbit_f, 0, sizeof s->rabbit_f);
    s->free_races = 0;
    memset(s->splits, 0, sizeof s->splits);
    memset(s->checkpoints, 0, sizeof s->checkpoints);

    for (int unit = 0; unit < NET_UNITS; unit++) {
        if (!(mask & (1u << unit)))
            continue;
        merge_hiscores(s, mask, unit);
        uint16_t track = s->u_track[unit];
        if (s->track == NO_TRACK ||
            Tracks_anTrackSelectionPriority[track] < Tracks_anTrackSelectionPriority[s->track])
            s->track = track;
        for (int k = 0; k < 5; k++) {
            float v = s->u_splits[unit][k];
            if (s->splits[k] == NO_SPLIT)
                s->splits[k] = v;
            else if (v != NO_SPLIT && v < s->splits[k])
                s->splits[k] = v;
        }
        PoleRecord *pole = &s->poles[s->pole_count++];
        pole->unit = (uint32_t)unit;
        pole->order = s->u_order[unit];
        pole->key = (uint32_t)unit + s->u_order[unit] * NET_UNITS; /* the original: * 4 */
        if (s->u_difficulty[unit] < s->difficulty)
            s->difficulty = s->u_difficulty[unit];
        if (s->u_no_timers[unit])
            s->no_timers = 1;
        if (!(s->u_codes[unit] & 1))
            s->codes &= ~1;
        if (!(s->u_codes[unit] & 2))
            s->codes &= ~2;
        if (s->random < s->u_random[unit])
            s->random = s->u_random[unit];
        if (s->start_secs < s->u_start_secs[unit])
            s->start_secs = s->u_start_secs[unit];
        for (int k = 0; k < 5; k++)
            if (s->checkpoints[k] < s->u_checkpoints[unit][k])
                s->checkpoints[k] = s->u_checkpoints[unit][k];
        if (s->u_rabbit_b0[unit])
            s->rabbit_b0 = 1;
        if (s->u_rabbit_b1[unit])
            s->rabbit_b1 = 1;
        if (s->u_rabbit_b2[unit] < s->rabbit_b2)
            s->rabbit_b2 = s->u_rabbit_b2[unit];
        if (s->rabbit_b3 < s->u_rabbit_b3[unit])
            s->rabbit_b3 = s->u_rabbit_b3[unit];
        if (s->rabbit_f[0] < s->u_rabbit_f0[unit])
            s->rabbit_f[0] = s->u_rabbit_f0[unit];
        if (s->rabbit_f[1] < s->u_rabbit_f1[unit])
            s->rabbit_f[1] = s->u_rabbit_f1[unit];
        if (s->rabbit_f[2] < s->u_rabbit_f2[unit])
            s->rabbit_f[2] = s->u_rabbit_f2[unit];
        if (s->rabbit_f[3] < s->u_rabbit_f3[unit])
            s->rabbit_f[3] = s->u_rabbit_f3[unit];
        if (s->free_races < s->u_free_races[unit])
            s->free_races = s->u_free_races[unit];
    }

    tracks_SetCurrentTrack(s->track, s->difficulty);
    Tracks_fCurrentStartSecs = s->start_secs;
    Player_fCountdownSecs = s->start_secs;
    memcpy(Tracks_afCurrentChkPtSecs, s->checkpoints, sizeof s->checkpoints);
    player_ResetPlayersToDefaults();
    controls_ZeroCachedControls();
    for (int i = 0; i < 4; i++)
        Player_afHumanFrameTime[i] = 0.015f;

    /* Humans take the slot of their unit id; the AI (unless the NoAI code is on) fill up to 16. */
    Player_nHumanCount = Net_Link_SeedHeader.seed_count;
    uint32_t ai = (s->codes & 1) ? 0 : 16 - Player_nHumanCount;
    uint32_t total = ai + Player_nHumanCount;
    Player_nAiCount = ai;
    Player_nTotalCount = total;
    for (int u = 0; u < NET_UNITS; u++)
        if (mask & (1u << u)) {
            PLAYER_FLAGS(PLAYER(u)) |= 1;
            PLAYER_BOAT(PLAYER(u)) = s->u_boat[u];
        }
    if (!ai) {
        uint8_t **out = Player_apData;
        for (int u = 0; u < NET_UNITS; u++)
            if (PLAYER_FLAGS(PLAYER(u)) & 1)
                *out++ = PLAYER(u);
    } else {
        for (uint32_t i = 0; i < total; i++)
            Player_apData[i] = PLAYER(i);
    }
    audits_StartARace();
    uint32_t *human = g_player_human_list;
    for (uint32_t i = 0; i < Player_nTotalCount; i++)
        if (PLAYER_FLAGS(Player_apData[i]) & 1)
            *human++ = PLAYER_INDEX(Player_apData[i]);
    Temp_bNoTimers = (int8_t)s->no_timers;
    ai_rabbit_SetNetSyncedData(s->rabbit_b3, s->free_races, (int8_t)s->rabbit_b1, s->rabbit_b2, s->rabbit_f[0],
                               s->rabbit_f[1], s->rabbit_f[2], s->rabbit_f[3]);

    /* Pole positions: by pre-race join order, then unit. */
    uint32_t n = Net_Link_SeedHeader.seed_count;
    for (uint32_t i = 0; i + 1 < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            if (s->poles[j].key < s->poles[i].key) {
                PoleRecord t = s->poles[i];
                s->poles[i] = s->poles[j];
                s->poles[j] = t;
            }
    if (getenv("HYDRO_NET_LOG") && atoi(getenv("HYDRO_NET_LOG"))) {
        char line[400];
        int k = snprintf(line, sizeof line, "track %u difficulty %u random %.6f codes %u poles", s->track,
                         s->difficulty, s->random, s->codes);
        for (uint32_t i = 0; i < n && k < (int)sizeof line - 16; i++)
            k += snprintf(line + k, sizeof line - k, " %u(%u)", s->poles[i].unit + 1, s->poles[i].key);
        hy_log("net: linked race: %s", line);
    }
    for (uint32_t i = 0; i < n; i++) {
        s->poles[i].order = i + 1;
        PLAYER_POLE(PLAYER(s->poles[i].unit)) = i + 1;
        PLAYER_RANK(PLAYER(s->poles[i].unit)) = i + 1;
    }

    /* A real linked race: the merged hiscores and best splits become this track's. */
    if (n > 1) {
        uint32_t count = s->merged_hiscore_count;
        for (uint32_t i = 0; i + 1 < count; i++)
            for (uint32_t j = i + 1; j < count; j++)
                if (s->merged_hiscores[j].time < s->merged_hiscores[i].time) {
                    HiScore t = s->merged_hiscores[i];
                    s->merged_hiscores[i] = s->merged_hiscores[j];
                    s->merged_hiscores[j] = t;
                }
        for (int i = 0; i < 10; i++)
            hi_score_CopyHiScoreStruct(&Hi_Score_Table[Tracks_nCurrentTrack * 10 + i], &s->merged_hiscores[i]);
        memcpy(&Hud_SplitTimes[Tracks_nCurrentTrack * 5], s->splits, sizeof s->splits);
    }
    race_SeedRandom(s->random);
    if (s->codes & 2)
        player_EnableHumanToHumanCheats(0);
    player_SetCheckForFreeRaceVar(s->codes, Player_nHumanCount);
}
HY_PORT_AT(statemgr_SetupLinkedRace, 0x0016e2d0u)
