/*
 * First-run setup: extract the game files from the arcade hard disk image.
 *
 * hydro.exe carries none of the game's files. They come from MAME's hydro.chd (a CHD v5 hard disk,
 * read with vendor/libchdr), whose MBR/EBR chain holds three FAT16 volumes, C: D: and E:. Every file is
 * extracted to <data>\<letter>\...
 *
 * Extraction is restartable: each file is written as .part and renamed when complete, and
 * C:\HT.R2 and C:\HYDRO.EXE (the files whose presence means "extracted") go last.
 */
#include "disk_setup.h"
#include "host.h"
#include "../common/sha256.h"

#include <libchdr/chd.h>

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHA-256 of the HYDRO.EXE every address in the port refers to (MAME's hydro.chd). */
static const char k_exe_sha256[] = "323eb5d23541c233285d8c071c2c32c13aad34c03c53a7c016f839bcdc67da6e";

enum { SECTOR = 512, RUN_BYTES = 1 << 20 };

/* ---- CHD as a flat disk ------------------------------------------------------------------ */

static chd_file *g_chd;
static uint32_t g_hunkbytes;
static uint64_t g_disk_bytes;
static uint8_t *g_hunk;
static uint32_t g_hunk_no = 0xffffffffu;

static int disk_read(uint64_t off, void *buf, size_t n)
{
    uint8_t *out = buf;
    if (off + n > g_disk_bytes)
        return 0;
    while (n) {
        uint32_t h = (uint32_t)(off / g_hunkbytes), o = (uint32_t)(off % g_hunkbytes);
        if (h != g_hunk_no) {
            chd_error err = chd_read(g_chd, h, g_hunk);
            if (err != CHDERR_NONE) {
                hy_log("disk: hunk %u: %s", h, chd_error_string(err));
                g_hunk_no = 0xffffffffu;
                return 0;
            }
            g_hunk_no = h;
        }
        size_t take = g_hunkbytes - o < n ? g_hunkbytes - o : n;
        memcpy(out, g_hunk + o, take);
        out += take;
        off += take;
        n -= take;
    }
    return 1;
}

/* ---- FAT12/16 ---------------------------------------------------------------------------- */

typedef struct Vol {
    char letter;
    uint64_t base;        /* byte offset of the boot sector */
    uint32_t cluster;     /* bytes per cluster */
    uint32_t nclusters;
    uint64_t root, data;  /* byte offsets of the root directory and cluster 2 */
    uint32_t nroot;
    int fat12;
    uint8_t *fat;
} Vol;

typedef struct Entry {
    Vol *vol;
    char path[MAX_PATH]; /* relative to <data>\<letter> */
    uint16_t clus;
    uint32_t size;
    int is_dir;
} Entry;

static Vol g_vols[8];
static int g_nvols;
static Entry *g_entries;
static int g_nentries, g_cap;

static uint32_t fat_next(const Vol *v, uint32_t c)
{
    if (c < 2 || c >= v->nclusters + 2)
        return 0;
    if (v->fat12) {
        uint32_t w = v->fat[c * 3 / 2] | v->fat[c * 3 / 2 + 1] << 8;
        w = c & 1 ? w >> 4 : w & 0xfff;
        return w >= 0xff8 ? 0 : w;
    }
    uint32_t w = v->fat[c * 2] | v->fat[c * 2 + 1] << 8;
    return w >= 0xfff8 ? 0 : w;
}

static uint64_t cluster_off(const Vol *v, uint32_t c)
{
    return v->data + (uint64_t)(c - 2) * v->cluster;
}

static int vol_open(Vol *v, char letter, uint64_t base)
{
    uint8_t bs[SECTOR];
    if (!disk_read(base, bs, sizeof bs) || bs[510] != 0x55 || bs[511] != 0xaa)
        return 0;
    uint16_t bps = *(uint16_t *)(bs + 11), rsvd = *(uint16_t *)(bs + 14), nroot = *(uint16_t *)(bs + 17);
    uint16_t tot16 = *(uint16_t *)(bs + 19), spf = *(uint16_t *)(bs + 22);
    uint8_t spc = bs[13], nfats = bs[16];
    uint32_t total = tot16 ? tot16 : *(uint32_t *)(bs + 32);
    uint32_t root_sec = rsvd + nfats * spf, data_sec = root_sec + (nroot * 32 + bps - 1) / bps;
    if (bps != SECTOR || !spc || !nfats || !spf || total <= data_sec)
        return 0;
    v->letter = letter;
    v->base = base;
    v->cluster = spc * bps;
    v->nclusters = (total - data_sec) / spc;
    v->fat12 = v->nclusters < 4085;
    v->root = base + (uint64_t)root_sec * bps;
    v->data = base + (uint64_t)data_sec * bps;
    v->nroot = nroot;
    v->fat = malloc((size_t)spf * bps);
    return v->fat && disk_read(base + (uint64_t)rsvd * bps, v->fat, (size_t)spf * bps);
}

/* Partitions in DOS drive-letter order (primaries, then the logical drives). */
static void find_volumes(void)
{
    uint8_t s[SECTOR];
    uint32_t ext_base = 0, ebr = 0;
    int drive = 0;
    if (!disk_read(0, s, sizeof s) || s[510] != 0x55 || s[511] != 0xaa)
        return;
    for (int pass = 0;; pass++) {
        uint32_t next = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s + 446 + i * 16;
            uint32_t start = *(uint32_t *)(e + 8);
            if (!e[4])
                continue;
            if (e[4] == 0x05 || e[4] == 0x0f) {
                if (pass == 0)
                    ext_base = start;
                else
                    next = ext_base + start;
                continue;
            }
            char letter = (char)('C' + drive++);
            int fat = e[4] == 0x01 || e[4] == 0x04 || e[4] == 0x06 || e[4] == 0x0e;
            if (fat && g_nvols < (int)(sizeof g_vols / sizeof g_vols[0]) &&
                vol_open(&g_vols[g_nvols], letter, (uint64_t)(ebr + start) * SECTOR))
                g_nvols++;
        }
        ebr = pass == 0 ? ext_base : next;
        if (!ebr || pass > 64 || !disk_read((uint64_t)ebr * SECTOR, s, sizeof s) || s[510] != 0x55 ||
            s[511] != 0xaa)
            break;
    }
}

