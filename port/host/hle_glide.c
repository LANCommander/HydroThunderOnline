/*
 * Route the statically linked Glide 2.53 to a Glide 2.x DLL (dgVoodoo2's glide2x.dll).
 *
 * The static library was built __cdecl (`_grDrawTriangle`), the DLL exports __stdcall
 * (`_grDrawTriangle@12`). The @N suffix gives the argument size, so each entry point gets a
 * generated thunk that re-pushes the N bytes of arguments and calls the DLL, which pops them.
 * No prototypes needed. Float results come back in ST(0) untouched.
 */
#include "hook.h"
#include "host.h"
#include "window.h"

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HMODULE g_glide;

typedef int(__stdcall *PFN_grSstWinOpen)(uint32_t hwnd, int res, int refresh, int cformat, int origin,
                                         int ncolor, int naux);
typedef void(__stdcall *PFN_grBufferSwap)(int interval);

static PFN_grSstWinOpen real_grSstWinOpen;
static PFN_grBufferSwap real_grBufferSwap;

/* Glide 2 GrVertex as the game was built (GLIDE_NUM_TMU 2: grDrawPolygonVertexList steps 0x3c). */
typedef struct GrVertex {
    float x, y, z, r, g, b, ooz, a, oow;
    struct { float sow, tow, oow; } tmu[2];
} GrVertex;
typedef char grvertex_is_60_bytes[sizeof(GrVertex) == 60 ? 1 : -1];

/*
 * The game draws a 512x400 screen (a coin-op video timing on Glide's 512x384, which dgVoodoo
 * ignores), so the top 16 rows fell outside dgVoodoo's 384-row surface (origin is lower left).
 * Glide has no 512x400 mode, so a 512x384 open becomes 640x480 and everything in screen space is
 * scaled by 5/4 x 6/5: vertices, the clip window and LFB accesses (a shadow, resampled at unlock).
 * 640x480 is 4:3 like the cabinet's monitor, so dgVoodoo's aspect modes show the right shape, and
 * LFB art only gets pixels doubled, never dropped. HYDRO_GLIDE_DEBUG=noscale keeps 512x384.
 */
#define GAME_W 512
#define GAME_H 400
static int g_scaled;
static int g_xn = 1, g_xd = 1, g_yn = 1, g_yd = 1; /* surface = game * n / d */
static float g_sx = 1, g_sy = 1;
static int g_surf_w = 512, g_surf_h = 384; /* the open Glide surface */
static int g_screen_h = 384;               /* rows the game can use */

int glide_screen_rows(void) { return g_screen_h; }

static int __cdecl my_grSstWinOpen(uint32_t hwnd, int res, int refresh, int cformat, int origin, int ncolor,
                                   int naux)
{
    static const short widths[] = {320, 320, 400, 512, 640, 640, 640, 640, 800, 960, 856, 512, 1024, 1280, 1600, 400};
    static const short heights[] = {200, 240, 256, 384, 200, 350, 400, 480, 600, 720, 480, 256, 768, 1024, 1200, 300};
    const char *dbg = getenv("HYDRO_GLIDE_DEBUG");
    g_scaled = res == 3 /* 512x384 */ && !(dbg && strstr(dbg, "noscale"));
    if (g_scaled) {
        res = 7; /* 640x480 */
        g_xn = 5, g_xd = 4, g_yn = 6, g_yd = 5;
        g_sx = 1.25f, g_sy = 1.2f;
    } else {
        g_xn = g_xd = g_yn = g_yd = 1;
        g_sx = g_sy = 1;
    }
    if (res >= 0 && res < (int)(sizeof heights / sizeof heights[0])) {
        g_surf_w = widths[res];
        g_surf_h = heights[res];
    }
    g_screen_h = g_scaled ? GAME_H : g_surf_h;
    HWND w = window_get();
    hy_log("grSstWinOpen(hwnd=%x res=%d refresh=%d cformat=%d origin=%d color=%d aux=%d) -> hwnd %p%s", hwnd,
           res, refresh, cformat, origin, ncolor, naux, w, g_scaled ? " (game's 512x400 scaled to 640x480)" : "");
    int r = real_grSstWinOpen((uint32_t)(uintptr_t)w, res, refresh, cformat, origin, ncolor, naux);
    hy_log("grSstWinOpen = %d", r);
    return r;
}

