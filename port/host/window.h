#pragma once
#include <windows.h>

/* The game window. Created on the game thread; messages are pumped from grBufferSwap/Sleep. */
void window_create(int width, int height);
HWND window_get(void);
void window_pump(void);
/* Saves the volume to CMOS and exits (the window's close button, Esc, the menu's QUIT). */
__declspec(noreturn) void window_quit(void);
