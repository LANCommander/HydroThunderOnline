/* R2FILE.OBJ: the HT.R2 asset archive. Port-only: an overlay archive whose objects are appended to
 * HT.R2's directory at open, so new art (the player markers 9-16) loads through the original obsys code
 * without touching HT.R2. The overlay is data\C\MARKERS.R2X if that file exists, otherwise the one
 * host/markers.cpp builds from the markers embedded in hydro.exe. The comprehensive checksum the link
 * compares is computed from HT.R2 alone, before the overlay goes in. */
#include "port.h"

#include "../host/hook.h"
#include "../host/host.h"
#include "../host/markers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R2_HDR_SIZE 0x88u
#define R2_DIRENT 0x4cu
#define R2_MEMENT 20u /* the packed in-memory entry: 6-bit name + offset/size/refcount, +0x10 refs */

#define R2file_hFile (*(void **)0x00589674u)
#define R2file_aDir (*(uint8_t **)0x0058965cu)
#define R2file_nDir (*(uint32_t *)0x0058966cu)

#define r2file_OpenForRead HY_CALL(int(__cdecl *)(const char *), r2file_OpenForRead)
#define r2file_NameToDoid HY_CALL(int(__cdecl *)(const char *), r2file_NameToDoid)
#define r2file_PackDirEntry ((void(__cdecl *)(const void *, void *))0x001697b0u) /* (disk entry, mem entry) */
#define cacheio_fseek HY_CALL(int(__cdecl *)(void *, int), cacheio_fseek)
#define cacheio_fread HY_CALL(uint32_t(__cdecl *)(void *, uint32_t, void *), cacheio_fread)

/* Overlay offsets live past HT.R2's end (0xaab45c8). ReadRaw seeks only for offsets > -1. */
#define OVERLAY_BASE 0x40000000u

static uint8_t *g_ovl;
static uint32_t g_ovl_size;
static int g_ovl_active; /* the last seek went into the overlay; offset -1 reads continue there */
static uint32_t g_ovl_pos;

int __cdecl r2file_ReadRaw(void *buf, uint32_t size, int offset)
{
    if (!size)
        return 1;
    if (offset > -1) {
        g_ovl_active = g_ovl && (uint32_t)offset >= OVERLAY_BASE;
        if (g_ovl_active)
            g_ovl_pos = (uint32_t)offset - OVERLAY_BASE;
        else if (!cacheio_fseek(R2file_hFile, offset))
            return 0;
    }
    if (g_ovl_active) {
        if (g_ovl_pos > g_ovl_size || size > g_ovl_size - g_ovl_pos)
            return 0;
        memcpy(buf, g_ovl + g_ovl_pos, size);
        g_ovl_pos += size;
        return 1;
    }
    return cacheio_fread(buf, size, R2file_hFile) == size;
}
HY_PORT(r2file_ReadRaw)

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *size = b ? (uint32_t)n : 0;
    return b;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Appends the overlay's objects to the directory r2file_OpenForRead just built. */
static void add_overlay(void)
{
    char path[300];
    snprintf(path, sizeof path, "%s\\C\\MARKERS.R2X", g_cfg.data_root);
    uint32_t size;
    uint8_t *b = read_file(path, &size);
    if (!b) {
        char r2[300];
        snprintf(r2, sizeof r2, "%s\\C\\HT.R2", g_cfg.data_root);
        b = markers_build_overlay(r2, &size);
        snprintf(path, sizeof path, "the built-in markers");
    }
    if (!b)
        return;
    if (size < R2_HDR_SIZE || memcmp(b, "James Cameron Rules", 20) || rd32(b + 0x14) != R2_HDR_SIZE) {
        hy_log("r2file: %s is not an R2 archive, ignored", path);
        free(b);
        return;
    }
    uint32_t dofs = rd32(b + 0x20), n = rd32(b + 0x2c);
    if (n > 4096 || dofs > size || n * R2_DIRENT > size - dofs) {
        hy_log("r2file: %s has a bad directory, ignored", path);
        free(b);
        return;
    }

    uint32_t old = R2file_nDir;
    uint8_t *dir = calloc(old + n, R2_MEMENT); /* never freed: r2file_ModuleClose only forgets it */
    memcpy(dir, R2file_aDir, old * R2_MEMENT);
    uint32_t added = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t e[R2_DIRENT];
        memcpy(e, b + dofs + i * R2_DIRENT, R2_DIRENT);
        char name[12];
        memcpy(name, e + 12, 11);
        name[11] = 0;
        uint32_t off = rd32(e), body = rd32(e + 4), nrefs = rd32(e + 8);
        uint32_t end = off + 4 + body + 4 + nrefs * 16 + 12;
        if (!body || (off & 3) || nrefs > 0x3fff || body >= 0x800000 || off > size || end < off || end > size ||
            (rd32(b + off) & 0xffffff) != body) {
            hy_log("r2file: overlay %s: bad record, skipped", name);
            continue;
        }
        if (r2file_NameToDoid(name) != -1) {
            hy_log("r2file: overlay %s is already in HT.R2, skipped", name);
            continue;
        }
        uint32_t v = OVERLAY_BASE + off;
        memcpy(e, &v, 4);
        uint8_t *m = dir + (old + added) * R2_MEMENT;
        r2file_PackDirEntry(e, m);
        uint32_t refs = (uint32_t)(uintptr_t)(b + off + 4 + body + 4); /* OpenForRead preloads these */
        memcpy(m + 0x10, &refs, 4);
        added++;
    }
    if (!added) {
        free(dir);
        free(b);
        return;
    }
    g_ovl = b;
    g_ovl_size = size;
    R2file_aDir = dir;
    R2file_nDir = old + added;
    hy_log("r2file: added %u objects from %s", added, path);
}

static int __cdecl open_with_overlay(const char *name)
{
    int r = r2file_OpenForRead(name);
    if (R2file_aDir && R2file_nDir)
        add_overlay();
    return r;
}

void r2file_install(void)
{
    const uint32_t at = 0x0013d3d8u; /* gameloop_ModuleInit: call r2file_OpenForRead("ht.r2") */
    hook_patch32(at + 1, HY_FN_r2file_OpenForRead - (at + 5), (uint32_t)(uintptr_t)open_with_overlay - (at + 5));
}
