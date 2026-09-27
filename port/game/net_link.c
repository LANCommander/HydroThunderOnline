/*
 * NET_COMM.OBJ and NET_LINK.OBJ: the linked-play protocol above NETDRIVER (docs/network.md),
 * ported with room for 16 units. The original's per-unit tables have 4 entries, its unit loops
 * stop at 4, and it uses 4 as the "multicast" destination; everything else, the message formats
 * included, is unchanged. The module's state lives here instead of at 0x575eb8-0x579d53.
 *
 * Port every function here together (HYDRO_UNHOOK can't split this module), and with
 * statemgr_link.c and player.c, which read the per-unit seed data and the human list.
 */
#include "port.h"

#include "net_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/host.h"

typedef void(__cdecl *VoidFn)(void);

/* ---------------------------------------------------------------- image globals and calls */

#define OperatorSettings ((int *)HY_VAR_OperatorSettings)
#define Statemgr_nState (**(int **)HY_VAR_Statemgr_pLocalData)
#define Net_Link_nFlags HY_GLOBAL(uint32_t, Net_Link_nFlags)
#define Net_Link_nSelectMask HY_GLOBAL(uint32_t, Net_Link_nSelectMask)
#define Net_Link_nTrackNum HY_GLOBAL(int, Net_Link_nTrackNum)
#define Net_Link_nBoatNum HY_GLOBAL(int, Net_Link_nBoatNum)
#define Net_Link_nAttractSyncCount HY_GLOBAL(int, Net_Link_nAttractSyncCount)
#define Net_Link_bNewAttractMember HY_GLOBAL(int, Net_Link_bNewAttractMember)
#define Net_Link_bAdvanceAttractStage HY_GLOBAL(int, Net_Link_bAdvanceAttractStage)
#define Net_Link_bResetAttractMode HY_GLOBAL(int, Net_Link_bResetAttractMode)
#define Net_Link_nSecretCodes HY_GLOBAL(uint8_t, Net_Link_nSecretCodes)
#define Net_Link_nNumFreeRacesInARow HY_GLOBAL(uint8_t, Net_Link_nNumFreeRacesInARow)
#define Player_nHuman HY_GLOBAL(uint32_t, Player_nHuman)
#define Temp_bNoTimers HY_GLOBAL(uint8_t, Temp_bNoTimers)
#define Hi_Score_Table ((HiScore *)HY_VAR_Hi_Score_Table)                  /* [track][10] */
#define Hud_SplitTimes ((float *)HY_VAR_Hud_SplitTimes)                    /* [track][5] */
#define Tracks_anTrackSelectionPriority ((const uint32_t *)HY_FN_Tracks_anTrackSelectionPriority)
/* Operator settings the seed carries (OPERATOR / CMOS) */
#define Op_anTrackDifficulty ((int *)0x0062b350u) /* per track */
#define Op_nRabbit0 (*(uint8_t *)0x0062b334u)
#define Op_nRabbit1 (*(uint8_t *)0x0062b3b8u)
#define Op_nRabbit2 (*(uint8_t *)0x0062b324u)
#define Op_anRabbit3 ((uint8_t *)0x0062b384u) /* per track, stride 4 */

/* The human controls in Player_aData[unit] (+0x2c; the struct is 0x32c bytes). */
#define PLAYER_CONTROLS(unit) ((void *)(uintptr_t)(0x0062b484u + (uint32_t)(unit) * 0x32cu))

#define xclib_MemCopy HY_CALL(void(__cdecl *)(void *, const void *, uint32_t), xclib_MemCopy)
#define netdriver_Init HY_CALL(int(__cdecl *)(void *, NetAddr *, NetAddr *, NetAddr *), netdriver_Init)
#define netdriver_Shutdown HY_CALL(VoidFn, netdriver_Shutdown)
#define netdriver_Send HY_CALL(int(__cdecl *)(const void *, int, NetAddr *), netdriver_Send)
#define netdriver_SendToId HY_CALL(int(__cdecl *)(const void *, int, uint32_t), netdriver_SendToId)
#define netdriver_Recieve HY_CALL(int(__cdecl *)(void), netdriver_Recieve)
#define netdriver_GetAddressFromNetId HY_CALL(int(__cdecl *)(uint32_t, NetAddr *), netdriver_GetAddressFromNetId)
#define netdriver_EnableMulticasting HY_CALL(int(__cdecl *)(int), netdriver_EnableMulticasting)
#define timer_AllocTimer HY_CALL(uint32_t(__cdecl *)(void), timer_AllocTimer)
#define timer_ReleaseTimer HY_CALL(void(__cdecl *)(uint32_t), timer_ReleaseTimer)
#define timer_ResetTime HY_CALL(void(__cdecl *)(uint32_t, uint32_t), timer_ResetTime)
#define timer_ElapsedTicks HY_CALL(uint32_t(__cdecl *)(uint32_t), timer_ElapsedTicks)
#define timer_Update HY_CALL(VoidFn, timer_Update)
#define gameinit_GetCodeChecksum HY_CALL(uint32_t(__cdecl *)(void), gameinit_GetCodeChecksum)
#define r2file_GetComprehensiveChecksum HY_CALL(uint32_t(__cdecl *)(void), r2file_GetComprehensiveChecksum)
#define controls_CopyCachedControls HY_CALL(void(__cdecl *)(void *), controls_CopyCachedControls)
#define controls_CopyControlStruct HY_CALL(void(__cdecl *)(void *, const void *), controls_CopyControlStruct)
#define controls_ZeroControlStruct HY_CALL(void(__cdecl *)(void *), controls_ZeroControlStruct)
#define hi_score_CopyHiScoreStruct HY_CALL(void(__cdecl *)(HiScore *, const HiScore *), hi_score_CopyHiScoreStruct)
#define xmath_RandomFloat HY_CALL(float(__cdecl *)(void), xmath_RandomFloat)
#define tracks_GetStartingSecs HY_CALL(float(__cdecl *)(int, int), tracks_GetStartingSecs)
#define tracks_GetCheckptSecs HY_CALL(float(__cdecl *)(int, int, int), tracks_GetCheckptSecs)
#define ai_rabbit_GetUnsyncedData                                                                      \
    HY_CALL(void(__cdecl *)(int, float *, float *, float *, float *), ai_rabbit_GetUnsyncedData)
#define audits_LogALostLinkDuringARace HY_CALL(VoidFn, audits_LogALostLinkDuringARace)

/* ---------------------------------------------------------------- packets */

typedef struct NetAddr {
    uint32_t ip;
    void *sa;
} NetAddr;

#pragma pack(push, 1)
typedef struct PktHeader {
    uint16_t unit, type;
    uint32_t ip; /* the sender's */
} PktHeader;

typedef struct GenericPkt { /* 0x18 */
    PktHeader h;
    union {
        uint32_t a;
        struct {
            uint16_t a_lo, a_hi;
        };
    };
    uint32_t b, c, d;
} GenericPkt;

typedef struct Controls {
    uint8_t raw[0x1c];
} Controls;

typedef struct InputPkt { /* 0x48, type 20 */
    PktHeader h;
    uint32_t frame;
    Controls next; /* the controls for the next frame */
    Controls cur;  /* the ones used this frame */
    uint32_t iters;
} InputPkt;

typedef struct SeedPkt { /* 0xa8, type 19 */
    PktHeader h;
    HiScore hiscores[10];
    uint16_t track, boat;
    float splits[5];
    uint8_t order, difficulty, no_timers, codes;
    float random, start_secs;
    float checkpoints[5];
    uint8_t rabbit_b0, rabbit_b1, rabbit_b2, rabbit_b3;
    float rabbit_f0, rabbit_f1, rabbit_f2, rabbit_f3;
    uint32_t free_races;
} SeedPkt;
#pragma pack(pop)