/* Screen-space entry points, scaled by g_sx/g_sy. real_* are the cdecl bridge thunks. */
typedef void(__cdecl *PFN_v2)(const GrVertex *, const GrVertex *);
typedef void(__cdecl *PFN_v3)(const GrVertex *, const GrVertex *, const GrVertex *);
typedef void(__cdecl *PFN_aav3)(const GrVertex *, const GrVertex *, const GrVertex *, int, int, int);
typedef void(__cdecl *PFN_vlist)(int, const GrVertex *);
typedef void(__cdecl *PFN_clip)(uint32_t, uint32_t, uint32_t, uint32_t);
static PFN_v2 real_grDrawLine, real_grAADrawLine;
static PFN_v3 real_grDrawTriangle;
static PFN_aav3 real_grAADrawTriangle;
static PFN_vlist real_grDrawPolygonVertexList, real_grDrawPlanarPolygonVertexList;
static PFN_clip real_grClipWindow;

static const GrVertex *sv(GrVertex *d, const GrVertex *s)
{
    *d = *s;
    d->x *= g_sx;
    d->y *= g_sy;
    return d;
}

static void __cdecl sc_grDrawLine(const GrVertex *a, const GrVertex *b)
{
    GrVertex v[2];
    if (!g_scaled)
        real_grDrawLine(a, b);
    else
        real_grDrawLine(sv(&v[0], a), sv(&v[1], b));
}

static void __cdecl sc_grAADrawLine(const GrVertex *a, const GrVertex *b)
{
    GrVertex v[2];
    if (!g_scaled)
        real_grAADrawLine(a, b);
    else
        real_grAADrawLine(sv(&v[0], a), sv(&v[1], b));
}

static void __cdecl sc_grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    GrVertex v[3];
    if (!g_scaled)
        real_grDrawTriangle(a, b, c);
    else
        real_grDrawTriangle(sv(&v[0], a), sv(&v[1], b), sv(&v[2], c));
}

static void __cdecl sc_grAADrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c, int ab, int bc,
                                        int ca)
{
    GrVertex v[3];
    if (!g_scaled)
        real_grAADrawTriangle(a, b, c, ab, bc, ca);
    else
        real_grAADrawTriangle(sv(&v[0], a), sv(&v[1], b), sv(&v[2], c), ab, bc, ca);
}

static void scaled_list(PFN_vlist fn, int n, const GrVertex *list)
{
    if (!g_scaled || n <= 0) {
        fn(n, list);
        return;
    }
    GrVertex buf[32];
    GrVertex *v = n <= 32 ? buf : malloc(n * sizeof *v);
    if (!v)
        return;
    for (int i = 0; i < n; i++)
        sv(&v[i], &list[i]);
    fn(n, v);
    if (v != buf)
        free(v);
}
static void __cdecl sc_grDrawPolygonVertexList(int n, const GrVertex *l)
{
    scaled_list(real_grDrawPolygonVertexList, n, l);
}
static void __cdecl sc_grDrawPlanarPolygonVertexList(int n, const GrVertex *l)
{
    scaled_list(real_grDrawPlanarPolygonVertexList, n, l);
}

/* The clip max is exclusive: round the far edges up so the game's last column/row stays covered. */
static void __cdecl sc_grClipWindow(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (g_scaled) {
        x0 = x0 * g_xn / g_xd, x1 = (x1 * g_xn + g_xd - 1) / g_xd;
        y0 = y0 * g_yn / g_yd, y1 = (y1 * g_yn + g_yd - 1) / g_yd;
    }
    real_grClipWindow(x0, y0, x1, y1);
}

static const struct {
    const char *name;
    void **real;
    const void *hook;
} k_scaled[] = {
    {"_grDrawLine", (void **)&real_grDrawLine, sc_grDrawLine},
    {"_grAADrawLine", (void **)&real_grAADrawLine, sc_grAADrawLine},
    {"_grDrawTriangle", (void **)&real_grDrawTriangle, sc_grDrawTriangle},
    {"_grAADrawTriangle", (void **)&real_grAADrawTriangle, sc_grAADrawTriangle},
    {"_grDrawPolygonVertexList", (void **)&real_grDrawPolygonVertexList, sc_grDrawPolygonVertexList},
    {"_grDrawPlanarPolygonVertexList", (void **)&real_grDrawPlanarPolygonVertexList,
     sc_grDrawPlanarPolygonVertexList},
    {"_grClipWindow", (void **)&real_grClipWindow, sc_grClipWindow},
};

