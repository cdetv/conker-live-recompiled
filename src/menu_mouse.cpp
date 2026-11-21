/**
 * Mouse in menus (to-do 40, SPEC 10.1; user 2026-11-14).
 *
 * While keyboard and mouse are on (keyboard_mouse) and menu_mouse is, in the
 * game's own menus (the controls' menu, in-level menu and single player pause
 * sets: controls_mode) the mouse is the menus', not the game's: the toolkit
 * gets the keys only (xbox_KbmSetKeysOnly), so it neither holds nor hides the
 * pointer, and this file shows it and turns what the mouse does into the
 * game's own button presses (xbox_KbmForce: the game hears them as the pad,
 * so its sounds, turns and fades are its own).
 *
 * The pointer: hidden until the mouse moves, hidden again after IDLE_HIDE_MS
 * still, or at once on a key or a controller press. Windows' own arrow, the
 * hand over something that can be clicked. In full screen it is kept on the
 * game's screen (ClipCursor; a click on another monitor would send the game
 * to the back). Coming from play, where the toolkit held it, it starts in the
 * middle of the picture, not at the edge mouse look left it on. A
 * touchscreen's taps and drags work as the mouse's but draw no pointer
 * (to-do 270; F2's touch_cursor = 1 draws it): overlay.cpp tells them.
 *
 * Everywhere: right click = B (back), the wheel = up / down presses.
 *
 * The main menu's boards (conker_MenuBoards: the pub, no board zoomed into):
 * an arrow of ours at each side, fading in while the mouse is used; a click
 * (or holding the button) turns to the next board that way, as the arrow
 * keys do, from wherever the camera looks. A drag turns the camera by hand
 * (user 2026-11-14): the room follows the pointer as if grabbed, the camera
 * turning on its fixed eye (conker_MenuBoardsTheta / At in recomp_manual.c
 * turn angles into the game's own board + progress, held there on the game
 * thread), and it stays where it is let go, even between two boards.
 *
 * Each board reacts only on its title or its spot (user 2026-11-14): the
 * Live doorway behind the bar (Multiplayer), the fireplace (Options), the
 * door (Chapters), the hallway door (Game 1), the window (Game 2), the
 * saloon doors (Game 3). The spots are outlines of directions from the
 * camera's eye (yaw / pitch, measured on screenshots, _work\mm41\spots.py),
 * projected through the game's own camera every frame, so they move with
 * it; the titles are pinned to the room by the game side (recomp_manual.c
 * conker_MenuTitlePin, for every turn), their boxes moved the same way. A click on
 * one opens that board (A), turning to it first if the camera is elsewhere.
 * The board's title is dim while the pointer shows and lit (the game's own
 * look) while it is on the title or the spot; with the pointer hidden it is
 * as the game shows it. Right click there is the quit question, as Esc and
 * B are (overlay.cpp). A turn is pressed only once the last one has finished
 * (turn speed 0) and let go as soon as it starts, so owed turns never chain
 * past what was asked for. RECOMP_MENU_SPOTS=1 draws the spots' outlines.
 *
 * The arrows are drawn by ImGui (background list, under any window of ours),
 * in the style of the menu's title font (FrontendTitle: a fat-face italic):
 * a chevron with a thin upper arm and a thick lower one, leaning like the
 * italics, ivory to gold as the board titles, a warm glow around it. Dim at
 * rest, lit under the pointer.
 *
 * Threads: menu_mouse_message runs on the window thread, the rest on the
 * executor (the overlay, once a present). The window thread queues the mouse
 * for the executor; the executor posts WM_MENU_MOUSE when the pointer's mode
 * or visibility changed, so the cursor and the clip are always set on the
 * window's own thread, and in order with the toolkit's own messages (its
 * FB_WM_KBM, which lets go of the clip when keys-only begins, is followed by
 * one of ours that clips again).
 */

#include <windows.h>
#include <windowsx.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"     /* ShadeVertsLinearColorGradientKeepAlpha */

#include "menu_mouse.h"
#include "menu_page.h"
#include "controls.h"
#include "settings.h"
#include "xbox_kbm.h"
#include "legacy_cp.h"          /* WM_CHAR's code page (to-do 262) */

extern "C" {
int conker_MenuBoards(float *turn);
float conker_MenuBoardsPos(void);
int conker_MenuPage(void);
float conker_MenuBoardsTheta(float pos);
float conker_MenuBoardsAt(float theta);
void conker_MenuBoardsHold(float pos, int soft);
int conker_MenuBoardsFlingStopped(void);
void conker_MenuTitleAlpha(int g, float a);
void conker_MenuGlow(uint32_t text);
float conker_MenuTitlePin(int g, float *vis);
int conker_MenuCamera(float r[9], float *p00, float *p11);
int xbox_FramebufferWindowIsFullscreen(void);
int xbox_FramebufferWindowGpuViewRect(int *x, int *y, int *w, int *h);
}

#define WM_MENU_MOUSE   (WM_APP + 0x40)  /* executor -> window: apply */
#define FB_WM_KBM       (WM_APP + 4)     /* fb_present.c: the toolkit's */
#define IDLE_HIDE_MS    3000    /* pointer still this long: hidden */
#define ARROW_IDLE_MS   2000    /* the arrows fade out sooner */
#define PRESS_MS        90      /* a click's press: ~3 frames at 30 fps */
#define GAP_MS          60      /* between two presses */
#define TURN_WAIT_MS    350     /* a turn press the game never took */
#define DRAG_START      0.012f  /* of the width: a click that moved less */
#define ARROW_H         0.115f  /* of the height */
#define ARROW_W         0.62f   /* of the arrow's height */
#define TITLE_DIM       0.5f    /* the board title's alpha, pointer elsewhere */
#define FLING_MIN       25.0f   /* degrees / s at the let go: slower = no fling */
#define FLING_MAX       900.0f
#define FLING_FRICTION  4.0f    /* the fling's speed falls by e every 1 / this s */
#define FLING_STOP      6.0f    /* degrees / s: it has stopped */

/* ---- shared ---------------------------------------------------------------- */

enum { EV_MOVE, EV_LDOWN, EV_LUP, EV_RDOWN, EV_WHEEL, EV_CANCEL };
struct MmEvent {
    int type;
    float x, y;
    int n;
};
static SRWLOCK s_lock = SRWLOCK_INIT;
static MmEvent s_q[128];
static int s_qn;

static volatile LONG s_on;          /* the pointer is the menus' */
static volatile LONG s_shown;       /* and showing */
static volatile LONG s_hand;        /* over something clickable */
static volatile LONG s_move_tick;   /* GetTickCount of the last real move */
static volatile LONG s_moved;       /* moved since the mode began */
static volatile LONG s_keys_used;   /* a key or controller press since the executor looked */
static volatile LONG s_cy_keys;     /* the same, for the cylinder lists' hold */
static volatile LONG s_touch;       /* the last move or press was a touch (to-do 270) */
static volatile LONG s_touch_cursor; /* touch_cursor = 1: the pointer shows for touch too */
static HWND s_hwnd;

static void push(int type, float x, float y, int n)
{
    AcquireSRWLockExclusive(&s_lock);
    /* Moves coalesce: only the latest position matters between clicks. */
    if (type == EV_MOVE && s_qn && s_q[s_qn - 1].type == EV_MOVE)
        s_q[s_qn - 1] = MmEvent{type, x, y, n};
    else if (s_qn < (int)(sizeof(s_q) / sizeof(s_q[0])))
        s_q[s_qn++] = MmEvent{type, x, y, n};
    ReleaseSRWLockExclusive(&s_lock);
}

static void post_apply(void)
{
    if (s_hwnd)
        PostMessageA(s_hwnd, WM_MENU_MOUSE, 0, 0);
}

/* ---- window thread --------------------------------------------------------- */

static bool s_clipped;          /* ClipCursor is ours */
static bool s_on_seen;          /* s_on at the last WM_MENU_MOUSE */
static int s_last_cx = INT_MIN, s_last_cy = INT_MIN;
static int s_wheel_rest;
static bool s_captured;

static bool client_screen_rect(HWND h, RECT *r)
{
    if (!GetClientRect(h, r))
        return false;
    MapWindowPoints(h, NULL, (POINT *)r, 2);
    return true;
}

static void clip_follow(HWND h)
{
    RECT r;
    bool want = s_on && xbox_FramebufferWindowIsFullscreen()
                && GetForegroundWindow() == h;
    if (want && client_screen_rect(h, &r)) {
        ClipCursor(&r);
        s_clipped = true;
    } else if (s_clipped) {
        ClipCursor(NULL);
        s_clipped = false;
    }
}

/* The pointer's look: none while hidden, nor after a touch unless
 * touch_cursor = 1 (to-do 270: the menus still follow the finger, as they do
 * the mouse; only the arrow is not drawn). */
static HCURSOR pointer_cursor(void)
{
    if (!s_shown || (s_touch && !s_touch_cursor))
        return NULL;
    return LoadCursorA(NULL, s_hand ? IDC_HAND : IDC_ARROW);
}

/* A real move or press, by the mouse or a touch: the pointer is in use. */
static void pointer_used(int touch)
{
    LONG was = InterlockedExchange(&s_touch, touch ? 1 : 0);
    if (!s_shown || was != (touch ? 1 : 0)) {
        InterlockedExchange(&s_shown, 1);
        SetCursor(pointer_cursor());
    }
}

/* The cursor's look now, if it is over the picture. */
static void apply_cursor(HWND h)
{
    POINT p;
    RECT r;
    if (!s_on || GetForegroundWindow() != h || !GetCursorPos(&p)
        || !client_screen_rect(h, &r) || !PtInRect(&r, p))
        return;
    SetCursor(pointer_cursor());
}

/* Coming from play: the toolkit held the pointer (clipped to the picture,
 * hidden), and mouse look left it anywhere, often at an edge, on an arrow.
 * The middle of the picture instead; that move is not the player's. */
static void centre_if_held(HWND h)
{
    RECT clip, r;
    int vx, vy, vw, vh;
    POINT p;
    if (!GetClipCursor(&clip) || !client_screen_rect(h, &r)
        || memcmp(&clip, &r, sizeof r) != 0)
        return;
    if (!xbox_FramebufferWindowGpuViewRect(&vx, &vy, &vw, &vh)) {
        vx = vy = 0;
        vw = r.right - r.left;
        vh = r.bottom - r.top;
    }
    p.x = vx + vw / 2;
    p.y = vy + vh / 2;
    s_last_cx = p.x;
    s_last_cy = p.y;
    ClientToScreen(h, &p);
    SetCursorPos(p.x, p.y);
}