enum {
    MSG_HELLO, MSG_HELLO_ACK, MSG_ONLINE, MSG_SELECT_STATE, MSG_ATTRACT_JOIN, MSG_ATTRACT_REPLY,
    MSG_ATTRACT_DONE, MSG_ATTRACT_ADVANCE, MSG_ATTRACT_RESET, MSG_SELECT_ENTER, MSG_TRACK,
    MSG_SELECT_LEAVE, MSG_GROUP_LOCK, MSG_GROUP_REPLY, MSG_READY, MSG_HEARTBEAT, MSG_RELINK,
    MSG_RELINK_DONE, MSG_ERROR, MSG_SEED, MSG_INPUT,
    MSG_INPUT_BATCH /* port-only: race input with an input delay (see "Input delay") */
};

enum { SEND_ALL, SEND_UNIT, SEND_MULTICAST, SEND_IP }; /* net_comm_TransmitMessage modes */

/* Port-only: a GROUP_LOCK also carries the sender's lock mask (b), marked by d (fields the
 * message doesn't use otherwise), so a lost lock gets answered again; see MSG_GROUP_LOCK. */
#define LOCK_MASK_MARK 0x4b434f4cu /* "LOCK" */

#define DEST_ALL 0x100u    /* multicast (the original's 4, which is a unit id now) */
#define DEST_NONE 0x17cfbu /* nobody else in the group */

enum { ST_LINK = 1, ST_ATTRACT, ST_SELECT, ST_PRERACE, ST_RACE, ST_HISCORE, ST_ERROR, ST_RELINK };

/* ================================================================ NET_COMM */

/* The original has 30; 16 units' lock and seed rounds can burst past that in one frame. */
#define POOL_SLOTS 512

typedef int(__cdecl *PacketHandler)(void *pkt, int len);

static struct {
    int multicast; /* 0x575eb8 */
    struct {
        int used;
        int len;
        uint8_t data[0x200];
    } pool[POOL_SLOTS];
    NetAddr unit[NET_UNITS];
    NetAddr all, mcast;
    PacketHandler handler;
    uint32_t my_unit;
    int up;
} nc;

static void nc_clear(void)
{
    nc.up = 0;
    nc.handler = NULL;
    nc.my_unit = 0;
    nc.multicast = 0;
    memset(nc.unit, 0, sizeof nc.unit);
    memset(&nc.all, 0, sizeof nc.all);
    memset(&nc.mcast, 0, sizeof nc.mcast);
}

int net_comm_ModuleInit(void)
{
    memset(nc.pool, 0, sizeof nc.pool);
    nc_clear();
    return 1;
}
HY_PORT(net_comm_ModuleInit)

/* NETDRIVER hands every datagram here; it waits in the pool for ProcessMsgsInBufferPool. */
static void __cdecl net_comm_RecvCallback(const void *buf, uint32_t len)
{
    if (len > 0x200)
        len = 0x200;
    if (!nc.up)
        return;
    for (int i = 0; i < POOL_SLOTS; i++)
        if (!nc.pool[i].used) {
            xclib_MemCopy(nc.pool[i].data, buf, len);
            nc.pool[i].len = (int)len;
            nc.pool[i].used = 1;
            return;
        }
    hy_log("net: receive pool full, packet dropped");
}

/* id[0] = my unit; returns my net id (IP) in id[1]. */
int net_comm_Init(uint32_t *id, PacketHandler handler)
{
    nc.my_unit = id[0];
    nc.up = nc.my_unit < NET_UNITS && netdriver_Init(net_comm_RecvCallback, &nc.unit[nc.my_unit], &nc.mcast, &nc.all);
    if (!nc.up) {
        nc_clear();
        return 0;
    }
    nc.multicast = 0;
    nc.handler = handler;
    id[1] = nc.unit[nc.my_unit].ip;
    return 1;
}
HY_PORT(net_comm_Init)

void net_comm_Shutdown(void)
{
    memset(nc.pool, 0, sizeof nc.pool);
    nc_clear();
    netdriver_Shutdown();
}
HY_PORT(net_comm_Shutdown)

int net_comm_TransmitMessage(int mode, uint32_t dest, const void *buf, int len)
{
    if (!nc.up)
        return 1;
    switch (mode) {
    case SEND_ALL:
        return netdriver_Send(buf, len, &nc.all);
    case SEND_UNIT:
        if (dest != nc.my_unit && dest < NET_UNITS)
            return netdriver_Send(buf, len, &nc.unit[dest]);
        break;
    case SEND_MULTICAST:
        if (nc.multicast)
            return netdriver_Send(buf, len, &nc.mcast);
        break;
    case SEND_IP:
        return netdriver_SendToId(buf, len, dest);
    }
    return 1;
}
HY_PORT(net_comm_TransmitMessage)

int net_comm_ProcessMsgsInBufferPool(void)
{
    int count = 0;
    if (!nc.up)
        return 0;
    netdriver_Recieve();
    for (int i = 0; i < POOL_SLOTS; i++)
        if (nc.pool[i].used) {
            count++;
            if (nc.handler(nc.pool[i].data, nc.pool[i].len))
                nc.pool[i].used = 0;
        }
    return count;
}
HY_PORT(net_comm_ProcessMsgsInBufferPool)

void net_comm_EnableLocalMulticasting(int on)
{
    int ok = 1;
    if (on ? !nc.multicast : nc.multicast)
        ok = netdriver_EnableMulticasting(on ? 1 : 0);
    if (ok == 1)
        nc.multicast = on;
}
HY_PORT(net_comm_EnableLocalMulticasting)

int net_comm_RegisterAUnit(uint32_t ip, uint32_t unit)
{
    if (unit < NET_UNITS && unit != nc.my_unit)
        return netdriver_GetAddressFromNetId(ip, &nc.unit[unit]);
    return 0;
}
HY_PORT(net_comm_RegisterAUnit)

/* ================================================================ NET_LINK */

SeedData g_net_seed;

static struct {
    uint32_t iters_in;       /* 0x579bec: PreWorkReceive's argument */
    uint32_t last_heartbeat; /* 0x579bf0 */
    InputPkt input;          /* 0x579bf4 */
    uint32_t timer;          /* 0x579c3c: link clock (ticks) */
    uint32_t code_checksum;  /* 0x579c40 */
    uint32_t work_iters;     /* 0x579c44: max sim iterations of the group this frame */
    SeedPkt seed;            /* 0x579c4c */
    uint32_t dest;           /* 0x579cf4: race traffic goes to a unit, DEST_ALL or DEST_NONE */
    uint8_t up;              /* 0x579cfc */
    uint8_t connected;       /* 0x579cfd: link-up finished */
    uint8_t my_unit;         /* 0x579cfe */
    uint16_t my_mask;        /* 0x579cff (8-bit in the original) */
    uint8_t online_count;    /* 0x579d00 */
    uint16_t online_mask;    /* 0x579d01 (8-bit in the original) */
    uint16_t online_history; /* 0x579d02 */
    uint32_t heard_from[NET_UNITS]; /* 0x579d04: IP, 0 = not heard from */
    uint8_t group_size;             /* 0x579d14 */
    uint8_t group_lock;             /* 0x579d15 */
    uint16_t group_mask;            /* 0x579d16 */
    uint32_t last_heard[NET_UNITS]; /* 0x579d18 */
    uint32_t input_mask;            /* 0x579d28: units whose input for this frame arrived */
    GenericPkt gen;                 /* 0x579d2c */
    uint32_t frame;                 /* 0x579d44 */
    uint32_t now;                   /* 0x579d48 */
    uint32_t data_checksum;         /* 0x579d4c */
    uint32_t resend_timer;          /* 0x579d50 */
} nl;

