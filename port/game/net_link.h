#pragma once
/*
 * NET_COMM / NET_LINK, ported with 16 units instead of 4 (docs/network.md). Shared with the other
 * ports that read the link's per-unit data (statemgr_link.c, player.c).
 */
#include <stdint.h>

#define NET_UNITS 16

/* Net_Link_SeedData: the header stays in the image at 0x62ae94 (statemgr's pre-race code reads
 * its masks and counts, which are 16-bit and hold 16 units); the merged race setup and the
 * per-unit arrays, which the original sized for 4, live here. Layout follows the original's. */
#pragma pack(push, 1)
typedef struct HiScore {
    uint8_t initials[4];
    float time;
} HiScore;

typedef struct PoleRecord {
    uint32_t unit, order, key;
} PoleRecord;

typedef struct SeedHeader { /* 0x62ae94 */
    uint16_t seed_mask, seed_count;   /* units whose seed arrived */
    uint16_t ready_count, ready_mask; /* type 14 */
    uint16_t lock_mask, lock_count;   /* type 12 */
} SeedHeader;

typedef struct SeedData {
    /* merged (statemgr_SetupLinkedRace) */
    HiScore merged_hiscores[NET_UNITS * 10];
    uint16_t merged_hiscore_count, track;
    float splits[5];
    uint32_t pole_count;
    PoleRecord poles[NET_UNITS];
    uint16_t difficulty;
    uint8_t no_timers, codes;
    float random, start_secs;
    float checkpoints[5];
    uint8_t rabbit_b0, rabbit_b1, rabbit_b2, rabbit_b3;
    float rabbit_f[4];
    uint32_t free_races;
    /* per unit */
    HiScore u_hiscores[NET_UNITS][10];
    uint16_t u_track[NET_UNITS];
    uint16_t u_boat[NET_UNITS];
    float u_splits[NET_UNITS][5];
    uint8_t u_order[NET_UNITS]; /* pre-race join order (1 + group replies seen) */
    uint8_t u_difficulty[NET_UNITS];
    uint8_t u_no_timers[NET_UNITS];
    uint8_t u_codes[NET_UNITS];
    float u_random[NET_UNITS];
    float u_start_secs[NET_UNITS];
    float u_checkpoints[NET_UNITS][5];
    uint8_t u_rabbit_b0[NET_UNITS], u_rabbit_b1[NET_UNITS], u_rabbit_b2[NET_UNITS], u_rabbit_b3[NET_UNITS];
    float u_rabbit_f0[NET_UNITS], u_rabbit_f1[NET_UNITS], u_rabbit_f2[NET_UNITS], u_rabbit_f3[NET_UNITS];
    uint8_t u_free_races[NET_UNITS];
} SeedData;
#pragma pack(pop)

#define Net_Link_SeedHeader (*(SeedHeader *)HY_VAR_Net_Link_SeedData)
extern SeedData g_net_seed;

/* Player_anHumanList, moved out of the image (it had room for 4, and Player_anRank follows it). */
extern uint32_t g_player_human_list[16];