typedef int(__stdcall *PFN_grSstQueryHardware)(int *hw);
static PFN_grSstQueryHardware real_grSstQueryHardware;

/* GrHwConfiguration (Glide 2): num_sst, then per SST: type, fbRam MB, fbiRev, nTexelfx, sliDetect,
 * tmuConfig[2] {tmuRev, tmuRam MB}. INIT3DFX sizes its texture memory from this. */
static int __cdecl my_grSstQueryHardware(int *hw)
{
    int r = real_grSstQueryHardware(hw);
    /* init3dfx_Init sets Init3dfx_nChipsetCode = 2 (Voodoo2) only for fbiRev >= 0x100, and only
     * then does anim_air_DoLensFlare probe the depth buffer under the sun; otherwise the flare is
     * drawn through all geometry. dgVoodoo emulates a Voodoo2 but reports the Voodoo1's fbiRev 2. */
    if (r && hw[3] < 0x100)
        hw[3] |= 0x100;
    hy_log("grSstQueryHardware = %d: %d SST, type %d, fb %d MB, fbiRev %d, %d TMU (%d MB, %d MB)", r, hw[0],
           hw[1], hw[2], hw[3], hw[4], hw[7], hw[9]);
    return r;
}

static void stats_dump(void);
static void capture_frame_boundary(void);
static int g_stats;

static void __cdecl my_grBufferSwap(int interval)
{
    capture_frame_boundary();
    real_grBufferSwap(interval);
    window_pump();
    if (g_stats)
        stats_dump();
}

/*
 * LFB locks get a shadow in game coordinates (2048-byte stride, 400+ rows) whenever the screen is
 * scaled, and always for the operator menu. Unlock resamples written shadows onto the surface.
 * - The menu's text layer (vtext_work) and monitor patterns write with a hard-coded 2048-byte
 *   stride (the Voodoo2's), and vtext's full-screen clear writes 400 rows whatever grLfbLock
 *   reports. Its shadow persists across locks and is copied whole.
 * - Other writers (blit_Raw, sysfont) write only some pixels. Their shadow starts out filled with
 *   a sentinel (near black, alpha 0), and only the pixels that changed reach the surface.
 * - Reads (the lens flare's depth probe) fill the shadow from the surface.
 */
typedef struct GrLfbInfo {
    int size;
    void *lfbPtr;
    uint32_t strideInBytes;
    int writeMode, origin;
} GrLfbInfo;
typedef int(__stdcall *PFN_grLfbLock)(int type, int buffer, int mode, int origin, int pipeline, GrLfbInfo *info);
typedef int(__stdcall *PFN_grLfbUnlock)(int type, int buffer);
static PFN_grLfbLock real_grLfbLock;
static PFN_grLfbUnlock real_grLfbUnlock;

#define SHADOW_STRIDE 2048
#define SHADOW_ROWS 480 /* GAME_H plus slack */
#define SENTINEL16 0x0001u
#define SENTINEL32 0x00000001u
static uint8_t *g_menu_shadow, *g_lfb_shadow;
static struct {
    int active, write, bpp, sparse;
    uint8_t *buf;
    GrLfbInfo real;
} g_lk;

static int menu_caller(uint32_t ret)
{
    return (ret >= 0x001bb200u && ret < 0x001c2200u) || (ret >= 0x001ea128u && ret < 0x001ead28u);
}

/* Bytes per pixel of a lock: reads return the buffer's 16 bits; writes follow the write mode. */
static int lfb_bpp(int write, int mode)
{
    if (!write)
        return 2;
    switch (mode) {
    case 4:   /* 888 */
    case 5:   /* 8888 */
    case 0xc: /* 565_DEPTH */
    case 0xd: /* 555_DEPTH */
    case 0xe: /* 1555_DEPTH */
        return 4;
    default:
        return 2;
    }
}