/* The original's memset(0x579cfc, 0, 0x2c): up .. last_heard. */
static void nl_clear_state(void)
{
    nl.up = nl.connected = nl.my_unit = 0;
    nl.my_mask = 0;
    nl.online_count = 0;
    nl.online_mask = 0;
    nl.online_history = 0;
    memset(nl.heard_from, 0, sizeof nl.heard_from);
    nl.group_size = nl.group_lock = 0;
    nl.group_mask = 0;
    memset(nl.last_heard, 0, sizeof nl.last_heard);
}

static void clear_link_flags(void)
{
    Net_Link_nFlags = 0;
    Net_Link_bNewAttractMember = 0;
    Net_Link_bAdvanceAttractStage = 0;
    Net_Link_bResetAttractMode = 0;
    Net_Link_nSecretCodes = 0;
    *(uint32_t *)HY_VAR_Net_Link_nSecretCodes = 0;
    *(uint32_t *)HY_VAR_Net_Link_nNumFreeRacesInARow = 0;
}

int net_link_SendGenericMsg(uint16_t type, int mode, uint32_t dest);
static void delay_start_race(void);
static void net_link_RegisterUnit(int add, uint32_t unit, uint32_t ip);
static void net_link_DropUnit(uint32_t unit);

int net_link_ModuleInit(void)
{
    nl_clear_state();
    uint8_t unit = (uint8_t)OperatorSettings[0];
    nl.my_mask = (uint16_t)(1u << (unit & 0x1f));
    nl.group_mask = nl.my_mask;
    nl.my_unit = unit;
    nl.group_size = 1;
    nl.gen.h.ip = 0;
    nl.seed.h.type = MSG_SEED;
    nl.seed.h.ip = 0;
    nl.gen.h.unit = nl.seed.h.unit = nl.input.h.unit = unit;
    nl.input.h.type = MSG_INPUT;
    nl.input.h.ip = 0;
    Net_Link_nFlags = 0;
    Net_Link_nAttractSyncCount = 0;
    Net_Link_nSelectMask = 0;
    Net_Link_nTrackNum = -1;
    Net_Link_nBoatNum = 0;
    memset(&Net_Link_SeedHeader, 0, sizeof Net_Link_SeedHeader);
    memset(&g_net_seed, 0, sizeof g_net_seed);
    nl.frame = 0;
    nl.input_mask = 0;
    nl.resend_timer = 0;
    nl.timer = 0;
    nl.last_heartbeat = 0;
    nl.now = 0;
    Net_Link_bNewAttractMember = 0;
    Net_Link_bAdvanceAttractStage = 0;
    Net_Link_bResetAttractMode = 0;
    *(uint32_t *)HY_VAR_Net_Link_nSecretCodes = 0;
    *(uint32_t *)HY_VAR_Net_Link_nNumFreeRacesInARow = 0;
    nl.work_iters = 1;
    nl.iters_in = 1;
    nl.dest = DEST_NONE;
    return 1;
}
HY_PORT(net_link_ModuleInit)

static int __cdecl net_link_HandlePacket(void *pkt, int len);

int net_link_Init(void)
{
    if (OperatorSettings[1]) { /* NETWORK ENABLED is off */
        Net_Link_nFlags = 1;
        nl.up = 0;
        return 0;
    }
    uint32_t id[2] = {nl.my_unit, 0};
    if (!net_comm_Init(id, net_link_HandlePacket)) {
        Net_Link_nFlags = 6;
        nl.up = 0;
        return 0;
    }
    nl.resend_timer = timer_AllocTimer();
    nl.timer = timer_AllocTimer();
    timer_ResetTime(nl.timer, nl.my_unit * 0x6fu + 0x1bu); /* staggers the heartbeats */
    nl.group_mask = nl.my_mask;
    nl.group_size = 1;
    nl.last_heartbeat = 0;
    nl.now = 0;
    Net_Link_nFlags = 0;
    nl.heard_from[nl.my_unit] = id[1];
    nl.up = 1;
    nl.gen.h.ip = nl.seed.h.ip = nl.input.h.ip = id[1];
    Net_Link_bNewAttractMember = 0;
    Net_Link_bAdvanceAttractStage = 0;
    Net_Link_bResetAttractMode = 0;
    *(uint32_t *)HY_VAR_Net_Link_nSecretCodes = 0;
    *(uint32_t *)HY_VAR_Net_Link_nNumFreeRacesInARow = 0;
    nl.code_checksum = gameinit_GetCodeChecksum() ^ 0x10000u;
    nl.data_checksum = r2file_GetComprehensiveChecksum();
    return 1;
}
HY_PORT(net_link_Init)

void net_link_Shutdown(void)
{
    net_comm_Shutdown();
    nl_clear_state();
    nl.my_unit = (uint8_t)OperatorSettings[0];
    nl.my_mask = (uint16_t)(1u << (nl.my_unit & 0x1f));
    nl.group_mask = nl.my_mask;
    nl.group_size = 1;
    timer_ReleaseTimer(nl.resend_timer);
    timer_ReleaseTimer(nl.timer);
    nl.last_heartbeat = 0;
    nl.now = 0;
    nl.gen.h.ip = nl.seed.h.ip = nl.input.h.ip = 0;
    clear_link_flags();
    nl.dest = DEST_NONE;
}
HY_PORT(net_link_Shutdown)

/* Where race traffic goes for the current group: nobody, the one other unit, or everyone. */
static void resolve_dest(void)
{
    switch (nl.group_size) {
    case 0:
        break;
    case 1:
        nl.dest = DEST_NONE;
        break;
    case 2:
        for (uint32_t u = 0; u < NET_UNITS; u++) {
            uint32_t bit = 1u << u;
            if (bit != nl.my_mask && (nl.group_mask & bit)) {
                nl.dest = u;
                break;
            }
        }
        break;
    default: /* the original: 3 or 4 */
        net_comm_EnableLocalMulticasting(1);
        nl.dest = DEST_ALL;
        break;
    }
}

/* Race traffic (seed, input) to the group. Returns what TransmitMessage did (1 = nothing to send). */
static int send_to_group(const void *pkt, int len)
{
    if (nl.dest == DEST_ALL)
        return net_comm_TransmitMessage(SEND_MULTICAST, 0, pkt, len);
    if (nl.dest == DEST_NONE)
        return 1;
    return net_comm_TransmitMessage(SEND_UNIT, nl.dest, pkt, len);
}

int net_link_SendGameSeed(void)
{
    uint32_t me = nl.my_unit;
    int result = 1;
    if (!(Net_Link_SeedHeader.seed_mask & nl.my_mask)) {
        SeedData *s = &g_net_seed;
        uint16_t track = s->u_track[me];
        Net_Link_SeedHeader.seed_mask |= nl.my_mask;
        Net_Link_SeedHeader.seed_count++;
        s->u_boat[me] = (uint16_t)Net_Link_nBoatNum;
        s->u_codes[me] = Net_Link_nSecretCodes;
        s->u_free_races[me] = Net_Link_nNumFreeRacesInARow;
        for (int i = 0; i < 10; i++)
            hi_score_CopyHiScoreStruct(&nl.seed.hiscores[i], &Hi_Score_Table[track * 10 + i]);
        nl.seed.track = track;
        nl.seed.boat = s->u_boat[me];
        nl.seed.random = s->u_random[me];
        nl.seed.no_timers = s->u_no_timers[me];
        nl.seed.difficulty = s->u_difficulty[me];
        memcpy(nl.seed.splits, &Hud_SplitTimes[track * 5], sizeof nl.seed.splits);
        nl.seed.start_secs = s->u_start_secs[me];
        nl.seed.rabbit_f0 = s->u_rabbit_f0[me];
        nl.seed.order = s->u_order[me];
        nl.seed.codes = s->u_codes[me];
        nl.seed.rabbit_b0 = s->u_rabbit_b0[me];
        nl.seed.rabbit_b2 = s->u_rabbit_b2[me];
        memcpy(nl.seed.checkpoints, s->u_checkpoints[me], sizeof nl.seed.checkpoints);
        nl.seed.rabbit_f1 = s->u_rabbit_f1[me];
        nl.seed.rabbit_f2 = s->u_rabbit_f2[me];
        nl.seed.rabbit_f3 = s->u_rabbit_f3[me];
        nl.seed.rabbit_b1 = s->u_rabbit_b1[me];
        nl.seed.free_races = s->u_free_races[me];
        nl.seed.rabbit_b3 = s->u_rabbit_b3[me];
        nl.frame = 0;
        nl.input_mask = 0;
        delay_start_race();
    }
    if (nl.up) {
        resolve_dest();
        if (nl.dest == DEST_NONE)
            return 1;
        result = send_to_group(&nl.seed, sizeof nl.seed);
    }
    return result;
}
HY_PORT(net_link_SendGameSeed)

