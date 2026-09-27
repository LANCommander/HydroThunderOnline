#pragma once
/*
 * The port's own player tracking markers 9-16 (assets/markers/marker09..16.png, embedded in hydro.exe
 * by hydro.rc as RCDATA 109..116).
 *
 * HT.R2 has markers 1-8. The new ones are packed into an overlay archive in HT.R2's own format, using
 * HT.R2's marker 8 mesh and texture as the template, which game/r2file.c appends to the archive's
 * directory at load. HT.R2 itself is never modified, and nothing from it is embedded in the exe.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resource id of markerNN.png is MARKERS_RESOURCE_BASE + NN (hydro.rc). */
#define MARKERS_RESOURCE_BASE 100

/* Builds the overlay from ht_r2_path's template objects and the embedded PNGs. Returns a malloc'd
 * archive (the caller owns it) and its size, or NULL if no marker could be built. */
uint8_t *markers_build_overlay(const char *ht_r2_path, uint32_t *size);

#ifdef __cplusplus
}
#endif
