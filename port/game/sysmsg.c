/*
 * SYSMSG.OBJ: the two link status screens (statemgr states 1 and 8), ported to list up to 16
 * units. The original draws a row per unit 1-4, 21 pixels apart, labelled "UNIT " + ('1' + i).
 * Here the rows run to the highest unit in play (at least 4, or the lobby's size), and closer
 * together, in smaller text, when there are more than 8.
 */
#include "port.h"

#include "net_link.h"

#include "../host/lobby_client.h"

#include <stdio.h>

typedef void(__cdecl *VoidFn)(void);

#define Text_fScale HY_GLOBAL(float, Text_fScale)
#define Init3dfx_nHorizontalPixels HY_GLOBAL(int, Init3dfx_nHorizontalPixels)
#define Gameloop_nFrameCounter HY_GLOBAL(uint32_t, Gameloop_nFrameCounter)

/* Strings in the image */
#define S_ESTABLISHING ((const char *)0x00303478u) /* "ESTABLISHING NETWORK LINK" */
#define S_WAITING_LINKED ((const char *)0x003034c0u) /* "WAITING FOR LINKED MACHINES" */
#define S_COLON ((const char *)0x00303474u)
#define S_ONLINE_ME ((const char *)0x00303468u)
#define S_ONLINE ((const char *)0x00303460u)
#define S_OFFLINE ((const char *)0x00303448u) /* "NO RESPONSE / OFFLINE" */
#define S_READY_ME ((const char *)0x003034b4u)
#define S_READY ((const char *)0x003034acu)
#define S_WAITING ((const char *)0x00303494u) /* "WAITING FOR RESPONSE" */
#define S_FOOTER ((char *)0x00303334u)       /* its bytes 2-3 hold the screen's code */

/* The progress bar's frame: 4 GrVertex (0x3c bytes each), x at +0, y at +4 */
#define BAR_VERTEX(i) ((float *)(uintptr_t)(0x0058a91cu + (i) * 0x3cu))

#define gutil_ClearFrameBufferToBlack HY_CALL(VoidFn, gutil_ClearFrameBufferToBlack)
#define text_SetFont HY_CALL(void(__cdecl *)(int), text_SetFont)
#define text_StrWidth HY_CALL(float(__cdecl *)(const char *), text_StrWidth)
#define text_PrintStr HY_CALL(void(__cdecl *)(float, float, const char *), text_PrintStr)
#define text_Flush HY_CALL(VoidFn, text_Flush)
#define grGlideGetState HY_CALL(void(__cdecl *)(void *), grGlideGetState)
#define grGlideSetState HY_CALL(void(__cdecl *)(const void *), grGlideSetState)
#define grColorCombine HY_CALL(void(__cdecl *)(int, int, int, int, int), grColorCombine)
#define grAlphaCombine HY_CALL(void(__cdecl *)(int, int, int, int, int), grAlphaCombine)
#define grConstantColorValue HY_CALL(void(__cdecl *)(uint32_t), grConstantColorValue)
#define grDrawTriangle HY_CALL(void(__cdecl *)(const void *, const void *, const void *), grDrawTriangle)
#define sysmsg_DrawBarCell ((void(__cdecl *)(int, int, int))0x001708c0u) /* (filled, x, y) */

#define ROW_Y 125.0f
#define ROW_STEP 21.0f

static void print(int font, float x, float y, const char *s, float scale)
{
    text_SetFont(font);
    Text_fScale = scale;
    text_PrintStr(x, y, s);
}

static void title(const char *s)
{
    if (!(Gameloop_nFrameCounter & 0xc))
        return;
    text_SetFont(0);
    Text_fScale = 1.0f;
    float w = text_StrWidth(s);
    text_PrintStr((float)(Init3dfx_nHorizontalPixels >> 1) - w * 0.5f, 70.0f, s);
}

static void footer(char code)
{
    S_FOOTER[2] = '0';
    S_FOOTER[3] = code;
    print(1, 20.0f, 380.0f, S_FOOTER, 1.0f);
    text_Flush();
}