int net_link_SendErrorMsg(int16_t code, uint16_t unit)
{
    if (!nl.up || !code)
        return 1;
    nl.gen.h.type = MSG_ERROR;
    nl.gen.a_lo = (uint16_t)code;
    nl.gen.a_hi = unit;
    return net_comm_TransmitMessage(SEND_ALL, 0, &nl.gen, sizeof nl.gen);
}
HY_PORT(net_link_SendErrorMsg)

int net_link_SendGenericMsg(uint16_t type, int mode, uint32_t dest)
{
    if (!nl.up)
        return 1;
    switch (type) {
    case MSG_HELLO:
        nl.gen.b = nl.code_checksum;
        nl.gen.a = nl.heard_from[nl.my_unit];
        nl.gen.c = nl.data_checksum;
        break;
    case MSG_HELLO_ACK:
        nl.gen.b = nl.code_checksum;
        nl.gen.a = nl.heard_from[nl.my_unit];
        nl.gen.d = (uint32_t)(int8_t)nl.connected;
        nl.gen.c = nl.data_checksum;
        break;
    case MSG_ONLINE:
        nl.connected = 1;
        net_link_RegisterUnit(1, nl.my_unit, nl.heard_from[nl.my_unit]);
        break;
    case MSG_SELECT_STATE:
        nl.gen.a = (uint32_t)(uint16_t)Net_Link_nTrackNum << 16 | nl.my_mask;
        break;
    case MSG_SELECT_ENTER:
    case MSG_SELECT_LEAVE:
        nl.gen.a = nl.my_mask;
        break;
    case MSG_TRACK:
        nl.gen.a = (uint32_t)Net_Link_nTrackNum;
        break;
    case MSG_GROUP_LOCK:
        nl.gen.a = nl.group_mask;
        nl.gen.b = Net_Link_SeedHeader.lock_mask;
        nl.gen.d = LOCK_MASK_MARK;
        break;
    case MSG_READY:
        nl.gen.a = nl.my_mask;
        if (nl.dest == DEST_ALL) {
            mode = SEND_MULTICAST;
            dest = 0;
        } else {
            if (nl.dest == DEST_NONE)
                return 1;
            mode = SEND_UNIT;
            dest = nl.dest;
        }
        break;
    case MSG_RELINK:
        nl.gen.a = nl.heard_from[nl.my_unit];
        break;
    }
    nl.gen.h.type = type;
    return net_comm_TransmitMessage(mode, dest, &nl.gen, sizeof nl.gen);
}
HY_PORT(net_link_SendGenericMsg)

static void send_error(uint16_t code, uint32_t unit)
{
    if (!nl.up)
        return;
    nl.gen.h.type = MSG_ERROR;
    nl.gen.a_lo = code;
    nl.gen.a_hi = (uint16_t)unit;
    net_comm_TransmitMessage(SEND_ALL, 0, &nl.gen, sizeof nl.gen);
}

/* Heartbeat every 500 ticks; drop units not heard from in 1300. */
static void heartbeat_and_timeouts(void)
{
    nl.now = timer_ElapsedTicks(nl.timer);
    if (nl.now - nl.last_heartbeat > 499) {
        net_link_SendGenericMsg(MSG_HEARTBEAT, SEND_ALL, 0);
        nl.last_heartbeat = nl.now;
    }
    if (nl.online_count > 1)
        for (uint32_t u = 0; u < NET_UNITS; u++)
            if (u != nl.my_unit && (nl.online_mask & (1u << u)) && nl.now - nl.last_heard[u] > 0x513) {
                send_error(2, u);
                net_link_DropUnit(u);
            }
}

/* Port-only, HYDRO_NET_LOG=1: the pre-race group state whenever it changes. */
static void log_group_state(void)
{
    static int enabled = -1;
    static char last[128];
    if (enabled < 0) {
        const char *e = getenv("HYDRO_NET_LOG");
        enabled = e && atoi(e);
    }
    if (!enabled || Statemgr_nState != ST_PRERACE)
        return;
    const SeedHeader *h = &Net_Link_SeedHeader;
    char now[128];
    snprintf(now, sizeof now, "online %02x group %02x/%u lock %d locks %02x/%u seeds %02x/%u ready %02x/%u sel %02x",
             nl.online_mask, nl.group_mask, nl.group_size, nl.group_lock, h->lock_mask, h->lock_count, h->seed_mask,
             h->seed_count, h->ready_mask, h->ready_count, Net_Link_nSelectMask);
    if (strcmp(now, last)) {
        hy_log("net: prerace %s", now);
        strcpy(last, now);
    }
}

/* ---------------------------------------------------------------- input delay (port-only)
 * The original lockstep sends each frame's controls at the end of the previous frame, so every
 * frame waits out the one-way latency to the slowest peer: fine on a LAN, 20 fps or less over an
 * internet relay. With an input delay of D frames (D > 1), the controls sampled at the end of
 * frame f-1 apply to frame f+D-1 on every unit, the local one included, so a packet has D frames
 * to arrive. Each MSG_INPUT_BATCH carries the sender's last 8 frames, so a lost packet is covered
 * by the next one. All units of a race must use the same D: it comes from the lobby (or, without
 * a lobby, [Network] input_delay), and is fixed when the race's seed goes out. D = 1 runs the
 * original code. */

#define RING 16
#define BATCH 8

typedef struct BatchEntry {
    uint32_t frame;
    Controls ctl;
    uint32_t iters;
} BatchEntry;

#pragma pack(push, 1)
typedef struct InputBatchPkt {
    PktHeader h;
    uint32_t count;
    BatchEntry e[BATCH];
} InputBatchPkt;
#pragma pack(pop)

int __cdecl hle_net_input_delay(void); /* host/hle_net.c */

static struct {
    uint32_t delay;
    uint32_t tag[NET_UNITS][RING]; /* the frame each slot holds */
    Controls ctl[NET_UNITS][RING];
    uint32_t iters[NET_UNITS][RING];
    InputBatchPkt out;
} dl = {1};

static void delay_start_race(void)
{
    int d = hle_net_input_delay();
    dl.delay = d < 1 ? 1 : d > 6 ? 6 : (uint32_t)d;
    memset(dl.tag, 0xff, sizeof dl.tag);
    memset(dl.ctl, 0, sizeof dl.ctl);
    /* The first race frame is 1; frames 1..D-1 have nobody's input: idle controls, 1 iteration. */
    for (uint32_t u = 0; u < NET_UNITS; u++)
        for (uint32_t f = 0; f < dl.delay; f++) {
            dl.tag[u][f % RING] = f;
            dl.iters[u][f % RING] = 1;
        }
}

