#pragma once
/*
 * dgVoodoo configuration through dgVoodoo's API library (vendor/dgVoodoo2, dgVoodooAPI.dll).
 *
 * The Glide wrapper reads dgVoodoo.conf from its own folder at every grGlideInit, and the game
 * shuts Glide down and re-inits it around the operator menu. So writing the file is enough: new
 * settings take effect when the menu exits (or at the next launch).
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Graphics settings the port manages. Everything else in dgVoodoo.conf is left as the user set it. */
typedef struct DgvGraphics {
    int windowed;   /* 1 = windowed, 0 = fullscreen */
    int resolution; /* DGV_RES_* */
    int scaling;    /* DGV_SCALE_* */
    int msaa;       /* 0, 2, 4 or 8 */
    int filter;     /* 0 = game, 1 = point, 2 = bilinear */
    int vsync;
    int resampling; /* 0 point, 1 bilinear, 2 bicubic, 3 lanczos-2, 4 lanczos-3 */
    int brightness; /* percent, 100 = neutral */
    int contrast;
    int color;
} DgvGraphics;

enum {
    DGV_RES_GAME, /* unforced: the 640x480 surface the game is scaled to (host/hle_glide.c) */
    DGV_RES_2X,
    DGV_RES_3X,
    DGV_RES_4X,
    DGV_RES_5X,
    DGV_RES_6X,
    DGV_RES_MAX,         /* desktop size, keeping the game's aspect */
    DGV_RES_MAX_INTEGER, /* largest integer multiple that fits the desktop */
    DGV_RES_DESKTOP,
    DGV_RES_COUNT
};

enum {
    DGV_SCALE_STRETCHED,
    DGV_SCALE_ASPECT,
    DGV_SCALE_ASPECT_4_3,
    DGV_SCALE_CENTERED,
    DGV_SCALE_CENTERED_ASPECT,
    DGV_SCALE_CRT_4_3,
    DGV_SCALE_COUNT
};

/* Write the settings into dgVoodoo.conf next to the Glide DLL (conf_path). Returns 1 on success. */
int dgv_write_conf(const DgvGraphics *g, const char *conf_path);

#ifdef __cplusplus
}
#endif
