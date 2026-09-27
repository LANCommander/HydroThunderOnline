/* SCISSOR.OBJ: the 2D clip rectangle for blit_Raw. */
#include "port.h"

int glide_screen_rows(void); /* host/hle_glide.c: rows of the open Glide resolution */

#define Scissor_nLeftX HY_GLOBAL(int, Scissor_nLeftX)
#define Scissor_nRightX HY_GLOBAL(int, Scissor_nRightX)
#define Scissor_nBottomY HY_GLOBAL(int, Scissor_nBottomY)
#define Scissor_nTopY HY_GLOBAL(int, Scissor_nTopY)
#define Viewport_hres HY_GLOBAL(int, Viewport_hres)
#define Viewport_vres HY_GLOBAL(int, Viewport_vres)

void scissor_SetToViewport(void)
{
    Scissor_nTopY = Viewport_vres - 1;
    Scissor_nRightX = Viewport_hres - 1;
    Scissor_nLeftX = 0;
    Scissor_nBottomY = 0;
    HY_CALL(void(__cdecl *)(int, int, int, int), grClipWindow)(0, 0, Scissor_nRightX, Scissor_nTopY);
    /* Port-only: the game runs 512x400 (a coin-op video timing on Glide's 512x384). The host scales
     * that onto a 640x480 surface and hands LFB locks a 400-row shadow, so this is normally a no-op.
     * With HYDRO_GLIDE_DEBUG=noscale the LFB has only 384 rows, and blit_Raw's direct LFB copy of a
     * full-screen picture (the attract stills) would write 16 rows past its end. */
    int rows = glide_screen_rows();
    if (rows > 0 && Scissor_nTopY > rows - 1)
        Scissor_nTopY = rows - 1;
}
HY_PORT(scissor_SetToViewport)