static int delay_have(uint32_t unit, uint32_t frame)
{
    return dl.tag[unit][frame % RING] == frame;
}

/* Group members (other than me) whose input for `frame` hasn't arrived. */
static uint32_t delay_missing(uint32_t frame)
{
    uint32_t missing = 0;
    for (uint32_t u = 0; u < NET_UNITS; u++)
        if (u != nl.my_unit && (nl.group_mask & (1u << u)) && !delay_have(u, frame))
            missing |= 1u << u;
    return missing;
}

static void delay_receive(const InputBatchPkt *p, uint32_t src)
{
    uint32_t n = p->count < BATCH ? p->count : BATCH;
    for (uint32_t i = 0; i < n; i++) {
        const BatchEntry *e = &p->e[i];
        if (e->frame < nl.frame || e->frame >= nl.frame + RING) /* stale, or too far ahead to hold */
            continue;
        uint32_t slot = e->frame % RING;
        dl.tag[src][slot] = e->frame;
        dl.ctl[src][slot] = e->ctl;
        dl.iters[src][slot] = e->iters;
    }
}

/* The race part of PreWorkReceive with a delay: wait for frame nl.frame's input, then apply it. */
static uint32_t delay_wait_and_apply(void)
{
    uint32_t f = nl.frame, slot = f % RING;
    uint32_t resends = 0, next_resend = nl.my_unit + 12u;
    timer_Update();
    timer_ResetTime(nl.resend_timer, 0);
    while (delay_missing(f)) {
        net_comm_ProcessMsgsInBufferPool();
        if (Statemgr_nState != ST_RACE)
            break;
        timer_Update();
        heartbeat_and_timeouts();
        uint32_t t = timer_ElapsedTicks(nl.resend_timer);
        if (resends < 7 && next_resend <= t) {
            resends++;
            next_resend += 12;
            if (dl.out.count)
                send_to_group(&dl.out, sizeof dl.out);
        } else if (t > 0x5d) {
            uint32_t missing = delay_missing(f);
            for (uint32_t u = 0; u < NET_UNITS; u++)
                if (missing & (1u << u)) {
                    send_error(3, u);
                    net_link_DropUnit(u);
                }
        }
    }
    uint32_t iters = delay_have(nl.my_unit, f) ? dl.iters[nl.my_unit][slot] : 1;
    for (uint32_t u = 0; u < NET_UNITS; u++)
        if (u != nl.my_unit && (nl.group_mask & (1u << u)) && delay_have(u, f)) {
            controls_CopyControlStruct(PLAYER_CONTROLS(u), &dl.ctl[u][slot]);
            if (iters < dl.iters[u][slot])
                iters = dl.iters[u][slot];
        }
    return iters;
}

/* End of frame f-1 (nl.frame is f now): what was just sampled applies to frame f+D-1. */
static void delay_send(void)
{
    uint32_t target = nl.frame + dl.delay - 1, me = nl.my_unit, slot = target % RING;
    dl.tag[me][slot] = target;
    controls_CopyCachedControls(&dl.ctl[me][slot]);
    dl.iters[me][slot] = nl.iters_in;
    if (!nl.up)
        return;
    dl.out.h = nl.input.h;
    dl.out.h.type = MSG_INPUT_BATCH;
    dl.out.count = 0;
    for (uint32_t i = 0; i < BATCH && i <= target; i++) {
        uint32_t fr = target - i;
        if (!delay_have(me, fr))
            break;
        BatchEntry *e = &dl.out.e[dl.out.count++];
        e->frame = fr;
        e->ctl = dl.ctl[me][fr % RING];
        e->iters = dl.iters[me][fr % RING];
    }
    send_to_group(&dl.out, sizeof dl.out);
}

/* Every frame before the sim: in a race, waits for every group member's input for this frame. */
uint32_t net_link_PreWorkReceive(uint32_t iters)
{
    nl.iters_in = iters;
    if (nl.up) {
        net_comm_ProcessMsgsInBufferPool();
        log_group_state();
        heartbeat_and_timeouts();
        if (Statemgr_nState == ST_RACE && dl.delay > 1) {
            nl.work_iters = delay_wait_and_apply();
            nl.input_mask = 0;
            controls_CopyControlStruct(PLAYER_CONTROLS(Player_nHuman), &dl.ctl[nl.my_unit][nl.frame % RING]);
            return nl.work_iters;
        }
        if (Statemgr_nState == ST_RACE) {
            uint32_t resends = 0, next_resend = nl.my_unit + 12u;
            timer_Update();
            timer_ResetTime(nl.resend_timer, 0);
            while (nl.input_mask != nl.group_mask) {
                net_comm_ProcessMsgsInBufferPool();
                if (Statemgr_nState != ST_RACE)
                    break;
                timer_Update();
                heartbeat_and_timeouts();
                uint32_t t = timer_ElapsedTicks(nl.resend_timer);
                if (resends < 7 && next_resend <= t) {
                    resends++;
                    next_resend += 12;
                    send_to_group(&nl.input, sizeof nl.input);
                } else if (t > 0x5d && nl.input_mask != nl.group_mask) {
                    for (uint32_t u = 0; u < NET_UNITS; u++) {
                        uint32_t bit = 1u << u;
                        if (u != nl.my_unit && (bit & nl.group_mask) && !(nl.input_mask & bit)) {
                            send_error(3, u);
                            net_link_DropUnit(u);
                        }
                    }
                }
            }
            nl.input_mask = 0;
        }
    }
    controls_CopyCachedControls(PLAYER_CONTROLS(Player_nHuman));
    return nl.work_iters;
}
HY_PORT(net_link_PreWorkReceive)

/* Port-only, HYDRO_NET_SYNCCHECK=N: every N race frames (1 = 300), log a hash of every boat's
 * position. Linked units run the same race, so their logs must show the same hashes. */
static void sync_check(void)
{
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("HYDRO_NET_SYNCCHECK");
        every = e ? atoi(e) : 0;
        if (every == 1)
            every = 300;
    }
    if (every <= 0 || nl.frame % (uint32_t)every)
        return;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 16; i++) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)(HY_VAR_Player_aData + i * 0x32cu);
        const uint8_t *phys = *(uint8_t *const *)(p + 0x48);
        if (!phys)
            continue;
        const uint8_t *pos = phys + 0xc0;
        for (int k = 0; k < 12; k++)
            h = (h ^ pos[k]) * 16777619u;
    }
    hy_log("net: sync frame %u hash %08x", nl.frame, h);
}

void net_link_PostWorkSend(void)
{
    nl.work_iters = nl.iters_in;
    if (Statemgr_nState != ST_RACE)
        return;
    nl.frame++;
    sync_check();
    nl.input_mask |= nl.my_mask;
    if (dl.delay > 1) {
        delay_send();
        return;
    }
    if (!nl.up)
        return;
    nl.input.frame = nl.frame;
    controls_CopyCachedControls(&nl.input.next);
    controls_CopyControlStruct(&nl.input.cur, PLAYER_CONTROLS(Player_nHuman));
    nl.input.iters = nl.iters_in;
    send_to_group(&nl.input, sizeof nl.input);
}
HY_PORT(net_link_PostWorkSend)

uint32_t net_link_GetNumberOfUnitsOnline(void) { return nl.online_count; }
HY_PORT(net_link_GetNumberOfUnitsOnline)
uint32_t net_link_GetCurrentGroupSize(void) { return nl.group_size; }
HY_PORT(net_link_GetCurrentGroupSize)
uint32_t net_link_GetMyUnitIdMask(void) { return nl.my_mask; }
HY_PORT(net_link_GetMyUnitIdMask)
void net_link_SetGroupLockState(uint8_t locked) { nl.group_lock = locked; }
HY_PORT(net_link_SetGroupLockState)
uint32_t net_link_ReturnOnlineHistoryMask(void) { return nl.online_history; }
HY_PORT(net_link_ReturnOnlineHistoryMask)
uint32_t net_link_GetOnlineUnitMask(void) { return nl.online_mask; }
HY_PORT(net_link_GetOnlineUnitMask)