static int __cdecl my_grLfbLock(int type, int buffer, int mode, int origin, int pipeline, GrLfbInfo *info)
{
    int r = real_grLfbLock(type, buffer, mode, origin, pipeline, info);
    if (!r || g_lk.active)
        return r;
    int menu = menu_caller((uint32_t)(uintptr_t)_ReturnAddress());
    if (!menu && !g_scaled)
        return r;
    uint8_t **slot = menu ? &g_menu_shadow : &g_lfb_shadow;
    if (!*slot) {
        *slot = VirtualAlloc(NULL, SHADOW_STRIDE * SHADOW_ROWS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!*slot)
            return r;
        hy_log("glide: %s LFB: stride %u, %dx%d surface; drawing into a %d-stride 512x%d shadow",
               menu ? "operator menu" : "game", info->strideInBytes, g_surf_w, g_surf_h, SHADOW_STRIDE, GAME_H);
    }
    uint8_t *buf = *slot;
    int write = type & 1;
    g_lk.active = 1;
    g_lk.write = write;
    g_lk.bpp = lfb_bpp(write, mode);
    g_lk.sparse = !menu;
    g_lk.buf = buf;
    g_lk.real = *info;
    if (!write) {
        /* Each game pixel samples the centre of its scaled footprint. */
        for (int y = 0; y < GAME_H; y++) {
            int Y = (y * g_yn + g_yn / 2) / g_yd;
            if (Y >= g_surf_h)
                break;
            const uint16_t *src = (const uint16_t *)((uint8_t *)info->lfbPtr + (size_t)Y * info->strideInBytes);
            uint16_t *dst = (uint16_t *)(buf + (size_t)y * SHADOW_STRIDE);
            for (int x = 0; x < GAME_W; x++) {
                int X = (x * g_xn + g_xn / 2) / g_xd;
                dst[x] = src[X < g_surf_w ? X : g_surf_w - 1];
            }
        }
    } else if (g_lk.sparse) {
        if (g_lk.bpp == 2) {
            uint16_t *p = (uint16_t *)buf;
            for (size_t i = 0; i < SHADOW_STRIDE / 2 * SHADOW_ROWS; i++)
                p[i] = SENTINEL16;
        } else {
            uint32_t *p = (uint32_t *)buf;
            for (size_t i = 0; i < SHADOW_STRIDE / 4 * SHADOW_ROWS; i++)
                p[i] = SENTINEL32;
        }
    }
    info->lfbPtr = buf;
    info->strideInBytes = SHADOW_STRIDE;
    return r;
}

static void lfb_writeback(void)
{
    int cols = SHADOW_STRIDE / g_lk.bpp;
    if (cols > GAME_W && g_scaled)
        cols = GAME_W;
    for (int Y = 0; Y < g_surf_h; Y++) {
        int y = Y * g_yd / g_yn;
        if (y >= SHADOW_ROWS)
            break;
        const uint8_t *srow = g_lk.buf + (size_t)y * SHADOW_STRIDE;
        uint8_t *drow = (uint8_t *)g_lk.real.lfbPtr + (size_t)Y * g_lk.real.strideInBytes;
        for (int X = 0; X < g_surf_w; X++) {
            int x = X * g_xd / g_xn;
            if (x >= cols)
                break;
            if (g_lk.bpp == 2) {
                uint16_t v = ((const uint16_t *)srow)[x];
                if (!g_lk.sparse || v != SENTINEL16)
                    ((uint16_t *)drow)[X] = v;
            } else {
                uint32_t v = ((const uint32_t *)srow)[x];
                if (!g_lk.sparse || v != SENTINEL32)
                    ((uint32_t *)drow)[X] = v;
            }
        }
    }
}

static int __cdecl my_grLfbUnlock(int type, int buffer)
{
    if (g_lk.active) {
        g_lk.active = 0;
        if (g_lk.write)
            lfb_writeback();
    }
    return real_grLfbUnlock(type, buffer);
}

/* Glide-internal routines that game code calls directly; no DLL equivalent. */
static uint32_t __cdecl my_sst1InitReturnStatus(uint32_t a)
{
    static LONG n;
    if (InterlockedIncrement(&n) <= 8)
        hy_log("sst1InitReturnStatus(%08x) from %08x", a, (uint32_t)(uintptr_t)_ReturnAddress());
    return 0;
}
static uint32_t __cdecl my_sst1InitRead32(uint32_t addr)
{
    static LONG n;
    if (InterlockedIncrement(&n) <= 8)
        hy_log("sst1InitRead32(%08x) from %08x", addr, (uint32_t)(uintptr_t)_ReturnAddress());
    return 0;
}
static void __cdecl my_sst1InitWrite32(uint32_t addr, uint32_t v)
{
    static LONG n;
    if (InterlockedIncrement(&n) <= 32)
        hy_log("sst1InitWrite32(%08x, %08x) from %08x", addr, v, (uint32_t)(uintptr_t)_ReturnAddress());
}