/* The units to list: at least 4 (the original), the lobby's size, and every unit seen. */
static uint32_t units_shown(uint32_t me, uint32_t mask)
{
    uint32_t n = 4;
    if (lobby_active(NULL) && (uint32_t)lobby_max() > n)
        n = (uint32_t)lobby_max();
    for (uint32_t u = 0; u < NET_UNITS; u++)
        if ((u == me || (mask & (1u << u))) && u + 1 > n)
            n = u + 1;
    return n > NET_UNITS ? NET_UNITS : n;
}

static void row(uint32_t unit, float y, const char *status, int dense)
{
    char label[16];
    float scale = dense ? 0.75f : 1.0f;
    snprintf(label, sizeof label, "UNIT %u", unit + 1);
    print(1, 85.0f, y, label, scale);
    print(1, dense ? 145.0f : 155.0f, y, S_COLON, scale);
    print(1, dense ? 160.0f : 175.0f, y, status, scale);
}

/* State 1: every unit, online or not; the link-up progress bar. */
void sysmsg_Display_EstablishingLink(uint32_t me, uint32_t heard_mask, float progress)
{
    gutil_ClearFrameBufferToBlack();
    title(S_ESTABLISHING);
    uint32_t n = units_shown(me, heard_mask);
    int dense = n > 8;
    float step = dense ? 240.0f / (float)n : ROW_STEP;
    for (uint32_t u = 0; u < n; u++)
        row(u, ROW_Y + step * (float)u, u == me ? S_ONLINE_ME : (heard_mask & (1u << u)) ? S_ONLINE : S_OFFLINE,
            dense);

    uint8_t state[1024]; /* GrState (312 bytes in the original's frame) */
    grGlideGetState(state);
    grColorCombine(1, 0, 1, 2, 0);
    grAlphaCombine(1, 0, 1, 2, 0);
    int cx = Init3dfx_nHorizontalPixels >> 1, x = cx - 0x95;
    grConstantColorValue(0xff000080u);
    BAR_VERTEX(0)[0] = BAR_VERTEX(1)[0] = (float)(cx - 0x98) + 0.5f;
    BAR_VERTEX(2)[0] = BAR_VERTEX(3)[0] = (float)(cx + 0x98) + 0.5f;
    BAR_VERTEX(0)[1] = BAR_VERTEX(3)[1] = 48.5f;
    BAR_VERTEX(1)[1] = BAR_VERTEX(2)[1] = 59.5f;
    grDrawTriangle(BAR_VERTEX(0), BAR_VERTEX(1), BAR_VERTEX(2));
    grDrawTriangle(BAR_VERTEX(0), BAR_VERTEX(2), BAR_VERTEX(3));
    int filled = (int)(progress * 30.0f);
    for (int i = 0; i < 30; i++, x += 10)
        sysmsg_DrawBarCell(i < filled, x, 0x32);
    grGlideSetState(state);
    footer('1');
}
HY_PORT(sysmsg_Display_EstablishingLink)

/* State 8: the units linked before (and me), each ready or still waiting. */
void sysmsg_Display_ReestablishingLink(uint32_t me, uint32_t linked_mask, uint32_t ready_mask)
{
    gutil_ClearFrameBufferToBlack();
    title(S_WAITING_LINKED);
    uint32_t n = 0;
    for (uint32_t u = 0; u < NET_UNITS; u++)
        n += u == me || (linked_mask & (1u << u));
    int dense = n > 8;
    float y = ROW_Y, step = dense ? 240.0f / (float)n : ROW_STEP;
    for (uint32_t u = 0; u < NET_UNITS; u++) {
        if (u != me && !(linked_mask & (1u << u)))
            continue;
        row(u, y, u == me ? S_READY_ME : (ready_mask & (1u << u)) ? S_READY : S_WAITING, dense);
        y += step;
    }
    footer('5');
}
HY_PORT(sysmsg_Display_ReestablishingLink)