void net_link_ResetGroup(int stop_multicast)
{
    nl.group_mask = nl.my_mask;
    nl.group_size = 1;
    if (stop_multicast)
        net_comm_EnableLocalMulticasting(0);
}
HY_PORT(net_link_ResetGroup)

/* This unit's part of the seed, from its own track/boat selection and settings. */
void net_link_InitSeedData(void)
{
    uint32_t me = nl.my_unit;
    SeedData *s = &g_net_seed;
    memset(&Net_Link_SeedHeader, 0, sizeof Net_Link_SeedHeader);
    memset(s, 0, sizeof *s);
    int track = Net_Link_nTrackNum;
    for (int i = 0; i < 10; i++)
        hi_score_CopyHiScoreStruct(&s->u_hiscores[me][i], &Hi_Score_Table[track * 10 + i]);
    int difficulty = Op_anTrackDifficulty[track];
    s->u_track[me] = (uint16_t)track;
    s->u_boat[me] = (uint16_t)Net_Link_nBoatNum;
    memcpy(s->u_splits[me], &Hud_SplitTimes[track * 5], sizeof s->u_splits[me]);
    s->u_order[me] = 1;
    s->u_difficulty[me] = (uint8_t)difficulty;
    s->u_no_timers[me] = Temp_bNoTimers;
    s->u_codes[me] = Net_Link_nSecretCodes;
    s->u_random[me] = xmath_RandomFloat();
    s->u_start_secs[me] = tracks_GetStartingSecs(track, difficulty);
    for (int k = 0; k < 5; k++)
        s->u_checkpoints[me][k] = tracks_GetCheckptSecs(track, difficulty, k);
    s->u_rabbit_b0[me] = Op_nRabbit0;
    s->u_rabbit_b1[me] = Op_nRabbit1;
    s->u_rabbit_b2[me] = Op_nRabbit2;
    s->u_rabbit_b3[me] = Op_anRabbit3[track * 4];
    ai_rabbit_GetUnsyncedData(track, &s->u_rabbit_f0[me], &s->u_rabbit_f1[me], &s->u_rabbit_f2[me],
                              &s->u_rabbit_f3[me]);
    s->u_free_races[me] = Net_Link_nNumFreeRacesInARow;
    nl.dest = DEST_NONE;
}
HY_PORT(net_link_InitSeedData)

void net_link_ResolveGameAddress(void)
{
    resolve_dest();
}
HY_PORT(net_link_ResolveGameAddress)

/* 1 = not a unit we ever linked with, 2 = linked and online, 0 = linked but lost, 3 = me. */
int net_link_IsReLinkNeededWithThisUnit(uint32_t unit)
{
    if (unit >= NET_UNITS)
        return 1;
    if (unit == nl.my_unit)
        return 3;
    uint32_t bit = 1u << unit;
    if (!(bit & nl.online_history))
        return 1;
    return (nl.online_mask & bit) ? 0 : 2;
}
HY_PORT(net_link_IsReLinkNeededWithThisUnit)

uint32_t net_link_GetHeardFromMask(void)
{
    uint32_t mask = 0;
    for (uint32_t u = 0; u < NET_UNITS; u++)
        if (nl.heard_from[u])
            mask |= 1u << u;
    return mask;
}
HY_PORT(net_link_GetHeardFromMask)

void net_link_ManageSelectMask(int add, uint32_t mask)
{
    if (add) {
        Net_Link_nSelectMask |= mask;
        return;
    }
    uint32_t left = Net_Link_nSelectMask & ~mask;
    int changed = Net_Link_nSelectMask != left;
    Net_Link_nSelectMask = left;
    if (changed && !left)
        Net_Link_nTrackNum = -1;
}
HY_PORT(net_link_ManageSelectMask)

/* ---------------------------------------------------------------- units coming and going */

/* 0x151128: add = 1 registers `unit` (at `ip`) as online, 0 takes it offline. */
static void net_link_RegisterUnit(int add, uint32_t unit, uint32_t ip)
{
    if (unit >= NET_UNITS)
        return;
    uint32_t bit = 1u << unit;
    if (!add) {
        if (nl.online_mask & bit) {
            nl.online_count--;
            nl.online_mask &= (uint16_t)~bit;
            Net_Link_nFlags |= 0x20; /* a unit was lost: relink after the race */
        }
        uint32_t left = Net_Link_nSelectMask & ~bit;
        int changed = Net_Link_nSelectMask != left;
        nl.heard_from[unit] = 0;
        Net_Link_nSelectMask = left;
        if (changed && !left)
            Net_Link_nTrackNum = -1;
        return;
    }
    nl.heard_from[unit] = ip;
    if (!(nl.online_mask & bit)) {
        nl.online_count++;
        nl.online_mask |= (uint16_t)bit;
        nl.online_history |= (uint16_t)bit;
    }
    nl.now = timer_ElapsedTicks(nl.timer);
    nl.last_heard[unit] = nl.now;
    uint32_t left = Net_Link_nSelectMask & ~bit;
    if (Net_Link_nSelectMask != left && !left)
        Net_Link_nTrackNum = -1;
    Net_Link_nSelectMask = left;
    if (unit != nl.my_unit && !net_comm_RegisterAUnit(ip, unit)) {
        nl.online_count--;
        nl.online_mask &= (uint16_t)~bit;
        nl.online_history &= (uint16_t)~bit;
        nl.heard_from[unit] = 0;
    }
    if (nl.online_mask == nl.online_history)
        Net_Link_nFlags &= ~0x20u;
}

/* 0x151288 */
static void net_link_DropUnit(uint32_t unit)
{
    if (unit >= NET_UNITS)
        return;
    uint32_t bit = 1u << unit;
    if (!(nl.online_mask & bit))
        return;
    if (nl.group_mask & bit) {
        nl.group_mask &= (uint16_t)~bit;
        nl.group_size--;
    }
    net_link_RegisterUnit(0, unit, nl.heard_from[unit]);
    if (Statemgr_nState == ST_RACE) {
        controls_ZeroControlStruct(PLAYER_CONTROLS(unit));
        resolve_dest();
        nl.input_mask &= ~bit;
        audits_LogALostLinkDuringARace();
        return;
    }
    if (Statemgr_nState == ST_PRERACE) {
        SeedHeader *h = &Net_Link_SeedHeader;
        if (h->ready_mask & bit) {
            h->ready_mask &= (uint16_t)~bit;
            h->ready_count--;
        }
        if (h->seed_mask & bit) {
            h->seed_mask &= (uint16_t)~bit;
            h->seed_count--;
        }
        if (h->lock_mask & bit) {
            h->lock_mask &= (uint16_t)~bit;
            h->lock_count--;
        }
    }
}

/* ---------------------------------------------------------------- receive */

static int track_beats(int proposed, int current)
{
    return Tracks_anTrackSelectionPriority[proposed] < Tracks_anTrackSelectionPriority[current];
}

/* Checksums and unit id of a HELLO / HELLO_ACK; sets the link-up error flags. */
static void check_hello(const GenericPkt *g)
{
    if (nl.code_checksum != g->b)
        Net_Link_nFlags |= 0x0c;
    if (g->h.unit == nl.my_unit)
        Net_Link_nFlags |= 0x14;
    if (nl.data_checksum != g->c)
        Net_Link_nFlags |= 0x44;
}