/* ---------------------------------------------------------------- stats (HYDRO_GLIDE_STATS=1) */

#define MAX_STAT 128
static const char *g_stat_name[MAX_STAT];
static volatile LONG g_stat_calls[MAX_STAT];
static unsigned g_nstat;
static volatile LONG g_src_fmt[2][16], g_dl_fmt[2][16], g_table[4];

/* GrTexInfo (Glide 2): smallLod, largeLod, aspectRatio, format, data */
typedef struct GrTexInfo {
    int small_lod, large_lod, aspect, format;
    void *data;
} GrTexInfo;

typedef void(__stdcall *PFN_tex4)(int tmu, uint32_t start, int evenodd, GrTexInfo *info);
typedef void(__stdcall *PFN_table)(int tmu, int type, void *data);
static PFN_tex4 real_grTexSource, real_grTexDownloadMipMap;
static PFN_table real_grTexDownloadTable;

static void __cdecl my_grTexSource(int tmu, uint32_t start, int evenodd, GrTexInfo *info)
{
    InterlockedIncrement(&g_src_fmt[tmu & 1][info->format & 15]);
    real_grTexSource(tmu, start, evenodd, info);
}

static void __cdecl my_grTexDownloadMipMap(int tmu, uint32_t start, int evenodd, GrTexInfo *info)
{
    InterlockedIncrement(&g_dl_fmt[tmu & 1][info->format & 15]);
    real_grTexDownloadMipMap(tmu, start, evenodd, info);
}

/* Voodoo2 palettes are 24-bit RGB: P_8 texels always have alpha 0xFF, whatever the top byte of
 * each GuTexPalette entry holds (the game uploads 0x00RRGGBB). Force the byte opaque so a wrapper
 * can't read it as alpha. (Not the cause of the invisible surfaces; that was the trilinear
 * combine below.) AP_88 takes alpha from the texel anyway. */
#define GR_TEXTABLE_PALETTE 2
static void __cdecl my_grTexDownloadTable(int tmu, int type, void *data)
{
    InterlockedIncrement(&g_table[type & 3]);
    if (type == GR_TEXTABLE_PALETTE && data) {
        uint32_t pal[256];
        const uint32_t *src = data;
        for (int i = 0; i < 256; i++)
            pal[i] = src[i] | 0xff000000u;
        real_grTexDownloadTable(tmu, type, pal);
        return;
    }
    real_grTexDownloadTable(tmu, type, data);
}

static void stats_dump(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 10000)
        return;
    last = now;
    char line[1024];
    int n = 0;
    for (unsigned i = 0; i < g_nstat; i++) {
        LONG c = InterlockedExchange(&g_stat_calls[i], 0);
        if (c && n < (int)sizeof line - 64)
            n += snprintf(line + n, sizeof line - n, " %s=%ld", g_stat_name[i] + 1, c);
    }
    hy_log("glide calls/10s:%s", line);
    for (int kind = 0; kind < 2; kind++)
        for (int tmu = 0; tmu < 2; tmu++) {
            n = 0;
            for (int f = 0; f < 16; f++) {
                LONG c = InterlockedExchange(kind ? &g_dl_fmt[tmu][f] : &g_src_fmt[tmu][f], 0);
                if (c)
                    n += snprintf(line + n, sizeof line - n, " fmt%d=%ld", f, c);
            }
            if (n)
                hy_log("glide %s tmu%d:%s", kind ? "download" : "source", tmu, line);
        }
    hy_log("glide tables: ncc0=%ld ncc1=%ld palette=%ld", InterlockedExchange(&g_table[0], 0),
           InterlockedExchange(&g_table[1], 0), InterlockedExchange(&g_table[2], 0));
}

