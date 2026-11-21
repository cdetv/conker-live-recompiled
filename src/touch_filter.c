/*
 * To-do 270: touch-made mouse messages. Windows marks them
 * (GetMessageExtraInfo 0xFF5157xx; 0x80 = touch, else pen). Wine (Deck run
 * T1, Proton 11) marks its Windows-style copy without 0x80 and sends one
 * more, unmarked copy of its own: a move and a press as the finger lands,
 * the release as it lifts, nothing in between. Both systems send
 * WM_POINTERDOWN first, so a mouse message is touch-made when marked, or
 * from WM_POINTERDOWN to TOUCH_TAIL_MS after WM_POINTERUP (Wine's copies
 * come within ~20 ms of it). A tap is one click: in a touch where an
 * unmarked press came, the marked presses are dropped, and the marked
 * releases once the unmarked release came too (Wine's second click; Windows
 * sends only the marked ones). The marked moves stay: Wine's own copy has
 * none while the finger moves (a drag).
 *
 * Used by the game's window (overlay.cpp's window hook) and by Report Bug's
 * small window (bug_report_win.c), where under Proton a tap clicked a
 * button twice and was a double-click in the text box (Linux Findings 31).
 * There the second press comes as WM_LBUTTONDBLCLK (buttons and edit boxes
 * have CS_DBLCLKS), so double-clicks count as presses; the game's window
 * class takes none. One window thread per process calls it, so plain
 * statics do.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "touch_filter.h"

#define TOUCH_TAIL_MS 250
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN  0x0246
#define WM_POINTERUP    0x0247
#endif

static int s_touch_contact;      /* a finger (or a pen) is down */
static int s_touch_tail;         /* lifted less than TOUCH_TAIL_MS ago */
static DWORD s_touch_up_tick;
static int s_touch_wine_press;   /* this touch's unmarked press came */
static int s_touch_wine_release; /* ... and its unmarked release */

int touch_filter(unsigned m, int *touch)
{
    int marked, in_touch, press;

    *touch = 0;
    if (m == WM_POINTERDOWN) {
        s_touch_contact = 1;
        s_touch_tail = 0;
        s_touch_wine_press = 0;
        s_touch_wine_release = 0;
        return 0;
    }
    if (m == WM_POINTERUP) {
        s_touch_contact = 0;
        s_touch_tail = 1;
        s_touch_up_tick = GetTickCount();
        return 0;
    }
    if (m < WM_MOUSEFIRST || m > WM_MOUSELAST)
        return 0;
    if (s_touch_tail && GetTickCount() - s_touch_up_tick > TOUCH_TAIL_MS)
        s_touch_tail = 0;
    marked = ((DWORD)GetMessageExtraInfo() & 0xFFFFFF00u) == 0xFF515700u;
    in_touch = s_touch_contact || s_touch_tail;
    *touch = marked || in_touch;
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        press = 1;
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP:
        press = 0;
        break;
    default:
        return 0;
    }
    if (!in_touch)
        return 0;
    if (!marked) {
        if (press)
            s_touch_wine_press = 1;
        else
            s_touch_wine_release = 1;
        return 0;
    }
    /* The marked release goes only once Wine's own came: on the Deck a
     * tap's unmarked release sometimes never came (Linux Findings 31, run
     * C3), and a press left without any release held the mouse. A second
     * release is harmless (nothing is pressed any more). */
    return press ? s_touch_wine_press : s_touch_wine_release;
}