/* 0x150268: one packet from the pool. 0 keeps it there for the next frame. */
static int __cdecl net_link_HandlePacket(void *pkt, int len)
{
    (void)len;
    const PktHeader *h = pkt;
    const GenericPkt *g = pkt;
    uint32_t me = nl.my_unit;
    if (h->ip == nl.heard_from[me]) /* my own broadcast */
        return 1;
    uint32_t src = h->unit;
    if (src == me && h->type > MSG_HELLO_ACK)
        return 1;
    if (src >= NET_UNITS) /* port-only: the tables have NET_UNITS entries */
        return 1;
    int state = Statemgr_nState;
    if (state == ST_RELINK && h->type != MSG_HELLO && h->type != MSG_ONLINE && h->type != MSG_HEARTBEAT &&
        h->type != MSG_RELINK)
        return 1;
    if (h->type > MSG_INPUT_BATCH)
        return 1;
    uint32_t bit = 1u << src;
    int known = nl.connected && nl.heard_from[src] && (nl.online_mask & bit);

    switch (h->type) {
    case MSG_HELLO:
        if (!nl.connected) {
            check_hello(g);
            if (!(Net_Link_nFlags & 0x58) && !nl.heard_from[src])
                nl.heard_from[src] = g->a;
        } else if (nl.code_checksum == g->b && src != me && nl.data_checksum == g->c && !nl.heard_from[src]) {
            nl.heard_from[src] = g->a;
        }
        net_link_SendGenericMsg(MSG_HELLO_ACK, SEND_IP, g->a);
        return 1;
    case MSG_HELLO_ACK:
        if (!nl.connected) {
            check_hello(g);
            if (!(Net_Link_nFlags & 0x58)) {
                if (g->d) { /* already linked: online at once */
                    net_link_RegisterUnit(1, src, g->a);
                    return 1;
                }
                if (!nl.heard_from[src])
                    nl.heard_from[src] = g->a;
            }
        }
        break;
    case MSG_ONLINE:
        if (nl.heard_from[src]) {
            net_link_RegisterUnit(1, src, nl.heard_from[src]);
            if (nl.connected && Statemgr_nState == ST_SELECT)
                net_link_SendGenericMsg(MSG_SELECT_STATE, SEND_UNIT, src);
        }
        break;
    case MSG_SELECT_STATE:
        if (known) {
            Net_Link_nSelectMask |= g->a_lo;
            int track = (int16_t)g->a_hi;
            if (Net_Link_nTrackNum == -1 || track_beats(track, Net_Link_nTrackNum))
                Net_Link_nTrackNum = track;
        }
        break;
    case MSG_ATTRACT_JOIN:
    case MSG_ATTRACT_REPLY:
        if (!nl.connected || state != ST_ATTRACT || !nl.heard_from[src] || !(nl.online_mask & bit))
            return 1;
        if (h->type == MSG_ATTRACT_JOIN)
            net_link_SendGenericMsg(MSG_ATTRACT_REPLY, SEND_UNIT, src);
        if ((nl.online_mask & bit) && !(nl.group_mask & bit)) {
            nl.group_mask |= (uint16_t)bit;
            nl.group_size++;
        }
        Net_Link_bNewAttractMember = 1;
        return 1;
    case MSG_ATTRACT_DONE:
        if (nl.connected) {
            if (state == ST_ATTRACT && nl.heard_from[src] && (nl.online_mask & bit))
                Net_Link_nAttractSyncCount++;
            nl.last_heard[src] = nl.now;
            return 1;
        }
        break;
    case MSG_ATTRACT_ADVANCE:
        if (known && state == ST_ATTRACT)
            Net_Link_bAdvanceAttractStage = 1;
        break;
    case MSG_ATTRACT_RESET:
        if (known && state == ST_ATTRACT)
            Net_Link_bResetAttractMode = 1;
        break;
    case MSG_SELECT_ENTER:
        if (known && !(g->a & Net_Link_nSelectMask)) {
            Net_Link_nSelectMask |= g->a;
            state = Statemgr_nState;
            if (state != ST_ATTRACT) {
                if (state == ST_PRERACE && !nl.group_lock) {
                    if (Net_Link_nTrackNum == -1) { /* tell it the track we're about to race */
                        Net_Link_nTrackNum = g_net_seed.u_track[me];
                        net_link_SendGenericMsg(MSG_TRACK, SEND_ALL, 0);
                        Net_Link_nTrackNum = -1;
                        return 1;
                    }
                } else if (state != ST_SELECT || Net_Link_nTrackNum == -1) {
                    return 1;
                }
                net_link_SendGenericMsg(MSG_TRACK, SEND_ALL, 0);
                return 1;
            }
            if ((nl.online_mask & bit) && (nl.group_mask & bit)) { /* it left the attract group */
                nl.group_mask &= (uint16_t)~bit;
                nl.group_size--;
                return 1;
            }
        }
        break;
    case MSG_TRACK:
        if (known && (Net_Link_nTrackNum == -1 || track_beats((int)g->a, Net_Link_nTrackNum)))
            Net_Link_nTrackNum = (int)g->a;
        break;
    case MSG_SELECT_LEAVE:
        if (known) {
            uint32_t left = Net_Link_nSelectMask & ~g->a;
            if (Net_Link_nSelectMask != left && !left)
                Net_Link_nTrackNum = -1;
            Net_Link_nSelectMask = left;
            if (Statemgr_nState == ST_PRERACE && !nl.group_lock) {
                net_link_SendGenericMsg(MSG_GROUP_REPLY, SEND_UNIT, src);
                if ((nl.online_mask & bit) && !(nl.group_mask & bit)) {
                    nl.group_mask |= (uint16_t)bit;
                    nl.group_size++;
                    return 1;
                }
            }
        }
        break;
    case MSG_GROUP_LOCK:
        if (known && state == ST_PRERACE && nl.group_lock) {
            for (uint32_t u = 0; u < NET_UNITS; u++) {
                uint32_t b = 1u << u;
                if (!(nl.group_mask & b) && (g->a & b) && (nl.online_mask & b)) {
                    nl.group_mask |= (uint16_t)b;
                    nl.group_size++;
                }
            }
            if (!(Net_Link_SeedHeader.lock_mask & bit)) {
                Net_Link_SeedHeader.lock_mask |= (uint16_t)bit;
                Net_Link_SeedHeader.lock_count++;
                net_link_SendGenericMsg(MSG_GROUP_LOCK, SEND_ALL, 0);
                return 1;
            }
            /* Port-only: the original answers a unit's first lock only. If that answer was lost,
             * the sender waits forever for our lock (it keeps asking every 25 frames; we ignored
             * it). Answer again while its lock mask says it hasn't got ours. */
            if (g->d == LOCK_MASK_MARK && !(g->b & nl.my_mask)) {
                net_link_SendGenericMsg(MSG_GROUP_LOCK, SEND_UNIT, src);
                return 1;
            }
        }
        break;
    case MSG_GROUP_REPLY:
        if (known && state == ST_PRERACE && (nl.online_mask & bit) && !(nl.group_mask & bit)) {
            nl.group_mask |= (uint16_t)bit;
            /* Port-only: once my seed is out, my order is the one the others merge; a late reply
             * (seen over an internet relay) would give me a different grid than theirs. */
            if (!(Net_Link_SeedHeader.seed_mask & nl.my_mask))
                g_net_seed.u_order[me]++;
            nl.group_size++;
            return 1;
        }
        break;
    case MSG_READY:
        if (known && state == ST_PRERACE && !(Net_Link_SeedHeader.ready_mask & g->a)) {
            Net_Link_SeedHeader.ready_mask |= g->a_lo;
            Net_Link_SeedHeader.ready_count++;
            nl.last_heard[src] = nl.now;
            return 1;
        }
        break;
    case MSG_HEARTBEAT:
        nl.last_heard[src] = nl.now;
        return 1;
    case MSG_RELINK:
        if (nl.connected && (!nl.heard_from[src] || !(nl.online_mask & bit))) {
            net_link_RegisterUnit(1, src, g->a);
            net_link_SendGenericMsg(MSG_RELINK, SEND_ALL, 0);
            return 1;
        }
        break;
    case MSG_RELINK_DONE:
        if (known && state == ST_SELECT) {
            net_link_SendGenericMsg(MSG_SELECT_STATE, SEND_UNIT, src);
            return 1;
        }
        break;
    case MSG_ERROR:
        switch (g->a_lo) {
        case 1: /* the sender is going offline */
            if (h->ip == nl.heard_from[src]) {
                net_link_DropUnit(src);
                nl.heard_from[src] = 0;
                return 1;
            }
            break;
        case 2: /* a_hi timed out */
        case 3: /* a_hi's race input is missing */
            if (nl.heard_from[src] && (nl.online_mask & bit)) {
                if (g->a_hi == me) { /* it's me they lost: drop everyone else */
                    for (uint32_t u = 0; u < NET_UNITS; u++)
                        if (u != me && nl.heard_from[u] && (nl.online_mask & (1u << u)))
                            net_link_DropUnit(u);
                    return 1;
                }
                net_link_DropUnit(g->a_hi);
                return 1;
            }
            break;
        case 4: /* clean shutdown */
            net_link_DropUnit(g->a_hi);
            nl.online_history = nl.online_mask;
            break;
        }
        break;
    case MSG_SEED:
        if (known && state == ST_PRERACE && (nl.group_mask & bit) && !(Net_Link_SeedHeader.seed_mask & bit)) {
            const SeedPkt *p = pkt;
            SeedData *s = &g_net_seed;
            Net_Link_SeedHeader.seed_mask |= (uint16_t)bit;
            Net_Link_SeedHeader.seed_count++;
            for (int i = 0; i < 10; i++)
                hi_score_CopyHiScoreStruct(&s->u_hiscores[src][i], &p->hiscores[i]);
            s->u_track[src] = p->track;
            s->u_boat[src] = p->boat;
            memcpy(s->u_splits[src], p->splits, sizeof p->splits);
            s->u_order[src] = p->order;
            s->u_difficulty[src] = p->difficulty;
            s->u_no_timers[src] = p->no_timers;
            s->u_codes[src] = p->codes;
            s->u_random[src] = p->random;
            s->u_start_secs[src] = p->start_secs;
            memcpy(s->u_checkpoints[src], p->checkpoints, sizeof p->checkpoints);
            s->u_rabbit_b0[src] = p->rabbit_b0;
            s->u_rabbit_b1[src] = p->rabbit_b1;
            s->u_rabbit_b2[src] = p->rabbit_b2;
            s->u_rabbit_b3[src] = p->rabbit_b3;
            s->u_rabbit_f0[src] = p->rabbit_f0;
            s->u_rabbit_f1[src] = p->rabbit_f1;
            s->u_rabbit_f2[src] = p->rabbit_f2;
            s->u_rabbit_f3[src] = p->rabbit_f3;
            s->u_free_races[src] = (uint8_t)p->free_races;
            nl.last_heard[src] = nl.now;
            return 1;
        }
        break;
    case MSG_INPUT_BATCH: /* port-only; also taken in pre-race, from units that started first */
        if (known && (state == ST_RACE || state == ST_PRERACE) && (nl.group_mask & bit)) {
            delay_receive(pkt, src);
            nl.last_heard[src] = nl.now;
        }
        return 1;
    case MSG_INPUT:
        if (known && state == ST_RACE && (nl.group_mask & bit)) {
            const InputPkt *p = pkt;
            if (p->frame == nl.frame) {
                if (!(bit & nl.input_mask)) {
                    nl.input_mask |= bit;
                    if (nl.work_iters < p->iters)
                        nl.work_iters = p->iters;
                    controls_CopyControlStruct(PLAYER_CONTROLS(src), &p->next);
                    nl.last_heard[src] = nl.now;
                    return 1;
                }
            } else if (p->frame >= nl.frame && p->frame == nl.frame + 1) {
                /* a frame ahead: take its "this frame" controls now, keep it for the next frame */
                if (!(bit & nl.input_mask)) {
                    nl.input_mask |= bit;
                    controls_CopyControlStruct(PLAYER_CONTROLS(src), &p->cur);
                }
                return 0;
            }
        }
        break;
    }
    return 1;
}