/* ---------------------------------------------------------------- overrides
 * grTexCombine is always overridden (split-trilinear fix below).
 * HYDRO_GLIDE_DEBUG rendering experiments: force a per-pixel test off to see whether "invisible"
 * geometry is drawn and then rejected: nodepth, noalphatest, nochroma, nobias; trilinear keeps
 * the original texture combine (comma separated). */
typedef void(__stdcall *PFN_int1)(int);
static PFN_int1 real_grDepthBufferFunction, real_grAlphaTestFunction, real_grChromakeyMode,
    real_grDepthBiasLevel;
static void __cdecl dbg_grDepthBufferFunction(int f) { (void)f; real_grDepthBufferFunction(7 /*ALWAYS*/); }
static void __cdecl dbg_grAlphaTestFunction(int f) { (void)f; real_grAlphaTestFunction(7 /*ALWAYS*/); }
static void __cdecl dbg_grChromakeyMode(int m) { (void)m; real_grChromakeyMode(0 /*DISABLE*/); }
static void __cdecl dbg_grDepthBiasLevel(int b) { (void)b; real_grDepthBiasLevel(0); }

/*
 * Voodoo2 single-pass trilinear: the game loads a texture's even mip levels on one TMU and the odd
 * levels on the other (grTexSource evenOdd = 1/2), then blends them on TMU0 with
 *   grTexCombine(TMU0, BLEND(7), LOD_FRACTION(5) or ONE_MINUS_LOD_FRACTION(0xd), same for alpha).
 * Almost every world surface uses it together with an alpha test at >= 0xFA. dgVoodoo's version
 * of that blend corrupts the alpha, so those surfaces fail the alpha test and vanish (ramps, AI
 * boats, scenery). Default fix: TMU0 outputs its own texture (bilinear + mipmaps; dgVoodoo can
 * force trilinear filtering itself). HYDRO_GLIDE_DEBUG=trilinear keeps the original combine.
 *
 * grTexCombine(tmu, rgb_func, rgb_factor, alpha_func, alpha_factor, rgb_invert, alpha_invert)
 */
typedef void(__stdcall *PFN_texcombine)(int, int, int, int, int, int, int);
static PFN_texcombine real_grTexCombine;
static void __cdecl fix_grTexCombine(int tmu, int rf, int rfac, int af, int afac, int ri, int ai)
{
    if (tmu == 0 && rf == 7 && (rfac & 7) == 5) {
        rf = af = 1;
        rfac = afac = 0;
    }
    real_grTexCombine(tmu, rf, rfac, af, afac, ri, ai);
}

static const void *debug_override(const char *n, void *p)
{
    const char *dbg = getenv("HYDRO_GLIDE_DEBUG");
    if (!strcmp(n, "_grTexCombine") && !(dbg && strstr(dbg, "trilinear") && !strstr(dbg, "notrilinear")))
        return real_grTexCombine = (PFN_texcombine)p, (const void *)fix_grTexCombine;
    if (!dbg)
        return NULL;
    if (strstr(dbg, "nodepth") && !strcmp(n, "_grDepthBufferFunction"))
        return real_grDepthBufferFunction = (PFN_int1)p, (const void *)dbg_grDepthBufferFunction;
    if (strstr(dbg, "noalphatest") && !strcmp(n, "_grAlphaTestFunction"))
        return real_grAlphaTestFunction = (PFN_int1)p, (const void *)dbg_grAlphaTestFunction;
    if (strstr(dbg, "nochroma") && !strcmp(n, "_grChromakeyMode"))
        return real_grChromakeyMode = (PFN_int1)p, (const void *)dbg_grChromakeyMode;
    if (strstr(dbg, "nobias") && !strcmp(n, "_grDepthBiasLevel"))
        return real_grDepthBiasLevel = (PFN_int1)p, (const void *)dbg_grDepthBiasLevel;
    return NULL;
}

/* ---------------------------------------------------------------- HYDRO_GLIDE_CAPTURE=<seconds>
 * Logs every bridged call of one whole frame (the first that starts after <seconds>) to
 * build\glide_frame.txt: raw args, plus decoded texture info and vertex screen positions. */
static DWORD g_capture_at, g_t0;
static int g_capture_state; /* 0 idle/armed, 1 capturing, 2 done */
static FILE *g_cap;
static unsigned g_stat_argbytes[MAX_STAT];

