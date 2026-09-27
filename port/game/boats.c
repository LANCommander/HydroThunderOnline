/* BOATS.OBJ: the tracking markers drawn over the other humans' boats. */
#include "port.h"

#include "../host/host.h"

#include <stdio.h>

#define Boats_bTrackingLoaded (*(int *)0x00352cfcu)
#define Boats_bTrackingEnabled (*(int *)0x00352d00u)
#define Boats_apTrackingMesh ((void **)0x00352cb8u) /* per player slot (16); the AI's marker follows */
#define Boats_TrackingGroup ((void *)0x002a11d8u)   /* obsys group: the meshes and where they go */
#define Boats_xfmMarkerLow ((void *)0x00351b08u)
#define Boats_xfmMarkerHigh ((void *)0x00351a98u)

/* obsys_LoadGroup's list: {where the object goes, its HT.R2 name}, ended by a null dest */
typedef struct TrackingLoad {
    void **dest;
    const char *name;
} TrackingLoad;

#define obsys_StartFrame HY_CALL(uint32_t(__cdecl *)(void), obsys_StartFrame)
#define obsys_ReleaseFrame HY_CALL(void(__cdecl *)(uint32_t), obsys_ReleaseFrame)
#define obsys_LoadGroup HY_CALL(int(__cdecl *)(void *, int), obsys_LoadGroup)
#define xfm_BuildXlatFromPoint HY_CALL(void(__cdecl *)(void *, float, float, float), xfm_BuildXlatFromPoint)

void boats_InitBoatTrackingSystem(void)
{
    uint32_t frame = obsys_StartFrame();
    Boats_bTrackingLoaded = 0;
    Boats_bTrackingEnabled = 0;
    if (obsys_LoadGroup(Boats_TrackingGroup, 0) != -1) {
        obsys_ReleaseFrame(frame);
        return;
    }
    xfm_BuildXlatFromPoint(Boats_xfmMarkerLow, 0.0f, 5.0f, 0.0f);
    xfm_BuildXlatFromPoint(Boats_xfmMarkerHigh, 0.0f, 7.5f, 0.0f);
    /* Port-only: the group loads markers for players 1-4 (slots 0-3); with up to 16 linked units, slots 4-15
     * would draw a null mesh. HT.R2 has unused markers 5-8 (GHWPLAYIDH5..8), and the overlay (r2file.c,
     * host/markers.cpp) adds GHWPLAYID09..16 (or any of 01..16), which win. A slot with neither reuses 1-4's. */
    char from[17]; /* per player: its HT.R2 digit, '+' MARKERS.R2X, '=' reuses 1-4 */
    for (int i = 0; i < 16; i++) {
        char overlay[12], builtin[12];
        snprintf(overlay, sizeof overlay, "GHWPLAYID%02d", i + 1);
        snprintf(builtin, sizeof builtin, "GHWPLAYIDH%d", i + 1);
        from[i] = i < 4 ? (char)('1' + i) : '=';
        void *mesh = NULL;
        TrackingLoad one[2] = {{&mesh, overlay}, {NULL, NULL}};
        if (obsys_LoadGroup(one, 0) == -1) { /* a missing name leaves mesh alone */
            Boats_apTrackingMesh[i] = mesh;
            from[i] = '+';
            continue;
        }
        if (i < 4)
            continue;
        Boats_apTrackingMesh[i] = NULL; /* may hold an earlier frame's mesh */
        if (i < 8) {
            one[0].name = builtin;
            if (obsys_LoadGroup(one, 0) == -1) {
                Boats_apTrackingMesh[i] = mesh;
                from[i] = (char)('1' + i);
                continue;
            }
        }
        Boats_apTrackingMesh[i] = Boats_apTrackingMesh[i % 4];
    }
    static int logged;
    if (!logged++)
        hy_log("boats: tracking markers 1-16 [%.16s] (digit: HT.R2, +: MARKERS.R2X, =: reuses 1-4)", from);
    Boats_bTrackingLoaded = 1;
}
HY_PORT(boats_InitBoatTrackingSystem)