/* ================================================================ 16 units elsewhere */

#include "../host/hook.h"

typedef struct FieldStr {
    const char *text;
    int value;
} FieldStr;

static const FieldStr k_unit_ids[NET_UNITS + 1] = {
    {"Unit 1", 0},   {"Unit 2", 1},   {"Unit 3", 2},   {"Unit 4", 3},   {"Unit 5", 4},   {"Unit 6", 5},
    {"Unit 7", 6},   {"Unit 8", 7},   {"Unit 9", 8},   {"Unit 10", 9},  {"Unit 11", 10}, {"Unit 12", 11},
    {"Unit 13", 12}, {"Unit 14", 13}, {"Unit 15", 14}, {"Unit 16", 15}, {NULL, 0},
};

/* AI_RABBIT: every human's speed running average (the "suck meter"), indexed by player slot and
 * made for slots 0-3. 16 of them here; ai_rabbit_ModuleInit creates them, ai_rabbit_SuckMeter
 * reads them. */
#define RUNAVG_SIZE 0x18
#define SUCK_SAMPLES 0x96
static uint8_t g_suck_ctx[16][RUNAVG_SIZE];
static float g_suck_buf[16][SUCK_SAMPLES];

/* The loops over units outside NET_LINK only draw; they stop at 4 through a byte immediate. */
void net_link_install_units(void)
{
    uint32_t ctx = (uint32_t)(uintptr_t)g_suck_ctx, buf = (uint32_t)(uintptr_t)g_suck_buf;
    hook_patch32(0x00105f5bu, 0x0034322cu, ctx);                     /* ModuleInit: mov edi, contexts */
    hook_patch32(0x00105f60u, 0x00343294u, buf);                     /* mov esi, buffers */
    hook_patch32(0x00105f7eu, 0x00343bf4u, buf + sizeof g_suck_buf); /* cmp esi, end */
    hook_patch32(0x00106e94u, 0x0034322cu, ctx);                     /* SuckMeter: lea [i*8 + contexts] x3 */
    hook_patch32(0x00106ea7u, 0x0034322cu, ctx);
    hook_patch32(0x00106ec7u, 0x0034322cu, ctx);

    /* (the link screens themselves are ported: sysmsg.c) */
    hook_patch8(0x0016d469u, 4, NET_UNITS); /* statemgr_Draw, relink: the units to list */
    hook_patch8(0x00144cecu, 4, NET_UNITS); /* hud_text_Draw: "OUT OF SYNC WITH UNIT %d" (debug) */
    /* NETWORK ADJUSTMENTS: UNIT ID offers 1-16 (the field's list, arg10 of its menu item) */
    hook_patch32(0x003105e0u, 0x00317860u, (uint32_t)(uintptr_t)k_unit_ids);
    /* "UNIT ID (1-4)" -> "UNIT ID (1-16)": one byte longer, into the string's zero padding */
    hook_patch(0x00310754u, "(1-4)\0\0", "(1-16)\0", 7);
    /* about screen: "Network Unit ID (1-4): %i" -> "Network Unit ID: %i" */
    hook_patch(0x0030ebffu, " (1-4): %i", ": %i\0\0\0\0\0\0", 10);
}