static void cap_vertex(const GrVertex *v)
{
    fprintf(g_cap, " (%.1f,%.1f a=%.0f rgb=%.0f,%.0f,%.0f oow=%.4g)", v->x, v->y, v->a, v->r, v->g, v->b, v->oow);
}

static void __cdecl capture_call(unsigned idx, const uint32_t *args)
{
    if (g_capture_state != 1 || !g_cap)
        return;
    const char *n = g_stat_name[idx] + 1;
    fprintf(g_cap, "%s", n);
    for (unsigned i = 0; i < g_stat_argbytes[idx] / 4; i++)
        fprintf(g_cap, " %08x", args[i]);
    if (!strcmp(n, "grTexSource") || !strcmp(n, "grTexDownloadMipMap")) {
        const GrTexInfo *ti = (const GrTexInfo *)(uintptr_t)args[3];
        fprintf(g_cap, "  [tmu%u @%x lod %d..%d aspect %d fmt %d]", args[0], args[1], ti->small_lod,
                ti->large_lod, ti->aspect, ti->format);
    } else if (!strcmp(n, "grDrawTriangle")) {
        for (int i = 0; i < 3; i++)
            cap_vertex((const GrVertex *)(uintptr_t)args[i]);
    } else if (!strcmp(n, "grDrawPlanarPolygonVertexList")) {
        cap_vertex((const GrVertex *)(uintptr_t)args[1]);
    }
    fputc('\n', g_cap);
}

static void capture_frame_boundary(void)
{
    if (!g_capture_at || g_capture_state == 2)
        return;
    if (g_capture_state == 1) {
        fclose(g_cap);
        g_cap = NULL;
        g_capture_state = 2;
        hy_log("glide capture: frame written to build\\glide_frame.txt");
    } else if (GetTickCount() - g_t0 >= g_capture_at) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\build\\glide_frame.txt", g_cfg.work_dir);
        g_cap = fopen(path, "w");
        g_capture_state = g_cap ? 1 : 2;
    }
}

static void *make_thunk(void *target, unsigned argbytes, volatile LONG *counter, int capture_idx)
{
    if (argbytes == 0 && !counter && capture_idx < 0)
        return target; /* stdcall with no args == cdecl */
    if (argbytes > 124)
        hy_fatal("glide thunk: %u arg bytes", argbytes);
    uint8_t *t = thunk_alloc(4 * (argbytes / 4) + 48), *p = t;
    if (capture_idx >= 0) {
        *p++ = 0x8D, *p++ = 0x44, *p++ = 0x24, *p++ = 0x04; /* lea eax, [esp+4] (args) */
        *p++ = 0x50;                                        /* push eax */
        *p++ = 0x68;                                        /* push idx */
        *(uint32_t *)p = (uint32_t)capture_idx, p += 4;
        *p++ = 0xB8; /* mov eax, capture_call; call eax; add esp, 8 */
        *(uint32_t *)p = (uint32_t)(uintptr_t)capture_call, p += 4;
        *p++ = 0xFF, *p++ = 0xD0;
        *p++ = 0x83, *p++ = 0xC4, *p++ = 0x08;
    }
    if (counter) {
        *p++ = 0xFF, *p++ = 0x05; /* inc dword [counter] */
        *(uint32_t *)p = (uint32_t)(uintptr_t)counter, p += 4;
    }
    for (unsigned i = 0; i < argbytes / 4; i++) {
        /* push dword [esp+N]: each push moves esp, so the same displacement walks the args */
        *p++ = 0xFF, *p++ = 0x74, *p++ = 0x24, *p++ = (uint8_t)argbytes;
    }
    *p++ = 0xB8; /* mov eax, target */
    *(uint32_t *)p = (uint32_t)(uintptr_t)target, p += 4;
    *p++ = 0xFF, *p++ = 0xD0; /* call eax */
    *p++ = 0xC3;              /* ret (caller cleans: cdecl) */
    return t;
}

static void *find_export(const char *cname, unsigned *argbytes)
{
    char buf[128];
    for (unsigned n = 0; n <= 64; n += 4) {
        snprintf(buf, sizeof buf, "%s@%u", cname, n);
        void *p = (void *)GetProcAddress(g_glide, buf);
        if (p) {
            *argbytes = n;
            return p;
        }
    }
    return NULL;
}