int menu_mouse_message(HWND h, UINT m, WPARAM w, LPARAM l, float x, float y,
                       int touch, LRESULT *result)
{
    s_hwnd = h;
    switch (m) {
    case WM_MENU_MOUSE:
        if (s_on && !s_on_seen)
            centre_if_held(h);
        s_on_seen = s_on != 0;
        if (!s_on && s_captured) {
            ReleaseCapture();
            s_captured = false;
        }
        clip_follow(h);
        apply_cursor(h);
        *result = 0;
        return 1;
    case FB_WM_KBM:
        /* The toolkit lets go of its clip (keys only): ours again after. */
        if (s_on)
            post_apply();
        return 0;
    case WM_SIZE:
    case WM_MOVE:
    case WM_SETFOCUS:
        if (s_on || s_clipped)
            post_apply();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            if (s_clipped) {
                ClipCursor(NULL);
                s_clipped = false;
            }
        } else if (s_on) {
            post_apply();
        }
        return 0;
    case WM_KILLFOCUS:
        if (s_clipped) {
            ClipCursor(NULL);
            s_clipped = false;
        }
        if (s_captured) {
            ReleaseCapture();
            s_captured = false;
        }
        push(EV_CANCEL, 0, 0, 0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* The keys are in use: the pointer goes until the mouse moves. */
        if (s_on) {
            InterlockedExchange(&s_keys_used, 1);
            InterlockedExchange(&s_cy_keys, 1);
        }
        if (s_on && s_shown) {
            InterlockedExchange(&s_shown, 0);
            apply_cursor(h);
        }
        return 0;
    }
    if (!s_on)
        return 0;
    switch (m) {
    case WM_SETCURSOR:
        if (LOWORD(l) != HTCLIENT)
            return 0;
        SetCursor(pointer_cursor());
        *result = TRUE;
        return 1;
    case WM_MOUSEMOVE: {
        int cx = GET_X_LPARAM(l), cy = GET_Y_LPARAM(l);
        /* Windows sends moves without movement too (a window over it
         * closing, the pointer set by us): only a real one shows it. */
        if (cx != s_last_cx || cy != s_last_cy) {
            s_last_cx = cx;
            s_last_cy = cy;
            InterlockedExchange(&s_move_tick, (LONG)GetTickCount());
            InterlockedExchange(&s_moved, 1);
            pointer_used(touch);
            push(EV_MOVE, x, y, 0);
        }
        *result = 0;
        return 1;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (!s_captured) {
            SetCapture(h);              /* a drag may leave the window */
            s_captured = true;
        }
        InterlockedExchange(&s_move_tick, (LONG)GetTickCount());
        pointer_used(touch);
        push(m == WM_LBUTTONDOWN ? EV_LDOWN : EV_RDOWN, x, y, 0);
        *result = 0;
        return 1;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (s_captured && !(GetKeyState(m == WM_LBUTTONUP ? VK_RBUTTON
                                                           : VK_LBUTTON) & 0x8000)) {
            ReleaseCapture();
            s_captured = false;
        }
        if (m == WM_LBUTTONUP)
            push(EV_LUP, x, y, 0);
        *result = 0;
        return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        *result = 0;
        return 1;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        *result = TRUE;
        return 1;
    case WM_MOUSEWHEEL:
        s_wheel_rest += GET_WHEEL_DELTA_WPARAM(w);
        if (s_wheel_rest >= WHEEL_DELTA || s_wheel_rest <= -WHEEL_DELTA) {
            push(EV_WHEEL, 0, 0, s_wheel_rest / WHEEL_DELTA);
            s_wheel_rest %= WHEEL_DELTA;
        }
        InterlockedExchange(&s_move_tick, (LONG)GetTickCount());
        *result = 0;
        return 1;
    }
    return 0;
}

/* Typing on the profile name keyboard (step 5d): while it is up, a key that
 * makes a character types it (WM_CHAR, so Shift, Caps Lock and dead keys are
 * Windows' own), Backspace, Enter (Create), Esc (Cancel) and left / right
 * (the text cursor) are the game keyboard's own; the game gets none of them
 * as a pad press. A key taken keeps its repeats and its let go, even once the
 * keyboard has gone (Enter's repeats would press A on the next screen). A key
 * already held when the keyboard came up is left alone. Other keys (up /
 * down, F-keys, Tab) go on as ever. */
static bool s_nk_own[256];      /* its press was taken */
static bool s_nk_char;          /* the last key down was taken: its character too */

int menu_mouse_name_key(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)h;
    switch (m) {
    case WM_KILLFOCUS:
        memset(s_nk_own, 0, sizeof s_nk_own);
        s_nk_char = false;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (w < 256 && s_nk_own[w]) {
            s_nk_own[w] = false;
            return 1;
        }
        return 0;
    case WM_KEYDOWN: {
        if (w >= 256)
            return 0;
        bool up = conker_NameKbSnap(NULL, NULL, NULL, NULL) != 0;
        bool repeat = (l & (1L << 30)) != 0;
        int code = w == VK_RETURN ? NAMEKB_CREATE : w == VK_ESCAPE ? NAMEKB_CANCEL
                 : w == VK_BACK ? NAMEKB_BACKSPACE : w == VK_LEFT ? NAMEKB_LEFT
                 : w == VK_RIGHT ? NAMEKB_RIGHT : 0;
        if (s_nk_own[w] && repeat) {
            /* Backspace and the arrows repeat (a letter's repeat comes as
             * its character); Enter and Esc act once. */
            s_nk_char = up;
            if (up && (w == VK_BACK || w == VK_LEFT || w == VK_RIGHT))
                conker_NameKbType(code);
            return 1;
        }
        s_nk_own[w] = false;
        s_nk_char = false;
        if (!up || repeat)
            return 0;                   /* held from before: not typing */
        bool console = settings_debug() && ((l >> 16) & 0x1FF) == 0x29;
        bool types = !code && !console
                     && ((MapVirtualKeyA((UINT)w, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu) != 0
                         || (w >= VK_NUMPAD0 && w <= VK_DIVIDE)
                         || w == VK_PACKET);   /* a character sent as itself (an IME,
                                                * Windows' touch keyboard) */
        if (!code && !types)
            return 0;
        s_nk_own[w] = true;
        s_nk_char = true;
        if (code) {
            fprintf(stderr, "[MOUSE] name keyboard: key %s\n",
                    code == NAMEKB_CREATE ? "Enter (Create)" : code == NAMEKB_CANCEL
                    ? "Esc (Cancel)" : code == NAMEKB_BACKSPACE ? "Backspace"
                    : code == NAMEKB_LEFT ? "left" : "right");
            conker_NameKbType(code);
        }
        return 1;
    }
    case WM_CHAR:
    case WM_DEADCHAR:
        if (!s_nk_char)
            return 0;
        if (m == WM_CHAR && w >= 0x20 && w != 0x7F) {
            /* The game window is an ANSI one: past ASCII, the PC's own code
             * page (not CP_ACP: that is UTF-8 under the manifest). */
            conker_NameKbType((int)conker_wm_char((unsigned)w));
        }
        return 1;
    }
    return 0;
}

/* ---- executor -------------------------------------------------------------- */

static ULONGLONG s_now;
static float s_w = 640.0f, s_h = 480.0f;
static float s_px = -1.0f, s_py = -1.0f;   /* the pointer, back-buffer px */
static bool s_ldown;            /* left button down, pressed on the menu */
static int s_press_arrow;       /* -1 / +1: the left button went down on one */
static int s_hold;              /* that arrow still held: turns repeat */
static bool s_dragged;          /* moved past DRAG_START since going down */
static float s_down_x;          /* where the left button went down */
static int s_down_board = -1;   /* the board whose title / spot that was on */
static float s_grab_x;          /* drag: the pointer's x when the room was grabbed */
static float s_grab_theta;      /* and the camera's turn then, degrees */
static double s_trk_t[8];       /* the drag's last turns: when (s) */
static float s_trk_th[8];       /* and where (degrees) */
static int s_trk_n;
static bool s_fling;            /* let go while moving: the room turns on */
static float s_fling_th, s_fling_v;     /* where (degrees) and how fast (degrees / s) */
static int s_owe;               /* turns owed, + right, - left */
static int s_turn_dir;          /* a turn press held (+1 / -1), 0 none */
static ULONGLONG s_turn_at;
static int s_click_board = -1;  /* a board clicked: turn to it, then open */
static int s_open_board = -1;   /* the owed turns end on it: then A */
static float s_fade;            /* the arrows, 0..1 */
static float s_lit[2];          /* each arrow lit, 0..1 (0 left, 1 right) */
static float s_pulse[2];        /* each arrow's click, 1 -> 0 */
static bool s_boards;
static int s_over_board = -1;   /* the board whose title / spot is under the pointer */
static float s_title_lit[6] = { 1, 1, 1, 1, 1, 1 };    /* each title, 0 dim .. 1 the game's own */
static int s_spots_debug = -1;  /* RECOMP_MENU_SPOTS=1: the outlines drawn */
static MenuPageSnap s_page;     /* a board's page open (game side, last frame) */
static bool s_page_ok;
/* The pages (step 3): rows, the Chapters page's arrows and drags. */
static int s_pg_over = -1;      /* the row under the pointer, -1 none */
static int s_pg_over_part;      /* 0 the row, 1 its No, 2 its Yes */
static int s_pg_over_pr = -1;   /* the prompt under the pointer (New Game, A Quit? ...) */
static int s_pg_down = -2;      /* where the left button went down: row, -3 - k prompt k, -2 none */
static int s_pg_down_part;
static float s_down_y;
static bool s_pg_moved;         /* the pointer moved this frame */
static int s_pg_hover = -1;     /* the row the last hover asked for */
static int s_pg_req = -1;       /* the cursor asked for */
static ULONGLONG s_pg_req_at;
static int s_pg_a = -1;         /* A owed once the cursor is on this row */
static ULONGLONG s_pg_a_at;
static uint32_t s_pg_list;      /* the list all that is about */
static int s_pg_drag;           /* 0 not yet, 1 chapters (sideways), 2 parts (up / down), 3 none */
static bool s_pg_dragged;
static float s_pg_dx, s_pg_dy;  /* where the drag stepped last */
static int s_ch_owe;            /* chapters owed, + next (right), - back */
static int s_ch_hold;           /* an arrow held: chapters keep coming */
static ULONGLONG s_ch_tap_at, s_ch_still_at;
static int s_pa_press;          /* the page arrow the button went down on: 1 ◄ 2 ► 3 ▲ 4 ▼ */
static int s_pa_over;           /* the one under the pointer */
static ULONGLONG s_pa_rep_at;   /* ▲ ▼ held: the next step */
static float s_pa_fade;         /* the page arrows, 0..1 */
static float s_pa_vis[4], s_pa_lit[4], s_pa_pulse[4];
static float s_pa_rx = -1.0f;   /* the ► arrow's x (it follows the chapter's name) */
static ULONGLONG s_pa_click_at; /* ► last pressed: it stays put a moment (more clicks) */

/* The game's camera this frame (conker_MenuCamera). */
static bool s_cam_ok;
static float s_cam_r[9], s_cam_p00 = 1.0f, s_cam_p11 = 1.0f;

/* Presses of one button at a time: A, B, up, down. */
static int s_taps[8], s_tapn;
static int s_tap_out = -1;
static bool s_tap_down;
static ULONGLONG s_tap_at;

static void tap(int out)
{
    if (s_tapn < (int)(sizeof(s_taps) / sizeof(s_taps[0])))
        s_taps[s_tapn++] = out;
}

static void taps_frame(void)
{
    if (s_tap_out >= 0) {
        if (s_tap_down && s_now - s_tap_at >= PRESS_MS) {
            xbox_KbmForce((unsigned)s_tap_out, 0);
            s_tap_down = false;
            s_tap_at = s_now;
        } else if (!s_tap_down && s_now - s_tap_at >= GAP_MS) {
            s_tap_out = -1;
        }
    }
    if (s_tap_out < 0 && s_tapn && !s_turn_dir) {
        s_tap_out = s_taps[0];
        memmove(s_taps, s_taps + 1, (size_t)(--s_tapn) * sizeof(s_taps[0]));
        xbox_KbmForce((unsigned)s_tap_out, 1);
        s_tap_down = true;
        s_tap_at = s_now;
    }
}

static void turn_release(void)
{
    if (s_turn_dir)
        xbox_KbmForce(s_turn_dir > 0 ? XBOX_KBM_DPAD_RIGHT : XBOX_KBM_DPAD_LEFT, 0);
    s_turn_dir = 0;
}

static void fling_stop(void);

static void release_all(void)
{
    fling_stop();
    turn_release();
    if (s_tap_out >= 0 && s_tap_down)
        xbox_KbmForce((unsigned)s_tap_out, 0);
    s_tap_out = -1;
    s_tap_down = false;
    s_tapn = 0;
    s_owe = 0;
    s_hold = 0;
    s_ldown = false;
    s_press_arrow = 0;
    if (s_dragged)
        conker_MenuBoardsHold(-1.0f, 0);
    s_dragged = false;
    s_click_board = -1;
    s_open_board = -1;
    s_pg_down = -2;
    s_pg_a = -1;
    s_pg_req = -1;
    s_pg_hover = -1;
    s_pg_drag = 0;
    s_pg_dragged = false;
    s_ch_owe = 0;
    s_ch_hold = 0;
    s_pa_press = 0;
}

static void owe(int dir)
{
    if (s_owe && (s_owe > 0) != (dir > 0))
        s_owe = 0;                      /* the other way cancels */
    s_owe += dir;
    if (s_owe > 5)
        s_owe = 5;
    if (s_owe < -5)
        s_owe = -5;
}

/* One turn press at a time, only on a board standing still; let go once the
 * turn has started (a tap turns a whole board), or if the game never took it. */
/* The camera standing exactly on board b. */
static bool on_board(int b)
{
    float pos = conker_MenuBoardsPos();
    return pos == (float)b;
}

/* A click on board b's title or spot: A if the camera stands on it, else
 * the turns there (the shorter way; from between two boards the first turn
 * only finishes the one under way) and A when they end. */
static void go_open(int b)
{
    float pos = conker_MenuBoardsPos();
    int c = (int)pos % 6, right, left;
    bool between = pos != (float)(int)pos;
    if (!between && c == b) {
        tap(XBOX_KBM_A);
        return;
    }
    if (between) {
        right = (b - (c + 1) + 12) % 6 + 1;
        left = (c - b + 12) % 6 + 1;
    } else {
        right = (b - c + 6) % 6;
        left = (c - b + 6) % 6;
    }
    s_owe = right <= left ? right : -left;
    s_open_board = b;
}

static void turns_frame(float turn)
{
    if (!s_boards) {
        turn_release();
        s_owe = 0;
        s_hold = 0;
        s_click_board = -1;
        s_open_board = -1;
        return;
    }
    if (!s_turn_dir && turn == 0.0f && !s_owe && s_tap_out < 0 && !s_tapn) {
        if (s_click_board >= 0) {
            int b = s_click_board;
            s_click_board = -1;
            go_open(b);
        } else if (s_open_board >= 0) {
            if (on_board(s_open_board))
                tap(XBOX_KBM_A);
            s_open_board = -1;
        }
    }
    if (s_turn_dir) {
        if (turn != 0.0f) {
            s_owe -= s_turn_dir;
            if ((s_owe > 0) != (s_turn_dir > 0))
                s_owe = 0;
            turn_release();
        } else if (s_now - s_turn_at > TURN_WAIT_MS) {
            turn_release();
            s_owe = 0;
        }
        return;
    }
    if (turn != 0.0f || s_tap_out >= 0)
        return;
    if (!s_owe && s_hold)
        s_owe = s_hold;
    if (s_owe) {
        s_turn_dir = s_owe > 0 ? 1 : -1;
        s_turn_at = s_now;
        s_pulse[s_turn_dir > 0] = 1.0f;
        xbox_KbmForce(s_turn_dir > 0 ? XBOX_KBM_DPAD_RIGHT : XBOX_KBM_DPAD_LEFT, 1);
    }
}

/* ---- the arrows ------------------------------------------------------------ */

struct ArrowBox {
    float cx, cy, hh;   /* centre, height */
};

static ArrowBox arrow_box(int right)
{
    ArrowBox b;
    b.hh = s_h * ARROW_H;
    float wd = b.hh * ARROW_W;
    float m = s_w * 0.035f;
    b.cx = right ? s_w - m - wd * 0.5f : m + wd * 0.5f;
    b.cy = s_h * 0.5f;
    return b;
}

/* -1 / +1: an arrow under (x, y) (a generous box round it), else 0. */
static int arrow_at(float x, float y)
{
    for (int r = 0; r < 2; r++) {
        ArrowBox b = arrow_box(r);
        float hw = b.hh * ARROW_W * 0.5f;
        if (fabsf(x - b.cx) <= hw * 1.1f + s_h * 0.02f
            && fabsf(y - b.cy) <= b.hh * 0.75f)
            return r ? 1 : -1;
    }
    return 0;
}

/* ---- the boards' spots and titles ----------------------------------------- */

/* Each board's spot: an outline of directions from the camera's eye (yaw =
 * atan2(x, z), pitch, degrees; the eye never moves on the boards, only the
 * camera's turn and its slow zoom sway do). Measured on the calibration
 * shots (_work\mm41\spots.py). */
struct Spot {
    int n;
    float yp[10][2];
};
static const Spot s_spots[6] = {
    /* Multiplayer: the Live doorway behind the bar, and the counter below it */
    {4, {{-165.9f, 21.3f}, {176.1f, 20.4f}, {176.6f, -18.2f}, {-165.9f, -18.9f}}},
    /* Options: the fireplace and its chimney */
    {10, {{122.4f, 32.0f}, {97.1f, 29.9f}, {96.9f, 9.5f}, {87.2f, 8.7f}, {87.0f, -8.8f},
          {127.5f, -10.3f}, {127.5f, 3.2f}, {132.5f, 3.2f}, {132.5f, 10.1f}, {122.4f, 10.3f}}},
    /* Chapters: the door */
    {4, {{57.8f, 9.7f}, {40.8f, 9.2f}, {41.2f, -9.6f}, {57.9f, -10.1f}}},
    /* Game 1: the door at the end of the hallway */
    {7, {{12.6f, -11.2f}, {12.6f, 2.9f}, {8.3f, 6.6f}, {0.4f, 8.0f}, {-7.5f, 6.6f},
         {-11.3f, 2.9f}, {-11.3f, -11.2f}}},
    /* Game 2: the window */
    {7, {{-29.6f, -3.9f}, {-29.4f, 9.7f}, {-33.0f, 13.7f}, {-40.5f, 14.8f}, {-48.1f, 13.7f},
         {-51.6f, 9.7f}, {-51.3f, -3.9f}}},
    /* Game 3: the saloon doors */
    {4, {{-94.3f, 5.2f}, {-113.2f, 5.1f}, {-113.0f, -7.8f}, {-94.4f, -7.9f}}},
};

/* Each board's title, where the game puts it on the screen: x from the
 * middle and y from the top, both in picture heights (left, top, right,
 * bottom), with room for its glow; measured at 16:9. The left edge and the
 * height stay at other shapes, the width goes with 16:9 over the shape
 * (4:3: a third wider; _work\mm41\a43_*.png). */
static const float s_titles[6][4] = {
    {-0.370f, 0.580f, 0.386f, 0.742f},      /* Multiplayer */
    {-0.076f, 0.764f, 0.357f, 0.903f},      /* Options */
    {-0.436f, 0.213f, 0.063f, 0.345f},      /* Chapters */
    {-0.142f, 0.117f, 0.254f, 0.257f},      /* Game 1 */
    {-0.429f, 0.206f, 0.004f, 0.345f},      /* Game 2 */
    {-0.524f, 0.257f, -0.084f, 0.385f},     /* Game 3 */
};

/* A spot's outline on the screen (back-buffer px), cut at the camera's
 * near side; returns its point count (0 = behind the camera). */
static int spot_screen(int b, ImVec2 *out)
{
    float v[24][3];
    int nv = 0, n = 0;
    const Spot &s = s_spots[b];
    if (!s_cam_ok)
        return 0;
    for (int i = 0; i < s.n; i++) {
        float yaw = s.yp[i][0] * 0.0174533f, pit = s.yp[i][1] * 0.0174533f;
        float d[3] = { cosf(pit) * sinf(yaw), sinf(pit), cosf(pit) * cosf(yaw) };
        for (int j = 0; j < 3; j++)
            v[nv][j] = d[0] * s_cam_r[j] + d[1] * s_cam_r[3 + j] + d[2] * s_cam_r[6 + j];
        nv++;
    }
    /* Keep what is in front (z < -kNear): one Sutherland-Hodgman pass. */
    const float kNear = 0.05f;
    float c[24][3];
    int nc = 0;
    for (int i = 0; i < nv; i++) {
        const float *a = v[i], *bq = v[(i + 1) % nv];
        bool ain = a[2] < -kNear, bin = bq[2] < -kNear;
        if (ain) {
            memcpy(c[nc++], a, sizeof c[0]);
        }
        if (ain != bin && nc < 23) {
            float t = (-kNear - a[2]) / (bq[2] - a[2]);
            for (int j = 0; j < 3; j++)
                c[nc][j] = a[j] + (bq[j] - a[j]) * t;
            nc++;
        }
    }
    if (nc < 3)
        return 0;
    for (int i = 0; i < nc; i++) {
        float iz = -1.0f / c[i][2];
        out[n++] = ImVec2((s_cam_p00 * c[i][0] * iz + 1.0f) * 0.5f * s_w,
                          (1.0f - s_cam_p11 * c[i][1] * iz) * 0.5f * s_h);
    }
    return n;
}

static bool in_poly(const ImVec2 *p, int n, float x, float y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i].y > y) != (p[j].y > y)
            && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
            in = !in;
    return in;
}

