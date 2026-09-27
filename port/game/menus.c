/* MENUS.OBJ: the operator/test menu driver (vinput/vgraphics/vtext + the MESS message loop). */
#include "port.h"

#include "../host/host.h"
#include "../host/input.h"
#include "../host/lobby_client.h"

typedef void(__cdecl *VoidFn)(void);
typedef int(__cdecl *IntFn)(void);
typedef void(__cdecl *HandlerFn)(void);

#define Menuutil_default_handler HY_GLOBAL(HandlerFn, Menuutil_default_handler)
/* Unnamed statics */
#define Menus_bCmosOk (*(int *)0x0060331cu)  /* wrs_Init + cmos_ModuleInit succeeded */
#define Menus_nFrame (*(int *)0x00603318u)   /* frames shown since the menu opened */
#define Vinput_bLowRes (*(int *)0x0063937cu) /* C:\htlowres.bin exists: 512x384 instead of 512x400 */
#define menus_LoadSettings ((VoidFn)0x001bb330u) /* read the CMOS blocks (or defaults), apply volume + ADC cal */
#define menus_Pump ((IntFn)0x001bb5c0u)          /* dispatch keys/messages; nonzero once the menu quits */

void operator_ClearLobbyOptions(void); /* operator.c */

/* mode 1 = development menu, 3 = scripted self-test, 4/6 = switch test, anything else = operator menu.
 * _main always passes 2. Returns once the operator picks START THE GAME. */
int menus(int mode)
{
    /* Port-only: the host's keyboard aliases for the menu (arrows/Enter/Backspace) apply while here,
     * and switches still held from entering (Test) are ignored until released. Before vinput's
     * first poll, which would otherwise queue the held Test as "select". */
    input_set_operator_menu(1);
    operator_ClearLobbyOptions(); /* port-only: the menu shows and saves the cabinet's own settings */
    lobby_take_start();           /* port-only: a start from while this unit was racing is stale */
    Menus_bCmosOk = 0;
    switch (mode) {
    case 1:
        Menuutil_default_handler = (HandlerFn)0x001bcb20u; /* menu_operator_advanced */
        break;
    case 3:
        Menuutil_default_handler = (HandlerFn)0x001bcb80u; /* diag_selftest */
        break;
    case 4:
    case 6:
        Menuutil_default_handler = (HandlerFn)0x001bc5c0u; /* menu_switches */
        break;
    default:
        Menuutil_default_handler = (HandlerFn)0x001bca40u; /* menu_operator */
        break;
    }

    HY_CALL(VoidFn, vinput_init_part1_of_2)();
    HY_CALL(void(__cdecl *)(int), vgraphics_init)(!Vinput_bLowRes);
    HY_CALL(VoidFn, vinput_init_part2_of_2)();
    HY_CALL(VoidFn, vtext_init)();
    HY_CALL(VoidFn, menusnd_init)();
    if (HY_CALL(int(__cdecl *)(int, int), wrs_Init)(0, 0)) {
        HY_CALL(VoidFn, cmos_ModuleInit)();
        Menus_bCmosOk = 1;
    }
    menus_LoadSettings();
    if (Menus_bCmosOk)
        HY_CALL(void(__cdecl *)(HandlerFn), mess_sethandler)(Menuutil_default_handler);
    else
        HY_CALL(void(__cdecl *)(HandlerFn), menuutil_cmos_error)(Menuutil_default_handler);

    Menus_nFrame = 0;
    HY_CALL(VoidFn, vinput_poll)();
    while (!menus_Pump()) {
        lobby_poll(); /* port-only: keeps a lobby membership alive while the menu runs */
        /* Port-only: the lobby's host picked START GAME. Leave like START THE GAME does (message 10,
         * which menus_Pump returns on), unless DIP switch 3 keeps the game from running. */
        if (lobby_take_start()) {
            if (HY_CALL(int(__cdecl *)(void), diegoio_comm_GetCachedDIPSwitches)() & 4) {
                hy_log("lobby: the host started the game, but DIP switch 3 is on");
            } else {
                input_set_capture(0); /* in case a text prompt was open */
                HY_CALL(void(__cdecl *)(int, int), mess_queue_message)(10, 0);
            }
        }
        HY_CALL(VoidFn, vtext_show)();
        Menus_nFrame++;
        HY_CALL(VoidFn, vinput_poll)();
    }
    input_set_operator_menu(0);

    HY_CALL(VoidFn, vgraphics_term)();
    HY_CALL(VoidFn, menusnd_term)();
    HY_CALL(VoidFn, vinput_term)();
    HY_CALL(VoidFn, menus_disksettings_write)();
    return 0;
}
HY_PORT(menus)