void hle_glide_install(void)
{
    g_glide = LoadLibraryA(g_cfg.glide_dll);
    if (!g_glide)
        hy_fatal("cannot load Glide DLL %s (error %lu)", g_cfg.glide_dll, GetLastError());
    const char *st = getenv("HYDRO_GLIDE_STATS");
    g_stats = st && *st && *st != '0';
    const char *cap = getenv("HYDRO_GLIDE_CAPTURE");
    g_t0 = GetTickCount();
    g_capture_at = cap ? (DWORD)(atof(cap) * 1000) : 0;
    if (g_capture_at)
        g_stats = 0; /* capture needs every call to go through the generic thunk */

    unsigned bridged = 0, internal = 0;
    for (unsigned i = 0; i < hy_symtab_count; i++) {
        const HySym *s = &hy_symtab[i];
        const char *n = s->name;
        if (s->section != HY_SEC_CODE || n[0] != '_' || strchr(n, '@'))
            continue;
        if (strncmp(n + 1, "gr", 2) && strncmp(n + 1, "gu", 2))
            continue;
        if (!(n[3] >= 'A' && n[3] <= 'Z'))
            continue;
        unsigned argbytes;
        void *p = find_export(n, &argbytes);
        if (!p) {
            internal++; /* Glide-internal helper; unreachable once the entry points are bridged */
            continue;
        }
        const void *dbg = debug_override(n, p);
        if (dbg) {
            hy_log("glide: overriding %s", n);
            hook_jmp(s->addr, dbg);
        } else if (!strcmp(n, "_grSstWinOpen")) {
            real_grSstWinOpen = (PFN_grSstWinOpen)p;
            hook_jmp(s->addr, my_grSstWinOpen);
        } else if (!strcmp(n, "_grSstQueryHardware")) {
            real_grSstQueryHardware = (PFN_grSstQueryHardware)p;
            hook_jmp(s->addr, my_grSstQueryHardware);
        } else if (!strcmp(n, "_grBufferSwap")) {
            real_grBufferSwap = (PFN_grBufferSwap)p;
            hook_jmp(s->addr, my_grBufferSwap);
        } else if (g_stats && !strcmp(n, "_grTexSource")) {
            real_grTexSource = (PFN_tex4)p;
            hook_jmp(s->addr, my_grTexSource);
        } else if (g_stats && !strcmp(n, "_grTexDownloadMipMap")) {
            real_grTexDownloadMipMap = (PFN_tex4)p;
            hook_jmp(s->addr, my_grTexDownloadMipMap);
        } else if (!strcmp(n, "_grLfbLock")) {
            real_grLfbLock = (PFN_grLfbLock)p;
            hook_jmp(s->addr, my_grLfbLock);
        } else if (!strcmp(n, "_grLfbUnlock")) {
            real_grLfbUnlock = (PFN_grLfbUnlock)p;
            hook_jmp(s->addr, my_grLfbUnlock);
        } else if (!strcmp(n, "_grTexDownloadTable")) {
            real_grTexDownloadTable = (PFN_table)p;
            hook_jmp(s->addr, my_grTexDownloadTable);
        } else {
            volatile LONG *counter = NULL;
            int capture_idx = -1;
            if ((g_stats || g_capture_at) && g_nstat < MAX_STAT) {
                g_stat_name[g_nstat] = n;
                g_stat_argbytes[g_nstat] = argbytes;
                if (g_stats)
                    counter = &g_stat_calls[g_nstat];
                else
                    capture_idx = (int)g_nstat;
                g_nstat++;
            }
            void *thunk = make_thunk(p, argbytes, counter, capture_idx);
            const void *target = thunk;
            for (unsigned k = 0; k < sizeof k_scaled / sizeof k_scaled[0]; k++)
                if (!strcmp(n, k_scaled[k].name)) {
                    *k_scaled[k].real = thunk; /* cdecl, so the scaler can call it directly */
                    target = k_scaled[k].hook;
                }
            hook_jmp(s->addr, target);
        }
        bridged++;
    }
    hook_name("sst1InitReturnStatus", my_sst1InitReturnStatus);
    hook_name("sst1InitRead32", my_sst1InitRead32);
    hook_name("sst1InitWrite32", my_sst1InitWrite32);
    hy_log("glide: %u entry points bridged to %s (%u static-only helpers left alone)", bridged,
           g_cfg.glide_dll, internal);
}