/* Board b's title box now (back-buffer px): moved with the room as the game
 * side moves the title (conker_MenuTitlePin), with room for its glow pulse
 * (the text grows and shrinks about its middle); returns how much of the
 * title shows (0..1). */
static float title_rect(int b, float *x0, float *y0, float *x1, float *y1)
{
    float vis, sh = conker_MenuTitlePin(b, &vis);
    float shape = s_w > 1.0f ? s_w / s_h : 16.0f / 9.0f, pad = 0.035f * s_h;
    float wd = (s_titles[b][2] - s_titles[b][0]) * s_h * (16.0f / 9.0f) / shape;
    *x0 = s_w * 0.5f + (s_titles[b][0] + sh) * s_h - pad;
    *x1 = *x0 + wd + 2.0f * pad;
    *y0 = (s_titles[b][1] - 0.012f) * s_h;
    *y1 = (s_titles[b][3] + 0.012f) * s_h;
    return vis;
}

/* The board whose title (one that shows) or spot is under (x, y), else -1. */
static int board_at(float x, float y)
{
    float x0, y0, x1, y1;
    ImVec2 p[24];
    int pg = conker_MenuPage();
    for (int i = 0; i < 6; i++) {
        int b = (pg + i) % 6;           /* the board shown first */
        if (title_rect(b, &x0, &y0, &x1, &y1) > 0.25f && x >= x0 && x <= x1 && y >= y0
            && y <= y1)
            return b;
    }
    for (int b = 0; b < 6; b++) {
        int n = spot_screen(b, p);
        if (n && in_poly(p, n, x, y))
            return b;
    }
    return -1;
}

/* The pointer's angle from the middle of the picture, degrees. */
static float pointer_angle(float x)
{
    float f = s_cam_p00 * s_w * 0.5f;
    return atanf((x - s_w * 0.5f) / (f > 1.0f ? f : 1.0f)) * 57.2957795f;
}

