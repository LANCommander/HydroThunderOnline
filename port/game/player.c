/*
 * PLAYER.OBJ, the parts that use the human list. Player_anHumanList had room for 4 (Player_anRank
 * follows it), so for 16 linked units it moves here; its writers (player_ResetPlayersToDefaults,
 * statemgr_link.c) and its one reader (the AI-near-human static below) are ported together.
 */
#include "port.h"

#include "net_link.h"

typedef void(__cdecl *VoidFn)(void);

uint32_t g_player_human_list[16];

#define PLAYER_SIZE 0x32c
#define PLAYER(i) ((uint8_t *)(uintptr_t)(HY_VAR_Player_aData + (uint32_t)(i) * PLAYER_SIZE))
#define Player_anRank ((uint8_t *)HY_VAR_Player_anRank)
#define Player_apData ((uint8_t **)HY_VAR_Player_apData)
#define Player_afHumanFrameTime ((float *)HY_VAR_Player_afHumanFrameTime)
#define Player_nHuman HY_GLOBAL(uint32_t, Player_nHuman)
#define Player_nHumanCount HY_GLOBAL(uint32_t, Player_nHumanCount)
#define Player_nAiCount HY_GLOBAL(uint32_t, Player_nAiCount)
#define Player_nTotalCount HY_GLOBAL(uint32_t, Player_nTotalCount)
#define Player_nFirstPlace HY_GLOBAL(uint32_t, Player_nFirstPlace)
#define Player_nFirstPlaceHuman HY_GLOBAL(uint32_t, Player_nFirstPlaceHuman)
#define Player_nLocalPlaceAmongHumans HY_GLOBAL(uint32_t, Player_nLocalPlaceAmongHumans)
#define Player_bCheckForFreeRace HY_GLOBAL(int, Player_bCheckForFreeRace)
#define Player_bHumanWonFreeRace HY_GLOBAL(int, Player_bHumanWonFreeRace)
#define Player_bEnableHumanToHumanCheats HY_GLOBAL(int, Player_bEnableHumanToHumanCheats)
#define Temp_bLoadAi HY_GLOBAL(int, Temp_bLoadAi)
/* unnamed PLAYER statics */
#define Player_nPhysicsStatus (*(int *)0x0058953cu)
#define Player_n589544 (*(int *)0x00589544u)
#define Player_n589540 (*(int *)0x00589540u)

#define xclib_MemSet HY_CALL(void(__cdecl *)(void *, int, uint32_t), xclib_MemSet)
#define controls_ZeroControlStruct HY_CALL(void(__cdecl *)(void *), controls_ZeroControlStruct)
#define powerup_ResetBooster HY_CALL(void(__cdecl *)(void *), powerup_ResetBooster)

void player_ResetPlayersToDefaults(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t *p = PLAYER(i);
        xclib_MemSet(p, 0, PLAYER_SIZE);
        *(uint32_t *)(p + 0x14) = 1; /* rank */
        *(uint32_t *)(p + 0x20) = 1; /* pole */
        controls_ZeroControlStruct(p + 0x2c);
        powerup_ResetBooster(p + 0x18);
        *(uint16_t *)(p + 4) = (uint16_t)i;
    }
    g_player_human_list[0] = Player_nHuman;
    for (int i = 0; i < 4; i++)
        Player_afHumanFrameTime[i] = 0.015f;
    *(uint32_t *)PLAYER(Player_nHuman) = 3; /* human, local */
    Player_nFirstPlace = 0;
    Player_nFirstPlaceHuman = 0;
    Player_nLocalPlaceAmongHumans = 0;
    Player_nHumanCount = 1;
    if (!Temp_bLoadAi) {
        Player_nAiCount = 0;
        Player_nTotalCount = 1;
        Player_apData[0] = PLAYER(Player_nHuman);
    } else {
        Player_nAiCount = 15;
        Player_nTotalCount = 16;
        for (int i = 0; i < 16; i++)
            Player_apData[i] = PLAYER(i);
    }
    Player_bCheckForFreeRace = 0;
    Player_bHumanWonFreeRace = 0;
    Player_nPhysicsStatus = 0;
    Player_n589544 = 0;
    Player_n589540 = 1;
    Player_bEnableHumanToHumanCheats = 1;
}
HY_PORT(player_ResetPlayersToDefaults)

#define AI_FLAGS(p) (*(uint32_t *)((p) + 0x138))
#define AI_FAR 0x800u     /* no human close by */
#define AI_CHANGED 0x2000u
#define POS_X(p) (*(float *)(*(uint8_t **)((p) + 0x48) + 0xc0))
#define POS_Y(p) (*(float *)(*(uint8_t **)((p) + 0x48) + 0xc4))
#define NEAR_ENTER (*(const float *)0x001f0a90u) /* 490000 = 700^2 */
#define NEAR_LEAVE (*(const float *)0x001f0a8cu) /* 250000 = 500^2 */

/*
 * 0x167dd0, called by player_UpdateRank: sorts the AI boats into near a human (squared distance
 * to the closest one, with hysteresis) and far (AI_FAR); at most `budget` stay near, the
 * farthest of them go far.
 * UpdateRank passes 3 per human for 1-4 humans and garbage beyond; the budget is computed here
 * instead, which is the same for 1-4.
 */
static void __cdecl player_MarkAiNearHumans(uint32_t budget)
{
    (void)budget;
    budget = 3 * Player_nHumanCount;
    struct {
        uint8_t *p;
        float dist;
        int near;
    } ai[16];
    uint32_t n = 0, near = 0;
    for (uint32_t i = 0; i < Player_nTotalCount; i++) {
        uint8_t *p = Player_apData[i];
        if (*p & 1)
            continue;
        float best = 0.0f;
        for (uint32_t h = 0; h < Player_nHumanCount; h++) {
            uint8_t *hp = PLAYER(g_player_human_list[h]);
            float dy = POS_Y(p) - POS_Y(hp);
            float dx = POS_X(p) - POS_X(hp);
            float d = dx * dx + dy * dy;
            if (h == 0 || d < best)
                best = d;
        }
        ai[n].p = p;
        ai[n].dist = best;
        ai[n].near = (AI_FLAGS(p) & AI_FAR) ? best < NEAR_LEAVE : best <= NEAR_ENTER;
        near += ai[n].near;
        n++;
    }
    for (; near > budget; near--) { /* over budget: the farthest near one stops being near */
        int far = -1;
        for (uint32_t i = 0; i < n; i++)
            if (ai[i].near && (far < 0 || ai[far].dist < ai[i].dist))
                far = (int)i;
        ai[far].near = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = AI_FLAGS(ai[i].p);
        if (!ai[i].near && !(f & AI_FAR))
            AI_FLAGS(ai[i].p) = f | AI_FAR | AI_CHANGED;
        else if (ai[i].near && (f & AI_FAR))
            AI_FLAGS(ai[i].p) = (f & ~AI_FAR) | AI_CHANGED;
    }
}
HY_PORT_AT(player_MarkAiNearHumans, 0x00167dd0u)
