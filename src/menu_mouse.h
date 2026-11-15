/**
 * Mouse in menus (to-do 40, SPEC 10.1; menu_mouse.cpp): the pointer over the
 * game's own menus while keyboard and mouse are on, its clicks, wheel and
 * drags turned into the game's own button presses, and the arrows of ours
 * drawn over the main menu. Called from overlay.cpp only.
 */
#ifndef CONKER_MENU_MOUSE_H
#define CONKER_MENU_MOUSE_H

#include <windows.h>

/* Window thread, from the overlay's message hook: every message while the
 * hook is in charge. x, y = a mouse message's position in back-buffer
 * pixels. Nonzero = taken (*result set). */
int menu_mouse_message(HWND hwnd, UINT m, WPARAM w, LPARAM l, float x, float y,
                       LRESULT *result);

/* Executor thread, once a present, before the keyboard and mouse are handed
 * out: blocked = a menu of ours, the pause key or photo mode has the input.
 * Returns nonzero while something of ours is to be drawn (menu_mouse_draw). */
int menu_mouse_frame(unsigned w, unsigned h, int blocked);
/* The pointer is the menus' now: the mouse is not the game's (keys only). */
int menu_mouse_on(void);
/* Inside the overlay's ImGui frame: the arrows. */
void menu_mouse_draw(unsigned w, unsigned h);
/* A controller button was pressed: the pointer hides until the mouse moves. */
void menu_mouse_pad_used(void);
/* Window thread, from the overlay's message hook while no menu of ours has
 * the keys: the profile name keyboard (step 5d) takes typing keys (letters
 * and every key that makes a character, Backspace, Enter, Esc, left / right)
 * while it is up, and their repeats until let go. Nonzero = taken. */
int menu_mouse_name_key(HWND hwnd, UINT m, WPARAM w, LPARAM l);

/* overlay.cpp: the quit question, as Esc on the main menu's boards. */
void overlay_open_quit(void);

#endif