/* Reads a whole cluster chain (directories only; they are small). */
static uint8_t *read_chain(const Vol *v, uint32_t c, size_t *len)
{
    uint8_t *buf = NULL;
    *len = 0;
    for (uint32_t n = 0; c && n <= v->nclusters; c = fat_next(v, c), n++) {
        uint8_t *grown = realloc(buf, *len + v->cluster);
        if (!grown || !disk_read(cluster_off(v, c), grown + *len, v->cluster)) {
            free(grown ? grown : buf);
            return NULL;
        }
        buf = grown;
        *len += v->cluster;
    }
    return buf;
}

static void walk(Vol *v, uint32_t clus, const char *prefix, int depth)
{
    size_t len;
    uint8_t *raw;
    if (clus == 0) {
        len = (size_t)v->nroot * 32;
        raw = malloc(len);
        if (raw && !disk_read(v->root, raw, len)) {
            free(raw);
            raw = NULL;
        }
    } else {
        raw = read_chain(v, clus, &len);
    }
    if (!raw)
        hy_fatal("disk image: cannot read a directory of %c:", v->letter);

    for (size_t i = 0; i + 32 <= len && raw[i]; i += 32) {
        const uint8_t *e = raw + i;
        if (e[0] == 0xe5 || e[11] == 0x0f || e[11] & 0x08)
            continue;
        char name[13];
        int n = 0;
        for (int k = 0; k < 8 && e[k] != ' '; k++)
            name[n++] = (char)e[k];
        if (e[8] != ' ') {
            name[n++] = '.';
            for (int k = 8; k < 11 && e[k] != ' '; k++)
                name[n++] = (char)e[k];
        }
        name[n] = 0;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        for (char *p = name; *p; p++)
            if ((unsigned char)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
                *p = '_';

        if (g_nentries == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 64;
            g_entries = realloc(g_entries, g_cap * sizeof *g_entries);
            if (!g_entries)
                hy_fatal("out of memory");
        }
        Entry *out = &g_entries[g_nentries++];
        out->vol = v;
        snprintf(out->path, sizeof out->path, "%s%s%s", prefix, *prefix ? "\\" : "", name);
        out->clus = *(uint16_t *)(e + 26);
        out->size = *(uint32_t *)(e + 28);
        out->is_dir = (e[11] & 0x10) != 0;
        if (out->is_dir && out->clus >= 2 && depth < 16) {
            char sub[MAX_PATH];
            snprintf(sub, sizeof sub, "%s", out->path);
            walk(v, out->clus, sub, depth + 1);
        }
    }
    free(raw);
}

static Entry *find_entry(char letter, const char *path)
{
    for (int i = 0; i < g_nentries; i++)
        if (g_entries[i].vol->letter == letter && !_stricmp(g_entries[i].path, path))
            return &g_entries[i];
    return NULL;
}

/* ---- progress window --------------------------------------------------------------------- */

static HWND g_ui;
static char g_ui_line[MAX_PATH + 32];
static uint64_t g_done, g_total;
static volatile int g_cancel;
static DWORD g_ui_tick;

static LRESULT CALLBACK ui_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT text = {16, 14, rc.right - 16, 34};
        DrawTextA(dc, g_ui_line, -1, &text, DT_SINGLELINE | DT_PATH_ELLIPSIS);
        RECT bar = {16, 42, rc.right - 16, 62};
        FrameRect(dc, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
        InflateRect(&bar, -2, -2);
        bar.right = bar.left + (LONG)((bar.right - bar.left) * (g_total ? (double)g_done / g_total : 0));
        FillRect(dc, &bar, GetSysColorBrush(COLOR_HIGHLIGHT));
        EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (MessageBoxA(w, "Stop extracting the game files?", "Hydro Thunder", MB_YESNO | MB_ICONQUESTION) ==
            IDYES)
            g_cancel = 1;
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static void ui_open(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "HydroThunderSetup";
    RegisterClassA(&wc);
    DWORD style = WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, 460, 78};
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    g_ui = CreateWindowA("HydroThunderSetup", "Hydro Thunder: extracting game files", style | WS_VISIBLE,
                         (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                         NULL, NULL, wc.hInstance, NULL);
}

static void ui_update(int force)
{
    MSG m;
    if (!g_ui || (!force && GetTickCount() - g_ui_tick < 50))
        return;
    g_ui_tick = GetTickCount();
    InvalidateRect(g_ui, NULL, FALSE);
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

static void ui_close(void)
{
    if (g_ui)
        DestroyWindow(g_ui);
    g_ui = NULL;
}

/* ---- extraction -------------------------------------------------------------------------- */

static void make_dirs(const char *path)
{
    int err = SHCreateDirectoryExA(NULL, path, NULL);
    if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS)
        hy_fatal("cannot create folder %s (error %d)", path, err);
}

/* Streams a file's cluster chain to out (and/or into mem), merging contiguous clusters into
 * reads of up to RUN_BYTES. Returns 0 on a read error. */
static int extract_entry(const Entry *e, FILE *out, uint8_t *mem)
{
    const Vol *v = e->vol;
    static uint8_t *buf;
    if (!buf && !(buf = malloc(RUN_BYTES)))
        hy_fatal("out of memory");
    uint32_t left = e->size, c = e->clus, guard = 0;
    while (left) {
        if (c < 2 || guard > v->nclusters)
            return 0;
        uint32_t first = c, n = 1;
        for (;;) {
            uint32_t nx = fat_next(v, c);
            guard++;
            c = nx;
            if (nx != first + n || (uint64_t)(n + 1) * v->cluster > RUN_BYTES || (uint64_t)n * v->cluster >= left)
                break;
            n++;
        }
        uint32_t take = n * v->cluster < left ? n * v->cluster : left;
        if (!disk_read(cluster_off(v, first), buf, take))
            return 0;
        if (out && fwrite(buf, 1, take, out) != take)
            return 0;
        if (mem) {
            memcpy(mem, buf, take);
            mem += take;
        }
        left -= take;
        g_done += take;
        ui_update(0);
        if (g_cancel)
            return 0;
    }
    return 1;
}

static void cancelled(void)
{
    hy_log("setup: cancelled");
    ExitProcess(1);
}

static void extract_file(const Entry *e, const char *path, const char *chd_path)
{
    char part[MAX_PATH + 8];
    snprintf(g_ui_line, sizeof g_ui_line, "%c:\\%s", e->vol->letter, e->path);
    ui_update(1);
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f)
        hy_fatal("cannot write %s", part);
    int ok = extract_entry(e, f, NULL);
    ok = !fclose(f) && ok;
    if (!ok || !MoveFileExA(part, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(part);
        if (g_cancel)
            cancelled();
        hy_fatal("extracting %c:\\%s from %s failed", e->vol->letter, e->path, chd_path);
    }
    hy_log("setup: %c:\\%s (%u bytes)", e->vol->letter, e->path, e->size);
}

static void check_exe(const Entry *exe)
{
    uint8_t *mem = malloc(exe->size ? exe->size : 1), digest[32];
    char hex[65];
    if (!mem || !extract_entry(exe, NULL, mem)) {
        if (g_cancel)
            cancelled();
        hy_fatal("disk image: cannot read C:\\HYDRO.EXE");
    }
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, mem, exe->size);
    sha256_final(&s, digest);
    free(mem);
    for (int i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    g_done = 0;
    if (strcmp(hex, k_exe_sha256))
        hy_fatal("This disk image holds a different version of the game.\n\n"
                 "C:\\HYDRO.EXE sha256 %s\nexpected %s\n\n"
                 "The port needs the hard disk of MAME's \"hydro\" set (hydro.chd).",
                 hex, k_exe_sha256);
}

static void extract(const char *chd_path)
{
    hy_log("setup: extracting %s to %s", chd_path, g_cfg.data_root);
    chd_error err = chd_open(chd_path, CHD_OPEN_READ, NULL, &g_chd);
    if (err != CHDERR_NONE)
        hy_fatal("cannot open %s: %s", chd_path, chd_error_string(err));
    const chd_header *h = chd_get_header(g_chd);
    g_hunkbytes = h->hunkbytes;
    g_disk_bytes = h->logicalbytes;
    if (!(g_hunk = malloc(g_hunkbytes)))
        hy_fatal("out of memory");

    find_volumes();
    for (int i = 0; i < g_nvols; i++)
        walk(&g_vols[i], 0, "", 0);
    Entry *exe = find_entry('C', "HYDRO.EXE"), *r2 = find_entry('C', "HT.R2");
    if (!exe || !r2 || exe->is_dir || r2->is_dir)
        hy_fatal("%s is not the Hydro Thunder hard disk (no C:\\HYDRO.EXE and C:\\HT.R2)", chd_path);

    snprintf(g_ui_line, sizeof g_ui_line, "Checking %s", chd_path);
    ui_open();
    ui_update(1);
    check_exe(exe);

    g_total = 0;
    for (int i = 0; i < g_nentries; i++)
        if (!g_entries[i].is_dir)
            g_total += g_entries[i].size;

    char path[MAX_PATH];
    for (int i = 0; i < g_nvols; i++) {
        snprintf(path, sizeof path, "%s\\%c", g_cfg.data_root, g_vols[i].letter);
        make_dirs(path);
    }
    /* The marker files last, so an interrupted run is redone next time. */
    int files = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_nentries; i++) {
            const Entry *e = &g_entries[i];
            if ((e == exe || e == r2) != pass)
                continue;
            snprintf(path, sizeof path, "%s\\%c\\%s", g_cfg.data_root, e->vol->letter, e->path);
            if (e->is_dir)
                make_dirs(path);
            else
                extract_file(e, path, chd_path), files++;
        }
    ui_close();
    hy_log("setup: %d files, %llu bytes extracted", files, (unsigned long long)g_total);

    chd_close(g_chd);
    g_chd = NULL;
    free(g_hunk);
    for (int i = 0; i < g_nvols; i++)
        free(g_vols[i].fat);
    free(g_entries);
    g_entries = NULL;
    g_nentries = g_cap = g_nvols = 0;
}

/* ---- finding the CHD --------------------------------------------------------------------- */

static int exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* hydro.chd next to the launch folder or hydro.exe, also in a MAME-style hydro\ subfolder. */
static int find_chd(char *out)
{
    char dirs[2][MAX_PATH];
    snprintf(dirs[0], MAX_PATH, "%s", g_cfg.work_dir);
    GetModuleFileNameA(NULL, dirs[1], MAX_PATH);
    char *slash = strrchr(dirs[1], '\\');
    if (slash)
        *slash = 0;
    static const char *const names[] = {"hydro.chd", "hydro\\hydro.chd", "roms\\hydro\\hydro.chd"};
    for (int d = 0; d < 2; d++)
        for (int n = 0; n < 3; n++) {
            snprintf(out, MAX_PATH, "%s\\%s", dirs[d], names[n]);
            if (exists(out))
                return 1;
        }
    return 0;
}

static int ask_chd(char *out)
{
    OPENFILENAMEA ofn = {0};
    out[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "Hydro Thunder hard disk (hydro.chd)\0*.chd\0All files\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_cfg.work_dir;
    ofn.lpstrTitle = "Hydro Thunder: locate the arcade hard disk image (hydro.chd)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn);
}

void disk_setup(const char *chd)
{
    char r2[MAX_PATH], found[MAX_PATH];
    snprintf(r2, sizeof r2, "%s\\C\\HT.R2", g_cfg.data_root);
    if (chd) {
        if (!exists(chd))
            hy_fatal("cannot find %s", chd);
        extract(chd);
    } else if (!exists(r2) || !exists(g_cfg.exe_path)) {
        hy_log("setup: no game data in %s", g_cfg.data_root);
        if (!find_chd(found)) {
            MessageBoxA(NULL,
                        "Hydro Thunder needs the game files from the arcade hard disk.\n\n"
                        "Select the disk image (hydro.chd, from MAME's \"hydro\" set). Its files are "
                        "extracted once, to the data folder next to hydro.exe.",
                        "Hydro Thunder", MB_OK | MB_ICONINFORMATION);
            if (!ask_chd(found))
                hy_fatal("No disk image selected. Put hydro.chd next to hydro.exe, or run hydro.exe --chd <file>.");
        }
        extract(found);
    }
    if (!exists(g_cfg.exe_path))
        hy_fatal("cannot find %s", g_cfg.exe_path);
}
