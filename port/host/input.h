#pragma once
#include <stdint.h>

/* Cabinet inputs as the DiegoIO board reports them. */
typedef struct InputState {
    uint8_t adc[2];   /* analog channels (steering / throttle), 0x80 = centre */
    uint8_t switches; /* the board's switch byte */
    uint8_t coin[5];  /* coin-drop counters, cycling 1..7; 0 = no coin seen yet */
} InputState;

void input_poll(InputState *out);
/* Digital left/right for menus: -1, 0 or +1 (arrow keys, d-pad, or the stick past half way). */
int input_menu_dir(void);
/* Set while the operator menu runs: enables its keyboard/pad aliases (see input.c). */
void input_set_operator_menu(int on);
int input_operator_menu_open(void);

/* Binding capture (PC SETTINGS): while on, the cabinet sees no input and Esc doesn't quit. */
void input_set_capture(int on);
int input_capturing(void);
int input_capture_key(void); /* a key newly pressed since the last call (VK code), or 0 */
int input_capture_pad(void); /* a pad button newly pressed (PAD_*), or PAD_NONE */
int input_any_held(void);    /* any key or pad button down */

/* Inline typing (the lobby's chat line): characters queue as for text entry, but the operator menu
 * still sees Up/Down (keys or d-pad) and Esc; every other key binding is muted. The pad works as usual. */
void input_set_typing(int on);

/* Text entry (while capturing or typing): typed characters from WM_CHAR, including '\r', '\b' and
 * 27 (Esc); 0 = none. */
void input_push_char(int c); /* window.c */
int input_text_char(void);