static double now_s(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

/* Hold the camera where it has turned th degrees. */
static void hold_at(float th, int soft)
{
    float pos = fmodf(conker_MenuBoardsAt(th), 6.0f);
    if (pos < 0.0f)
        pos += 6.0f;
    conker_MenuBoardsHold(pos, soft);
}

/* Turn the camera so the room point grabbed stays under the pointer. */
static void drag_to(float x)
{
    if (!s_dragged) {
        if (fabsf(x - s_down_x) <= s_w * DRAG_START)
            return;
        s_dragged = true;
        s_owe = 0;
        s_hold = 0;
        s_click_board = -1;
        s_open_board = -1;
        s_grab_x = x;           /* grabbed here, where the camera is now */
        s_grab_theta = conker_MenuBoardsTheta(conker_MenuBoardsPos());
        s_trk_n = 0;
    }
    float th = s_grab_theta + pointer_angle(s_grab_x) - pointer_angle(x);
    s_trk_t[s_trk_n % 8] = now_s();
    s_trk_th[s_trk_n % 8] = th;
    s_trk_n++;
    hold_at(th, 0);
}

/* How fast the drag turned the room as it was let go (degrees / s): over its
 * last tenth of a second; 0 if the pointer had stopped before. */
static float drag_speed(void)
{
    if (s_trk_n < 2)
        return 0.0f;
    int last = (s_trk_n - 1) % 8, first = last;
    if (now_s() - s_trk_t[last] > 0.08)
        return 0.0f;
    for (int k = 1; k < 8 && k < s_trk_n; k++) {
        int i = (s_trk_n - 1 - k) % 8;
        if (s_trk_t[last] - s_trk_t[i] > 0.1)
            break;
        first = i;
    }
    double dt = s_trk_t[last] - s_trk_t[first];
    return dt > 0.012 ? (float)((s_trk_th[last] - s_trk_th[first]) / dt) : 0.0f;
}

static void fling_stop(void)
{
    if (!s_fling)
        return;
    fprintf(stderr, "[MOUSE] fling: stopped at %.3f (%.0f deg/s)\n", conker_MenuBoardsPos(),
            s_fling_v);
    s_fling = false;
    conker_MenuBoardsHold(-1.0f, 0);        /* it stays where it got to */
}

/* The room still turning after a fling: slower and slower, then it stays.
 * A grab, a key or controller press, a board opened stop it. */
static void fling_frame(float dt)
{
    if (!s_fling)
        return;
    if (!s_boards || conker_MenuBoardsFlingStopped() || InterlockedExchange(&s_keys_used, 0)) {
        fling_stop();
        return;
    }
    s_fling_th += s_fling_v * dt;
    s_fling_v *= expf(-FLING_FRICTION * dt);
    if (fabsf(s_fling_v) < FLING_STOP) {
        fling_stop();
        return;
    }
    hold_at(s_fling_th, 1);
}

/* ---- the boards' pages (step 3) ---------------------------------------------
 *
 * A board opened (Game 1-3, Options, Chapters): the game side tells where
 * each row's text is on the screen this frame (menu_page.h) and which list
 * takes the keys. Hovering a row moves the list's cursor there, as the keys
 * would (the game's own move sound and highlight; the request goes in as the
 * list's own input, so a sideways list needs no thought); a click is A on it
 * (a No / Yes line: the word clicked, or the other value on its name). The
 * Chapters page's parts keep the chosen one in the middle, so hovering moves
 * nothing there (the list would slide under the pointer): a click brings a
 * part to the middle, a click on the middle one plays it; the wheel, a drag
 * up / down and our ▲ ▼ move along it, our ◄ ► beside the chapter's name and
 * a drag left / right change the chapter (the game's own left / right, one
 * at a time as its cross-fade allows). */

struct PxBox {
    float x0, y0, x1, y1;
    bool ok;
};

/* A box of the game side's (normalised, y up) in back-buffer px, grown by
 * gx / gy of its height on each side. */
static PxBox px_box(const MenuBox &b, float gx, float gy)
{
    PxBox r;
    float x0 = (b.x0 + 1.0f) * 0.5f * s_w, x1 = (b.x1 + 1.0f) * 0.5f * s_w;
    float y0 = (1.0f - b.y1) * 0.5f * s_h, y1 = (1.0f - b.y0) * 0.5f * s_h;
    float h = y1 - y0;
    r.x0 = x0 - h * gx;
    r.x1 = x1 + h * gx;
    r.y0 = y0 - h * gy;
    r.y1 = y1 + h * gy;
    r.ok = b.ok != 0;
    return r;
}

static bool in_px(const PxBox &b, float x, float y)
{
    return b.ok && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1;
}

/* The row under (x, y) (the nearest if two reach), and the part of it: 0 the
 * row, 1 its No, 2 its Yes; -1 none. */
static int page_row_at(float x, float y, int *part)
{
    int best = -1;
    float bd = 1e9f;
    *part = 0;
    for (int i = 0; i < s_page.n; i++) {
        const MenuPageItem &it = s_page.item[i];
        if (!it.label.ok)
            continue;
        PxBox lb = px_box(it.label, 0.35f, 0.22f);
        int p = -1;
        if (it.yesno) {
            PxBox nb = px_box(it.no, 0.3f, 0.22f), yb = px_box(it.yes, 0.3f, 0.22f), row = lb;
            const PxBox *w[2] = { &nb, &yb };
            for (int k = 0; k < 2; k++)
                if (w[k]->ok) {
                    row.x0 = fminf(row.x0, w[k]->x0);
                    row.y0 = fminf(row.y0, w[k]->y0);
                    row.x1 = fmaxf(row.x1, w[k]->x1);
                    row.y1 = fmaxf(row.y1, w[k]->y1);
                }
            if (in_px(yb, x, y))
                p = 2;
            else if (in_px(nb, x, y))
                p = 1;
            else if (in_px(row, x, y))
                p = 0;
        } else if (in_px(lb, x, y)) {
            p = 0;
        }
        if (p < 0)
            continue;
        float d = fabsf(y - (lb.y0 + lb.y1) * 0.5f);
        if (d < bd) {
            bd = d;
            best = i;
            *part = p;
        }
    }
    return best;
}

/* The prompt under (x, y) (a text with its button picture: New Game, the
 * pause screen's A Quit?, B Resume, Y Options ...; the nearest if two
 * reach), -1 none. */
static int page_prompt_at(float x, float y)
{
    int best = -1;
    float bd = 1e9f;
    for (int k = 0; k < s_page.nprompt; k++) {
        PxBox b = px_box(s_page.prompt[k].box, 0.35f, 0.22f);
        if (!in_px(b, x, y))
            continue;
        float d = fabsf(y - (b.y0 + b.y1) * 0.5f);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

static void prompt_press(int k)
{
    static const int out[4] = { XBOX_KBM_A, XBOX_KBM_B, XBOX_KBM_X, XBOX_KBM_Y };
    int b = s_page.prompt[k].button;
    if (b >= 0 && b < 4) {
        fprintf(stderr, "[MOUSE] page: prompt %d (%c)\n", k, "ABXY"[b]);
        tap(out[b]);
    }
}

/* The Chapters page's arrows: 0 ◄ and 1 ► beside the chapter's name, 2 ▲
 * above and 3 ▼ below its parts; *rx = where ► would go now (it follows
 * the name's end; page_frame moves it there unless the pointer is on it). */
static void page_arrows(ArrowBox *a, bool *vis, float *rx)
{
    for (int k = 0; k < 4; k++) {
        vis[k] = false;
        a[k] = ArrowBox{ 0.0f, 0.0f, 0.0f };
    }
    if (rx)
        *rx = -1.0f;
    if (!s_page_ok || s_page.chapter < 0)
        return;
    if (s_page.chapter_name.ok) {
        PxBox n = px_box(s_page.chapter_name, 0.0f, 0.0f);
        float h = n.y1 - n.y0, cy = (n.y0 + n.y1) * 0.5f, hh = h * 0.72f;
        float gap = h * 0.30f + hh * ARROW_W * 0.5f;
        a[0] = ArrowBox{ n.x0 - gap, cy, hh };
        a[1] = ArrowBox{ s_pa_rx >= 0.0f ? s_pa_rx : n.x1 + gap, cy, hh };
        /* The game takes no left on the first chapter and no right on the
         * last (0x001FAF70; in between it skips chapters not reached). */
        vis[0] = s_page.chapter > 0;
        vis[1] = s_page.chapter < 8;
        if (rx)
            *rx = n.x1 + gap;
    }
    if (s_page.kind == MENU_LIST_WHEEL && s_page.row_top.ok && s_page.row_bot.ok
        && s_page.n > 0) {
        /* Over the top row and under the bottom one (the wheel leans:
         * each on its own row's middle). */
        PxBox t = px_box(s_page.row_top, 0.0f, 0.0f), u = px_box(s_page.row_bot, 0.0f, 0.0f);
        float wd = s_h * 0.06f, half = wd * ARROW_W * 0.5f, gap = s_h * 0.014f;
        a[2] = ArrowBox{ (t.x0 + t.x1) * 0.5f, t.y0 - gap - half, wd };
        a[3] = ArrowBox{ (u.x0 + u.x1) * 0.5f, u.y1 + gap + half, wd };
        vis[2] = s_page.cursor > 0;
        vis[3] = s_page.cursor < s_page.n - 1;
    }
}

/* 1 ◄, 2 ►, 3 ▲, 4 ▼ under (x, y) (a generous box round it), else 0. */
static int page_arrow_at(float x, float y)
{
    ArrowBox a[4];
    bool vis[4];
    page_arrows(a, vis, NULL);
    for (int k = 0; k < 4; k++) {
        float hw, hh;
        if (!vis[k])
            continue;
        if (k < 2) {
            hw = a[k].hh * ARROW_W * 0.6f + s_h * 0.012f;
            hh = a[k].hh * 0.7f;
        } else {
            hw = a[k].hh * 0.65f;
            hh = a[k].hh * ARROW_W * 0.65f + s_h * 0.01f;
        }
        if (fabsf(x - a[k].cx) <= hw && fabsf(y - a[k].cy) <= hh)
            return k + 1;
    }
    return 0;
}

static void page_go(int target, int set)
{
    conker_MenuListGo(s_page.list, target, set);
    s_pg_req = target;
    s_pg_req_at = s_now;
}

/* One row along the list (+1 the next), from where the mouse asked last. */
static void page_step(int d)
{
    if (s_page.kind == MENU_LIST_NONE || s_page.n <= 0)
        return;
    int base = s_pg_req >= 0 && s_pg_req != s_page.cursor && s_now - s_pg_req_at < 800
             ? s_pg_req : s_page.cursor;
    int t = base + d;
    if (t < 0)
        t = 0;
    if (t > s_page.n - 1)
        t = s_page.n - 1;
    if (t != s_page.cursor || t != base)
        page_go(t, 0);
}

static void ch_owe(int d)
{
    if (s_ch_owe && (s_ch_owe > 0) != (d > 0))
        s_ch_owe = 0;                   /* the other way cancels */
    s_ch_owe += d;
    if (s_ch_owe > 8)
        s_ch_owe = 8;
    if (s_ch_owe < -8)
        s_ch_owe = -8;
}

static void page_click(int r, int part)
{
    const MenuPageItem &it = s_page.item[r];
    if (it.yesno) {
        page_go(r, part == 1 ? 2 : part == 2 ? 1 : it.on ? 2 : 1);
        return;
    }
    if (r == s_page.cursor && (s_pg_req < 0 || s_pg_req == r)) {
        tap(XBOX_KBM_A);
        return;
    }
    page_go(r, 0);
    if (s_page.kind == MENU_LIST_PLAIN) {
        s_pg_a = r;                     /* A once the cursor is there */
        s_pg_a_at = s_now;
    }
}

static void page_down(float x, float y)
{
    s_pg_down = -2;
    s_pg_drag = 0;
    s_pg_dragged = false;
    s_down_x = s_pg_dx = x;
    s_down_y = s_pg_dy = y;
    s_pa_press = page_arrow_at(x, y);
    if (s_pa_press) {
        int k = s_pa_press - 1;
        s_pa_pulse[k] = 1.0f;
        if (k == 1)
            s_pa_click_at = s_now;
        if (k < 2) {
            s_ch_hold = k ? 1 : -1;
            ch_owe(s_ch_hold);
        } else {
            page_step(k == 2 ? -1 : 1);
            s_pa_rep_at = s_now + 400;  /* held: more after a moment */
        }
        return;
    }
    int part, r = page_row_at(x, y, &part), k;
    if (r >= 0) {
        s_pg_down = r;
        s_pg_down_part = part;
    } else if ((k = page_prompt_at(x, y)) >= 0) {
        s_pg_down = -3 - k;
    }
}

static void page_move(float x, float y)
{
    if (s_pa_press) {
        if (page_arrow_at(x, y) != s_pa_press) {
            s_ch_hold = 0;              /* slid off: no more */
            s_pa_rep_at = 0;
        }
        return;
    }
    if (!s_pg_drag) {
        float dx = x - s_down_x, dy = y - s_down_y;
        if (sqrtf(dx * dx + dy * dy) <= s_w * DRAG_START)
            return;
        bool vert = fabsf(dy) > fabsf(dx);
        if (vert && s_page.kind == MENU_LIST_WHEEL)
            s_pg_drag = 2;
        else if (!vert && s_page.chapter >= 0)
            s_pg_drag = 1;
        else
            s_pg_drag = 3;
        s_pg_dragged = true;
    }
    if (s_pg_drag == 1) {               /* the room's way: drag left = the next */
        float step = s_w * 0.125f;
        while (x - s_pg_dx <= -step) {
            ch_owe(1);
            s_pg_dx -= step;
        }
        while (x - s_pg_dx >= step) {
            ch_owe(-1);
            s_pg_dx += step;
        }
    } else if (s_pg_drag == 2) {        /* the parts follow the hand */
        float step = s_h * 0.075f;
        while (y - s_pg_dy >= step) {
            page_step(-1);
            s_pg_dy += step;
        }
        while (y - s_pg_dy <= -step) {
            page_step(1);
            s_pg_dy -= step;
        }
    }
}

static void page_up(float x, float y)
{
    if (s_pa_press) {
        s_ch_hold = 0;
        s_pa_press = 0;
        s_pa_rep_at = 0;
    } else if (s_page_ok && !s_pg_dragged && s_pg_down != -2) {
        int part, r = page_row_at(x, y, &part);
        if (s_pg_down <= -3) {
            int k = -3 - s_pg_down;
            if (k < s_page.nprompt && page_prompt_at(x, y) == k)
                prompt_press(k);
        } else if (r == s_pg_down) {
            page_click(r, part);
        }
    }
    s_pg_down = -2;
    s_pg_drag = 0;
    s_pg_dragged = false;
}

static float ease(float v, float target, float rate, float dt);

/* Once a frame, with a page open or not. */
static void page_frame(float dt, ULONGLONG idle)
{
    s_pa_over = 0;
    s_pg_over = -1;
    s_pg_over_pr = -1;
    if (!s_page_ok) {
        if (s_pg_list || s_ch_owe || s_pa_press || s_pg_down != -2) {
            s_pg_list = 0;
            s_pg_down = -2;
            s_pg_a = -1;
            s_pg_req = -1;
            s_pg_drag = 0;
            s_pg_dragged = false;
            s_ch_owe = 0;
            s_ch_hold = 0;
            s_pa_press = 0;
        }
        s_pa_fade = ease(s_pa_fade, 0.0f, 6.0f, dt);
        if (s_pa_fade < 0.01f)
            s_pa_fade = 0.0f;
        return;
    }
    if (s_page.list != s_pg_list) {     /* another list takes the keys */
        s_pg_list = s_page.list;
        s_pg_req = -1;
        s_pg_a = -1;
    }
    if (s_shown) {
        s_pa_over = page_arrow_at(s_px, s_py);
        if (!s_pa_over && !s_pg_dragged) {
            s_pg_over = page_row_at(s_px, s_py, &s_pg_over_part);
            if (s_pg_over < 0)
                s_pg_over_pr = page_prompt_at(s_px, s_py);
        }
    }
    /* Hover: the cursor follows the pointer (rows lists only). */
    if (s_page.kind == MENU_LIST_PLAIN && s_shown && s_pg_moved && !s_ldown && s_pg_over >= 0
        && s_pg_over != s_page.cursor && s_pg_a < 0
        && !(s_pg_req == s_pg_over && s_now - s_pg_req_at < 800))
        page_go(s_pg_over, 0);
    /* A owed: once the cursor is on the row clicked. */
    if (s_pg_a >= 0) {
        if (s_page.cursor == s_pg_a && s_tap_out < 0 && !s_tapn) {
            tap(XBOX_KBM_A);
            s_pg_a = -1;
        } else if (s_now - s_pg_a_at > 1500) {
            s_pg_a = -1;
        }
    }
    /* ▲ ▼ held: a step every so often. */
    if ((s_pa_press == 3 || s_pa_press == 4) && s_pa_rep_at && s_now >= s_pa_rep_at) {
        page_step(s_pa_press == 3 ? -1 : 1);
        s_pa_pulse[s_pa_press - 1] = 1.0f;
        s_pa_rep_at = s_now + 150;
    }
    /* Chapters: one left / right press at a time, each once the name's
     * cross-fade is done (the game takes none during it). */
    if (s_page.chapter < 0) {
        s_ch_owe = 0;
        s_ch_hold = 0;
    } else {
        int want = s_ch_owe ? s_ch_owe : s_ch_hold;   /* held: more while held */
        if ((want < 0 && s_page.chapter == 0) || (want > 0 && s_page.chapter >= 8)) {
            s_ch_owe = 0;                   /* the end: the game would refuse */
            want = 0;
        }
        if (!s_page.chapter_still)
            s_ch_still_at = 0;
        else if (!s_ch_still_at)
            s_ch_still_at = s_now;
        if (want && s_ch_still_at && s_now - s_ch_still_at >= 50
            && s_now - s_ch_tap_at >= 280 && s_tap_out < 0 && !s_tapn) {
            int d = want > 0 ? 1 : -1;
            fprintf(stderr, "[MOUSE] chapter %d: %s\n", s_page.chapter, d > 0 ? "right" : "left");
            tap(d > 0 ? XBOX_KBM_DPAD_RIGHT : XBOX_KBM_DPAD_LEFT);
            if (s_ch_owe)
                s_ch_owe -= d;
            s_ch_tap_at = s_now;
            s_pa_pulse[d > 0 ? 1 : 0] = 1.0f;
        }
    }
    /* Our arrows: with the mouse in use, as on the boards. */
    ArrowBox a[4];
    bool vis[4];
    float rx;
    page_arrows(a, vis, &rx);
    if (s_page.chapter < 0) {
        s_pa_rx = -1.0f;
    } else if (rx >= 0.0f) {
        /* ► follows the name's end, but not from under the pointer: not
         * while on it, held, or just clicked (the next click is coming). */
        if (s_pa_rx < 0.0f)
            s_pa_rx = rx;
        else if (s_pa_over != 2 && s_pa_press != 2 && s_now - s_pa_click_at > 1500)
            s_pa_rx = ease(s_pa_rx, rx, 14.0f, dt);
    }
    bool show = s_page.chapter >= 0 && s_shown && s_moved
                && (idle < ARROW_IDLE_MS || s_pa_over || s_ldown);
    s_pa_fade = ease(s_pa_fade, show ? 1.0f : 0.0f, show ? 9.0f : 4.0f, dt);
    if (!show && s_pa_fade < 0.01f)
        s_pa_fade = 0.0f;
    for (int k = 0; k < 4; k++) {
        bool lit = s_pa_over == k + 1 || s_pa_press == k + 1;
        s_pa_vis[k] = ease(s_pa_vis[k], vis[k] ? 1.0f : 0.0f, 10.0f, dt);
        s_pa_lit[k] = ease(s_pa_lit[k], lit ? 1.0f : 0.0f, 12.0f, dt);
        s_pa_pulse[k] -= dt * 5.0f;
        if (s_pa_pulse[k] < 0.0f)
            s_pa_pulse[k] = 0.0f;
    }
}

/* ---- the multiplayer menus' cylinder lists (step 5) --------------------------
 *
 * Every multiplayer list (Welcome, Dumbots, Profiles, the map list, Configure,
 * team select, the multiplayer pause menu ...) is drawn into a texture and
 * wrapped round a glass cylinder (menu_page.h, MenuCylSnap): the game side
 * puts the cylinder on the screen as a grid of points, and the pointer goes
 * back through the grid cell it is in onto the texture, where each row's
 * letters were drawn. Hovering a row moves the game's selection there (its
 * own up / down, one a frame); a click is A on it. A scrolling list keeps
 * the chosen row in the middle: while the mouse is used the list is held
 * where it is (conker_MenuCylHold) and the bracket goes to the hovered row
 * instead (user 2026-11-14: no sliding under the pointer); a drag up / down
 * moves the list by hand, the wheel is the menus' up / down (the list then
 * follows the selection again, as a key or the controller makes it). */

static MenuCylSnap s_cy;
static bool s_cy_ok;
static int s_cy_over = -1;      /* the row under the pointer */
static int s_cy_down = -1;      /* the row the left button went down on */
static int s_cy_drag;           /* 0 not yet, 1 up / down the list, 2 none */
static float s_cy_down_y;
static int s_cy_drag_from;      /* the row in the middle when the drag began */
static int s_cy_held = -1;      /* the row we keep in the middle, -1 the game's */
static int s_cy_hover = -1;     /* the row the last hover asked for */
static ULONGLONG s_cy_hover_at;
static uint32_t s_cy_sig;       /* the list all that is about (its rows' texts) */
static int s_cya_row = -1;      /* the picker row our ◄ ► are on */
static int s_cya_over;          /* 1 ◄, 2 ► under the pointer */
static int s_cya_press;         /* the one the left button went down on (held: more) */
static ULONGLONG s_cya_rep_at;
static float s_cya_fade, s_cya_lit[2], s_cya_pulse[2];

static void cyl_hold(int c)
{
    if (c != s_cy_held) {
        s_cy_held = c;
        conker_MenuCylHold(c, s_cy.sig);
    }
}

static float cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

/* Inverse bilinear: (x, y) in the quad a b c d (a + s (b - a) + t (d - a)
 * + s t (a - b + c - d)); true with s, t in 0..1. */
static bool quad_st(const float *a, const float *b, const float *c, const float *d,
                    float x, float y, float *s, float *t)
{
    float ex = b[0] - a[0], ey = b[1] - a[1], fx = d[0] - a[0], fy = d[1] - a[1];
    float gx = a[0] - b[0] + c[0] - d[0], gy = a[1] - b[1] + c[1] - d[1];
    float hx = x - a[0], hy = y - a[1];
    float k2 = cross2(gx, gy, fx, fy);
    float k1 = cross2(ex, ey, fx, fy) + cross2(hx, hy, gx, gy);
    float k0 = cross2(hx, hy, ex, ey);
    float tt[2];
    int nt = 0;
    const float eps = 1e-3f;
    if (fabsf(k2) < 1e-9f) {
        if (fabsf(k1) < 1e-12f)
            return false;
        tt[nt++] = -k0 / k1;
    } else {
        float w = k1 * k1 - 4.0f * k0 * k2;
        if (w < 0.0f)
            return false;
        w = sqrtf(w);
        tt[nt++] = (-k1 - w) / (2.0f * k2);
        tt[nt++] = (-k1 + w) / (2.0f * k2);
    }
    for (int i = 0; i < nt; i++) {
        float v = tt[i];
        if (v < -eps || v > 1.0f + eps)
            continue;
        float dx = ex + gx * v, dy = ey + gy * v, u;
        if (fabsf(dx) > fabsf(dy)) {
            u = (hx - fx * v) / dx;
        } else {
            if (fabsf(dy) < 1e-12f)
                continue;
            u = (hy - fy * v) / dy;
        }
        if (u < -eps || u > 1.0f + eps)
            continue;
        *s = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        *t = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        return true;
    }
    return false;
}

/* The pointer (back-buffer px) on the cylinder's texture: u, v 0..1 (v down);
 * false off it. */
static bool cyl_uv(float x, float y, float *u, float *v)
{
    float nx = x / s_w * 2.0f - 1.0f, ny = 1.0f - y / s_h * 2.0f;
    for (int iv = 0; iv < MENU_CYL_VSTEPS; iv++)
        for (int iu = 0; iu < MENU_CYL_COLS; iu++) {
            const float *a = s_cy.pt[iv][iu], *b = s_cy.pt[iv][iu + 1];
            const float *c = s_cy.pt[iv + 1][iu + 1], *d = s_cy.pt[iv + 1][iu];
            float s, t;
            if (nx < fminf(fminf(a[0], b[0]), fminf(c[0], d[0])) - 1e-3f
                || nx > fmaxf(fmaxf(a[0], b[0]), fmaxf(c[0], d[0])) + 1e-3f
                || ny < fminf(fminf(a[1], b[1]), fminf(c[1], d[1])) - 1e-3f
                || ny > fmaxf(fmaxf(a[1], b[1]), fmaxf(c[1], d[1])) + 1e-3f)
                continue;
            if (quad_st(a, b, c, d, nx, ny, &s, &t)) {
                *u = ((float)iu + s) / (float)MENU_CYL_COLS;
                *v = ((float)iv + t) / (float)MENU_CYL_VSTEPS;
                return true;
            }
        }
    return false;
}

/* A point of the texture (u, v 0..1) on the screen, back-buffer px. */
static ImVec2 cyl_px(float u, float v)
{
    float fu = u * (float)MENU_CYL_COLS, fv = v * (float)MENU_CYL_VSTEPS;
    int iu = (int)fu, iv = (int)fv;
    if (iu > MENU_CYL_COLS - 1) iu = MENU_CYL_COLS - 1;
    if (iv > MENU_CYL_VSTEPS - 1) iv = MENU_CYL_VSTEPS - 1;
    if (iu < 0) iu = 0;
    if (iv < 0) iv = 0;
    float s = fu - (float)iu, t = fv - (float)iv;
    const float *a = s_cy.pt[iv][iu], *b = s_cy.pt[iv][iu + 1];
    const float *c = s_cy.pt[iv + 1][iu + 1], *d = s_cy.pt[iv + 1][iu];
    float nx = a[0] + s * (b[0] - a[0]) + t * (d[0] - a[0]) + s * t * (a[0] - b[0] + c[0] - d[0]);
    float ny = a[1] + s * (b[1] - a[1]) + t * (d[1] - a[1]) + s * t * (a[1] - b[1] + c[1] - d[1]);
    return ImVec2((nx + 1.0f) * 0.5f * s_w, (1.0f - ny) * 0.5f * s_h);
}

/* Texture units (-128..128, y down) to u, v. */
static float cyl_tu(float x) { return (x + 128.0f) / 256.0f; }
static float cyl_tv(float y) { return (y + 128.0f) / 256.0f; }

/* The row under (x, y): the whole width of the cylinder, half a row's
 * spacing round its letters' middle; -1 none. */
static int cyl_row_at(float x, float y)
{
    float u, v;
    if (!s_cy_ok || !cyl_uv(x, y, &u, &v))
        return -1;
    float ty = v * 256.0f - 128.0f, half = s_cy.pitch * 0.5f, bd = 1e9f;
    int best = -1;
    for (int i = 0; i < s_cy.n; i++) {
        const MenuCylRow &r = s_cy.row[i];
        if (!r.ok)
            continue;
        float cy = (r.y0 + r.y1) * 0.5f, h = fmaxf(half, (r.y1 - r.y0) * 0.5f);
        float d = fabsf(ty - cy);
        if (d <= h && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* One row's spacing on the screen near the pointer, px (for drags). */
static float cyl_row_px(void)
{
    float u, v;
    if (!cyl_uv(s_px, s_py, &u, &v)) {
        u = 0.5f;
        v = 0.5f;
    }
    float dv = s_cy.pitch / 256.0f, v0 = v - dv * 0.5f;
    if (v0 < 0.0f) v0 = 0.0f;
    if (v0 + dv > 1.0f) v0 = 1.0f - dv;
    ImVec2 a = cyl_px(u, v0), b = cyl_px(u, v0 + dv);
    float d = fabsf(b.y - a.y);
    return d > 4.0f ? d : 4.0f;
}

/* The picker's arrows: just outside the game's own side icons (centred at
 * x 45 and 203 of the 256-wide texture, 24 wide), on the value's line,
 * half again as tall as the value. */
#define CYA_GAME_L 45.0f
#define CYA_GAME_R 203.0f
#define CYA_OUT 22.0f           /* texture px outwards from the game's icon */
static bool cyl_arrows(int r, ArrowBox *a)
{
    if (r < 0 || r >= s_cy.n || !s_cy.row[r].picker || !s_cy.row[r].vok)
        return false;
    const MenuCylRow &w = s_cy.row[r];
    float v = cyl_tv((w.vy0 + w.vy1) * 0.5f);
    float v0 = cyl_tv(w.vy0), v1 = cyl_tv(w.vy1);
    for (int k = 0; k < 2; k++) {
        float u = (k ? CYA_GAME_R + CYA_OUT : CYA_GAME_L - CYA_OUT) / 256.0f;
        ImVec2 c = cyl_px(u, v), t = cyl_px(u, v0), b = cyl_px(u, v1);
        float hh = fabsf(b.y - t.y) * 1.5f;
        if (hh < 12.0f)
            hh = 12.0f;
        a[k] = ArrowBox{ c.x, c.y, hh };
    }
    return true;
}

/* 1 ◄, 2 ► of row r under (x, y) (ours or the game's icon beside it), else 0. */
static int cyl_arrow_at(int r, float x, float y)
{
    ArrowBox a[2];
    if (!cyl_arrows(r, a))
        return 0;
    const MenuCylRow &w = s_cy.row[r];
    float v = cyl_tv((w.vy0 + w.vy1) * 0.5f);
    for (int k = 0; k < 2; k++) {
        float hw = a[k].hh * ARROW_W * 0.6f + s_h * 0.012f, hh = a[k].hh * 0.75f;
        /* Reach over the game's icon too (its far side, 12 texture px past
         * its centre, towards the value). */
        ImVec2 g = cyl_px((k ? CYA_GAME_R - 12.0f : CYA_GAME_L + 12.0f) / 256.0f, v);
        float x0 = fminf(a[k].cx - hw, g.x), x1 = fmaxf(a[k].cx + hw, g.x);
        if (x >= x0 && x <= x1 && fabsf(y - a[k].cy) <= hh)
            return k + 1;
    }
    return 0;
}

/* ---- class select (step 5c) ----
 * The waiting room's class select: the class's model stands in the empty
 * cylinder (s_cy.cls >= 0, no rows). Our ◄ ► at the glass's left and right
 * edges, half way up (a click is the game's left / right, held repeats); a
 * drag left / right anywhere (not begun on a prompt or an arrow) steps one
 * class per CLS_STEP of the width, left = the next class (as a carousel
 * turns); a click on the model is A (picks it). The pad's own presses
 * (taps), so the game's sound and model swap are its own. */
#define CLS_ARROW_H 0.095f      /* of the height */
#define CLS_STEP 0.125f         /* of the width dragged: one class */
#define CLS_OUT 0.35f           /* an arrow's width outside the glass's edge */
static int s_cl_over;           /* 1 ◄, 2 ►, 3 the model, under the pointer */
static int s_cl_press;          /* the arrow the left button went down on (held: more) */
static ULONGLONG s_cl_rep_at;
static bool s_cl_down;          /* the left button went down here (not on an arrow) */
static bool s_cl_on_model;      /* ... on the model */
static bool s_cl_drag;          /* moved past DRAG_START since */
static int s_cl_steps;          /* classes this drag stepped, + right (the one before) */
static float s_cl_fade, s_cl_lit[2], s_cl_pulse[2];
static int s_cl_seen = -2;      /* the class last logged */

static bool cls_mode(void)
{
    return s_cy_ok && s_cy.cls >= 0;
}

static void cls_arrows(ArrowBox *a)
{
    float hh = s_h * CLS_ARROW_H, w = hh * ARROW_W;
    for (int k = 0; k < 2; k++) {
        ImVec2 c = cyl_px(k ? 1.0f : 0.0f, 0.5f);
        a[k] = ArrowBox{ c.x + (k ? 1.0f : -1.0f) * w * CLS_OUT, c.y, hh };
    }
}

/* The model's box, px: the glass from edge to edge, from 0.18 of its height
 * above its top (the model's head stands higher; the band above starts
 * there) to a tenth below. */
static void cls_model(float *x0, float *y0, float *x1, float *y1)
{
    ImVec2 l = cyl_px(0.0f, 0.5f), r = cyl_px(1.0f, 0.5f);
    ImVec2 t = cyl_px(0.5f, 0.0f), b = cyl_px(0.5f, 1.0f);
    float h = b.y - t.y;
    *x0 = fminf(l.x, r.x);
    *x1 = fmaxf(l.x, r.x);
    *y0 = t.y - h * 0.18f;
    *y1 = b.y + h * 0.1f;
}

/* 1 ◄, 2 ►, 3 the model under (x, y), else 0. */
static int cls_at(float x, float y)
{
    ArrowBox a[2];
    float x0, y0, x1, y1;
    cls_arrows(a);
    for (int k = 0; k < 2; k++) {
        float hw = a[k].hh * ARROW_W * 0.6f + s_h * 0.012f, hh = a[k].hh * 0.75f;
        if (fabsf(x - a[k].cx) <= hw && fabsf(y - a[k].cy) <= hh)
            return k + 1;
    }
    cls_model(&x0, &y0, &x1, &y1);
    return x >= x0 && x <= x1 && y >= y0 && y <= y1 ? 3 : 0;
}

/* One class: dir -1 the one before (the game's left), +1 the next (right).
 * Owed, not queued: one press at a time goes out (cls_frame), so a fast drag
 * back and forth cancels out instead of filling the presses' queue. */
static int s_cl_owe;            /* classes owed, + next */

static void cls_step(int dir)
{
    s_cl_owe += dir > 0 ? 1 : -1;
    if (s_cl_owe > 8) s_cl_owe = 8;
    if (s_cl_owe < -8) s_cl_owe = -8;
    s_cl_pulse[dir > 0] = 1.0f;
}

static void cls_down(float x, float y)
{
    int k = cls_at(x, y);
    s_cl_press = k == 1 || k == 2 ? k : 0;
    s_cl_drag = false;
    s_cl_steps = 0;
    s_down_x = x;
    s_cy_down_y = y;
    if (s_cl_press) {
        fprintf(stderr, "[MOUSE] class: arrow %s\n", s_cl_press == 1 ? "left" : "right");
        cls_step(s_cl_press == 1 ? -1 : 1);
        s_cl_rep_at = s_now + 450;      /* held: more after a moment */
        s_cl_down = false;
        return;
    }
    s_cl_down = true;
    s_cl_on_model = k == 3;
}

static void cls_move(float x, float y)
{
    if (s_cl_press && cls_at(x, y) != s_cl_press)
        s_cl_press = 0;                 /* slid off the arrow: no more */
    if (!s_cl_down)
        return;
    if (!s_cl_drag) {
        float dx = x - s_down_x, dy = y - s_cy_down_y;
        if (sqrtf(dx * dx + dy * dy) <= s_w * DRAG_START)
            return;
        s_cl_drag = true;
    }
    /* Whole steps of the width dragged (towards zero): left = the next. */
    int k = (int)((x - s_down_x) / (s_w * CLS_STEP));
    while (s_cl_steps != k) {
        int d = k > s_cl_steps ? 1 : -1;
        fprintf(stderr, "[MOUSE] class: drag %s\n", d < 0 ? "next" : "back");
        cls_step(-d);
        s_cl_steps += d;
    }
}

static void cls_up(float x, float y)
{
    if (s_cl_down && !s_cl_drag && s_cl_on_model && cls_at(x, y) == 3) {
        fprintf(stderr, "[MOUSE] class: pick %d (A)\n", s_cy.cls);
        tap(XBOX_KBM_A);
    }
    s_cl_press = 0;
    s_cl_down = false;
    s_cl_drag = false;
}

static void cls_reset(void)
{
    s_cl_over = 0;
    s_cl_press = 0;
    s_cl_down = false;
    s_cl_drag = false;
    s_cl_fade = 0.0f;
    s_cl_owe = 0;
}

/* Once a frame. */
static void cls_frame(float dt, ULONGLONG idle)
{
    if (!cls_mode()) {
        if (s_cl_seen != -2 || s_cl_down || s_cl_press || s_cl_fade > 0.0f) {
            cls_reset();
            s_cl_seen = -2;
        }
        return;
    }
    if (s_cy.cls != s_cl_seen) {
        fprintf(stderr, "[MOUSE] class: shown %d\n", s_cy.cls);
        s_cl_seen = s_cy.cls;
    }
    s_cl_over = s_shown && !s_cl_drag ? cls_at(s_px, s_py) : 0;
    if (s_cl_press && s_now >= s_cl_rep_at) {
        cls_step(s_cl_press == 1 ? -1 : 1);
        s_cl_rep_at = s_now + 220;
    }
    if (s_cl_owe && !s_tapn && s_tap_out < 0) {     /* the last press is done */
        tap(s_cl_owe > 0 ? XBOX_KBM_DPAD_RIGHT : XBOX_KBM_DPAD_LEFT);
        s_cl_owe += s_cl_owe > 0 ? -1 : 1;
    }
    bool show = s_shown && s_moved && (idle < ARROW_IDLE_MS || s_cl_over || s_ldown);
    s_cl_fade = ease(s_cl_fade, show ? 1.0f : 0.0f, show ? 9.0f : 4.0f, dt);
    if (!show && s_cl_fade < 0.01f)
        s_cl_fade = 0.0f;
    for (int k = 0; k < 2; k++) {
        bool lit = s_cl_over == k + 1 || s_cl_press == k + 1;
        s_cl_lit[k] = ease(s_cl_lit[k], lit ? 1.0f : 0.0f, 12.0f, dt);
        s_cl_pulse[k] -= dt * 5.0f;
        if (s_cl_pulse[k] < 0.0f)
            s_cl_pulse[k] = 0.0f;
    }
}

static void cyl_reset(void)
{
    s_cy_down = -1;
    s_cy_drag = 0;
    s_cy_hover = -1;
    cyl_hold(-1);
}

static void cyl_down(float x, float y)
{
    if (cls_mode()) {
        cls_down(x, y);
        return;
    }
    s_cya_press = s_cy.still ? cyl_arrow_at(s_cya_row, x, y) : 0;
    if (s_cya_press) {                  /* ◄ / ►: the picker's left / right */
        conker_MenuCylGo(s_cya_row, s_cya_press == 1 ? MENU_CYL_LEFT : MENU_CYL_RIGHT);
        s_cya_pulse[s_cya_press - 1] = 1.0f;
        s_cya_rep_at = s_now + 450;     /* held: more after a moment */
        s_cy_down = -1;
        s_cy_drag = 2;
        return;
    }
    s_cy_down = s_cy.still && !s_cy.grid ? cyl_row_at(x, y) : -1;
    s_cy_drag = 0;
    s_cy_down_y = y;
    s_down_x = x;
    s_cy_drag_from = s_cy_held >= 0 ? s_cy_held : s_cy.centre;
}

static void cyl_move(float x, float y)
{
    if (cls_mode()) {
        cls_move(x, y);
        return;
    }
    if (!s_cy_drag) {
        float dx = x - s_down_x, dy = y - s_cy_down_y;
        if (sqrtf(dx * dx + dy * dy) <= s_w * DRAG_START)
            return;
        s_cy_drag = fabsf(dy) > fabsf(dx) && s_cy.scrolls && !s_cy.grid ? 1 : 2;
    }
    if (s_cya_press && cyl_arrow_at(s_cya_row, x, y) != s_cya_press)
        s_cya_press = 0;                /* slid off the arrow: no more */
    if (s_cy_drag == 1) {               /* the list follows the hand */
        int k = (int)lroundf((y - s_cy_down_y) / cyl_row_px());
        int c = s_cy_drag_from - k;
        if (c < 0) c = 0;
        if (c > s_cy.n - 1) c = s_cy.n - 1;
        cyl_hold(c);
    }
}

static void cyl_up(float x, float y)
{
    if (cls_mode()) {
        cls_up(x, y);
        return;
    }
    s_cya_press = 0;
    if (!s_cy_drag && s_cy_down >= 0 && cyl_row_at(x, y) == s_cy_down && s_cy.still) {
        fprintf(stderr, "[MOUSE] list: row %d (selection %d)\n", s_cy_down, s_cy.sel);
        conker_MenuCylGo(s_cy_down, MENU_CYL_A);
        s_cy_hover = s_cy_down;
        s_cy_hover_at = s_now;
    }
    s_cy_down = -1;
    s_cy_drag = 0;
}

/* Once a frame. */
static void cyl_frame(float dt, ULONGLONG idle)
{
    cls_frame(dt, idle);
    if (!s_cy_ok) {
        if (s_cy_held >= 0 || s_cy_down >= 0)
            cyl_reset();
        s_cy_over = -1;
        s_cy_sig = 0;
        s_cya_row = -1;
        s_cya_over = 0;
        s_cya_press = 0;
        s_cya_fade = 0.0f;
        return;
    }
    if (s_cy.sig != s_cy_sig) {           /* another list */
        s_cy_sig = s_cy.sig;
        cyl_reset();
    }
    if (InterlockedExchange(&s_cy_keys, 0))
        cyl_hold(-1);                   /* keys or the controller: the game's own list */
    if (!s_cy.scrolls || s_cy.grid)
        cyl_hold(-1);
    s_cy_over = s_shown && s_cy_drag != 1 && s_cy.still && !s_cy.grid ? cyl_row_at(s_px, s_py) : -1;
    /* Hover: the selection follows the pointer (a scrolling list held still). */
    /* The picker's ◄ ►: on the picker row under the pointer, else the
     * selected one (kept while pressed). */
    if (!s_cya_press) {
        int r = s_cy_over >= 0 && s_cy.row[s_cy_over].picker ? s_cy_over
              : s_cy.sel >= 0 && s_cy.sel < s_cy.n && s_cy.row[s_cy.sel].picker ? s_cy.sel : -1;
        if (r != s_cya_row && !(s_cya_row >= 0 && s_cya_over))
            s_cya_row = r;
    }
    s_cya_over = s_shown && s_cy.still ? cyl_arrow_at(s_cya_row, s_px, s_py) : 0;
    if (s_cya_over)
        s_cy_over = -1;                 /* the arrow, not the row */
    if (s_cya_press && s_cya_rep_at && s_now >= s_cya_rep_at) {
        conker_MenuCylGo(s_cya_row, s_cya_press == 1 ? MENU_CYL_LEFT : MENU_CYL_RIGHT);
        s_cya_pulse[s_cya_press - 1] = 1.0f;
        s_cya_rep_at = s_now + 220;
    }
    if (s_shown && s_pg_moved && !s_ldown && s_cy_over >= 0 && s_cy_over != s_cy.sel
        && !(s_cy_hover == s_cy_over && s_now - s_cy_hover_at < 600)) {
        if (s_cy.scrolls && s_cy_held < 0)
            cyl_hold(s_cy.centre);
        {
            float u = -1.0f, v = -1.0f;
            cyl_uv(s_px, s_py, &u, &v);
            fprintf(stderr, "[MOUSE] list: hover row %d (selection %d) at %.0f,%.0f = %.3f,%.3f"
                    " (%.0f x %.0f)\n", s_cy_over, s_cy.sel, s_px, s_py, u, v, s_w, s_h);
        }
        conker_MenuCylGo(s_cy_over, MENU_CYL_NONE);
        s_cy_hover = s_cy_over;
        s_cy_hover_at = s_now;
    }
    bool show = s_cya_row >= 0 && s_shown && s_moved
                && (idle < ARROW_IDLE_MS || s_cya_over || s_ldown);
    s_cya_fade = ease(s_cya_fade, show ? 1.0f : 0.0f, show ? 9.0f : 4.0f, dt);
    if (!show && s_cya_fade < 0.01f)
        s_cya_fade = 0.0f;
    for (int k = 0; k < 2; k++) {
        bool lit = s_cya_over == k + 1 || s_cya_press == k + 1;
        s_cya_lit[k] = ease(s_cya_lit[k], lit ? 1.0f : 0.0f, 12.0f, dt);
        s_cya_pulse[k] -= dt * 5.0f;
        if (s_cya_pulse[k] < 0.0f)
            s_cya_pulse[k] = 0.0f;
    }
}

/* ---- the multiplayer screens' button prompts (step 5b) ----
 * The plate at the foot of each side panel (A Select, B Back, X Configure,
 * the waiting room's A Ready / Y Mission Brief ...): the hand over its words
 * and button picture, which glow; a click is that button's press. The game
 * side carries their box through the plate's tilted square onto the screen. */
static MenuPlateSnap s_pl;
static bool s_pl_ok;
static int s_pl_over = -1;          /* the prompt under the pointer, -1 none */
static uint32_t s_pl_over_key;      /* the last one logged */
static uint32_t s_pl_down_key;      /* the left button went down on this prompt, 0 none */
static int s_pl_down_button;

/* Prompt k's outline in back-buffer px (*n points): a band's as it is; a
 * plate's square (top left, top right, bottom right, bottom left) grown a
 * little round its words and picture, across by 0.3 of its height each
 * side, up and down by 0.25. */
static ImVec2 plate_pt(const MenuPlatePrompt &p, int i)
{
    return ImVec2((p.pt[i][0] + 1.0f) * 0.5f * s_w, (1.0f - p.pt[i][1]) * 0.5f * s_h);
}

static bool plate_px(int k, ImVec2 *o, int *n)
{
    const MenuPlatePrompt &p = s_pl.p[k];
    *n = p.n < 3 ? 0 : p.n > MENU_PLATE_PTS ? MENU_PLATE_PTS : p.n;
    for (int i = 0; i < *n; i++)
        o[i] = plate_pt(p, i);
    if (*n != 4)
        return *n > 0;
    float ax = (o[1].x - o[0].x + o[2].x - o[3].x) * 0.5f, ay = (o[1].y - o[0].y + o[2].y - o[3].y) * 0.5f;
    float dx = (o[3].x - o[0].x + o[2].x - o[1].x) * 0.5f, dy = (o[3].y - o[0].y + o[2].y - o[1].y) * 0.5f;
    float wl = sqrtf(ax * ax + ay * ay), hl = sqrtf(dx * dx + dy * dy);
    if (wl < 1.0f || hl < 1.0f)
        return false;
    float mx = hl * 0.3f, my = hl * 0.25f;
    for (int i = 0; i < 4; i++) {
        float sa = i == 1 || i == 2 ? 1.0f : -1.0f, sd = i >= 2 ? 1.0f : -1.0f;
        o[i].x += ax / wl * sa * mx + dx / hl * sd * my;
        o[i].y += ay / wl * sa * mx + dy / hl * sd * my;
    }
    return true;
}

/* The prompt under (x, y), back-buffer px (inside its outline, even-odd);
 * -1 none. */
static int plate_at(float x, float y)
{
    if (!s_pl_ok)
        return -1;
    for (int k = 0; k < s_pl.n; k++) {
        ImVec2 o[MENU_PLATE_PTS];
        int n;
        bool in = false;
        if (!plate_px(k, o, &n))
            continue;
        for (int i = 0, j = n - 1; i < n; j = i++)
            if ((o[i].y > y) != (o[j].y > y)
                && x < (o[j].x - o[i].x) * (y - o[i].y) / (o[j].y - o[i].y) + o[i].x)
                in = !in;
        if (in)
            return k;
    }
    return -1;
}

static void plate_press(int k)
{
    static const int out[4] = { XBOX_KBM_A, XBOX_KBM_B, XBOX_KBM_X, XBOX_KBM_Y };
    int b = s_pl.p[k].button;
    if (b >= 0 && b < 4) {
        fprintf(stderr, "[MOUSE] prompt: %c\n", "ABXY"[b]);
        tap(out[b]);
    }
}

/* Once a frame. */
static void plate_frame(void)
{
    s_pl_over = s_shown ? plate_at(s_px, s_py) : -1;
    uint32_t key = s_pl_over >= 0 ? s_pl.p[s_pl_over].key : 0u;
    if (key != s_pl_over_key) {
        if (key)
            fprintf(stderr, "[MOUSE] prompt: hover %c\n", "ABXY"[s_pl.p[s_pl_over].button & 3]);
        s_pl_over_key = key;
    }
}

/* ---- the profile name keyboard (step 5d) ----
 * New Profile / Rename: the game's own keyboard, flat in the 640 x 480 frame
 * (drawn at 0, 0; the frame is stretched over the picture, as all 2D is), so
 * each key's box is the game's layout (0x00255A70: rows every 29 px from y
 * 175; the buttons on the left 125 wide from x 150, the grid's keys 29 wide
 * from x 300, Backspace 116 and Space 174 wide, the arrows 58). Hovering a
 * key moves the game's highlight there (its move sound), a click on it
 * (pressed and let go on the same key) is A there, right click Cancel (the
 * game's Back), the wheel nothing. Over the screen behind it (the profile
 * list, its prompts) the mouse does nothing while it is up. */
static bool s_nk_ok;
static int s_nk_row, s_nk_col, s_nk_page, s_nk_shift;
static int s_nk_over = -1;          /* the key under the pointer: row | col << 4, -1 none */
static int s_nk_down = -1;          /* the key the left button went down on */
static int s_nk_hover = -1;         /* the key the last hover asked for */

static bool nk_box(int row, int col, float *x0, float *y0, float *x1, float *y1)
{
    float x, w = 29.0f, y = 175.0f + 29.0f * (float)row;
    if (!conker_NameKbValid(s_nk_page, s_nk_shift, row, col))
        return false;
    if (col == 0) {
        x = 150.0f;
        w = 125.0f;
    } else if (row == 4) {
        x = col == 1 ? 300.0f : col == 7 ? 474.0f : 532.0f;
        w = col == 1 ? 174.0f : 58.0f;
    } else if (row == 3 && col == 7) {
        x = 474.0f;
        w = 116.0f;
    } else {
        x = 300.0f + 29.0f * (float)(col - 1);
    }
    *x0 = x * s_w / 640.0f;
    *x1 = (x + w) * s_w / 640.0f;
    *y0 = y * s_h / 480.0f;
    *y1 = (y + 29.0f) * s_h / 480.0f;
    return true;
}

static int nk_at(float x, float y)
{
    float x0, y0, x1, y1;
    for (int row = 0; row <= 5; row++)
        for (int col = 0; col <= 10; col++)
            if (nk_box(row, col, &x0, &y0, &x1, &y1) && x >= x0 && x < x1 && y >= y0 && y < y1)
                return row | col << 4;
    return -1;
}

static void nk_frame(void)
{
    static bool was;
    if (s_nk_ok != was) {
        fprintf(stderr, "[MOUSE] name keyboard: %s\n", s_nk_ok ? "up" : "gone");
        was = s_nk_ok;
        s_nk_over = s_nk_down = s_nk_hover = -1;
    }
    if (!s_nk_ok)
        return;
    s_nk_over = s_shown ? nk_at(s_px, s_py) : -1;
    /* A move onto another key: the highlight there (once: the keys or the
     * pad may move it away again while the pointer rests). */
    if (s_pg_moved && s_shown && s_nk_over != s_nk_hover) {
        if (s_nk_over >= 0)
            conker_NameKbHover(s_nk_over & 15, s_nk_over >> 4);
        s_nk_hover = s_nk_over;
    }
}

static void events_frame(void)
{
    MmEvent ev[128];
    int n;

    AcquireSRWLockExclusive(&s_lock);
    n = s_qn;
    memcpy(ev, s_q, (size_t)n * sizeof(ev[0]));
    s_qn = 0;
    ReleaseSRWLockExclusive(&s_lock);

    for (int i = 0; i < n; i++) {
        const MmEvent &e = ev[i];
        switch (e.type) {
        case EV_MOVE:
            s_px = e.x;
            s_py = e.y;
            s_pg_moved = true;
            if (s_ldown && !s_boards && s_page_ok && !s_pl_down_key)
                page_move(e.x, e.y);
            if (s_ldown && !s_boards && !s_page_ok && s_cy_ok && !s_pl_down_key)
                cyl_move(e.x, e.y);
            if (s_ldown && !s_press_arrow && s_boards)
                drag_to(e.x);
            if (s_press_arrow && arrow_at(e.x, e.y) != s_press_arrow)
                s_hold = 0;             /* slid off the arrow: no more */
            break;
        case EV_LDOWN:
            s_px = e.x;
            s_py = e.y;
            s_ldown = true;
            s_press_arrow = 0;
            fling_stop();               /* caught: the room stays under the hand */
            if (s_dragged)
                conker_MenuBoardsHold(-1.0f, 0);
            s_dragged = false;
            s_down_x = e.x;
            s_down_board = -1;
            s_pl_down_key = 0;
            if (s_nk_ok) {                  /* the name keyboard: typed when let go on it */
                s_nk_down = nk_at(e.x, e.y);
                break;
            }
            if (!s_boards) {
                int k = plate_at(e.x, e.y);
                if (k >= 0) {           /* a prompt: pressed when let go on it */
                    s_pl_down_key = s_pl.p[k].key;
                    s_pl_down_button = s_pl.p[k].button;
                    s_cy_down = -1;
                    break;
                }
                if (s_page_ok)
                    page_down(e.x, e.y);
                else if (s_cy_ok)
                    cyl_down(e.x, e.y);
                break;
            }
            s_press_arrow = arrow_at(e.x, e.y);
            if (s_press_arrow) {
                s_click_board = -1;
                s_open_board = -1;
                s_hold = s_press_arrow;
                owe(s_press_arrow);
                s_pulse[s_press_arrow > 0] = 1.0f;
            } else {
                s_down_board = board_at(e.x, e.y);
            }
            break;
        case EV_LUP:
            s_px = e.x;
            s_py = e.y;
            if (s_nk_ok && s_ldown) {
                int k = nk_at(e.x, e.y);
                if (k >= 0 && k == s_nk_down) {
                    fprintf(stderr, "[MOUSE] name keyboard: click row %d col %d\n", k & 15, k >> 4);
                    conker_NameKbClick(k & 15, k >> 4);
                }
                s_nk_down = -1;
            } else if (!s_boards && s_ldown && s_pl_down_key) {
                int k = plate_at(e.x, e.y);
                if (k >= 0 && s_pl.p[k].key == s_pl_down_key
                    && s_pl.p[k].button == s_pl_down_button)
                    plate_press(k);
                s_pl_down_key = 0;
            } else if (!s_boards && s_ldown && s_page_ok)
                page_up(e.x, e.y);
            else if (!s_boards && s_ldown && s_cy_ok)
                cyl_up(e.x, e.y);
            if (s_dragged) {
                /* Let go while moving: the room turns on and slows down
                 * (momentum); let go still: it stays where it is. */
                float v = drag_speed();
                if (fabsf(v) >= FLING_MIN && s_boards) {
                    s_fling = true;
                    s_fling_th = s_trk_th[(s_trk_n - 1) % 8];
                    s_fling_v = v > FLING_MAX ? FLING_MAX : v < -FLING_MAX ? -FLING_MAX : v;
                    InterlockedExchange(&s_keys_used, 0);
                    fprintf(stderr, "[MOUSE] fling: %.0f deg/s from %.3f\n", s_fling_v,
                            conker_MenuBoardsPos());
                } else {
                    conker_MenuBoardsHold(-1.0f, 0);
                }
            } else if (s_ldown && s_boards && !s_press_arrow && s_down_board >= 0
                       && board_at(e.x, e.y) == s_down_board) {
                s_owe = 0;
                s_open_board = -1;
                s_click_board = s_down_board;
            }
            s_ldown = false;
            s_dragged = false;
            s_hold = 0;
            s_press_arrow = 0;
            s_down_board = -1;
            break;
        case EV_RDOWN:
            if (s_nk_ok) {                  /* B does nothing there: the game's Back, Cancel */
                fprintf(stderr, "[MOUSE] name keyboard: right click (Cancel)\n");
                conker_NameKbType(NAMEKB_CANCEL);
            } else if (s_boards)
                overlay_open_quit();
            else
                tap(XBOX_KBM_B);
            break;
        case EV_WHEEL:
            if (s_boards || s_nk_ok)
                break;
            if (s_page_ok && s_page.kind != MENU_LIST_NONE && s_page.n > 0) {
                /* Along the list taking the keys, whichever way it lies. */
                for (int k = 0; k < 4 && k < (e.n < 0 ? -e.n : e.n); k++)
                    page_step(e.n > 0 ? -1 : 1);
                break;
            }
            if (s_page_ok && s_page.screen == MENU_SCREEN_PAUSE)
                break;                  /* no list there: up / down do nothing */
            if (cls_mode()) {           /* class select: up = the class before, down = the next */
                for (int k = 0; k < 4 && k < (e.n < 0 ? -e.n : e.n); k++) {
                    fprintf(stderr, "[MOUSE] class: wheel %s\n", e.n > 0 ? "back" : "next");
                    cls_step(e.n > 0 ? -1 : 1);
                }
                break;
            }
            if (s_cy_ok)
                cyl_hold(-1);           /* the list follows the selection again */
            for (int k = 0; k < 4 && k < (e.n < 0 ? -e.n : e.n); k++)
                tap(e.n > 0 ? XBOX_KBM_DPAD_UP : XBOX_KBM_DPAD_DOWN);
            break;
        case EV_CANCEL:
            if (s_dragged)
                conker_MenuBoardsHold(-1.0f, 0);
            s_nk_down = -1;
            s_cy_down = -1;
            s_cy_drag = 0;
            s_cl_down = false;
            s_cl_drag = false;
            s_cl_press = 0;
            s_pl_down_key = 0;
            s_pg_down = -2;
            s_pg_drag = 0;
            s_pg_dragged = false;
            s_pa_press = 0;
            s_ch_hold = 0;
            s_ldown = false;
            s_dragged = false;
            s_hold = 0;
            s_press_arrow = 0;
            s_down_board = -1;
            break;
        }
    }
}

int menu_mouse_on(void)
{
    return s_on != 0;
}

void menu_mouse_pad_used(void)
{
    if (s_on) {
        InterlockedExchange(&s_keys_used, 1);
        InterlockedExchange(&s_cy_keys, 1);
    }
    if (s_on && s_shown) {
        InterlockedExchange(&s_shown, 0);
        post_apply();
    }
}

static float ease(float v, float target, float rate, float dt)
{
    float k = rate * dt;
    if (k > 1.0f)
        k = 1.0f;
    return v + (target - v) * k;
}

int menu_mouse_frame(unsigned w, unsigned h, int blocked)
{
    static LARGE_INTEGER last;
    LARGE_INTEGER now, freq;
    float dt, turn = 0.0f;
    int mode = controls_mode();
    bool want;

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart
                       : 0.0f;
    if (dt < 0.0f || dt > 0.25f)
        dt = 0.25f;
    last = now;
    s_now = GetTickCount64();
    s_w = (float)w;
    s_h = (float)h;

    want = !blocked && xbox_KbmConfigured()
           && strcmp(settings_get("menu_mouse"), "0") != 0
           && (mode == CONTROLS_MENU || mode == CONTROLS_MENU_GAME
               || mode == CONTROLS_PAUSE);
    if (want != (s_on != 0)) {
        release_all();
        InterlockedExchange(&s_shown, 0);
        InterlockedExchange(&s_moved, 0);
        InterlockedExchange(&s_on, want ? 1 : 0);
        fprintf(stderr, "[MOUSE] menus: pointer %s\n", want ? "the menus'"
                                                            : "the game's");
        post_apply();               /* before the toolkit's own (keys only) */
    }
    /* To-do 270: F2's "Show cursor for touch", at once. */
    {
        LONG tc = strcmp(settings_get("touch_cursor"), "0") != 0;
        if (InterlockedExchange(&s_touch_cursor, tc) != tc && s_on && s_touch)
            post_apply();
    }
    if (s_spots_debug < 0) {
        const char *e = getenv("RECOMP_MENU_SPOTS");
        s_spots_debug = e && *e == '1';
    }
    s_boards = s_on && conker_MenuBoards(&turn);
    s_cam_ok = s_boards && conker_MenuCamera(s_cam_r, &s_cam_p00, &s_cam_p11);
    s_page_ok = s_on && !s_boards && conker_MenuPageSnap(&s_page);
    s_cy_ok = s_on && !s_boards && !s_page_ok && conker_MenuCylSnap(&s_cy);
    s_pl_ok = false;
    s_pl.popup = 0;
    if (s_on && !s_boards && !s_page_ok) {
        s_pl_ok = conker_MenuPlateSnap(&s_pl) != 0;
        if (s_pl.popup)
            s_cy_ok = false;            /* a popup over the list: the list takes no mouse */
    }
    /* The name keyboard over everything behind it (the profile list, its prompts). */
    s_nk_ok = s_on && !s_boards
              && conker_NameKbSnap(&s_nk_row, &s_nk_col, &s_nk_page, &s_nk_shift);
    if (s_nk_ok)
        s_page_ok = s_cy_ok = s_pl_ok = false;
    {
        static int was = -1;
        int now_pg = !s_page_ok ? -1 : s_page.screen == MENU_SCREEN_PAUSE
                   ? 100 + s_page.pause_state : s_page.board;
        if (now_pg != was && s_on) {
            if (now_pg >= 100)
                fprintf(stderr, "[MOUSE] page: pause %d\n", now_pg - 100);
            else
                fprintf(stderr, "[MOUSE] page: %s%d\n", now_pg >= 0 ? "board " : "none ", now_pg);
            was = now_pg;
        }
    }
    {
        /* The chapter's name can miss a frame in its cross-fade: the
         * arrows beside it stay where they were. */
        static MenuBox name_last;
        if (s_page_ok && s_page.chapter >= 0) {
            if (s_page.chapter_name.ok)
                name_last = s_page.chapter_name;
            else
                s_page.chapter_name = name_last;
        } else {
            name_last.ok = 0;
        }
    }
    events_frame();
    if (!s_on) {
        AcquireSRWLockExclusive(&s_lock);
        s_qn = 0;
        ReleaseSRWLockExclusive(&s_lock);
    }
    if (!s_boards && s_dragged) {       /* a board opened or the menu left */
        conker_MenuBoardsHold(-1.0f, 0);
        s_dragged = false;
    }
    fling_frame(dt);

    ULONGLONG idle = (ULONGLONG)(DWORD)((DWORD)GetTickCount() - (DWORD)s_move_tick);
    page_frame(dt, idle);
    cyl_frame(dt, idle);
    plate_frame();
    nk_frame();
    s_pg_moved = false;

    int over = s_shown && s_boards ? arrow_at(s_px, s_py) : 0;
    s_over_board = s_shown && s_boards && !over && !s_dragged ? board_at(s_px, s_py) : -1;
    bool page_over = (s_page_ok && (s_pa_over || s_pg_over >= 0 || s_pg_over_pr >= 0))
                     || (s_cy_ok && (s_cy_over >= 0 || s_cya_over)) || s_pl_over >= 0
                     || (cls_mode() && s_cl_over && !s_cl_drag)
                     || (s_nk_ok && s_nk_over >= 0);
    /* The prompt under the pointer: its words glow (the game side eases it). */
    conker_MenuGlow(s_on && s_shown && s_page_ok && s_pg_over_pr >= 0 && !s_pg_dragged
                    ? s_page.prompt[s_pg_over_pr].text
                    : s_on && s_shown && s_pl_over >= 0 && s_cy_drag != 1
                    ? s_pl.p[s_pl_over].key : 0u);
    bool hand = s_on && s_shown && ((s_boards && !s_dragged && (over || s_over_board >= 0))
                                    || (page_over && !s_pg_dragged && s_cy_drag != 1));

    /* The titles: dim while the pointer shows, the game's own on its board. */
    {
        bool mouse = s_boards && s_shown && s_moved;
        for (int b = 0; b < 6; b++) {
            float want = !mouse || s_over_board == b ? 1.0f : 0.0f;
            float &lit = s_title_lit[b];
            lit = ease(lit, want, want > lit ? 10.0f : 6.0f, dt);
            if (fabsf(lit - want) < 0.01f)
                lit = want;
            conker_MenuTitleAlpha(b, s_boards ? TITLE_DIM + (1.0f - TITLE_DIM) * lit : 1.0f);
        }
    }
    if (hand != (s_hand != 0)) {
        InterlockedExchange(&s_hand, hand ? 1 : 0);
        post_apply();
    }
    /* Still too long: the pointer goes (not while on an arrow, a board's
     * title or spot, or held). */
    if (s_on && s_shown && !over && s_over_board < 0 && !page_over && !s_ldown
        && idle > IDLE_HIDE_MS) {
        InterlockedExchange(&s_shown, 0);
        post_apply();
    }

    taps_frame();
    turns_frame(turn);

    /* The arrows: with the mouse in use on the boards. */
    bool arrows = s_boards && s_shown && s_moved
                  && (idle < ARROW_IDLE_MS || over || s_ldown);
    s_fade = ease(s_fade, arrows ? 1.0f : 0.0f, arrows ? 9.0f : 4.0f, dt);
    if (!arrows && s_fade < 0.01f)
        s_fade = 0.0f;
    for (int r = 0; r < 2; r++) {
        bool lit = over == (r ? 1 : -1) || (s_hold && s_hold == (r ? 1 : -1));
        s_lit[r] = ease(s_lit[r], lit ? 1.0f : 0.0f, 12.0f, dt);
        s_pulse[r] -= dt * 5.0f;
        if (s_pulse[r] < 0.0f)
            s_pulse[r] = 0.0f;
    }
    return s_fade > 0.0f || s_pa_fade > 0.0f || s_cya_fade > 0.0f || s_cl_fade > 0.0f
           || (s_spots_debug > 0 && (s_boards || s_page_ok || s_cy_ok || s_pl_ok || s_nk_ok));
}

/* A chevron pointing right (dir +1) or left (-1), its points in px: one arm
 * thick, the other a hairline, as a broad pen draws it (the downstroke thick:
 * the right arrow's upper arm, the left arrow's lower one, so the left is the
 * right turned round), the cut ends flat, the whole leaning right like the
 * title font's italics. */
static int arrow_points(const ArrowBox &b, int dir, float scale, ImVec2 *p)
{
    const float H = b.hh * scale, W = H * ARROW_W;
    if (dir == 2 || dir == 3) {
        /* ▲ / ▼ (the Chapters page's parts): two even strokes, upright (a
         * thick and a thin one read as a tick turned this way). H runs
         * across, W up and down; ends cut flat. */
        float s = dir == 2 ? -1.0f : 1.0f;      /* the tip's side */
        float hx = H * 0.5f, hy = W * 0.5f, t = H * 0.17f, k = t * W / hx;
        float xs[6] = { -hx, -hx + t, 0.0f, hx - t, hx, 0.0f };
        float ys[6] = { -hy, -hy, hy - k, -hy, -hy, hy };
        for (int i = 0; i < 6; i++) {
            int j = s > 0.0f ? i : 5 - i;   /* flipped: the same winding (the fill needs it) */
            p[j] = ImVec2(b.cx + xs[i], b.cy + s * ys[i]);
        }
        return 6;
    }
    /* In a 0..1 box: A top end (left), B top end (right), C the tip, D bottom
     * end (right), E bottom end (left), F the inner corner where the arms'
     * inner edges meet (parallel to their outer edges). */
    float ax = 0.0f, ay = 0.0f, bx = 0.42f * W, by = 0.0f;
    float cx = W, cy = 0.5f * H, dx = 0.16f * W, dy = H, ex = 0.02f * W, ey = H;
    float ux = cx - bx, uy = cy - by;       /* upper outer edge */
    float lx = cx - dx, ly = cy - dy;       /* lower outer edge */
    /* A + t u = E + s l */
    float den = ux * ly - uy * lx;
    float t = ((ex - ax) * ly - (ey - ay) * lx) / den;
    float fx = ax + t * ux, fy = ay + t * uy;
    float xs[6] = { ax, bx, cx, dx, ex, fx }, ys[6] = { ay, by, cy, dy, ey, fy };
    const float lean = 0.15f;
    for (int i = 0; i < 6; i++) {
        float x = xs[i] - W * 0.5f, y = ys[i] - H * 0.5f;
        if (dir < 0) {
            x = -x;
            y = -y;
        }
        x -= y * lean;
        p[i] = ImVec2(b.cx + x, b.cy + y);
    }
    return 6;
}

static void arrow_fill(ImDrawList *dl, const ImVec2 *p, int n, float ox, float oy,
                       ImU32 col)
{
    ImVec2 q[6];
    for (int i = 0; i < n; i++)
        q[i] = ImVec2(p[i].x + ox, p[i].y + oy);
    dl->AddConcavePolyFilled(q, n, col);
}

/* One of our arrows (dir -1 ◄, 1 ►, 2 ▲, 3 ▼): fade 0..1, lit under the
 * pointer, pulse 1 -> 0 after a click. */
static void draw_chevron(ImDrawList *dl, const ArrowBox &b, int dir, float fade, float lit,
                         float pulse)
{
    float a = fade * (0.60f + 0.40f * lit);
    float scale = 1.0f + 0.06f * lit - 0.08f * pulse;
    ImVec2 p[6];
    int n = arrow_points(b, dir, scale, p);
    float H = b.hh * scale, V = dir >= 2 ? H * ARROW_W : H;  /* V: its height */

    /* A soft dark shadow for any background, then the warm glow. */
    arrow_fill(dl, p, n, H * 0.03f, H * 0.045f,
               IM_COL32(0, 0, 0, (int)(a * 0.40f * 255.0f)));
    for (int ring = 0; ring < 2; ring++) {
        float rad = H * (ring ? 0.075f : 0.04f);
        float ga = a * (0.05f + 0.07f * lit) * (ring ? 0.7f : 1.0f);
        for (int k = 0; k < 8; k++) {
            float ang = (float)k * 0.785398f;
            arrow_fill(dl, p, n, cosf(ang) * rad, sinf(ang) * rad,
                       IM_COL32(255, 205, 110, (int)(ga * 255.0f)));
        }
    }
    int v0 = dl->VtxBuffer.Size;
    arrow_fill(dl, p, n, 0.0f, 0.0f, IM_COL32(255, 255, 255, (int)(a * 255.0f)));
    int v1 = dl->VtxBuffer.Size;
    /* Ivory at the top to gold below, as the board titles; brighter lit. */
    ImU32 top = lit > 0.5f ? IM_COL32(255, 255, 236, 255) : IM_COL32(255, 248, 206, 255);
    ImU32 bot = lit > 0.5f ? IM_COL32(250, 222, 140, 255) : IM_COL32(232, 196, 112, 255);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1,
                                                  ImVec2(b.cx, b.cy - V * 0.5f),
                                                  ImVec2(b.cx, b.cy + V * 0.5f),
                                                  top, bot);
}

static void draw_arrow(ImDrawList *dl, int r)
{
    draw_chevron(dl, arrow_box(r), r ? 1 : -1, s_fade, s_lit[r], s_pulse[r]);
}

void menu_mouse_draw(unsigned w, unsigned h)
{
    (void)w;
    (void)h;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (s_spots_debug > 0 && s_boards) {
        /* RECOMP_MENU_SPOTS=1: each board's spot, and the title shown. */
        ImVec2 p[24];
        for (int b = 0; b < 6; b++) {
            int n = spot_screen(b, p);
            if (n)
                dl->AddPolyline(p, n, b == s_over_board ? IM_COL32(255, 255, 0, 255)
                                                        : IM_COL32(0, 255, 120, 255),
                                ImDrawFlags_Closed, 2.0f);
        }
        for (int b = 0; b < 6; b++) {
            float x0, y0, x1, y1, vis = title_rect(b, &x0, &y0, &x1, &y1);
            if (vis > 0.25f)
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                            b == s_over_board ? IM_COL32(255, 255, 0, 255)
                                              : IM_COL32(0, 200, 255, 255),
                            0.0f, 0, 2.0f);
        }
    }
    if (s_spots_debug > 0 && s_page_ok) {
        /* RECOMP_MENU_SPOTS=1 on a page: the texts the mouse knows. */
        auto rect = [&](const MenuBox &b, ImU32 c) {
            if (b.ok)
                dl->AddRect(ImVec2((b.x0 + 1.0f) * 0.5f * s_w, (1.0f - b.y1) * 0.5f * s_h),
                            ImVec2((b.x1 + 1.0f) * 0.5f * s_w, (1.0f - b.y0) * 0.5f * s_h),
                            c, 0.0f, 0, 2.0f);
        };
        for (int i = 0; i < s_page.n; i++) {
            const MenuPageItem &it = s_page.item[i];
            rect(it.label, i == s_page.cursor ? IM_COL32(255, 255, 0, 255)
                                              : IM_COL32(0, 255, 120, 255));
            rect(it.no, IM_COL32(255, 80, 80, 255));
            rect(it.yes, IM_COL32(80, 160, 255, 255));
        }
        rect(s_page.rows, IM_COL32(200, 120, 255, 255));
        for (int k = 0; k < s_page.nprompt; k++)
            rect(s_page.prompt[k].box, k == s_pg_over_pr ? IM_COL32(255, 255, 0, 255)
                                                         : IM_COL32(255, 160, 0, 255));
        rect(s_page.chapter_name, IM_COL32(0, 200, 255, 255));
    }
    if (s_spots_debug > 0 && s_cy_ok) {
        /* RECOMP_MENU_SPOTS=1 on a cylinder list: its outline, each row's
         * letters (the selection yellow, the row under the pointer white),
         * and the bracket's place. */
        ImVec2 p[2 * (MENU_CYL_COLS + 1)];
        int n = 0;
        for (int iu = 0; iu <= MENU_CYL_COLS; iu++)
            p[n++] = cyl_px((float)iu / MENU_CYL_COLS, 0.0f);
        for (int iu = MENU_CYL_COLS; iu >= 0; iu--)
            p[n++] = cyl_px((float)iu / MENU_CYL_COLS, 1.0f);
        dl->AddPolyline(p, n, IM_COL32(0, 200, 255, 255), ImDrawFlags_Closed, 2.0f);
        for (int i = 0; i < s_cy.n; i++) {
            const MenuCylRow &r = s_cy.row[i];
            if (!r.ok)
                continue;
            ImU32 col = i == s_cy_over ? IM_COL32(255, 255, 255, 255)
                      : i == s_cy.sel ? IM_COL32(255, 255, 0, 255)
                                      : r.picker ? IM_COL32(255, 120, 255, 255)
                                                 : IM_COL32(0, 255, 120, 255);
            ImVec2 q[18];
            int m = 0;
            for (int k = 0; k <= 8; k++)
                q[m++] = cyl_px(cyl_tu(r.x0 + (r.x1 - r.x0) * k / 8.0f), cyl_tv(r.y0));
            for (int k = 8; k >= 0; k--)
                q[m++] = cyl_px(cyl_tu(r.x0 + (r.x1 - r.x0) * k / 8.0f), cyl_tv(r.y1));
            dl->AddPolyline(q, m, col, ImDrawFlags_Closed, 2.0f);
        }
        if (s_cy.n > 0) {
            ImVec2 b0 = cyl_px(0.0f, s_cy.bracket), b1 = cyl_px(1.0f, s_cy.bracket);
            dl->AddLine(b0, b1, IM_COL32(255, 60, 60, 255), 2.0f);
        }
        if (cls_mode()) {               /* class select: the model's box */
            float x0, y0, x1, y1;
            cls_model(&x0, &y0, &x1, &y1);
            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                        s_cl_over == 3 ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 120, 255, 255),
                        0.0f, 0, 2.0f);
        }
    }
    if (s_spots_debug > 0 && s_pl_ok) {
        /* RECOMP_MENU_SPOTS=1: the prompts' words and pictures (thin) and
         * what takes the click (the hovered one yellow). */
        for (int k = 0; k < s_pl.n; k++) {
            const MenuPlatePrompt &p = s_pl.p[k];
            ImVec2 q[MENU_PLATE_PTS], o[MENU_PLATE_PTS];
            int n;
            if (p.n == 4) {
                for (int i = 0; i < 4; i++)
                    q[i] = plate_pt(p, i);
                dl->AddPolyline(q, 4, IM_COL32(255, 160, 0, 255), ImDrawFlags_Closed, 1.0f);
            }
            if (plate_px(k, o, &n))
                dl->AddPolyline(o, n, k == s_pl_over ? IM_COL32(255, 255, 0, 255)
                                                     : IM_COL32(255, 120, 0, 255),
                                ImDrawFlags_Closed, 2.0f);
        }
    }
    if (s_spots_debug > 0 && s_nk_ok) {
        /* RECOMP_MENU_SPOTS=1: every key the mouse knows (the game's
         * highlight yellow, the one under the pointer white). */
        float x0, y0, x1, y1;
        for (int row = 0; row <= 5; row++)
            for (int col = 0; col <= 10; col++)
                if (nk_box(row, col, &x0, &y0, &x1, &y1)) {
                    int k = row | col << 4;
                    dl->AddRect(ImVec2(x0 + 1.0f, y0 + 1.0f), ImVec2(x1 - 1.0f, y1 - 1.0f),
                                k == s_nk_over ? IM_COL32(255, 255, 255, 255)
                                : row == s_nk_row && col == s_nk_col ? IM_COL32(255, 255, 0, 255)
                                                                     : IM_COL32(0, 255, 120, 255),
                                0.0f, 0, 2.0f);
                }
    }
    if (s_pa_fade > 0.0f && s_page_ok) {
        static const int dir[4] = { -1, 1, 2, 3 };
        ArrowBox a[4];
        bool vis[4];
        page_arrows(a, vis, NULL);
        for (int k = 0; k < 4; k++)
            if (s_pa_vis[k] > 0.01f && a[k].hh > 0.0f)
                draw_chevron(dl, a[k], dir[k], s_pa_fade * s_pa_vis[k], s_pa_lit[k],
                             s_pa_pulse[k]);
    }
    if (s_cya_fade > 0.0f && s_cy_ok) {
        ArrowBox a[2];
        if (cyl_arrows(s_cya_row, a))
            for (int k = 0; k < 2; k++)
                draw_chevron(dl, a[k], k ? 1 : -1, s_cya_fade, s_cya_lit[k], s_cya_pulse[k]);
    }
    if (s_cl_fade > 0.0f && cls_mode()) {
        ArrowBox a[2];
        cls_arrows(a);
        for (int k = 0; k < 2; k++)
            draw_chevron(dl, a[k], k ? 1 : -1, s_cl_fade, s_cl_lit[k], s_cl_pulse[k]);
    }
    if (s_fade <= 0.0f)
        return;
    draw_arrow(dl, 0);
    draw_arrow(dl, 1);
}
