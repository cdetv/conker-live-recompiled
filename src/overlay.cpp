/**
 * The F2 settings menu (SPEC 10.1), drawn with Dear ImGui over the game.
 *
 * Two toolkit hooks carry it (fork branch feat/overlay-hooks):
 *
 * - the window's message hook (window thread) turns F2 (menu), the pause
 *   key (P), photo mode (F4; both keys from F3's photo table) and F1 (FPS
 *   counter) on and off and, while the menu is open, takes the keyboard and
 *   mouse and queues them;
 * - the D3D11 back end's overlay hook (executor thread) draws the menu into
 *   the back buffer before each Present. The game keeps running under the
 *   menu. P pauses it, menu or not: the hook asks the back end to hold the
 *   flip, the executor keeps presenting the same frame (with the menu or a
 *   "Paused" label on it) and the game soon waits for the GPU. All ImGui
 *   and D3D11 work happens on the executor thread; the window thread only
 *   fills a locked queue.
 *
 * The pad follows the menu's focus: while the menu has it (on opening, or
 * after a click on it) the pad drives the menu (read here with XInput) and
 * the toolkit blocks it for the game; a click outside the menu gives the
 * pad back to the game, once every button is let go. During a P pause or
 * photo mode the sound stops too unless settings.ini says pause_silence = 0.
 * Photo mode (photo_mode.c) keeps the pad blocked; its corner note (main
 * keys, the last photo's outcome) is drawn here like the pause label, and
 * so is its photo panel (Tab / Start, a third menu, MENU_PHOTO). A photo is
 * the back buffer at the top of the hook, before any ImGui, written as a
 * PNG by a thread of its own (take_shot, write_shot: WIC). The panel's
 * folder buttons run the Windows folder dialog and Explorer on a thread of
 * their own too (folder_job), so the picture keeps drawing.
 *
 * Back + LB on the pad (Back + Black on an Xbox pad) opens F2 as the F2 key
 * does (pad_opens_menu, polled at each present while no menu is up).
 *
 * Report Bug (to-do 12, SPEC 9.2; F10 = F3's report_key, caught in the
 * window hook with its key-up, or F2's "Report Bug..." button) is a menu of
 * its own, MENU_REPORT: the picture of that moment grabbed as a photo is,
 * the game frozen as under P, a description and Create Report, the zip
 * written by bug_report.c on a thread of its own, then Open Folder / Open
 * GitHub Issue (build_report and the jobs above it).
 *
 * The menu is a small window, top left by default, that can be moved by
 * its title bar and resized by its edges; where it is goes to settings.ini
 * (menu_window, as parts of the picture) once the mouse lets go.
 *
 * "Close game" at the bottom asks first, then closes the window as its X
 * button does.
 *
 * F3 opens the controls menu instead (one menu at a time; F2 and F3 switch
 * between them): keyboard and mouse on or off, and for single player and
 * multiplayer each the mouse sensitivity and one key per action
 * (controls.c). Clicking an action's key waits for the next key, mouse
 * button or wheel notch; every key is taken then, Esc too, so the question
 * has its own buttons for no key and cancel. Saved and handed to the
 * toolkit at once. While the game is paused, or a menu is open and has the
 * focus, the keyboard and mouse are not the game's (xbox_KbmSetActive); a
 * click outside an open menu gives the keys back to the game, as the pad,
 * but not the mouse (xbox_KbmSetKeysOnly): the pointer stays free for the
 * menu.
 *
 * The FPS counter (F1 steps off / fps / fps + frame times; not in the
 * menu; remembered as fps_counter in settings.ini) sits in the top left
 * corner: the
 * game's own presents per second, then also the average time between them
 * and the longest, over half a second. A held frame
 * (P) is not a present; the count starts again after one. Drawn by ImGui
 * after the filter chain, so never filtered.
 *
 * Every change is written to settings.ini at once. The display mode, the
 * render resolution and render scale (xbox_SetRenderRes,
 * xbox_SetRenderScale; the back end switches at its next present; the
 * menu shows the size drawn at, xbox_RenderScaleSize), the upscaler
 * (xbox_SetUpscaler), texture filtering (xbox_SetTextureFilter, from the
 * next draw) and the Picture options (anti-aliasing, the sliders and
 * CRT / scanlines: xbox_SetAntiAlias, xbox_SetPicture, xbox_SetSharpen and
 * xbox_SetCrt, the toolkit's
 * filter chain; from the next present, also while paused; the glow slider,
 * conker_SetGlow, scales the game's own glow from its next frame, and
 * cutscene blur, conker_SetCutsceneBlur, likewise; shadow quality,
 * conker_SetShadowRes in shadows.c, from each shadow's next draw; the
 * frame rate, the dip fix, grass detail, fog, screen haze, vignette and
 * screen drops, conker_SetFrameRate, conker_SetDipFix, conker_SetGrassFull,
 * conker_SetFog, conker_SetUnderwaterFog, conker_SetScreenHaze, conker_SetVignette, conker_SetScreenDrops,
 * conker_SetUnderwaterOverlay, conker_SetPooFix and
 * conker_SetMainMenuSpeed, conker_SetSubmenuSpeed, conker_SetCutsceneSkip
 * and conker_SetSkipHold
 * in recomp_manual.c, from the
 * game's next frame; uncensored text, decensor_set, from the next
 * level that loads) also
 * apply at once; the rest
 * apply at the next start and say so. A slider is saved once let go, not
 * at every step of a drag. The menu's text and spacing follow the picture's height, so a new
 * resolution rebuilds the style and puts the window back where
 * menu_window says. The menu needs the GPU back end: with RECOMP_GPU=sw
 * nothing draws it and F2 does nothing.
 */

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <xinput.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "imgui.h"
#include "imgui_internal.h"         /* ClearActiveID: Report Bug's Esc */
#include "backends/imgui_impl_dx11.h"

#include "settings.h"
#include "controls.h"
#include "photo_mode.h"
#include "decensor.h"
#include "precompile.h"
#include "bug_report.h"

extern "C" {
void xbox_KbmSetActive(int on);
void xbox_KbmSetKeysOnly(int on);
typedef int (*xbox_GpuOverlayFn)(void *dev, void *ctx, void *back_rtv,
                                 unsigned w, unsigned h);
void xbox_SetGpuOverlay(xbox_GpuOverlayFn fn);
typedef int (*xbox_WindowMessageHook)(void *hwnd, unsigned msg, uintptr_t w,
                                      intptr_t l, intptr_t *result);
void xbox_SetWindowMessageHook(xbox_WindowMessageHook hook);
void xbox_FramebufferWindowSetDisplayMode(int mode);
int xbox_FramebufferWindowGpuViewRect(int *x, int *y, int *w, int *h);
void xbox_InputSetBlocked(int on);
int xbox_InputFirstHostPad(void);
DWORD xbox_InputHostPadRead(DWORD slot, XINPUT_STATE *st);
#define XBOX_INPUT_HOST_PADS 8      /* xinput_xbox.h: 4 XInput, 4 PlayStation */
void xbox_InputSetBackgroundPads(int on);
void xbox_ApuSetPaused(int on);
int xbox_SetRenderRes(unsigned w, unsigned h);
int xbox_SetTextureFilter(int mode);
int xbox_SetPicture(float gamma, float contrast, float saturation);
int xbox_SetSharpen(float percent);
int xbox_SetAntiAlias(int mode);
int xbox_SetRenderScale(int percent);
int xbox_RenderScaleSize(unsigned *w, unsigned *h);
int xbox_SetUpscaler(int mode);
int xbox_SetVsync(int mode);
int xbox_SetCrt(int mode, float strength);
void conker_SetGlow(float pct);
void conker_SetSoundChannel(int ch, float pct, int mute);
void conker_SetSoundSolo(int ch);
int conker_SoundSolo(void);
void conker_SetMasterVolume(float pct);
void conker_SetMasterMute(int on);
void conker_SetCutsceneVolume(float pct);
void conker_SetCutsceneBlur(int mode);
void conker_SetShadowRes(unsigned factor);
void conker_SetShadowSoftness(float pct);
void conker_SetSmoothLightmaps(int on);
void conker_SetFrameRate(int fps);
void conker_SetDipFix(int on);
void conker_SetMainMenuSpeed(int on);
void conker_SetSubmenuSpeed(int on);
void conker_SetPhys30(int on);
void conker_SetCutsceneSkip(int on);
void conker_SetSkipHold(int on);
float conker_SkipRing(void);
float conker_SkipCover(void);
float conker_LoadCover(void);
int conker_MainMenuBoard(void);
void conker_SetLoadingScreen(int on);
void conker_SetFastLoads(int on);
int xbox_FramebufferWindowIsFullscreen(void);
int xbox_FramebufferWindowPlacement(int *x, int *y, int *w, int *h,
                                    int *maximized);
void conker_SetGrassFull(int on);
void conker_SetPopinPart(int part, int on);
void conker_SetParticleDensity(float percent);
void conker_SetFog(int on);
void conker_SetUnderwaterFog(int on);
void conker_SetScreenHaze(int on);
void conker_SetVignette(int on);
void conker_SetScreenDrops(int on);
void conker_SetUnderwaterOverlay(int on);
void conker_SetPooFix(int on);
void conker_StartSoak(void);
int conker_SoakState(void);
void overlay_init(void);
}

/* ---- shared between the window thread and the executor -------------------- */

static volatile LONG s_gpu_seen;    /* the overlay hook has run: F2 can work */
static volatile LONG s_place_dirty; /* the game window was moved or resized */
/* Which menu: none, F2's settings, F3's controls or photo mode's panel. */
/* MENU_QUIT: only the quit question, from B / Esc on the main menu's
 * boards (to-do 28). */
/* MENU_REPORT: Report Bug's window (to-do 12; F10 or F2's button). */
enum { MENU_NONE, MENU_SETTINGS, MENU_CONTROLS, MENU_PHOTO, MENU_QUIT,
       MENU_REPORT };
static volatile LONG s_want_open;   /* F2/F3 state; the executor follows it */
static volatile LONG s_want_pause;  /* the pause key's state (P; F4 before
                                     * photo mode), likewise */
static volatile LONG s_pause_asked; /* GetTickCount when it was asked */
static volatile LONG s_want_fps;    /* F1 state (0-2, fps_counter), likewise */
static volatile LONG s_keys_to_game; /* a menu is up without the focus: keys
                                      * pass on to the game (executor sets) */
static HWND volatile s_hwnd;        /* the frame window */
/* Setting a key in F3 (executor; the window hook leaves the pause and
 * photo keys alone meanwhile): this set's action, else -1. */
static int s_capture_set = -1;
static volatile int s_capture_idx = -1;
static volatile LONG s_bb_w = 640, s_bb_h = 480;   /* back buffer size */
/* Report Bug (to-do 12): a picture of this moment wanted (taken at the next
 * present, before any ImGui); the window was opened by its key (the
 * description box then takes the typing); a report is being written (the
 * window cannot close). */
static volatile LONG s_report_shot_want;
static volatile LONG s_report_by_key;
static volatile LONG s_report_busy;

/* Input for ImGui, queued on the window thread. Mouse positions are already
 * in back-buffer pixels. A full queue drops events: nobody types 256 keys
 * in one 16 ms frame. */
struct Event {
    UINT msg;
    WPARAM w;
    LPARAM l;
    float x, y;
};
static CRITICAL_SECTION s_lock;
static Event s_queue[256];
static unsigned s_queue_n;

static void push_event(UINT msg, WPARAM w, LPARAM l, float x, float y)
{
    EnterCriticalSection(&s_lock);
    if (s_queue_n < sizeof(s_queue) / sizeof(s_queue[0]))
        s_queue[s_queue_n++] = Event{msg, w, l, x, y};
    LeaveCriticalSection(&s_lock);
}

/* A mouse position in the frame window's client area, in back-buffer
 * pixels: the swap chain sits on a child view, stretched to it. */
static void to_back_buffer(LPARAM l, float *x, float *y)
{
    int vx, vy, vw, vh;
    float mx = (float)(short)LOWORD(l), my = (float)(short)HIWORD(l);
    if (!xbox_FramebufferWindowGpuViewRect(&vx, &vy, &vw, &vh)) {
        *x = mx;
        *y = my;
        return;
    }
    *x = (mx - vx) * (float)s_bb_w / (float)vw;
    *y = (my - vy) * (float)s_bb_h / (float)vh;
}

/* ---- the pause key -------------------------------------------------------- */

/* To-do 117 (user 2026-11-06): the pause key freezes the game as photo
 * mode does (photo_mode.c enter_pause: the game's own pause flag and sound
 * pause, nothing to catch up on afterwards), only where photo mode can
 * freeze. The old pause held the flip: the game stopped at its next frame,
 * then caught up on up to 1 s of time when it resumed, a jump of a few
 * frames, and a cutscene's sound fell behind its pictures. old_pause (F2
 * Debug, --debug only) brings it back: it pauses everywhere. */
static bool old_pause_on(void)
{
    return settings_debug() && strcmp(settings_get("old_pause"), "0") != 0;
}

static void want_pause(bool on)
{
    if (on)
        InterlockedExchange(&s_pause_asked, (LONG)GetTickCount());
    InterlockedExchange(&s_want_pause, on ? 1 : 0);
}

extern "C" int overlay_pause_wanted(unsigned *age_ms)
{
    if (!s_want_pause || old_pause_on())
        return 0;
    *age_ms = (unsigned)(GetTickCount() - (DWORD)s_pause_asked);
    return 1;
}

extern "C" void overlay_pause_refused(void)
{
    InterlockedExchange(&s_want_pause, 0);
}

/* ---- window thread ---------------------------------------------------------- */

/* No new picture for this long = Report Bug's key means the game froze. */
#define FREEZE_MS 2000
static volatile LONG64 s_last_present;  /* GetTickCount64 at the last hook call */
static bool s_report_key_down;          /* the last press was taken by the hook */
extern "C" void conker_freeze_report(unsigned stale_ms);    /* main.c */

/* A press less than FREEZE_MS after the last picture asks the next picture for
 * the window. If none comes, nothing would ever draw it: this waits for the
 * window to open, and otherwise makes it the freeze path. So any press works,
 * however soon after the picture stopped (user's try 2026-11-08). */
static volatile LONG s_report_watching;
static bool report_window_up(void);

static DWORD WINAPI report_watch_job(void *)
{
    for (;;) {
        ULONGLONG ago;
        Sleep(100);
        if (report_window_up())
            break;                      /* it is up: the game is alive */
        if (s_want_open != MENU_REPORT)
            break;                      /* closed again by the key */
        ago = GetTickCount64() - (ULONGLONG)s_last_present;
        if (ago >= FREEZE_MS) {
            InterlockedCompareExchange(&s_want_open, MENU_NONE, MENU_REPORT);
            conker_freeze_report((unsigned)ago);
            break;
        }
    }
    InterlockedExchange(&s_report_watching, 0);
    return 0;
}

/* Report Bug's key (window thread): opens the window with a picture of
 * this moment, from any menu; closes it again unless a report is being
 * written. */
static void report_key(void)
{
    if (s_want_open == MENU_REPORT) {
        if (!s_report_busy)
            InterlockedCompareExchange(&s_want_open, MENU_NONE, MENU_REPORT);
        return;
    }
    InterlockedExchange(&s_report_by_key, 1);
    InterlockedExchange(&s_report_shot_want, 1);
    InterlockedExchange(&s_want_open, MENU_REPORT);
    if (!InterlockedExchange(&s_report_watching, 1)) {
        HANDLE t = CreateThread(NULL, 0, report_watch_job, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            InterlockedExchange(&s_report_watching, 0);
    }
}

static int window_hook(void *hwnd, unsigned m, uintptr_t w, intptr_t l,
                       intptr_t *result)
{
    float x = 0, y = 0;

    s_hwnd = (HWND)hwnd;   /* for Close game */
    /* --precompile-shaders (precompile.c): no menus and no keys for the
     * game; Esc stops the pass, as closing the window does (the result
     * file says so). F11, Alt+Enter and Alt+F4 stay the window's. */
    if (precompile_on()) {
        *result = 0;
        if (m == WM_CLOSE)
            precompile_stop();
        if (m == WM_KEYDOWN && w == VK_ESCAPE) {
            precompile_stop();
            PostMessageA((HWND)hwnd, WM_CLOSE, 0, 0);
            return 1;
        }
        if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR)
            && w != VK_F11)
            return 1;
        return 0;
    }
    /* To-do 29: the window moved or resized (or maximized) by the player,
     * not full screen's own changes; saved at the next frame. */
    if (m == WM_EXITSIZEMOVE
        || (m == WM_SIZE && (w == SIZE_MAXIMIZED || w == SIZE_RESTORED)
            && !xbox_FramebufferWindowIsFullscreen()))
        InterlockedExchange(&s_place_dirty, 1);
    /* Report Bug's key with no new picture for a while (SPEC 9.2 "Freeze"):
     * nothing would draw a window of ours (and maybe no picture yet at all),
     * so the small one of its own, with where the threads are in the log. */
    {
        ULONGLONG ago = GetTickCount64() - (ULONGLONG)s_last_present;
        int rk = controls_report_key();
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && rk > 0 && (int)w == rk
            && !(HIWORD(l) & KF_ALTDOWN) && ago >= FREEZE_MS) {
            *result = 0;
            s_report_key_down = true;
            if (!(l & (1L << 30)))
                conker_freeze_report((unsigned)ago);
            return 1;
        }
    }
    if (!s_gpu_seen)
        return 0;
    *result = 0;
    /* F2 the settings, F3 the controls; the open one's key closes it. Not
     * over Report Bug's window: it is closed by its own buttons. */
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && (w == VK_F2 || w == VK_F3)) {
        LONG which = w == VK_F2 ? MENU_SETTINGS : MENU_CONTROLS;
        if (!(l & (1L << 30)) && s_want_open != MENU_REPORT)  /* not repeat */
            InterlockedExchange(&s_want_open,
                                s_want_open == which ? MENU_NONE : which);
        return 1;
    }
    /* Report Bug's key (F10 by default, F3's photo table; to-do 12). F10
     * is Windows' menu key: it comes as WM_SYSKEYDOWN, and DefWindowProc
     * would turn its key-up into the window's menu loop (SC_KEYMENU), which
     * takes the keys that follow; so the key-up of a press taken here is
     * taken too. Not while a key is being set (the press is the answer),
     * and with the report window up a key that types a character is the
     * description's. */
    {
        int rk = controls_report_key();
        if ((m == WM_KEYUP || m == WM_SYSKEYUP) && s_report_key_down
            && (int)w == rk) {
            s_report_key_down = false;
            return 1;
        }
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && rk > 0 && (int)w == rk
            && !(HIWORD(l) & KF_ALTDOWN)
            && !(s_want_open && s_capture_idx >= 0)
            && !(s_want_open == MENU_REPORT
                 && MapVirtualKeyA((UINT)w, MAPVK_VK_TO_CHAR) != 0)) {
            s_report_key_down = true;
            if (!(l & (1L << 30)))
                report_key();
            return 1;
        }
    }
    /* The pause key (P) and the photo mode key (F4), from F3's photo
     * table, unless a key is being set (the press is the answer) or Report
     * Bug's window is up (they are letters to type there). Not
     * WM_SYSKEYDOWN: Alt+F4 still closes the window. */
    if (m == WM_KEYDOWN && !(s_want_open && s_capture_idx >= 0)
        && s_want_open != MENU_REPORT
        && ((int)w == controls_pause_key()
            || (int)w == controls_photo_toggle_key())) {
        if (!(l & (1L << 30))) {
            if ((int)w == controls_pause_key())
                want_pause(s_want_pause == 0);
            else
                photo_request_toggle();
        }
        return 1;
    }
    /* Esc on the main menu's boards: the quit question (to-do 28). The
     * game never sees this Esc (there it would be B, which does nothing). */
    if (m == WM_KEYDOWN && w == VK_ESCAPE && !s_want_open && !s_want_pause
        && !photo_active() && conker_MainMenuBoard()) {
        if (!(l & (1L << 30)))
            InterlockedCompareExchange(&s_want_open, MENU_QUIT, MENU_NONE);
        return 1;
    }
    /* Off, fps, fps and frame times, off again. */
    if (m == WM_KEYDOWN && w == VK_F1) {
        if (!(l & (1L << 30)))
            InterlockedExchange(&s_want_fps, (s_want_fps + 1) % 3);
        return 1;
    }
    /* Photo mode's zoom: the wheel kept from the game's keys. */
    if (m == WM_MOUSEWHEEL && !s_want_open && photo_active()) {
        photo_wheel(GET_WHEEL_DELTA_WPARAM(w));
        return 1;
    }
    if (!s_want_open)
        return 0;

    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        push_event(m, w, l, 0, 0);
        /* F11, Alt+Enter and Alt+F4 still belong to the window, and every
         * key does when the game has the keys (a click outside the menu). */
        return !(w == VK_F11 || m == WM_SYSKEYDOWN || s_keys_to_game);
    case WM_KEYUP:
    case WM_SYSKEYUP:
        /* Passed on as well, so a key held when the menu opened is not left
         * down in the game's keyboard state. */
        push_event(m, w, l, 0, 0);
        return 0;
    case WM_CHAR:
        push_event(m, w, l, 0, 0);
        return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        to_back_buffer(l, &x, &y);
        push_event(m, w, l, x, y);
        return 1;
    case WM_MOUSEWHEEL:
        push_event(m, w, l, 0, 0);
        return 1;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        /* For a key being set in the controls menu. */
        push_event(m, w, l, 0, 0);
        *result = TRUE;
        return 1;
    case WM_SETCURSOR:
        /* Full screen hides the pointer; the menu wants it. */
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            *result = TRUE;
            return 1;
        }
        return 0;
    case WM_KILLFOCUS:
        push_event(m, w, l, 0, 0);
        return 0;
    }
    return 0;
}

/* ---- executor thread ------------------------------------------------------- */

static bool s_ready;          /* ImGui and its D3D11 objects made */
static bool s_open;           /* a menu is showing */
static int s_menu;            /* which one (MENU_SETTINGS, MENU_CONTROLS) */

static bool report_window_up(void)
{
    return s_open && s_menu == MENU_REPORT;
}
static int s_capture_got = -1;  /* the input pressed for it, once pressed */
static bool s_capture_hover;  /* the mouse was over a button last frame:
                               * a click there is for the button */
static char s_capture_msg[96];  /* why the last key was not taken */
static int s_ask_reset = -1;  /* "Reset all ...?" is up for this set */
static bool s_paused;         /* the pause key: the flip is held */
static bool s_apu_held;       /* the sound is paused with it */
static bool s_pad_menu;       /* the pad drives the menu, not the game */
static bool s_popup_open;     /* a list or question was open last frame */
static bool s_item_active;    /* a slider was held or typed in last frame:
                               * B and Esc are its, not the menu's */
static bool s_ask_quit;       /* "Do you want to close the game?" is up */
static bool s_report_unfocus; /* Report Bug: Esc / B in the description: the
                               * box lets go of the keys, its text kept */
static bool s_focus;          /* give the menu the focus next frame */
static WORD s_pad_prev;       /* pad buttons last frame, for presses */
static WORD s_pad_latch;      /* held at opening, kept from ImGui until let go */
static int s_tab_step;        /* F2's tabs: LB (-1) or RB (+1) this frame */
static int s_tab_cur;         /* the F2 tab shown */
static LARGE_INTEGER s_last_frame;
static char s_error[160];     /* last save failure, shown in the menu */
static ImVec2 s_win_pos, s_win_size;   /* the menu's window last frame */
static bool s_win_dragged;    /* moved or resized with the mouse down */
static unsigned s_style_h;    /* picture height the style was made for */
static bool s_replace[5];     /* put a menu window back from settings.ini */
static bool s_held;           /* the last call held the flip: this one
                               * presents the same frame again */
static int s_fps;             /* F1: 0 off, 1 fps, 2 fps and frame times */
static bool s_fps_restart;    /* start counting afresh at the next present */
static LARGE_INTEGER s_fps_prev, s_fps_t0;
static unsigned s_fps_n;      /* presents since s_fps_t0 */
static double s_fps_max;      /* longest of them, ms */
static bool s_fps_have;       /* the three below are made */
static double s_fps_rate, s_fps_ms, s_fps_long;
static float s_fps_bottom;    /* the label's lower edge, for the pause label */

static ImGuiKey key_from_vk(WPARAM vk)
{
    switch (vk) {
    case VK_TAB:    return ImGuiKey_Tab;
    case VK_LEFT:   return ImGuiKey_LeftArrow;
    case VK_RIGHT:  return ImGuiKey_RightArrow;
    case VK_UP:     return ImGuiKey_UpArrow;
    case VK_DOWN:   return ImGuiKey_DownArrow;
    case VK_PRIOR:  return ImGuiKey_PageUp;
    case VK_NEXT:   return ImGuiKey_PageDown;
    case VK_HOME:   return ImGuiKey_Home;
    case VK_END:    return ImGuiKey_End;
    case VK_SPACE:  return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_BACK:   return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_SHIFT:  return ImGuiMod_Shift;
    case VK_CONTROL:return ImGuiMod_Ctrl;
    case VK_MENU:   return ImGuiMod_Alt;
    }
    /* Letters and digits: Ctrl+A / C / V / X / Z in a text box (Report
     * Bug's description). */
    if (vk >= 'A' && vk <= 'Z')
        return (ImGuiKey)(ImGuiKey_A + (int)(vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return (ImGuiKey)(ImGuiKey_0 + (int)(vk - '0'));
    return ImGuiKey_None;
}

/* The first connected pad, as the toolkit picks player 1: an Xbox pad
 * (XInput) or a PlayStation pad (SDL3), in XInput's layout either way. */
/* Slot 1's controller only (user 2026-11-07: no other controller may reach
 * the menus, so none can quit the game through them): the host pad behind
 * controller port 0, whatever F3 Slots gave it. Before, the first connected
 * host pad. False with no controller on slot 1 (the menus then take the
 * keyboard and mouse only). */
static bool read_pad(XINPUT_STATE *st)
{
    int pad = controls_pad_of_port(0);
    if (pad >= 0 && xbox_InputHostPadRead((DWORD)pad, st) == ERROR_SUCCESS)
        return true;
    memset(st, 0, sizeof(*st));
    return false;
}

/* Text about 1/50 of the picture's height, whatever the render size: the
 * back buffer is stretched to the window, so this is the size on screen
 * too (1/34 at first; the user found the menu too big). Made afresh for
 * each height: ScaleAllSizes multiplies what is there. Between frames, so
 * the next NewFrame takes the new font size. */
static void set_style(unsigned h)
{
    float px = (float)h / 50.0f;
    ImGuiStyle style;                 /* the dark colours */

    if (px < 14.0f)
        px = 14.0f;
    style.FontSizeBase = px;
    style.ScaleAllSizes(px / 13.0f);
    style.WindowRounding = px * 0.4f;
    style.FrameRounding = px * 0.2f;
    style.Colors[ImGuiCol_WindowBg].w = 0.75f;   /* the game shows through */
    ImGui::GetStyle() = style;
    s_style_h = h;
    fprintf(stderr, "[OVERLAY] text %.0f px for a picture %u tall\n", px, h);
}

static bool init_imgui(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                       unsigned h)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;            /* no imgui.ini next to the .exe */
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                    | ImGuiConfigFlags_NavEnableGamepad;
    /* A drag in the window's body is for its widgets, not for moving it. */
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.BackendPlatformName = "conker_overlay";

    io.Fonts->AddFontDefaultVector();
    set_style(h);

    if (!ImGui_ImplDX11_Init(dev, ctx)) {
        fprintf(stderr, "[OVERLAY] ImGui D3D11 back end failed; F2 is off\n");
        ImGui::DestroyContext();
        return false;
    }
    fprintf(stderr, "[OVERLAY] ready (ImGui %s)\n", IMGUI_VERSION);
    return true;
}

static void report_begin(void);
static void report_end(void);

static void open_menu(void)
{
    XINPUT_STATE pad;
    read_pad(&pad);
    s_pad_prev = pad.Gamepad.wButtons;   /* a held button is not a press */
    s_pad_latch = pad.Gamepad.wButtons;  /* nor a key down for ImGui */
    xbox_InputSetBlocked(1);
    s_pad_menu = true;
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    s_error[0] = '\0';
    s_focus = true;
    s_open = true;
    s_menu = (int)s_want_open;
    s_ask_quit = s_menu == MENU_QUIT;
    s_capture_idx = -1;
    s_ask_reset = -1;
    fprintf(stderr, "[OVERLAY] opened%s\n",
            s_menu == MENU_CONTROLS ? " (controls)"
            : s_menu == MENU_PHOTO ? " (photo panel)"
            : s_menu == MENU_QUIT ? " (quit question)"
            : s_menu == MENU_REPORT ? " (Report Bug)" : "");
    if (s_menu == MENU_REPORT)
        report_begin();
}

/* F3 with F2's menu open, or the other way round: the other menu, as if
 * just opened, the pad staying where it is. */
static void switch_menu(void)
{
    if (s_menu == MENU_REPORT)
        report_end();
    s_menu = (int)s_want_open;
    s_error[0] = '\0';
    s_focus = true;
    s_ask_quit = s_menu == MENU_QUIT;
    s_capture_idx = -1;
    s_ask_reset = -1;
    fprintf(stderr, "[OVERLAY] switched to the %s menu\n",
            s_menu == MENU_CONTROLS ? "controls"
            : s_menu == MENU_PHOTO ? "photo"
            : s_menu == MENU_QUIT ? "quit question"
            : s_menu == MENU_REPORT ? "Report Bug" : "settings");
    if (s_menu == MENU_REPORT)
        report_begin();
}

static void close_menu(void)
{
    if (s_open && s_menu == MENU_REPORT)
        report_end();
    s_open = false;
    s_capture_idx = -1;
    InterlockedExchange(&s_want_open, MENU_NONE);
    if (s_pad_menu)
        xbox_InputSetBlocked(photo_frozen());   /* photo mode keeps it */
    s_pad_menu = false;
    fprintf(stderr, "[OVERLAY] closed\n");
}

/* The pad to the menu while it has the focus, else to the game. Buttons
 * held at the switch count as pressed for neither: feed_pad keeps track of
 * them, and the toolkit waits for them to be let go. */
static void pad_to_menu(bool on)
{
    if (on == s_pad_menu)
        return;
    s_pad_menu = on;
    xbox_InputSetBlocked(on || photo_frozen());   /* photo mode keeps it */
    fprintf(stderr, "[OVERLAY] pad to the %s\n", on ? "menu" : "game");
}

/* For photo mode (photo_mode.c, game thread): a menu is up, so the pad and
 * keys are the menu's; the menu has the pad (blocked for the game). */
extern "C" int overlay_menu_open(void)
{
    return s_want_open != 0;
}

extern "C" int overlay_pad_menu(void)
{
    return s_pad_menu ? 1 : 0;
}

/* Photo mode's panel key (game thread): opens unless a menu is up. */
extern "C" void overlay_photo_panel(void)
{
    InterlockedCompareExchange(&s_want_open, MENU_PHOTO, MENU_NONE);
}

static volatile LONG s_shot_want;   /* take a photo at the next present */

extern "C" void overlay_photo_shot(void)
{
    InterlockedExchange(&s_shot_want, 1);
}

/* The pause key, and whether the sound waits with it (pause_silence, which the menu
 * can change during the pause). */
static void follow_pause(void)
{
    /* The flip is held only for the old pause (debug); the new one is a
     * freeze of the game thread's (photo_frozen). */
    bool paused = s_want_pause != 0 && old_pause_on();
    bool hold_apu = (paused || (photo_frozen() && !photo_world_running()))
                    && strcmp(settings_get("pause_silence"), "0") != 0;

    if (paused != s_paused) {
        s_paused = paused;
        fprintf(stderr, "[OVERLAY] game %s\n", paused ? "paused" : "resumed");
    }
    if (hold_apu != s_apu_held) {
        s_apu_held = hold_apu;
        xbox_ApuSetPaused(hold_apu);
    }
}

static void save(const char *key, const char *value);
static void follow_photo_picture(void);

/* F1: the counter's level, into settings.ini. Counting starts afresh when
 * it comes on. */
static void follow_fps(void)
{
    static const char *const names[] = { "off", "fps", "fps + frame times" };
    int level = (int)s_want_fps;
    char v[2] = { (char)('0' + level), '\0' };

    if (level == s_fps)
        return;
    if (!s_fps) {
        s_fps_restart = true;
        s_fps_have = false;
    }
    s_fps = level;
    if (strcmp(settings_get("fps_counter"), v))
        save("fps_counter", v);
    fprintf(stderr, "[OVERLAY] fps counter: %s\n", names[level]);
}

/* Once per call of the hook: fresh is a game present, not a held frame
 * shown again. Every half second the numbers are made from what was
 * counted. */
static void fps_tick(bool fresh)
{
    LARGE_INTEGER now, freq;
    double ms, span;

    if (!s_fps)
        return;
    if (!fresh) {
        s_fps_restart = true;     /* the pause is no frame of the game's */
        return;
    }
    QueryPerformanceCounter(&now);
    if (s_fps_restart) {
        s_fps_restart = false;
        s_fps_prev = s_fps_t0 = now;
        s_fps_n = 0;
        s_fps_max = 0.0;
        return;
    }
    QueryPerformanceFrequency(&freq);
    ms = (double)(now.QuadPart - s_fps_prev.QuadPart) * 1000.0
       / (double)freq.QuadPart;
    s_fps_prev = now;
    s_fps_n++;
    if (ms > s_fps_max)
        s_fps_max = ms;
    span = (double)(now.QuadPart - s_fps_t0.QuadPart) * 1000.0
         / (double)freq.QuadPart;
    if (span < 500.0)
        return;
    s_fps_rate = s_fps_n * 1000.0 / span;
    s_fps_ms = span / s_fps_n;
    s_fps_long = s_fps_max;
    s_fps_have = true;
    s_fps_t0 = now;
    s_fps_n = 0;
    s_fps_max = 0.0;
}

/* Queued keyboard and mouse into ImGui. Escape with no list open closes. */
static void drain_events(bool *close)
{
    Event ev[256];
    unsigned n;
    ImGuiIO &io = ImGui::GetIO();

    EnterCriticalSection(&s_lock);
    n = s_queue_n;
    memcpy(ev, s_queue, n * sizeof(Event));
    s_queue_n = 0;
    LeaveCriticalSection(&s_lock);

    for (unsigned i = 0; i < n; i++) {
        const Event &e = ev[i];
        bool down = true;
        /* Setting a key: the first key, mouse button or wheel notch is it.
         * Keys go no further (Space or Enter would also press the
         * question's button); the mouse does, so its buttons work. */
        if (s_capture_idx >= 0 && s_capture_got < 0) {
            int got = -1;
            if ((e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN)
                && !(e.l & (1L << 30))) {
                if (e.w == VK_F9 || e.w == VK_F11)
                    snprintf(s_capture_msg, sizeof(s_capture_msg),
                             "F1-F3, F9 and F11 are the port's own keys");
                else if (!controls_key_name((int)e.w)[0])
                    snprintf(s_capture_msg, sizeof(s_capture_msg),
                             "That key cannot be used (key code 0x%02X)",
                             (unsigned)e.w);
                else
                    got = (int)e.w;
            } else if (!s_capture_hover) {
                switch (e.msg) {
                case WM_LBUTTONDOWN: got = VK_LBUTTON; break;
                case WM_RBUTTONDOWN: got = VK_RBUTTON; break;
                case WM_MBUTTONDOWN: got = VK_MBUTTON; break;
                case WM_XBUTTONDOWN:
                    got = HIWORD(e.w) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
                    break;
                case WM_MOUSEWHEEL:
                    got = GET_WHEEL_DELTA_WPARAM(e.w) > 0 ? 256 : 257;
                    break;
                }
            }
            if (got >= 0) {
                s_capture_got = got;
                continue;           /* the press is the key, nothing else */
            }
            if (e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN
                || e.msg == WM_KEYUP || e.msg == WM_SYSKEYUP
                || e.msg == WM_CHAR)
                continue;
        }
        switch (e.msg) {
        case WM_KEYUP:
        case WM_SYSKEYUP:
            down = false;
            /* fall through */
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            ImGuiKey k = key_from_vk(e.w);
            if (e.w == VK_ESCAPE && down && !s_popup_open && !s_item_active
                && s_capture_idx < 0)
                *close = true;
            /* Report Bug: Esc in the description leaves the box with the
             * text kept; ImGui never gets it (its Esc throws away what was
             * typed since the box was clicked). */
            if (s_menu == MENU_REPORT && e.w == VK_ESCAPE) {
                if (down && s_item_active)
                    s_report_unfocus = true;
                break;
            }
            /* The photo panel's own key closes it too (Tab: not ImGui's). */
            if (s_menu == MENU_PHOTO && (int)e.w
                    == controls_key(CONTROLS_PHOTO, PHOTO_PANEL)) {
                if (down && !(e.l & (1L << 30)) && !s_item_active)
                    *close = true;
                break;
            }
            if (k != ImGuiKey_None)
                io.AddKeyEvent(k, down);
            break;
        }
        case WM_CHAR: {
            /* The game window is an ANSI one: a character past ASCII comes
             * in the ANSI code page (é is 0xE9 either way, € is not). */
            unsigned c = (unsigned)e.w;
            if (c >= 0x80 && c < 0x100) {
                char b = (char)c;
                wchar_t wc;
                if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, &b, 1,
                                        &wc, 1) == 1)
                    c = wc;
            }
            io.AddInputCharacter(c);
            break;
        }
        case WM_MOUSEMOVE:
            io.AddMousePosEvent(e.x, e.y);
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(0, e.msg == WM_LBUTTONDOWN);
            break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(1, e.msg == WM_RBUTTONDOWN);
            break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(2, e.msg == WM_MBUTTONDOWN);
            break;
        case WM_MOUSEWHEEL:
            io.AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(e.w)
                                        / (float)WHEEL_DELTA);
            break;
        case WM_KILLFOCUS:
            io.AddFocusEvent(false);
            break;
        }
    }
}

/* The game window's own size and place to settings.ini (window_rect, to-do
 * 29) once the player has moved or resized it: only a placement that
 * differs from the one the game started with (or last saved), so a window
 * nobody touched leaves the line empty. Executor thread, each present. */
static void remember_game_window(void)
{
    static char last[64];
    static bool have_last;
    int x, y, w, h, mx;
    char v[64];

    if (!have_last) {
        if (!xbox_FramebufferWindowPlacement(&x, &y, &w, &h, &mx))
            return;
        snprintf(last, sizeof(last), "%d,%d,%d,%d%s", x, y, w, h,
                 mx ? ",max" : "");
        have_last = true;
        InterlockedExchange(&s_place_dirty, 0);
        return;
    }
    if (!InterlockedExchange(&s_place_dirty, 0))
        return;
    if (!xbox_FramebufferWindowPlacement(&x, &y, &w, &h, &mx))
        return;
    snprintf(v, sizeof(v), "%d,%d,%d,%d%s", x, y, w, h, mx ? ",max" : "");
    if (!strcmp(v, last))
        return;
    strcpy_s(last, sizeof(last), v);
    save("window_rect", v);
    fprintf(stderr, "[OVERLAY] window_rect = %s\n", v);
}

/* Back + LB (Back + Black on an Xbox pad) with no menu up opens F2's menu
 * as the F2 key does (user 2026-10-29); Back then closes it (feed_pad).
 * Executor thread, each present. With no pad, looked for once a second:
 * XInput is slow on empty slots. */
static void pad_opens_menu(void)
{
    static WORD prev;
    static DWORD retry_at;
    const WORD combo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_SHOULDER;
    XINPUT_STATE st;

    if (retry_at && GetTickCount() - retry_at > 0x80000000u)
        return;                     /* not yet */
    if (!read_pad(&st)) {
        retry_at = GetTickCount() + 1000;
        if (!retry_at)
            retry_at = 1;
        prev = 0;
        return;
    }
    retry_at = 0;
    WORD b = st.Gamepad.wButtons;
    if (!s_want_open && (b & combo) == combo && (prev & combo) != combo) {
        InterlockedCompareExchange(&s_want_open, MENU_SETTINGS, MENU_NONE);
        fprintf(stderr, "[OVERLAY] Back + LB: settings menu\n");
    }
    /* B on the main menu's boards: the quit question (to-do 28). Slot 1's
     * controller only (read_pad; user 2026-11-07); the keyboard has Esc. */
    else if (!s_want_open && (b & XINPUT_GAMEPAD_B)
             && !(prev & XINPUT_GAMEPAD_B) && !s_paused && !photo_active()
             && conker_MainMenuBoard()) {
        InterlockedCompareExchange(&s_want_open, MENU_QUIT, MENU_NONE);
        fprintf(stderr, "[OVERLAY] B on the main menu: quit question\n");
    }
    prev = b;
}

/* The pad into ImGui's gamepad keys, as imgui_impl_win32 maps it. Start,
 * Back, or B with no list open close the menu. While the pad is the
 * game's, the menu sees it at rest. */
static void feed_pad(bool *close)
{
    ImGuiIO &io = ImGui::GetIO();
    XINPUT_STATE st;
    bool have = read_pad(&st);
    WORD pressed = (WORD)(st.Gamepad.wButtons & ~s_pad_prev);

    if (have)
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    s_pad_prev = st.Gamepad.wButtons;
    /* A button held when the menu opened reaches ImGui only once let go
     * and pressed again: ImGui took the held B that opened the quit
     * question as a press of its No, and the question closed at once
     * (user's DualShock 4, 2026-11-07). */
    s_pad_latch &= st.Gamepad.wButtons;
    st.Gamepad.wButtons &= (WORD)~s_pad_latch;
    if (!s_pad_menu) {
        memset(&st, 0, sizeof(st));
        pressed = 0;
    }
    const XINPUT_GAMEPAD &g = st.Gamepad;
    /* Not with the Close game question up: it wants an answer. */
    if ((pressed & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) && !s_ask_quit)
        *close = true;
    if ((pressed & XINPUT_GAMEPAD_B) && !s_popup_open && !s_item_active)
        *close = true;
    /* Report Bug: B in the description leaves the box, the text kept (as
     * Esc there); ImGui never gets B (its cancel throws the text away). */
    if (s_menu == MENU_REPORT) {
        if ((pressed & XINPUT_GAMEPAD_B) && s_item_active)
            s_report_unfocus = true;
        st.Gamepad.wButtons &= (WORD)~XINPUT_GAMEPAD_B;
    }
    /* LB / RB step through F2's tabs (to-do 11); not while a list, a
     * question or a held slider has the pad. */
    s_tab_step = 0;
    if (!s_popup_open && !s_item_active && !s_ask_quit)
        s_tab_step = (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1
                   : (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ? -1 : 0;

#define BTN(key, bit) io.AddKeyEvent(key, (g.wButtons & (bit)) != 0)
#define ANALOG(key, v, lo, hi) do { \
        float f = ((float)(v) - (lo)) / ((float)(hi) - (lo)); \
        f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; \
        io.AddKeyAnalogEvent(key, f > 0.10f, f); } while (0)
    BTN(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    BTN(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    BTN(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
    BTN(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
    BTN(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    BTN(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    BTN(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    BTN(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    BTN(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
    BTN(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    ANALOG(ImGuiKey_GamepadL2, g.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    ANALOG(ImGuiKey_GamepadR2, g.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    ANALOG(ImGuiKey_GamepadLStickLeft, g.sThumbLX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    ANALOG(ImGuiKey_GamepadLStickRight, g.sThumbLX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
    ANALOG(ImGuiKey_GamepadLStickUp, g.sThumbLY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
    ANALOG(ImGuiKey_GamepadLStickDown, g.sThumbLY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
#undef BTN
#undef ANALOG
}

/* ---- the menu -------------------------------------------------------------- */

/* Under an option: why its value is not the one in effect, if it is not. */
/* Grey text wrapped to the window, which may be narrow. */
static void note(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void option_note(const char *key, bool applies_now)
{
    const char *env = settings_env_override(key);
    if (env) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped("    %s is set, so this is not used", env);
        ImGui::PopStyleColor();
    } else if (!applies_now
               && strcmp(settings_get(key), settings_started(key))) {
        note("    applies when the game is started again");
    }
}

static void save(const char *key, const char *value)
{
    if (settings_set(key, value))
        s_error[0] = '\0';
    else
        snprintf(s_error, sizeof(s_error), "Could not save %s = %s "
                 "(see the log)", key, value);
}

/* --dump-settings (SPEC 9.2): the launcher's Settings tab is built from the
 * F2 menu itself, so the two never drift apart. overlay_dump_settings runs
 * build_menu once with no window, and while it does, the option helpers
 * (section, checkbox, choice, slider) also write each entry they show to
 * s_dump. s_dump_needs: what the entries being shown greyed out need
 * (begin_needs). */
static FILE *s_dump;
static bool s_dump_first;
static const char *s_dump_needs;
/* The single player scheme the entries being shown belong to (the launcher
 * shows them under that scheme only). */
static const char *s_dump_scheme;
/* The checkbox the entries being shown sit under (shown only while it is
 * on, indented). */
static const char *s_dump_parent;

/* Opens the entry's JSON object, with the fields every entry has; the
 * caller adds its own and closes it. False when no dump is running. */
static bool dump_item(const char *type, const char *label, const char *key)
{
    if (!s_dump)
        return false;
    fprintf(s_dump, "%s    { \"type\": \"%s\", \"label\": ",
            s_dump_first ? "" : ",\n", type);
    s_dump_first = false;
    settings_json_string(s_dump, label);
    if (key) {
        fputs(", \"key\": ", s_dump);
        settings_json_string(s_dump, key);
    }
    if (s_dump_needs)
        fprintf(s_dump, ", \"needs\": \"%s\"", s_dump_needs);
    if (s_dump_scheme)
        fprintf(s_dump, ", \"scheme\": \"%s\"", s_dump_scheme);
    if (s_dump_parent)
        fprintf(s_dump, ", \"parent\": \"%s\"", s_dump_parent);
    return true;
}

static void dump_list(const char *name, const char *const *list, int count)
{
    fprintf(s_dump, ", \"%s\": [", name);
    for (int i = 0; i < count; i++) {
        if (i)
            fputs(", ", s_dump);
        settings_json_string(s_dump, list[i]);
    }
    fputc(']', s_dump);
}

/* A heading over a group of options. */
static void section(const char *label)
{
    ImGui::SeparatorText(label);
    if (dump_item("section", label, NULL))
        fputs(" }", s_dump);
}

/* A plain line between groups of options (no heading). */
static void line(void)
{
    ImGui::Separator();
    if (dump_item("line", "", NULL))
        fputs(" }", s_dump);
}

/* Options greyed out unless what they need is there; the dump names it
 * (the launcher checks for it itself). */
static void begin_needs(const char *what, bool missing)
{
    ImGui::BeginDisabled(missing);
    s_dump_needs = what;
}

static void end_needs(void)
{
    ImGui::EndDisabled();
    s_dump_needs = NULL;
}

/* A note the launcher shows too (F3's explanations); note() alone for the
 * ones about the game's state now. */
static void info(const char *text)
{
    note(text);
    if (dump_item("note", text, NULL))
        fputs(" }", s_dump);
}

/* tip: shown with the mouse over the option or the pad's highlight on it. */
static void checkbox(const char *label, const char *key, bool applies_now,
                     const char *tip = NULL)
{
    bool on = strcmp(settings_get(key), "0") != 0;
    bool changed = ImGui::Checkbox(label, &on);
    if (tip)
        ImGui::SetItemTooltip("%s", tip);
    if (dump_item("checkbox", label, key)) {
        if (tip) {
            fputs(", \"tip\": ", s_dump);
            settings_json_string(s_dump, tip);
        }
        fputs(" }", s_dump);
    }
    if (changed) {
        save(key, on ? "1" : "0");
        if (!strcmp(key, "slowdown_fix") && !settings_env_override(key))
            conker_SetDipFix(on);
        if (!strcmp(key, "loading_screen") && !settings_env_override(key))
            conker_SetLoadingScreen(on);
        if (!strcmp(key, "fast_loads") && !settings_env_override(key))
            conker_SetFastLoads(on);
        if (!strncmp(key, "rumble", 6))
            controls_apply_pad_feel(key[6] - '1', -1.0f, -1.0f);
        if (!strcmp(key, "main_menu_speed") && !settings_env_override(key))
            conker_SetMainMenuSpeed(on);
        if (!strcmp(key, "submenu_speed") && !settings_env_override(key))
            conker_SetSubmenuSpeed(on);
        if (!strcmp(key, "physics_30hz") && !settings_env_override(key))
            conker_SetPhys30(on);
        if (!strcmp(key, "skippable_cutscenes") && !settings_env_override(key))
            conker_SetCutsceneSkip(on);
        if (!strcmp(key, "hold_to_skip") && !settings_env_override(key))
            conker_SetSkipHold(on);
        if (!strcmp(key, "grass_full_detail") && !settings_env_override(key))
            conker_SetGrassFull(on);
        if (!strncmp(key, "popin_", 6) && !settings_env_override(key))
            conker_SetPopinPart(!strcmp(key, "popin_grass") ? 0
                                : !strcmp(key, "popin_characters") ? 1
                                : !strcmp(key, "popin_fur") ? 2 : 3, on);
        if (!strcmp(key, "fog") && !settings_env_override(key))
            conker_SetFog(on);
        if (!strcmp(key, "underwater_fog") && !settings_env_override(key))
            conker_SetUnderwaterFog(on);
        if (!strcmp(key, "screen_haze") && !settings_env_override(key))
            conker_SetScreenHaze(on);
        if (!strcmp(key, "vignette") && !settings_env_override(key))
            conker_SetVignette(on);
        if (!strcmp(key, "screen_drops") && !settings_env_override(key))
            conker_SetScreenDrops(on);
        if (!strcmp(key, "underwater_overlay") && !settings_env_override(key))
            conker_SetUnderwaterOverlay(on);
        if (!strcmp(key, "poo_fix") && !settings_env_override(key))
            conker_SetPooFix(on);
        if (!strcmp(key, "smooth_world_shadows")
            && !settings_env_override(key))
            conker_SetSmoothLightmaps(on);
        /* Audio waits for the next start: the voice banks stay open. */
        if (!strcmp(key, "uncensored_text") && !settings_env_override(key))
            decensor_set(1, on);
        if (!strcmp(key, "control_out_of_focus") && !settings_env_override(key))
            xbox_InputSetBackgroundPads(on);
        if (!strcmp(key, "keyboard_mouse") || strstr(key, "_invert_y"))
            controls_apply();
    }
    option_note(key, applies_now);
}

/* render_resolution, just saved: to the runtime at once, unless the
 * environment decides it (the note under the option says so). */
static void apply_resolution(void)
{
    const char *v = settings_get("render_resolution");
    unsigned w, h;

    if (settings_env_override("render_resolution"))
        return;
    if (!settings_render_size(v, &w, &h)) {
        snprintf(s_error, sizeof(s_error), "Could not work out a size for "
                 "%s", v);
        return;
    }
    if (!xbox_SetRenderRes(w, h)) {
        snprintf(s_error, sizeof(s_error), "%ux%u is too small or too big",
                 w, h);
        return;
    }
    if (w)
        fprintf(stderr, "[OVERLAY] render resolution %s: %ux%u\n", v, w, h);
    else
        fprintf(stderr, "[OVERLAY] render resolution %s: the title's own\n",
                v);
}

/* texture_filter, just saved: to the renderer at once, unless the
 * environment decides it. */
static void apply_tex_filter(void)
{
    static const char *const names[] = {
        "off", "original", "aniso2", "aniso4", "aniso8", "aniso16",
    };
    static const int modes[] = { 0, 1, 2, 4, 8, 16 };
    const char *v = settings_get("texture_filter");

    if (settings_env_override("texture_filter"))
        return;
    for (int i = 0; i < 6; i++)
        if (!_stricmp(v, names[i])) {
            xbox_SetTextureFilter(modes[i]);
            return;
        }
}

/* render_scale, just saved: to the renderer at once, unless the environment
 * decides it. */
static void apply_render_scale(void)
{
    if (settings_env_override("render_scale"))
        return;
    if (!xbox_SetRenderScale(atoi(settings_get("render_scale"))))
        snprintf(s_error, sizeof(s_error), "Render scale %s %% not taken",
                 settings_get("render_scale"));
}

/* upscaler, just saved: to the filter chain at once, unless the
 * environment decides it. Values in xbox_SetUpscaler's order. */
static void apply_upscaler(void)
{
    if (!settings_env_override("upscaler"))
        xbox_SetUpscaler(_stricmp(settings_get("upscaler"), "fsr") ? 0 : 1);
}

/* vsync, just saved: to the present at once, unless the environment
 * decides it. Values in xbox_SetVsync's order. */
static void apply_vsync(void)
{
    static const char *const names[] = { "off", "on", "vrr" };
    const char *v = settings_get("vsync");

    if (settings_env_override("vsync"))
        return;
    for (int i = 0; i < 3; i++)
        if (!_stricmp(v, names[i]))
            xbox_SetVsync(i);
}

/* display_mode, just saved: to the window at once, unless the environment
 * decides it. Values in xbox_FramebufferWindowSetDisplayMode's order. */
static void apply_display_mode(void)
{
    static const char *const names[] = { "windowed", "borderless",
                                         "fullscreen" };
    const char *v = settings_get("display_mode");

    if (settings_env_override("display_mode"))
        return;
    for (int i = 0; i < 3; i++)
        if (!_stricmp(v, names[i]))
            xbox_FramebufferWindowSetDisplayMode(i);
}

/* antialiasing, just saved: to the filter chain at once, unless the
 * environment decides it. Values in xbox_SetAntiAlias's order. */
static void apply_aa(void)
{
    static const char *const names[] = { "off", "fxaa", "smaa" };
    const char *v = settings_get("antialiasing");

    if (settings_env_override("antialiasing"))
        return;
    for (int i = 0; i < 3; i++)
        if (!_stricmp(v, names[i])) {
            xbox_SetAntiAlias(i);
            return;
        }
}

/* crt, just saved: to the filter chain at once, unless the environment
 * decides it. Values in xbox_SetCrt's order; the strength is a slider's. */
static void apply_crt(void)
{
    static const char *const names[] = { "off", "scanlines", "crt" };
    const char *v = settings_get("crt");

    if (settings_env_override("crt"))
        return;
    for (int i = 0; i < 3; i++)
        if (!_stricmp(v, names[i])) {
            xbox_SetCrt(i, -1.0f);
            return;
        }
}

/* A drop-down over values[], shown as names[]. The current value is always
 * in the list, even one only reachable by editing the file. */
static void choice(const char *label, const char *key,
                   const char *const *values, const char *const *names,
                   int count, bool applies_now)
{
    const char *cur = settings_get(key);
    const char *preview = cur;
    bool listed = false;

    for (int i = 0; i < count; i++)
        if (!_stricmp(values[i], cur)) {
            preview = names[i];
            listed = true;
        }
    /* The name above, the list the window's full width: the window may be
     * narrow, and a name beside the list was cut off. */
    if (dump_item("choice", label, key)) {
        dump_list("values", values, count);
        dump_list("names", names, count);
        fputs(" }", s_dump);
    }
    char id[64];
    snprintf(id, sizeof(id), "##%s", key);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, preview)) {
        s_popup_open = true;
        for (int i = 0; i < count; i++) {
            bool sel = !_stricmp(values[i], cur);
            if (ImGui::Selectable(names[i], sel) && !sel) {
                save(key, values[i]);
                if (!s_error[0] && !strcmp(key, "render_resolution"))
                    apply_resolution();
                if (!s_error[0] && !strcmp(key, "render_scale"))
                    apply_render_scale();
                if (!s_error[0] && !strcmp(key, "upscaler"))
                    apply_upscaler();
                if (!s_error[0] && !strcmp(key, "vsync"))
                    apply_vsync();
                if (!s_error[0] && !strcmp(key, "display_mode"))
                    apply_display_mode();
                if (!s_error[0] && !strcmp(key, "texture_filter"))
                    apply_tex_filter();
                if (!s_error[0] && !strcmp(key, "antialiasing"))
                    apply_aa();
                if (!s_error[0] && !strcmp(key, "crt"))
                    apply_crt();
                if (!s_error[0] && !strcmp(key, "frame_rate")
                    && !settings_env_override(key))
                    conker_SetFrameRate(atoi(values[i]));
                if (!s_error[0] && !strcmp(key, "cutscene_blur")
                    && !settings_env_override(key))
                    conker_SetCutsceneBlur(i);
                if (!s_error[0] && !strcmp(key, "shadow_resolution")
                    && !settings_env_override(key))
                    conker_SetShadowRes((unsigned)atoi(values[i]));
                if (!s_error[0] && (!strcmp(key, "mp_zoom_mode")
                                    || !strcmp(key, "story_slingshot_aim"))) {
                    controls_apply();
                    fprintf(stderr, "[OVERLAY] %s = %s\n", key, values[i]);
                }
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        if (!listed)
            ImGui::Selectable(cur, true);
        ImGui::EndCombo();
    }
    option_note(key, applies_now);
}

/* The Picture sliders (toolkit filter chain). While one is moved its value
 * lives here, goes to the renderer every frame and is saved to settings.ini
 * once the slider is let go. Gamma, contrast and saturation first, in
 * xbox_SetPicture's order. */
struct Slide {
    const char *key;
    float v;
    bool dirty;                   /* moved, not saved yet */
};
static Slide s_slides[] = {
    { "gamma", 1.0f, false },
    { "contrast", 1.0f, false },
    { "saturation", 1.0f, false },
    { "sharpen", 0.0f, false },
    { "crt_strength", 50.0f, false },
    { "glow", 100.0f, false },
    { "story_mouse_x", 100.0f, false },
    { "story_mouse_y", 100.0f, false },
    { "mp_mouse_x", 100.0f, false },
    { "mp_mouse_y", 100.0f, false },
    { "particle_density", 15.0f, false },
    { "shadow_softness", 50.0f, false },
    { "rumble_strength1", 100.0f, false },
    { "rumble_strength2", 100.0f, false },
    { "rumble_strength3", 100.0f, false },
    { "rumble_strength4", 100.0f, false },
    { "stick_dead_zone1", 10.0f, false },
    { "stick_dead_zone2", 10.0f, false },
    { "stick_dead_zone3", 10.0f, false },
    { "stick_dead_zone4", 10.0f, false },
    /* Sound tab (to-do 13); the channels in conker_SetSoundChannel's order. */
    { "master_volume", 100.0f, false },
    { "music_volume", 100.0f, false },
    { "effects_volume", 100.0f, false },
    { "dialogue_volume", 100.0f, false },
    { "other_volume", 100.0f, false },
    { "cutscene_volume", 100.0f, false },
};

/* The sound channels' settings keys, in conker_SetSoundChannel's order. */
static const char *const s_sound_channels[] = {
    "music", "effects", "dialogue", "other",
};

/* "music_volume" / "music_mute" -> 0 (the channel), else -1. */
static int sound_channel_of(const char *key)
{
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(s_sound_channels[i]);
        if (!strncmp(key, s_sound_channels[i], n) && key[n] == '_')
            return i;
    }
    return -1;
}
#define NUM_SLIDES ((int)(sizeof(s_slides) / sizeof(s_slides[0])))

static Slide *slide_for(const char *key)
{
    for (int i = 0; i < NUM_SLIDES; i++)
        if (!strcmp(s_slides[i].key, key))
            return &s_slides[i];
    return NULL;
}

static float slide_value(const Slide *s)
{
    return s->dirty ? s->v : (float)atof(settings_get(s->key));
}

/* Every slider to the renderer at once; one the environment decides is
 * left as it is (-1 for xbox_SetPicture and xbox_SetCrt, not sent for
 * sharpen and glow). Glow is the game's own, scaled in recomp_manual.c,
 * from the next frame the game draws. */
static void apply_picture(void)
{
    float v[NUM_SLIDES];
    for (int i = 0; i < NUM_SLIDES; i++)
        v[i] = settings_env_override(s_slides[i].key)
             ? -1.0f : slide_value(&s_slides[i]);
    xbox_SetPicture(v[0], v[1], v[2]);
    if (v[3] >= 0.0f)
        xbox_SetSharpen(v[3]);
    xbox_SetCrt(-1, v[4]);
    if (v[5] >= 0.0f)
        conker_SetGlow(v[5]);
}

static bool reset_icon(const char *id, const char *tip);

/* A slider's value to where it is used: the picture's to the renderer
 * while it moves, the mouse's to the controls once saved (the game does
 * not get the mouse while the menu is open anyway). */
static void apply_slider(const char *key, bool saved)
{
    if (!strncmp(key, "story_", 6) || !strncmp(key, "mp_", 3)) {
        if (saved)
            controls_apply();
        return;
    }
    if (!strcmp(key, "particle_density")) {
        if (!settings_env_override(key))
            conker_SetParticleDensity(slide_value(slide_for(key)));
        return;
    }
    if (!strcmp(key, "shadow_softness")) {
        if (!settings_env_override(key))
            conker_SetShadowSoftness(slide_value(slide_for(key)));
        return;
    }
    /* Per slot (to-do 29): the live value while it moves. */
    if (!strncmp(key, "rumble_strength", 15)) {
        controls_apply_pad_feel(key[15] - '1', slide_value(slide_for(key)),
                                -1.0f);
        return;
    }
    if (!strncmp(key, "stick_dead_zone", 15)) {
        controls_apply_pad_feel(key[15] - '1', -1.0f,
                                slide_value(slide_for(key)));
        return;
    }
    /* Sound (to-do 13): live while it moves. */
    if (!strcmp(key, "master_volume")) {
        if (!settings_env_override(key))
            conker_SetMasterVolume(slide_value(slide_for(key)));
        return;
    }
    if (!strcmp(key, "cutscene_volume")) {
        if (!settings_env_override(key))
            conker_SetCutsceneVolume(slide_value(slide_for(key)));
        return;
    }
    if (sound_channel_of(key) >= 0) {
        if (!settings_env_override(key))
            conker_SetSoundChannel(sound_channel_of(key),
                                   slide_value(slide_for(key)), -1);
        return;
    }
    apply_picture();
}

/* The name above; the slider the window's full width but for its reset
 * icon (back to neutral, the original) at the end of the line. fmt shows
 * the value and rounds it as it shows it; tip is the icon's. mute: a sound
 * channel's mute key, a Mute box after the name (and a Solo box for testing
 * with --debug, solo_ch the channel; F2 only, not saved). */
static void slider(const char *label, const char *key, float lo, float hi,
                   const char *neutral, const char *fmt, const char *tip,
                   const char *mute = NULL, int solo_ch = -1)
{
    Slide *s = slide_for(key);
    float v = slide_value(s);
    float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    char id[64];

    if (dump_item("slider", label, key)) {
        /* fmt as decimals and a unit: "%.0f %%" is 0 and " %". */
        const char *f = strchr(fmt, '.'), *u = strchr(fmt, 'f');
        char unit[16] = "";
        if (u && !strncmp(u + 1, " %%", 3))
            strcpy_s(unit, sizeof(unit), " %");
        else if (u && !strncmp(u + 1, " dB", 3))
            strcpy_s(unit, sizeof(unit), " dB");
        fprintf(s_dump, ", \"min\": %g, \"max\": %g, \"neutral\": ", lo, hi);
        settings_json_string(s_dump, neutral);
        fprintf(s_dump, ", \"decimals\": %d, \"unit\": ", f ? atoi(f + 1) : 0);
        settings_json_string(s_dump, unit);
        fputs(", \"reset_tip\": ", s_dump);
        settings_json_string(s_dump, tip);
        if (mute) {
            fputs(", \"mute\": ", s_dump);
            settings_json_string(s_dump, mute);
        }
        fputs(" }", s_dump);
    }
    snprintf(id, sizeof(id), "##%s", key);
    /* User 2026-11-07: the Mute boxes in one column, each right above its
     * reset icon, the word before the box (Solo, --debug, before that). A
     * click on a word ticks its box too. */
    float left = ImGui::GetCursorPosX();
    float right = left + ImGui::GetContentRegionAvail().x;
    if (mute)
        ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (mute) {
        bool on = strcmp(settings_get(mute), "0") != 0;
        bool solo_shown = settings_debug() && solo_ch >= 0;
        float fh = ImGui::GetFrameHeight();
        float in = ImGui::GetStyle().ItemInnerSpacing.x;
        float sp = ImGui::GetFontSize();
        float mute_box = right - fh;
        float mute_txt = mute_box - in - ImGui::CalcTextSize("Mute").x;
        float solo_box = mute_txt - sp - fh;
        float solo_txt = solo_box - in - ImGui::CalcTextSize("Solo").x;
        float first = solo_shown ? solo_txt : mute_txt;
        float past = left + ImGui::CalcTextSize(label).x + sp - first;
        bool flip = false;
        if (past > 0.0f) {          /* a long name: after it instead */
            mute_box += past; mute_txt += past;
            solo_box += past; solo_txt += past;
        }
        if (solo_shown) {
            bool solo = conker_SoundSolo() == solo_ch;
            ImGui::SameLine(solo_txt);
            ImGui::TextUnformatted("Solo");
            flip = ImGui::IsItemClicked();
            ImGui::SameLine(solo_box);
            snprintf(id, sizeof(id), "##solo_%s", key);
            if (ImGui::Checkbox(id, &solo) || flip)
                conker_SetSoundSolo(flip ? (solo ? -1 : solo_ch)
                                         : (solo ? solo_ch : -1));
            ImGui::SetItemTooltip("Debug: only this channel heard (not "
                                  "saved), to find which sounds are which");
        }
        ImGui::SameLine(mute_txt);
        ImGui::TextUnformatted("Mute");
        flip = ImGui::IsItemClicked();
        ImGui::SameLine(mute_box);
        snprintf(id, sizeof(id), "##%s", mute);
        if (ImGui::Checkbox(id, &on) || flip) {
            if (flip)
                on = !on;
            save(mute, on ? "1" : "0");
            if (!settings_env_override(mute)) {
                if (!strcmp(mute, "master_mute"))
                    conker_SetMasterMute(on);
                else
                    conker_SetSoundChannel(sound_channel_of(mute), -1.0f,
                                           on);
            }
        }
        ImGui::SetItemTooltip("Mute %s", label);
        snprintf(id, sizeof(id), "##%s", key);
    }
    ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + gap));
    if (ImGui::SliderFloat(id, &v, lo, hi, fmt,
                           ImGuiSliderFlags_AlwaysClamp)) {
        s->v = v;
        s->dirty = true;
        apply_slider(key, false);
    }
    if (s->dirty && !ImGui::IsItemActive()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", s->v);
        s->dirty = false;
        save(key, buf);
        apply_slider(key, true);  /* what was saved, should saving fail */
    }
    ImGui::SameLine(0.0f, gap);
    snprintf(id, sizeof(id), "##reset_%s", key);
    if (reset_icon(id, tip)) {
        s->dirty = false;
        save(key, neutral);
        apply_slider(key, true);
        fprintf(stderr, "[OVERLAY] %s reset\n", key);
    }
    option_note(key, true);
}

/* A square button with a circular arrow drawn on it (the font has no
 * U+21BA): the arc runs most of the way round, the arrowhead at the top
 * points left, anticlockwise. True when pressed. */
static bool reset_icon(const char *id, const char *tip)
{
    float sz = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(sz, sz));
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 bg = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive
                                  : hot ? ImGuiCol_ButtonHovered
                                        : ImGuiCol_Button);
    ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    float r = sz * 0.27f, th = sz * 0.08f, h = sz * 0.13f;

    if (th < 1.5f)
        th = 1.5f;
    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), bg,
                      ImGui::GetStyle().FrameRounding);
    /* Angles grow clockwise on screen; -pi/2 is the top. From the top round
     * to the upper left, leaving a gap there. */
    const float pi = 3.14159265f;
    dl->PathArcTo(c, r, -pi * 0.5f + 0.15f, pi * 1.5f - 1.1f, 24);
    dl->PathStroke(fg, ImDrawFlags_None, th);
    dl->AddTriangleFilled(ImVec2(c.x - h * 1.2f, c.y - r),
                          ImVec2(c.x + h * 0.6f, c.y - r - h),
                          ImVec2(c.x + h * 0.6f, c.y - r + h), fg);
    if (hot)
        ImGui::SetItemTooltip("%s", tip);
    return pressed;
}

/* Inside Begin/End: after the mouse moved or resized the window and let
 * go, save where it is, as parts of the picture. Only mouse changes count,
 * not the fit of the first opening. */
static void remember_window(const char *key, unsigned w, unsigned h)
{
    ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    char v[64];

    if (down && (pos.x != s_win_pos.x || pos.y != s_win_pos.y
                 || size.x != s_win_size.x || size.y != s_win_size.y))
        s_win_dragged = true;
    s_win_pos = pos;
    s_win_size = size;
    if (!s_win_dragged || down)
        return;
    s_win_dragged = false;
    /* Partly off the picture is fine; the file wants 0 <= left/top < 1. */
    float fx = pos.x / w, fy = pos.y / h;
    fx = fx < 0.0f ? 0.0f : fx > 0.99f ? 0.99f : fx;
    fy = fy < 0.0f ? 0.0f : fy > 0.99f ? 0.99f : fy;
    snprintf(v, sizeof(v), "%.3f,%.3f,%.3f,%.3f", fx, fy,
             size.x / w, size.y / h);
    save(key, v);
}

/* Close game's question, a modal in the middle of the screen. Yes closes
 * the frame window, exactly as its X button or Alt+F4 does (the toolkit
 * then ends the process; files are already written). No, Esc or B go back
 * to the menu, which never closes under it (s_popup_open). s_ask_quit is
 * the state, not ImGui's popup stack: a question left open by F2 is gone
 * when the menu opens again. */
static void ask_quit(void)
{
    const char *id = "Close game";

    if (s_ask_quit && !ImGui::IsPopupOpen(id))
        ImGui::OpenPopup(id);
    if (!ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_AlwaysAutoResize
                                | ImGuiWindowFlags_NoSavedSettings))
        return;
    s_popup_open = true;
    if (s_ask_quit) {
        ImGui::TextUnformatted("Do you want to close the game?");
        if (ImGui::Button("Yes")) {
            fprintf(stderr, "[OVERLAY] Close game\n");
            fflush(stderr);
            PostMessageA(s_hwnd, WM_CLOSE, 0, 0);
        }
        ImGui::SameLine();
        bool no = ImGui::Button("No");
        /* No has the pad/keyboard cursor first: a stray A does not quit. */
        ImGui::SetItemDefaultFocus();
        if (no || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
                || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            s_ask_quit = false;
    }
    if (!s_ask_quit)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/* Before Begin: where the menu was left (settings.ini, key), else small at
 * the top left, wide (in text heights) and tall (part of the picture, 0 =
 * fitted to what it holds). On the first opening, and again after the
 * picture's size changed (the window is kept in pixels): otherwise ImGui
 * keeps the window where the mouse puts it. Focus only on opening:
 * focusing the window again every frame would close a drop-down list as
 * soon as it opened. */
static void place_window(int which, const char *key, unsigned w, unsigned h,
                         float wide, float tall)
{
    float px = ImGui::GetFontSize(), m = h * 0.02f;
    float rx, ry, rw, rh;
    ImGuiCond cond = s_replace[which] ? ImGuiCond_Always
                                      : ImGuiCond_FirstUseEver;
    s_replace[which] = false;
    if (sscanf(settings_get(key), "%f,%f,%f,%f", &rx, &ry, &rw, &rh) == 4) {
        ImGui::SetNextWindowPos(ImVec2(rx * w, ry * h), cond);
        ImGui::SetNextWindowSize(ImVec2(rw * w, rh * h), cond);
    } else {
        ImGui::SetNextWindowPos(ImVec2(m, m), cond);
        ImGui::SetNextWindowSize(ImVec2(px * wide, tall * h), cond);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(px * 12.0f, px * 5.0f),
                                        ImVec2((float)w, (float)h));
    if (s_focus) {
        ImGui::SetNextWindowFocus();
        s_focus = false;
    }
}

/* F2's tabs (to-do 11; layout user 2026-11-06): Display, Picture, Sound,
 * Fixes (what changes the game itself: the decensor patch, cutscene skips,
 * pop-in, menu speeds, Poo Fix). w and h: the picture's size; close: set
 * to close the menu. */
static void display_tab(unsigned w, unsigned h, bool *close)
{
    /* The picture takes the resolution's shape: there is no separate
     * widescreen option. */
    static const char *const res_values[] = {
        "auto", "auto-4:3", "native", "640x480",
        "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160",
        "1440x1080", "1920x1440",
    };
    static const char *const res_names[] = {
        "Auto (monitor shape, max 1440p)", "Auto 4:3 (max 1440p)",
        "Native (monitor size)", "640x480 (original Xbox)",
        "1280x720 (16:9)", "1600x900 (16:9)", "1920x1080 (16:9)",
        "2560x1440 (16:9)", "3840x2160 (16:9, slow)",
        "1440x1080 (4:3)", "1920x1440 (4:3)",
    };
    static const char *const filter_values[] = {
        "off", "original", "aniso2", "aniso4", "aniso8", "aniso16",
    };
    static const char *const filter_names[] = {
        "Off (sharpest, shimmers far away)", "Original (as on the Xbox)",
        "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x",
        "Anisotropic 16x (sharpest steady)",
    };
    /* As multiples (user 2026-11-07); settings.ini keeps the percent.
     * 5x-10x only with --debug (to-do 30). */
    static const char *const scale_values[] = {
        "50", "67", "75", "100", "125", "150", "200", "250", "300", "400",
        "500", "600", "700", "800", "900", "1000",
    };
    static const char *const scale_names[] = {
        "0.5x (fastest, softest)", "0.67x", "0.75x", "1x (default)",
        "1.25x", "1.5x", "2x", "2.5x", "3x", "4x (sharpest, slowest)",
        "5x (debug)", "6x (debug)", "7x (debug)", "8x (debug)", "9x (debug)",
        "10x (debug)",
    };
    static const char *const mode_values[] = {
        "windowed", "borderless", "fullscreen",
    };
    static const char *const mode_names[] = {
        "Windowed", "Borderless (instant Alt+Tab)",
        "Full screen (exclusive)",
    };
    static const char *const vsync_values[] = { "on", "off", "vrr" };
    static const char *const vsync_names[] = {
        "On (no tearing)", "Off (lowest delay, may tear)",
        "G-Sync / FreeSync (VRR)",
    };
    /* 144 and up wait for fixes (settings.c frame_rate); 240 and uncapped
     * are listed with --debug (to-do 101, user 2026-11-06). */
    static const char *const fps_values[] = {
        "30", "60", "90", "120", "240", "uncapped",
    };
    static const char *const fps_names[] = {
        "30 fps (original)", "60 fps", "90 fps", "120 fps",
        "240 fps (debug)", "Uncapped (debug)",
    };
    static const char *const up_values[] = { "bilinear", "fsr" };
    static const char *const up_names[] = {
        "Bilinear (plain, soft)", "AMD FSR 1 (sharper edges)",
    };
    /* RECOMP_LANGUAGE: the toolkit's answer to XGetLanguage, read once. */
    static const char *const lang_values[] = { "english", "french", "german",
                                               "italian", "spanish",
                                               "japanese" };
    static const char *const lang_names[] = { "English", "French", "German",
                                              "Italian", "Spanish",
                                              "Japanese" };
    (void)w; (void)h; (void)close;

    choice("Display mode", "display_mode", mode_values, mode_names,
           (int)(sizeof(mode_values) / sizeof(mode_values[0])), true);
    choice("Render resolution", "render_resolution", res_values, res_names,
           (int)(sizeof(res_values) / sizeof(res_values[0])), true);
    choice("Render scale", "render_scale", scale_values, scale_names,
           settings_debug() ? 16 : 10, true);
    {
        /* The size the game is drawn at, and when the toolkit capped it
         * (texture size, video memory), the multiple that is. */
        unsigned dw, dh;
        int pct = xbox_RenderScaleSize(&dw, &dh);
        char buf[160];
        if (pct < atoi(settings_get("render_scale")))
            snprintf(buf, sizeof(buf), "    Drawn at %ux%u: capped at %.2gx,"
                     " the largest the renderer and this graphics card take"
                     " here", dw, dh, pct / 100.0);
        else
            snprintf(buf, sizeof(buf), "    Drawn at %ux%u", dw, dh);
        note(buf);
    }
    choice("Upscaler (render scale below 1x)", "upscaler", up_values,
           up_names, (int)(sizeof(up_values) / sizeof(up_values[0])), true);
    choice("Texture filtering", "texture_filter", filter_values, filter_names,
           (int)(sizeof(filter_values) / sizeof(filter_values[0])), true);
    choice("Frame rate", "frame_rate", fps_values, fps_names,
           settings_debug() ? 6 : 4, true);
    /* The dip fix at 30 is on by default and a debug choice (user
     * 2026-10-22); 60 and above always have it. */
    if (settings_debug() && !strcmp(settings_get("frame_rate"), "30")) {
        checkbox("Keep speed in slowdowns (debug)", "slowdown_fix", true);
        note("    Off = original (runs fast when the frame rate dips)");
    }
    choice("VSync", "vsync", vsync_values, vsync_names,
           (int)(sizeof(vsync_values) / sizeof(vsync_values[0])), true);
    /* Was "Text and menus" under its own Language heading (to-do 11). */
    choice("Text language", "language", lang_values, lang_names,
           (int)(sizeof(lang_values) / sizeof(lang_values[0])), false);
    note("    Voices are English in every language");
}

static void picture_tab(unsigned w, unsigned h, bool *close)
{
    static const char *const aa_values[] = { "off", "fxaa", "smaa" };
    static const char *const aa_names[] = {
        "Off (original)", "FXAA (quick, slightly soft)",
        "SMAA (cleaner edges, sharper)",
    };
    static const char *const crt_values[] = { "off", "scanlines", "crt" };
    static const char *const crt_names[] = {
        "Off (original)", "Scanlines", "CRT (scanlines, curve, glow)",
    };
    /* atoi gives conker_SetShadowRes's factor ("original": 0 = 1x). */
    static const char *const shadow_values[] = { "original", "2x", "4x",
                                                 "8x", "16x" };
    static const char *const shadow_names[] = {
        "Original (soft, blocky)", "2x", "4x (sharper)", "8x (very sharp)",
        "16x (sharpest, more GPU)",
    };
    /* In conker_SetCutsceneBlur's order. */
    static const char *const dof_values[] = {
        "off", "original", "updated", "cinematic",
    };
    static const char *const dof_names[] = {
        "Off (always sharp)", "Original (blocky at high resolutions)",
        "Updated (original look, smooth)", "Cinematic (stronger, bokeh)",
    };
    (void)w; (void)close;

    choice("Anti-aliasing", "antialiasing", aa_values, aa_names,
           (int)(sizeof(aa_values) / sizeof(aa_values[0])), true);
    choice("Shadows", "shadow_resolution", shadow_values, shadow_names,
           (int)(sizeof(shadow_values) / sizeof(shadow_values[0])), true);
    /* To-do 105: the game's own shadows at original, nothing to soften. */
    ImGui::BeginDisabled(!_stricmp(settings_get("shadow_resolution"),
                                   "original"));
    slider("Shadow softness", "shadow_softness", 0.0f, 100.0f, "0",
           "%.0f %%", "Back to off (0 %: as sharp as Shadows draws them)");
    ImGui::EndDisabled();
    checkbox("Smooth world shadows", "smooth_world_shadows", true,
             "The shadows baked into the levels (fences, plants, buildings) "
             "read smoothly instead of as blocky steps. Off = as the "
             "original");
    {
        static const char *const orig = "Back to the original (1.00)";
        slider("Gamma", "gamma", 0.5f, 1.5f, "1", "%.2f", orig);
        slider("Contrast", "contrast", 0.5f, 1.5f, "1", "%.2f", orig);
        slider("Saturation", "saturation", 0.0f, 2.0f, "1", "%.2f", orig);
        slider("Sharpening", "sharpen", 0.0f, 100.0f, "0", "%.0f %%",
               "Back to off (0 %)");
        slider("Glow (the game's own)", "glow", 0.0f, 100.0f, "100",
               "%.0f %%", "Back to the original (100 %)");
    }
    checkbox("Fog", "fog", true,
             "The distance fog that fades far scenery into the level's "
             "colour. Off = far scenery as clear as near (the level's far "
             "end may show). Under water, Underwater Fog decides");
    checkbox("Underwater Fog", "underwater_fog", true,
             "The murky water colour while the camera is under water. Off "
             "= clear water");
    checkbox("Haze", "screen_haze", true,
             "The mist or colour wash in some levels. Off = a clear view "
             "(e.g. no green gas in Chemical Warfare)");
    checkbox("Vignette", "vignette", true,
             "The dark corners round the picture in some levels. Off = "
             "the picture as bright at the edges as in the middle");
    checkbox("Screen drops", "screen_drops", true,
             "Water and blood drops on the screen and the streaks they "
             "leave. Off = a clean screen");
    checkbox("Underwater Shimmer", "underwater_overlay", true,
             "The shimmering haze round Conker under water, which shows as "
             "a band at eye level. Off = a clear view under water");
    choice("Cutscene blur", "cutscene_blur", dof_values, dof_names,
           (int)(sizeof(dof_values) / sizeof(dof_values[0])), true);
    choice("CRT / scanlines", "crt", crt_values, crt_names,
           (int)(sizeof(crt_values) / sizeof(crt_values[0])), true);
    slider("CRT strength", "crt_strength", 0.0f, 100.0f, "50", "%.0f %%",
           "Back to the default (50 %)");
    if (_stricmp(settings_get("crt"), "off") && h < 960) {
        /* The toolkit fades them out below 2 pixels per line. */
        char buf[128];
        snprintf(buf, sizeof(buf), "    The picture is %u pixels tall: "
                 "scanlines show fully from 960", h);
        note(buf);
    }
}

static void sound_tab(unsigned w, unsigned h, bool *close)
{
    static const char *const mix_values[] = {
        "xemu", "speakers", "evenodd", "front",
    };
    static const char *const mix_names[] = {
        "xemu (default)", "speakers (wider stereo)", "evenodd (debug)",
        "front (debug)",
    };
    (void)w; (void)h; (void)close;

    /* To-do 13 (user 2026-11-07): master, a mute per channel, Solo only
     * with --debug, a cutscene volume for everything a cutscene plays. */
    {
        static const char *const full = "Back to full (100 %)";
        slider("Master volume", "master_volume", 0.0f, 100.0f, "100",
               "%.0f %%", full, "master_mute");
        slider("Music", "music_volume", 0.0f, 100.0f, "100", "%.0f %%", full,
               "music_mute", 0);
        slider("Effects", "effects_volume", 0.0f, 100.0f, "100", "%.0f %%",
               full, "effects_mute", 1);
        slider("Dialogue", "dialogue_volume", 0.0f, 100.0f, "100", "%.0f %%",
               full, "dialogue_mute", 2);
        slider("Other (ambience, menu sounds)", "other_volume", 0.0f, 100.0f,
               "100", "%.0f %%", full, "other_mute", 3);
        /* User 2026-11-07: percent, the original in the middle. */
        /* No note under it (user 2026-11-07); settings.ini explains it. */
        slider("Cutscene volume", "cutscene_volume", 0.0f, 200.0f, "100",
               "%.0f %%", "Back to the original (100 %)");
    }
    /* evenodd and front are for testing: listed only with --debug. */
    choice("Mix", "mix", mix_values, mix_names, settings_debug() ? 4 : 2,
           false);
    checkbox("3D sound (HRTF)", "hrtf", false);
    checkbox("Silence sound while paused", "pause_silence", true);
}

/* In the user's order (2026-11-06); a plain line ends the Cutscenes group,
 * so the options after it do not read as part of it. */
static void fixes_tab(unsigned w, unsigned h, bool *close)
{
    (void)w; (void)h;

    /* The decensor patch (decensor.c): greyed out without it. */
    section("Uncensored");
    bool have_audio = decensor_available(0) != 0;
    bool have_text = decensor_available(1) != 0;
    begin_needs("decensor", !have_audio);
    checkbox("Uncensored audio", "uncensored_audio", false,
             "Cutscene voices and songs without the bleeps (decensor "
             "patch). Changes when the game is started again");
    end_needs();
    begin_needs("decensor", !have_text);
    checkbox("Uncensored text", "uncensored_text", true,
             "Subtitles and other text without the censoring (decensor "
             "patch). Changes from the next level that loads");
    end_needs();
    if (!have_audio && !have_text)
        note("    Needs the decensor patch in a \"decensor\" folder next to "
             "the game (or paths.decensor_dir in settings.ini)");

    /* To-do 16 (user 2026-11-04): Hold to Skip shown while the first is
     * on; the dump always lists it, under its parent. */
    section("Cutscenes");
    checkbox("Skippable Cutscenes", "skippable_cutscenes", true,
             "Every story cutscene can be skipped, also the ones the game "
             "does not let you skip; a scene in parts skips one part at a "
             "time");
    if (s_dump || strcmp(settings_get("skippable_cutscenes"), "0")) {
        ImGui::Indent();
        s_dump_parent = "skippable_cutscenes";
        checkbox("Hold to Skip", "hold_to_skip", true,
                 "Scenes the game does not let you skip need the skip button "
                 "held for 3 seconds (off: a press skips them)");
        /* User 2026-11-04: a debug choice. */
        if (settings_debug())
            checkbox("Tail-shaped ring (debug)", "skip_ring_tail", true,
                     "The hold-to-skip ring drawn as Conker's curled-up tail");
        s_dump_parent = NULL;
        ImGui::Unindent();
    }

    line();
    checkbox("Pop-in Fix", "grass_full_detail", true,
             "Eliminates pop-in for elements such as grass, fur, and distant "
             "characters");
    /* Its parts, shown while it is on (user 2026-10-31). The dump always
     * lists them: the launcher shows and hides them by their parent. */
    if (s_dump || strcmp(settings_get("grass_full_detail"), "0")) {
        ImGui::Indent();
        s_dump_parent = "grass_full_detail";
        checkbox("Grass & flowers", "popin_grass", true,
                 "Grass keeps waving and flowers never thin out or vanish "
                 "with distance");
        checkbox("Characters", "popin_characters", true,
                 "Characters no longer fade out far from the camera");
        checkbox("Fur", "popin_fur", true,
                 "Fur stays full at any distance instead of thinning out");
        checkbox("Ambient Particles (experimental)", "popin_particles", true,
                 "Falling leaves, fluff and dust fill the whole area instead "
                 "of only a small box around the camera");
        if (s_dump || strcmp(settings_get("popin_particles"), "0")) {
            ImGui::Indent();
            s_dump_parent = "popin_particles";
            slider("Particle density", "particle_density", 0.0f, 100.0f,
                   "15", "%.0f %%", "Back to the default (15 %)");
            s_dump_parent = "grass_full_detail";
            ImGui::Unindent();
        }
        s_dump_parent = NULL;
        ImGui::Unindent();
    }
    checkbox("Main Menu Speed", "main_menu_speed", true,
             "Above 60 fps the main menu turns between its boards and zooms "
             "into them at the original speed (off: faster, twice as fast at "
             "120)");
    checkbox("Sub-menu Speed", "submenu_speed", true,
             "Above 60 fps game select and the chapter list fade at the "
             "original speed (off: faster, twice as fast at 120)");
    /* To-do 26 (Fixes tab: user 2026-11-07): the game's own loading
     * screen, which the port drew but never showed. */
    checkbox("Loading Screen", "loading_screen", true,
             "The game's loading screen while a level loads (off: black, "
             "then the level fades in). From the next load");
    /* To-do 118: the loads' wait for the old music's fade-out (off by
     * default, user 2026-11-07). */
    checkbox("Fast Load (experimental)", "fast_loads", true,
             "Loads don't wait for the old music to fade out, so a load from "
             "the main menu takes about half a second instead of 3 or more; "
             "the music stops at once when a load starts (off: the original "
             "wait). From the next load");
    /* It tints the Underwater Shimmer (Picture tab): greyed out while that
     * is off (it was shown under it before the tabs, user 2026-11-03). */
    begin_needs("underwater_overlay",
                !strcmp(settings_get("underwater_overlay"), "0"));
    checkbox("Poo Fix", "poo_fix", true,
             "Under the poo after the Bull fight, the haze is brown to "
             "match instead of a blue band. Off = the original. Normal "
             "water is never changed. Needs Underwater Shimmer (Picture) on");
    end_needs();
    /* On by default, a debug choice (user 2026-11-04). */
    if (settings_debug())
        checkbox("Physics at 30 Hz (debug)", "physics_30hz", true,
                 "Above 30 fps tails, ragdolls and cloth move at the original "
                 "speed (off: faster, 4x as fast at 120)");

    if (settings_debug()) {
        section("Debug");
        note("Started with --debug: debug choices are shown.");
        /* To-do 117: the pause key as before 0.2.3 (old_pause_on). */
        checkbox("Old P pause: holds the picture (debug)", "old_pause", true,
                 "P stops the game at its next frame, everywhere; on resume it "
                 "catches up on the time (a jump), and cutscene sound falls "
                 "behind. Off: P freezes the game as photo mode does");
        /* The game's own soak test (recomp_manual.c, conker_StartSoak). */
        int soak = conker_SoakState();
        if (soak == 2) {
            note("Soak test running (Back hides its text; close the game to stop)");
        } else {
            ImGui::BeginDisabled(soak != 1);
            if (ImGui::Button("Start soak test")) {
                conker_StartSoak();
                *close = true;
            }
            ImGui::EndDisabled();
            note(soak == 1 ? "    Every map in turn, then the chapters, over and over"
                           : "    Only from the main menu");
        }
    }
}

struct MenuTab {
    const char *name;
    void (*body)(unsigned w, unsigned h, bool *close);
};
static const MenuTab s_menu_tabs[] = {
    { "Display", display_tab },
    { "Picture", picture_tab },
    { "Sound", sound_tab },
    { "Fixes", fixes_tab },
};
static const int MENU_TABS = (int)(sizeof(s_menu_tabs) / sizeof(s_menu_tabs[0]));

/* A row of tabs that goes on in a second row (and more) when the window is
 * too narrow for all of them (user 2026-11-06; ImGui's own tab bar only
 * squeezes or scrolls them). Buttons in the tab colours, the shown one
 * lit, a line under the rows. Returns the tab chosen; focus: the shown
 * tab takes the keyboard / pad focus (after LB / RB, so down goes into
 * it). */
template <typename Tab>
static int wrapped_tabs(const char *id, const Tab *tabs, int n, int cur,
                        bool focus)
{
    const ImGuiStyle &st = ImGui::GetStyle();
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = st.ItemInnerSpacing.x;
    float x = 0.0f;
    int chosen = cur;

    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        float tw = ImGui::CalcTextSize(tabs[i].name).x + st.FramePadding.x * 2.0f;
        if (i > 0 && x + gap + tw <= avail) {
            ImGui::SameLine(0.0f, gap);
            x += gap + tw;
        } else {
            x = tw;
        }
        bool lit = i == cur;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              st.Colors[lit ? ImGuiCol_TabSelected : ImGuiCol_Tab]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              st.Colors[lit ? ImGuiCol_TabSelected : ImGuiCol_TabHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              st.Colors[ImGuiCol_TabSelected]);
        if (ImGui::Button(tabs[i].name))
            chosen = i;
        if (lit && focus)
            ImGui::SetKeyboardFocusHere(-1);   /* (presses it: no change) */
        ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    ImGui::PushStyleColor(ImGuiCol_Separator, st.Colors[ImGuiCol_TabSelected]);
    ImGui::Separator();
    ImGui::PopStyleColor();
    return chosen;
}

static void build_menu(unsigned w, unsigned h, bool *close)
{
    /* 24 wide: "Ambient Particles (experimental)", indented, is the widest
     * line (clipped at 22, 2026-10-31). */
    place_window(MENU_SETTINGS, "menu_window", w, h, 24.0f, 0.0f);
    ImGui::Begin("Settings", NULL,
                 ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoSavedSettings);
    {
        bool paused = s_want_pause != 0;
        char label[64];
        snprintf(label, sizeof(label), "Pause the game (%s)",
                 controls_key_label(controls_pause_key()));
        if (ImGui::Checkbox(label, &paused))
            want_pause(paused);
    }
    if (!s_pad_menu)
        note("Controller: the game. Click this menu (or F2 twice) to use it.");

    /* The dump goes through every tab; the menu shows the chosen one (the
     * same one again at the next opening). LB / RB on the pad step
     * through them (feed_pad). */
    if (s_dump) {
        for (const MenuTab &t : s_menu_tabs) {
            if (dump_item("tab", t.name, NULL))
                fputs(" }", s_dump);
            t.body(w, h, close);
        }
    } else {
        bool stepped = s_tab_step != 0;
        if (stepped)
            s_tab_cur = (s_tab_cur + s_tab_step + MENU_TABS) % MENU_TABS;
        s_tab_step = 0;
        s_tab_cur = wrapped_tabs("##settings_tabs", s_menu_tabs, MENU_TABS,
                                 s_tab_cur, stepped);
        s_menu_tabs[s_tab_cur].body(w, h, close);
    }

    ImGui::SeparatorText("Controls");  /* a button, nothing to dump */
    if (ImGui::Button("Keyboard and mouse (F3)"))
        InterlockedExchange(&s_want_open, MENU_CONTROLS);

    if (s_error[0])
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_error);
    ImGui::Separator();
    if (ImGui::Button("Back to the game"))
        *close = true;
    ImGui::SameLine();
    note("(F2, Esc, B or Start)");
    /* Report Bug (to-do 12): its window in place of this menu, with the
     * picture of the next frame (the game runs under this menu, so that is
     * the game without menus). */
    if (ImGui::Button("Report Bug...")) {
        InterlockedExchange(&s_report_by_key, 0);
        InterlockedExchange(&s_report_shot_want, 1);
        InterlockedExchange(&s_want_open, MENU_REPORT);
    }
    {
        int rk = controls_report_key();
        if (rk > 0) {
            char k[48];
            snprintf(k, sizeof(k), "(%s)", controls_key_label(rk));
            ImGui::SameLine();
            note(k);
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Close game"))
        s_ask_quit = true;
    ask_quit();
    s_item_active = ImGui::IsAnyItemActive();
    remember_window("menu_window", w, h);
    ImGui::End();
}

/* ---- the controls menu (F3) ------------------------------------------------ */

/* An action's key was set: save, hand to the toolkit, log. */
static void set_key(int set, int i, int input)
{
    const ControlAction *a = controls_action(set, i);
    save(a->key, controls_key_name(input));
    controls_apply();
    fprintf(stderr, "[OVERLAY] %s = %s\n", a->key, controls_key_name(input));
}

/* "Press a key" for the action being set: a modal over the menu. Its two
 * buttons are for the mouse (or the pad): every key is taken as the
 * answer. A click away from them is a mouse button to bind. */
static void ask_key(void)
{
    const char *id = "Set a key";

    if (s_capture_idx < 0)
        return;
    if (!ImGui::IsPopupOpen(id))
        ImGui::OpenPopup(id);
    if (!ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_AlwaysAutoResize
                                | ImGuiWindowFlags_NoSavedSettings))
        return;
    s_popup_open = true;
    const ControlAction *a = controls_action(s_capture_set, s_capture_idx);
    bool done = false;
    ImGui::Text("Press a key or a mouse button for:");
    ImGui::Text("    %s", a->label);
    note("Clicking here (not on a button) sets that mouse button; Esc is a "
         "key like any other.");
    if (s_capture_msg[0])
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           s_capture_msg);
    if (s_capture_got >= 0) {
        set_key(s_capture_set, s_capture_idx, s_capture_got);
        done = true;
    }
    if (ImGui::Button("No key")) {
        set_key(s_capture_set, s_capture_idx, 0);
        done = true;
    }
    s_capture_hover = ImGui::IsItemHovered();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        done = true;
    s_capture_hover = s_capture_hover || ImGui::IsItemHovered();
    if (done) {
        s_capture_idx = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static const char *set_name(int set)
{
    return set == CONTROLS_MP ? "multiplayer"
         : set == CONTROLS_PHOTO ? "photo mode" : "single player";
}

/* What "Reset all ... controls" puts back: every key of a set's table
 * (single player's of both schemes) and the set's mouse options, each once,
 * into out; returns how many. */
static int reset_keys(int set, char (*out)[48], int max)
{
    static const char *const prefix[] = { "story", "mp" };
    static const char *const mouse[] = { "_mouse_x", "_mouse_y", "_invert_y" };
    const ControlAction *a;
    int n = 0, scheme;

    for (int i = 0; (a = controls_table_row(set, i, &scheme)) && n < max;
         i++) {
        bool seen = false;
        for (int j = 0; j < n && !seen; j++)
            seen = !strcmp(out[j], a->key);
        if (!seen)
            snprintf(out[n++], sizeof(out[0]), "%s", a->key);
    }
    for (int i = 0; i < 3 && set != CONTROLS_PHOTO && n < max; i++)
        snprintf(out[n++], sizeof(out[0]), "%s%s", prefix[set], mouse[i]);
    return n;
}

/* "Reset all ... controls?" before putting every key of a set back. */
static void ask_reset(void)
{
    const char *id = "Reset controls";

    if (s_ask_reset < 0)
        return;
    if (!ImGui::IsPopupOpen(id))
        ImGui::OpenPopup(id);
    if (!ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_AlwaysAutoResize
                                | ImGuiWindowFlags_NoSavedSettings))
        return;
    s_popup_open = true;
    ImGui::Text(s_ask_reset == CONTROLS_PHOTO
                ? "Put every %s key back to the defaults?"
                : "Put every %s key and the mouse back to the defaults?",
                set_name(s_ask_reset));
    bool done = false;
    if (ImGui::Button("Yes")) {
        char keys[64][48];
        int set = s_ask_reset;
        int n = reset_keys(set, keys, 64);
        for (int i = 0; i < n; i++)
            save(keys[i], settings_default(keys[i]));
        controls_apply();
        fprintf(stderr, "[OVERLAY] %s controls reset\n", set_name(set));
        done = true;
    }
    ImGui::SameLine();
    bool no = ImGui::Button("No");
    ImGui::SetItemDefaultFocus();
    if (no || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
           || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        done = true;
    if (done) {
        s_ask_reset = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static bool same_section(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static bool begin_keys(const char *id)
{
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg
                           | ImGuiTableFlags_SizingStretchProp))
        return false;
    float px = ImGui::GetFontSize();
    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed,
                            px * 8.0f);
    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFrameHeight());
    return true;
}

/* Swimming and the bat show no button (user 2026-10-29): there the button
 * is not the one on land. */
static bool shows_button(const ControlAction *a)
{
    return !a->section || (strcmp(a->section, "Swimming")
                           && strcmp(a->section, "Bat (Count Batula)"));
}

/* keys_table for --dump-settings: every row of the set's table, single
 * player's with the scheme it belongs to (the launcher shows the chosen
 * scheme's), a section heading where keys_table starts a table. A row's
 * "section" and "clashes_all" say which rows its key clashes with, as
 * keys_table colours them. */
static void dump_keys(int set)
{
    static const char *const scheme_name[] = { NULL, "classic", "modern" };
    const ControlAction *a;
    const char *cur = NULL;
    int scheme;

    for (int i = 0; (a = controls_table_row(set, i, &scheme)); i++) {
        if (!same_section(a->section, cur)) {
            cur = a->section;
            section(cur);
        }
        dump_item("key", a->label, a->key);
        fputs(", \"button\": ", s_dump);
        settings_json_string(s_dump, a->button);
        if (!shows_button(a))
            fputs(", \"button_shown\": false", s_dump);
        if (a->section) {
            fputs(", \"section\": ", s_dump);
            settings_json_string(s_dump, a->section);
        }
        if (scheme_name[scheme])
            fprintf(s_dump, ", \"scheme\": \"%s\"", scheme_name[scheme]);
        if (set == CONTROLS_PHOTO && i <= PHOTO_REPORT)
            fputs(", \"clashes_all\": true", s_dump);
        fputs(" }", s_dump);
    }
}

/* A set's table of actions and their keys, one more table per section
 * (Swimming, Bat; Photo mode's two). A key also bound to another action of
 * the same table that presses another button shows orange; a section's keys
 * work elsewhere (in the water), so sharing a key with the main list is no
 * clash, and the bat's Launch and Fly up are both B. */
static void keys_table(int set)
{
    char id[64];
    int n = controls_count(set);
    float gap = ImGui::GetStyle().ItemInnerSpacing.x;

    if (s_dump) {
        dump_keys(set);
        return;
    }
    {
        const char *section = NULL;
        bool open = begin_keys("##keys");
        for (int i = 0; i < n; i++) {
            const ControlAction *a = controls_action(set, i);
            if (!same_section(a->section, section)) {
                if (open)
                    ImGui::EndTable();
                section = a->section;
                ImGui::SeparatorText(section);
                snprintf(id, sizeof(id), "##keys_%s", section);
                open = begin_keys(id);
            }
            if (!open)
                continue;
            int input = controls_key(set, i);
            const char *also = NULL;
            /* Photo mode's three "anywhere" keys (photo mode, pause,
             * Report Bug) clash with all its rows. */
            for (int j = 0; j < n && input > 0; j++)
                if (j != i && controls_key(set, j) == input
                    && (same_section(controls_action(set, j)->section,
                                     a->section)
                        || (set == CONTROLS_PHOTO
                            && (i <= PHOTO_REPORT || j <= PHOTO_REPORT)))
                    && strcmp(controls_action(set, j)->button, a->button))
                    also = controls_action(set, j)->label;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            /* The button beside the action when both fit, else under it:
             * the menu may be narrow. */
            char btn[48];
            snprintf(btn, sizeof(btn), "(%s)", a->button);
            ImGui::AlignTextToFramePadding();
            if (!shows_button(a)) {
                ImGui::TextWrapped("%s", a->label);
            } else if (ImGui::CalcTextSize(a->label).x + gap
                + ImGui::CalcTextSize(btn).x
                <= ImGui::GetContentRegionAvail().x) {
                ImGui::TextUnformatted(a->label);
                ImGui::SameLine(0.0f, gap);
                ImGui::TextDisabled("%s", btn);
            } else {
                ImGui::TextWrapped("%s", a->label);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle()
                                      .Colors[ImGuiCol_TextDisabled]);
                ImGui::TextWrapped("%s", btn);
                ImGui::PopStyleColor();
            }

            ImGui::TableNextColumn();
            snprintf(id, sizeof(id), "%s##%s", controls_key_label(input),
                     a->key);
            if (also)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            if (ImGui::Button(id, ImVec2(-FLT_MIN, 0.0f))) {
                s_capture_set = set;
                s_capture_idx = i;
                s_capture_got = -1;
                s_capture_msg[0] = '\0';
                s_capture_hover = true;   /* the click that asked */
            }
            if (also) {
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Also used for: %s", also);
            }

            ImGui::TableNextColumn();
            if (strcmp(settings_get(a->key), settings_default(a->key))) {
                char tip[96];
                snprintf(id, sizeof(id), "##reset_%s", a->key);
                snprintf(tip, sizeof(tip), "Back to the default (%s)",
                         controls_key_label(controls_parse_key(
                             settings_default(a->key))));
                if (reset_icon(id, tip))
                    set_key(set, i, controls_parse_key(
                                settings_default(a->key)));
            }
        }
        if (open)
            ImGui::EndTable();
    }
}

/* A set's "Reset all ... controls" button; it asks first (ask_reset). The
 * dump gives the question and what it puts back. */
static void reset_button(int set)
{
    char id[64];
    snprintf(id, sizeof(id), "Reset all %s controls", set_name(set));
    if (s_dump) {
        char keys[64][48], q[96];
        const char *list[64];
        int n = reset_keys(set, keys, 64);
        for (int i = 0; i < n; i++)
            list[i] = keys[i];
        snprintf(q, sizeof(q), set == CONTROLS_PHOTO
                 ? "Put every %s key back to the defaults?"
                 : "Put every %s key and the mouse back to the defaults?",
                 set_name(set));
        dump_item("reset", id, NULL);
        fputs(", \"question\": ", s_dump);
        settings_json_string(s_dump, q);
        dump_list("keys", list, n);
        fputs(" }", s_dump);
    }
    size_t len = strlen(id);
    snprintf(id + len, sizeof(id) - len, "##%d", set);
    if (ImGui::Button(id))
        s_ask_reset = set;
}

/* One player set's tab: the mouse, then its keys. */
static void controls_tab(int set)
{
    static const char *const prefix[] = { "story", "mp" };
    char key[64];

    section("Mouse (looking around)");
    snprintf(key, sizeof(key), "%s_mouse_x", prefix[set]);
    slider("Sensitivity left and right", key, 10.0f, 400.0f, "100",
           "%.0f %%", "Back to the default (100 %)");
    snprintf(key, sizeof(key), "%s_mouse_y", prefix[set]);
    slider("Sensitivity up and down", key, 10.0f, 400.0f, "100", "%.0f %%",
           "Back to the default (100 %)");
    snprintf(key, sizeof(key), "%s_invert_y", prefix[set]);
    checkbox("Invert up and down", key, true);

    section("Keys");
    info("Click a key to change it. In brackets: the controller button it "
         "presses.");
    keys_table(set);
    reset_button(set);
}

/* Photo mode's tab (SPEC 10.1): its keys only; in brackets the controller
 * button for the same thing. */
static void photo_tab(void)
{
    info("Click a key to change it. In brackets: the controller button that "
         "does the same.");
    keys_table(CONTROLS_PHOTO);
    reset_button(CONTROLS_PHOTO);
}

/* Is input one of the four in t? */
static bool key_in(int input, const int *t)
{
    return input == t[0] || input == t[1] || input == t[2] || input == t[3];
}

/* An action's keys for the Menus tab, in table order: "E/Space/Enter";
 * all of W/A/S/D and all four arrows are shortened ("W/A/S/D or arrow
 * keys"). */
static void key_list(const int *keys, int n, char *out, size_t size)
{
    static const int wasd[4] = { 'W', 'A', 'S', 'D' };
    static const int arrows[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    bool w = true, a = true;
    size_t len;

    for (int t = 0; t < 4; t++) {
        bool hw = false, ha = false;
        for (int i = 0; i < n; i++) {
            hw |= keys[i] == wasd[t];
            ha |= keys[i] == arrows[t];
        }
        w &= hw;
        a &= ha;
    }
    snprintf(out, size, "%s", w ? "W/A/S/D" : "");
    for (int i = 0; i < n; i++) {
        if ((w && key_in(keys[i], wasd)) || (a && key_in(keys[i], arrows)))
            continue;
        len = strlen(out);
        snprintf(out + len, size - len, "%s%s", len ? "/" : "",
                 controls_key_label(keys[i]));
    }
    if (a) {
        len = strlen(out);
        snprintf(out + len, size - len, "%s", len ? " or arrow keys"
                                                  : "Arrow keys");
    }
}

/* The fixed keys of menus, cutscenes and the single player pause screen,
 * one table each: one line per action with all its keys, no controller
 * buttons (user 2026-10-26: the button column squeezed the tab). Rows
 * with the same label are one action; a row without a label is not
 * listed. width: the action column's, the same in every table so the
 * keys line up. section: only the set's rows of that section (NULL: its
 * main list; the multiplayer menu's map options are a section of the
 * menu keys). */
static void fixed_keys(int set, const char *section, const char *id,
                       float width)
{
    int n = controls_count(set);
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed,
                            width);
    ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch);
    for (int i = 0; i < n; i++) {
        const char *label = controls_action(set, i)->label;
        bool seen = !*label
                    || !same_section(controls_action(set, i)->section,
                                     section);
        for (int j = 0; j < i && !seen; j++)
            seen = !strcmp(controls_action(set, j)->label, label);
        if (seen)
            continue;
        int keys[16], nk = 0;
        for (int j = i; j < n && nk < 16; j++)
            if (!strcmp(controls_action(set, j)->label, label))
                keys[nk++] = controls_key(set, j);
        char text[160];
        key_list(keys, nk, text, sizeof(text));
        if (dump_item("fixed", label, NULL)) {
            fputs(", \"keys\": ", s_dump);
            settings_json_string(s_dump, text);
            fputs(" }", s_dump);
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", text);
    }
    ImGui::EndTable();
}

/* A host pad by its name: Windows' list for an Xbox pad 0-3 ("Xbox
 * Controller"), SDL's for a PlayStation pad 4-7 ("PS4 Controller"), with
 * its number when another connected one has the same name ("Xbox
 * Controller (2)"); "Controller 2" when it has no name, "Controller 3 (not
 * connected)" when it is gone (user 2026-10-28: numbers alone do not say
 * which controller is which). PlayStation pads count on their own:
 * "PlayStation controller 1". */
static void pad_label(int pad, char *out, size_t size)
{
    char other[96], num[48];
    bool twin = false;

    if (pad < CONTROLS_XINPUT_PADS)
        snprintf(num, sizeof(num), "Controller %d", pad + 1);
    else
        snprintf(num, sizeof(num), "PlayStation controller %d",
                 pad - CONTROLS_XINPUT_PADS + 1);
    if (!controls_pad_connected(pad)) {
        snprintf(out, size, "%s (not connected)", num);
        return;
    }
    if (!controls_pad_name(pad, out, (int)size)) {
        snprintf(out, size, "%s", num);
        return;
    }
    for (int q = 0; q < CONTROLS_HOST_PADS; q++)
        if (q != pad && controls_pad_name(q, other, (int)sizeof(other))
            && !strcmp(other, out))
            twin = true;
    if (twin) {
        size_t len = strlen(out);
        snprintf(out + len, size - len, " (%d)",
                 pad % CONTROLS_XINPUT_PADS + 1);
    }
}

/* A port's controller as the list shows it: No input, Any controller (with
 * the one it is now, for the port's own line), or a controller's label. */
static void port_source_name(int src, int port, char *out, size_t size)
{
    if (src == -1) {
        snprintf(out, size, "No input");
    } else if (src == -2) {
        int pad = port >= 0 ? controls_pad_of_port(port) : -1;
        char now[96];
        if (pad >= 0) {
            pad_label(pad, now, sizeof(now));
            snprintf(out, size, "Any controller (%s)", now);
        } else {
            snprintf(out, size, "Any controller");
        }
    } else {
        pad_label(src, out, size);
    }
}

/* The Slots tab: the game's four players, each with a list of
 * controllers and a box for the keyboard and mouse (user 2026-10-28). A
 * controller is one port's at a time: picking one another port has puts
 * that port back to No input. The keyboard boxes are one choice: ticking
 * one moves the keyboard there, unticking it turns the keyboard off (the
 * same as "Use keyboard and mouse" at the top). */
static void ports_tab(void)
{
    static const char *const values[] = {
        "none", "any", "1", "2", "3", "4", "ps1", "ps2", "ps3", "ps4",
    };
    static const int sources[] = { -1, -2, 0, 1, 2, 3, 4, 5, 6, 7 };
    bool kbm = strcmp(settings_get("keyboard_mouse"), "0") != 0;
    int kb = controls_keyboard_port();
    char key[32], name[160], id[32];

    info("Which controller is each player: slot 1 is player 1 (controller "
         "port 1 on the Xbox). The list shows the connected controllers by "
         "the names in Windows' game controller list; PlayStation "
         "controllers (DualShock 4, DualSense) work without other "
         "programs. A slot with "
         "no controller and no keyboard is empty, as if nothing were "
         "plugged in there; the game sees the change after a second.");
    /* The launcher names the controllers from --list-pads; a controller is
     * one slot's at a time, and the keyboard boxes are one choice, as
     * below. */
    if (dump_item("slots", "Slots", NULL)) {
        fputs(", \"count\": 4, \"key\": \"controller_slot\", "
              "\"keyboard_key\": \"keyboard_slot\", "
              "\"keyboard_on_key\": \"keyboard_mouse\"", s_dump);
        dump_list("values", values, (int)(sizeof(values) / sizeof(values[0])));
        fputs(" }", s_dump);
    }
    if (!ImGui::BeginTable("##ports", 3, ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int p = 0; p < 4; p++) {
        int cur = controls_port_source(p);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Slot %d", p + 1);

        ImGui::TableNextColumn();
        port_source_name(cur, p, name, sizeof(name));
        snprintf(id, sizeof(id), "##slot%d", p + 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, name)) {
            s_popup_open = true;
            for (int i = 0; i < (int)(sizeof(sources) / sizeof(sources[0]));
                 i++) {
                bool sel = sources[i] == cur;
                /* Only connected controllers, and the one set if it is
                 * gone, so the list says what the slot is set to. */
                if (sources[i] >= 0 && !sel
                    && !controls_pad_connected(sources[i]))
                    continue;
                port_source_name(sources[i], sel ? p : -1, name, sizeof(name));
                size_t len = strlen(name);
                snprintf(name + len, sizeof(name) - len, "##src%d", i);
                if (ImGui::Selectable(name, sel) && !sel) {
                    if (sources[i] != -1)
                        for (int q = 0; q < 4; q++)
                            if (q != p && controls_port_source(q) == sources[i]) {
                                snprintf(key, sizeof(key),
                                         "controller_slot%d", q + 1);
                                save(key, "none");
                            }
                    snprintf(key, sizeof(key), "controller_slot%d", p + 1);
                    save(key, values[i]);
                    controls_apply();
                    fprintf(stderr, "[OVERLAY] slot %d = %s\n", p + 1,
                            values[i]);
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::TableNextColumn();
        bool on = kbm && kb == p;
        snprintf(id, sizeof(id), "##kb%d", p + 1);
        if (ImGui::Checkbox(id, &on)) {
            if (on) {
                snprintf(name, sizeof(name), "%d", p + 1);
                save("keyboard_slot", name);
                save("keyboard_mouse", "1");
            } else {
                save("keyboard_mouse", "0");
            }
            controls_apply();
            fprintf(stderr, "[OVERLAY] keyboard and mouse: %s\n",
                    on ? name : "off");
        }
        ImGui::SetItemTooltip(on ? "The keyboard and mouse are this player"
                                 : "Make the keyboard and mouse this player");
    }
    ImGui::EndTable();
    /* Always shown: a note coming and going moved the boxes under the
     * pointer. */
    info("The mouse turns the camera directly only on slot 1; on another "
         "slot it works as that player's right stick.");
    checkbox("Control when out of focus", "control_out_of_focus", true,
             "Controllers keep playing the game while another window is in "
             "front. Off: they do nothing until the game window is in front "
             "again (the keyboard always waits).");
    /* To-do 29 (per slot, user 2026-11-07): the toolkit scales the game's
     * motor speeds and reshapes the sticks (xinput_device.c); shown for
     * the slots with a controller, all four in the dump. */
    section("Rumble and sticks");
    info("Each slot's controller: rumble (when the game's own Vibration "
         "option is on too), how strong it is, and how far a stick tilts "
         "before it moves anything (default 10 %; 14 % is the game's own, "
         "lower responds sooner, higher helps a stick that drifts).");
    for (int p = 0; p < 4; p++) {
        char rk[16], sk[24], dk[24], lr[40], ls[48], ld[48];
        snprintf(key, sizeof(key), "controller_slot%d", p + 1);
        if (!s_dump && !strcmp(settings_get(key), "none"))
            continue;
        snprintf(rk, sizeof(rk), "rumble%d", p + 1);
        snprintf(sk, sizeof(sk), "rumble_strength%d", p + 1);
        snprintf(dk, sizeof(dk), "stick_dead_zone%d", p + 1);
        snprintf(lr, sizeof(lr), "Slot %d rumble", p + 1);
        snprintf(ls, sizeof(ls), "Slot %d rumble strength", p + 1);
        snprintf(ld, sizeof(ld), "Slot %d stick dead zone", p + 1);
        checkbox(lr, rk, true, "This slot's controller rumbles (off: never)");
        /* The strength shown while rumble is on (as the Pop-in Fix's
         * parts); the dump lists it under its parent. */
        if (s_dump || strcmp(settings_get(rk), "0")) {
            ImGui::Indent();
            s_dump_parent = rk;
            slider(ls, sk, 0.0f, 200.0f, "100", "%.0f %%",
                   "Back to the default (100 %: as the game asks)");
            s_dump_parent = NULL;
            ImGui::Unindent();
        }
        /* Resets to the default 10 (user 2026-11-07), not the game's 14. */
        slider(ld, dk, 0.0f, 40.0f, "10", "%.0f %%",
               "Back to the default (10 %)");
    }
}

static void story_tab(void)
{
    static const char *const values[] = { "classic", "modern" };
    static const char *const names[] = { "Classic", "Modern" };

    section("Scheme");
    if (dump_item("radio", "Scheme", "story_scheme")) {
        dump_list("values", values, 2);
        dump_list("names", names, 2);
        fputs(" }", s_dump);
    }
    int was = strcmp(settings_get("story_scheme"), "modern") ? 0 : 1;
    int scheme = was;
    ImGui::RadioButton(names[0], &scheme, 0);
    ImGui::RadioButton(names[1], &scheme, 1);
    if (scheme != was) {
        save("story_scheme", values[scheme]);
        controls_apply();
        fprintf(stderr, "[OVERLAY] story_scheme = %s\n", values[scheme]);
    }
    /* Modern only (user 2026-10-30): the context key toggles a slingshot
     * pad's slingshot, as Classic's B, instead of the aim key holding it. */
    if (scheme == 1 || s_dump) {
        static const char *const sling_values[] = { "hold", "toggle" };
        static const char *const sling_names[] = { "Hold", "Toggle" };
        s_dump_scheme = "modern";
        choice("Slingshot Aim", "story_slingshot_aim", sling_values,
               sling_names, 2, true);
        s_dump_scheme = NULL;
    }
    controls_tab(CONTROLS_STORY);
}

static void mp_tab(void)
{
    static const char *const zoom_values[] = { "toggle", "hold" };
    static const char *const zoom_names[] = {
        "Toggle (press to zoom in, press again to zoom out)",
        "Hold (zoomed in while the key is held)",
    };

    info("These keys follow the game's \"Emily\" control scheme: "
         "choose Emily in the multiplayer controller options.");
    section("Zoom");
    choice("Zoom key", "mp_zoom_mode", zoom_values, zoom_names, 2, true);
    controls_tab(CONTROLS_MP);
}

static void menus_tab(void)
{
    static const int sets[] = {
        CONTROLS_MENU, CONTROLS_CUTSCENE, CONTROLS_PAUSE,
    };
    float w = 0.0f;

    info("Fixed keys, the same in every menu: the main menu, the "
         "multiplayer menus and pause menu. The mouse "
         "does nothing in menus. Esc goes back on the main menu and "
         "the multiplayer menus, and closes the multiplayer pause "
         "menu.");
    for (int set : sets)
        for (int i = 0; i < controls_count(set); i++) {
            float x = ImGui::CalcTextSize(controls_action(set, i)->label).x;
            if (x > w)
                w = x;
        }
    w += ImGui::GetFontSize();
    section("Menus");
    fixed_keys(CONTROLS_MENU, NULL, "##menu_keys", w);
    section("Cutscenes");
    fixed_keys(CONTROLS_CUTSCENE, NULL, "##cutscene_keys", w);
    section("Single player pause screen");
    fixed_keys(CONTROLS_PAUSE, NULL, "##pause_keys", w);
    section("Multiplayer menu");
    fixed_keys(CONTROLS_MENU, "Multiplayer menu", "##mp_menu_keys", w);
}

struct ControlsTab {
    const char *name;
    void (*body)(void);
};
static const ControlsTab s_controls_tabs[] = {
    { "Single player", story_tab },
    { "Multiplayer", mp_tab },
    { "Photo mode", photo_tab },
    { "Menus", menus_tab },
    { "Slots", ports_tab },
};

static void build_controls(unsigned w, unsigned h, bool *close)
{
    place_window(MENU_CONTROLS, "controls_window", w, h, 28.0f, 0.85f);
    ImGui::Begin("Controls", NULL,
                 ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoSavedSettings);
    checkbox("Use keyboard and mouse", "keyboard_mouse", true);
    {
        static const char *const now[] = {
            "In use now: the single player keys.",
            "In use now: the multiplayer keys (the game is in multiplayer).",
            "In use now: the menu keys (see Menus).",
            "In use now: the pause screen keys (see Menus).",
            "In use now: the menu keys, Esc closes the menu (see Menus).",
            "In use now: the cutscene keys (see Menus).",
            "In use now: the single player keys, swimming at the surface.",
            "In use now: the single player keys, underwater.",
            "In use now: the single player keys on the beehive gun "
            "(Forward aims up, Back aims down).",
            "In use now: the multiplayer keys in the Mule 52's gun seat "
            "(Forward aims up, Back aims down).",
            /* The Vault's suit and the alien's tail: not listed (user
             * 2026-10-28). */
            "In use now: the single player keys.",
            "In use now: the single player keys.",
            "In use now: the single player keys, the bat on the ground.",
            "In use now: the single player keys, the bat flying.",
            "In use now: the photo mode keys.",
        };
        /* One line per key set (controls.h); photo mode was missing and
         * F3 in photo mode crashed on the read past the end (user
         * 2026-10-29). */
        static_assert(sizeof(now) / sizeof(now[0]) == CONTROLS_PHOTO + 1,
                      "a line per key set");
        int set = controls_mode();
        if (set >= 0 && set <= CONTROLS_PHOTO)
            note(now[set]);
    }

    /* The dump goes through every tab; the menu shows the chosen one. */
    if (s_dump) {
        for (const ControlsTab &t : s_controls_tabs) {
            if (dump_item("tab", t.name, NULL))
                fputs(" }", s_dump);
            t.body();
        }
    } else if (ImGui::BeginTabBar("##sets")) {
        for (const ControlsTab &t : s_controls_tabs)
            if (ImGui::BeginTabItem(t.name)) {
                t.body();
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }

    if (s_error[0])
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_error);
    ImGui::Separator();
    if (ImGui::Button("Back to the game"))
        *close = true;
    ImGui::SameLine();
    note("(F3, Esc, B or Start)");
    ask_key();
    ask_reset();
    s_item_active = ImGui::IsAnyItemActive();
    remember_window("controls_window", w, h);
    ImGui::End();
}

/* ---- photos ---------------------------------------------------------------- */

/* A photo: the back buffer as the overlay hook gets it (after the filter
 * chain, before any ImGui text) copied to a staging texture and read back
 * at once (one short stall, the world is frozen anyway), turned into
 * 24-bit BGR rows and handed to a thread that writes the PNG with WIC, so
 * the executor does not wait for the encoder. Into photo_dir,
 * conker_<date>_<time>.png (_2, _3 ... when taken in the same second). */
struct Shot {
    BYTE *bgr;
    unsigned w, h;
    wchar_t path[MAX_PATH];
    char name[64];
};

static char s_shot_msg[160];        /* the last photo's outcome (s_lock) */
static DWORD s_shot_msg_at;         /* GetTickCount when it was set */

static void shot_message(const char *msg)
{
    EnterCriticalSection(&s_lock);
    snprintf(s_shot_msg, sizeof(s_shot_msg), "%s", msg);
    s_shot_msg_at = GetTickCount();
    LeaveCriticalSection(&s_lock);
}

/* The message while it is fresh (4 s), else false. */
static bool shot_message_now(char *out, size_t size)
{
    bool fresh;
    EnterCriticalSection(&s_lock);
    fresh = s_shot_msg[0] && GetTickCount() - s_shot_msg_at < 4000;
    if (fresh)
        snprintf(out, size, "%s", s_shot_msg);
    LeaveCriticalSection(&s_lock);
    return fresh;
}

template <class T> static void release(T *&p)
{
    if (p) {
        p->Release();
        p = NULL;
    }
}

/* 24-bit BGR rows (w x h) as a PNG at path, with WIC; COM is set up on
 * this thread. No half-written file is left behind. */
static HRESULT write_png(const wchar_t *path, const BYTE *bgr, unsigned w,
                         unsigned h)
{
    IWICImagingFactory *fac = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    UINT stride = w * 3;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    bool made = false;

    if (SUCCEEDED(hr))
        hr = fac->CreateStream(&st);
    if (SUCCEEDED(hr))
        hr = st->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) {
        made = true;
        hr = fac->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc);
    }
    if (SUCCEEDED(hr))
        hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = enc->CreateNewFrame(&fr, NULL);
    if (SUCCEEDED(hr))
        hr = fr->Initialize(NULL);
    if (SUCCEEDED(hr))
        hr = fr->SetSize(w, h);
    if (SUCCEEDED(hr))
        hr = fr->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR)
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = fr->WritePixels(h, stride, stride * h, (BYTE *)bgr);
    if (SUCCEEDED(hr))
        hr = fr->Commit();
    if (SUCCEEDED(hr))
        hr = enc->Commit();
    release(fr);
    release(enc);
    release(st);
    release(fac);
    if (FAILED(hr) && made)
        DeleteFileW(path);
    return hr;
}

static DWORD WINAPI write_shot(void *arg)
{
    Shot *s = (Shot *)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = write_png(s->path, s->bgr, s->w, s->h);
    char msg[160];

    if (SUCCEEDED(hr)) {
        snprintf(msg, sizeof(msg), "Photo saved: %s", s->name);
        fprintf(stderr, "[PHOTO] saved %s (%ux%u)\n", s->name, s->w, s->h);
    } else {
        snprintf(msg, sizeof(msg), "The photo could not be saved (0x%08lX)",
                 (unsigned long)hr);
        fprintf(stderr, "[PHOTO] %s could not be written: 0x%08lX\n",
                s->name, (unsigned long)hr);
    }
    shot_message(msg);
    free(s->bgr);
    delete s;
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

/* The folder photos go to: photo_folder (UTF-8, chosen in the photo
 * panel), else Photos\ in the save folder. Executor thread (settings). */
static void photo_dir(wchar_t *out, size_t n)
{
    const char *f = settings_get("photo_folder");
    wchar_t save[MAX_PATH];

    if (f && f[0] && MultiByteToWideChar(CP_UTF8, 0, f, -1, out, (int)n) > 0)
        return;
    if (!MultiByteToWideChar(CP_ACP, 0, settings_save_dir(), -1, save,
                             MAX_PATH))
        save[0] = L'\0';
    swprintf(out, n, L"%ls\\Photos", save);
}

/* The folder made if missing; a name not yet taken. */
static bool shot_path(Shot *s)
{
    wchar_t dir[MAX_PATH], name[64];
    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;

    photo_dir(dir, MAX_PATH);
    SHCreateDirectoryExW(NULL, dir, NULL);
    localtime_s(&tm, &t);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
    for (int n = 1; n < 100; n++) {
        if (n == 1)
            snprintf(s->name, sizeof(s->name), "conker_%s.png", stamp);
        else
            snprintf(s->name, sizeof(s->name), "conker_%s_%d.png", stamp, n);
        MultiByteToWideChar(CP_ACP, 0, s->name, -1, name, 64);
        if (swprintf(s->path, MAX_PATH, L"%ls\\%ls", dir, name) < 0)
            return false;
        if (GetFileAttributesW(s->path) == INVALID_FILE_ATTRIBUTES)
            return true;
    }
    return false;
}

/* ---- the photo folder ------------------------------------------------------ */

/* The photo panel's folder buttons: the folder dialog (pick) or Explorer
 * (open) on a thread of their own, as both wait on the shell. The dialog
 * hands the chosen folder back through s_picked; the executor saves it. */
struct FolderJob {
    bool pick;
    wchar_t dir[MAX_PATH];
};

static volatile LONG s_picking;     /* the folder dialog is up */
static volatile LONG s_picked_new;  /* s_picked holds a new choice */
static char s_picked[MAX_PATH];     /* UTF-8 (s_lock) */

static DWORD WINAPI folder_job(void *arg)
{
    FolderJob *j = (FolderJob *)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED
                                 | COINIT_DISABLE_OLE1DDE);

    if (!j->pick) {
        SHCreateDirectoryExW(NULL, j->dir, NULL);
        HINSTANCE r = ShellExecuteW(NULL, L"open", j->dir, NULL, NULL,
                                    SW_SHOWNORMAL);
        fprintf(stderr, "[PHOTO] folder opened%s\n",
                (INT_PTR)r > 32 ? "" : " (failed)");
    } else {
        IFileOpenDialog *dlg = NULL;
        IShellItem *item = NULL;
        wchar_t *path = NULL;
        DWORD o = 0;
        HRESULT hr = CoCreateInstance(__uuidof(FileOpenDialog), NULL,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dlg));
        if (SUCCEEDED(hr)) {
            dlg->GetOptions(&o);
            dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
                            | FOS_PATHMUSTEXIST);
            dlg->SetTitle(L"Photo folder");
            SHCreateDirectoryExW(NULL, j->dir, NULL);
            if (SUCCEEDED(SHCreateItemFromParsingName(j->dir, NULL,
                                                      IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
                item = NULL;
            }
            hr = dlg->Show(s_hwnd);
        }
        if (SUCCEEDED(hr))
            hr = dlg->GetResult(&item);
        if (SUCCEEDED(hr))
            hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (SUCCEEDED(hr)) {
            EnterCriticalSection(&s_lock);
            if (WideCharToMultiByte(CP_UTF8, 0, path, -1, s_picked, MAX_PATH,
                                    NULL, NULL) > 0)
                InterlockedExchange(&s_picked_new, 1);
            LeaveCriticalSection(&s_lock);
        }
        if (path)
            CoTaskMemFree(path);
        if (item)
            item->Release();
        if (dlg)
            dlg->Release();
        InterlockedExchange(&s_picking, 0);
    }
    delete j;
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

static void start_folder_job(bool pick)
{
    FolderJob *j = new FolderJob();
    j->pick = pick;
    photo_dir(j->dir, MAX_PATH);
    if (pick)
        InterlockedExchange(&s_picking, 1);
    HANDLE t = CreateThread(NULL, 0, folder_job, j, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {
        delete j;
        InterlockedExchange(&s_picking, 0);
    }
}

/* A folder the dialog chose, to settings.ini (executor thread). */
static void take_picked_folder(void)
{
    char dir[MAX_PATH];

    if (!s_picked_new || !InterlockedExchange(&s_picked_new, 0))
        return;
    EnterCriticalSection(&s_lock);
    snprintf(dir, sizeof(dir), "%s", s_picked);
    LeaveCriticalSection(&s_lock);
    save("photo_folder", dir);
    fprintf(stderr, "[PHOTO] photo folder: %s\n", dir);
}

/* The back buffer as the overlay hook gets it (after the filter chain,
 * before any ImGui) as 24-bit BGR rows, malloc'd, *pw x *ph; NULL with a
 * reason in *why. A staging copy read back at once: one short stall. */
static BYTE *grab_back_buffer(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                              ID3D11RenderTargetView *rtv, unsigned *pw,
                              unsigned *ph, const char **why)
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *bb = NULL, *flat = NULL, *stage = NULL, *src;
    D3D11_TEXTURE2D_DESC d, sd;
    D3D11_MAPPED_SUBRESOURCE m;
    BYTE *bgr = NULL;
    int order;          /* 0 BGRA, 1 RGBA, 2 RGB10A2 */

    *why = NULL;
    rtv->GetResource(&res);
    if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(&bb)))) {
        *why = "no back buffer";
        goto done;
    }
    bb->GetDesc(&d);
    switch (d.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        order = 0;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        order = 1;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        order = 2;
        break;
    default:
        *why = "back buffer format not handled";
        goto done;
    }
    src = bb;
    if (d.SampleDesc.Count > 1) {
        sd = d;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.MipLevels = sd.ArraySize = 1;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.CPUAccessFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, NULL, &flat))) {
            *why = "no resolve texture";
            goto done;
        }
        ctx->ResolveSubresource(flat, 0, bb, 0, d.Format);
        src = flat;
    }
    sd = d;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.MipLevels = sd.ArraySize = 1;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&sd, NULL, &stage))) {
        *why = "no staging texture";
        goto done;
    }
    ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, src, 0, NULL);
    if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        *why = "the copy could not be read";
        goto done;
    }
    bgr = (BYTE *)malloc((size_t)d.Width * d.Height * 3);
    if (bgr) {
        for (unsigned y = 0; y < d.Height; y++) {
            const BYTE *in = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
            BYTE *out = bgr + (size_t)y * d.Width * 3;
            for (unsigned x = 0; x < d.Width; x++, in += 4, out += 3) {
                if (order == 0) {
                    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
                } else if (order == 1) {
                    out[0] = in[2]; out[1] = in[1]; out[2] = in[0];
                } else {
                    UINT32 v;
                    memcpy(&v, in, 4);
                    out[2] = (BYTE)((v & 0x3FF) >> 2);
                    out[1] = (BYTE)(((v >> 10) & 0x3FF) >> 2);
                    out[0] = (BYTE)(((v >> 20) & 0x3FF) >> 2);
                }
            }
        }
    }
    ctx->Unmap(stage, 0);
    if (!bgr)
        *why = "out of memory";
    *pw = d.Width;
    *ph = d.Height;
done:
    release(stage);
    release(flat);
    release(bb);
    release(res);
    return bgr;
}

static void take_shot(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                      ID3D11RenderTargetView *rtv)
{
    const char *why = NULL;
    unsigned w = 0, h = 0;
    BYTE *bgr = grab_back_buffer(dev, ctx, rtv, &w, &h, &why);

    if (bgr) {
        Shot *s = new Shot();
        s->bgr = bgr;
        s->w = w;
        s->h = h;
        if (!shot_path(s)) {
            why = "no free file name in Photos";
        } else {
            HANDLE t = CreateThread(NULL, 0, write_shot, s, 0, NULL);
            if (t) {
                CloseHandle(t);
                s = NULL;               /* the thread's now */
            } else {
                why = "no writer thread";
            }
        }
        if (s) {
            free(s->bgr);
            delete s;
        }
    }
    if (why) {
        char msg[160];
        snprintf(msg, sizeof(msg), "The photo could not be taken (%s)", why);
        shot_message(msg);
        fprintf(stderr, "[PHOTO] no photo: %s\n", why);
    }
}

/* ---- the photo panel ------------------------------------------------------- */

/* Start or Tab in photo mode: zoom, roll, speed, the HUD and Conker, depth
 * of field, and the keys. Every change shows at once (the camera holds
 * still while the panel is open); fly speed and blur strength go to
 * settings.ini when let go. */
/* The panel's picture toggles (to-do 32, SPEC 10.1): photo mode's own pop-in
 * fix, haze, vignette, screen drops and CRT. They go straight to the same
 * setters as F2's, never to F2's settings, which come back when photo mode
 * ends; "Save photo settings" keeps them (photo_picture, one line in
 * settings.ini; empty = photo mode starts from F2's). Executor thread. */
struct PhotoPicture {
    bool popin, grass, chars, fur, particles, haze, vignette, drops;
    float density, crt_strength;
    int crt;                        /* xbox_SetCrt's order: off, scanlines, crt */
    int cut_blur;                   /* photo_cut_blur: -1 as F2 says */
};
static PhotoPicture s_pp;
static bool s_pp_on;                /* photo mode's are in force */
static bool s_pp_f2_open;

static bool on_setting(const char *key)
{
    return strcmp(settings_get(key), "0") != 0;
}

static int crt_setting(void)
{
    const char *v = settings_get("crt");
    return !_stricmp(v, "scanlines") ? 1 : !_stricmp(v, "crt") ? 2 : 0;
}

static void pp_from_settings(PhotoPicture *p)
{
    p->popin = on_setting("grass_full_detail");
    p->grass = on_setting("popin_grass");
    p->chars = on_setting("popin_characters");
    p->fur = on_setting("popin_fur");
    p->particles = on_setting("popin_particles");
    p->density = slide_value(slide_for("particle_density"));
    p->haze = on_setting("screen_haze");
    p->vignette = on_setting("vignette");
    p->drops = on_setting("screen_drops");
    p->crt = crt_setting();
    p->crt_strength = slide_value(slide_for("crt_strength"));
    p->cut_blur = -1;
}

static float pp_field(const char *line, const char *name, float def)
{
    char key[24];
    const char *at;

    snprintf(key, sizeof(key), "%s=", name);
    at = strstr(line, key);
    return at ? (float)atof(at + strlen(key)) : def;
}

/* Over F2's values, so a line from an older version still works. */
static void pp_from_saved(PhotoPicture *p, const char *line)
{
    p->popin = pp_field(line, "popin", p->popin) != 0.0f;
    p->grass = pp_field(line, "grass", p->grass) != 0.0f;
    p->chars = pp_field(line, "chars", p->chars) != 0.0f;
    p->fur = pp_field(line, "fur", p->fur) != 0.0f;
    p->particles = pp_field(line, "particles", p->particles) != 0.0f;
    p->density = pp_field(line, "density", p->density);
    p->haze = pp_field(line, "haze", p->haze) != 0.0f;
    p->vignette = pp_field(line, "vignette", p->vignette) != 0.0f;
    p->drops = pp_field(line, "drops", p->drops) != 0.0f;
    p->crt = (int)pp_field(line, "crt", (float)p->crt);
    p->crt_strength = pp_field(line, "strength", p->crt_strength);
    p->cut_blur = (int)pp_field(line, "blur", (float)p->cut_blur);
}

static void pp_save(const PhotoPicture *p)
{
    char line[160];

    snprintf(line, sizeof(line), "popin=%d grass=%d chars=%d fur=%d particles=%d "
             "density=%.0f haze=%d vignette=%d drops=%d crt=%d strength=%.0f blur=%d",
             p->popin, p->grass, p->chars, p->fur, p->particles, p->density,
             p->haze, p->vignette, p->drops, p->crt, p->crt_strength, p->cut_blur);
    save("photo_picture", line);
}

/* As F2 does it: a setting the environment overrides stays with that. */
static void pp_apply(const PhotoPicture *p)
{
    if (!settings_env_override("grass_full_detail"))
        conker_SetGrassFull(p->popin);
    if (!settings_env_override("popin_grass"))
        conker_SetPopinPart(0, p->grass);
    if (!settings_env_override("popin_characters"))
        conker_SetPopinPart(1, p->chars);
    if (!settings_env_override("popin_fur"))
        conker_SetPopinPart(2, p->fur);
    if (!settings_env_override("popin_particles"))
        conker_SetPopinPart(3, p->particles);
    if (!settings_env_override("particle_density"))
        conker_SetParticleDensity(p->density);
    if (!settings_env_override("screen_haze"))
        conker_SetScreenHaze(p->haze);
    if (!settings_env_override("vignette"))
        conker_SetVignette(p->vignette);
    if (!settings_env_override("screen_drops"))
        conker_SetScreenDrops(p->drops);
    xbox_SetCrt(settings_env_override("crt") ? -1 : p->crt,
                settings_env_override("crt_strength") ? -1.0f : p->crt_strength);
}

/* Each present: photo mode's picture in when it starts, F2's back when it
 * ends; F2 changed meanwhile (that menu applies at once): photo mode's again
 * when it closes; B changed the blur: kept when saving. */
static void follow_photo_picture(void)
{
    bool active = photo_active() != 0;
    bool f2 = s_open && s_menu == MENU_SETTINGS;

    if (active != s_pp_on) {
        PhotoPicture p;
        s_pp_on = active;
        pp_from_settings(&p);
        if (active) {
            const char *line = settings_get("photo_picture");
            if (line[0]) {
                pp_from_saved(&p, line);
                photo_set_cut_blur(p.cut_blur);
                fprintf(stderr, "[PHOTO] saved picture settings: %s\n", line);
            }
            s_pp = p;
        }
        pp_apply(&p);
    } else if (active) {
        if (s_pp_f2_open && !f2)
            pp_apply(&s_pp);
        if (s_pp.cut_blur != photo_cut_blur()) {
            s_pp.cut_blur = photo_cut_blur();
            if (settings_get("photo_picture")[0])
                pp_save(&s_pp);
        }
    }
    s_pp_f2_open = f2;
}

/* The panel's section. */
static void build_photo_picture(float right, float gap)
{
    static const char *const crt_names[] = { "Off", "Scanlines", "CRT" };
    bool keep = settings_get("photo_picture")[0] != 0;
    bool changed = false, moved = false, done = false;

    (void)gap;
    ImGui::SeparatorText("Picture");
    note("Only in photo mode: your own settings stay as they are.");
    changed |= ImGui::Checkbox("Pop-in Fix", &s_pp.popin);
    if (s_pp.popin) {
        ImGui::Indent();
        changed |= ImGui::Checkbox("Grass & flowers", &s_pp.grass);
        changed |= ImGui::Checkbox("Characters", &s_pp.chars);
        changed |= ImGui::Checkbox("Fur", &s_pp.fur);
        changed |= ImGui::Checkbox("Ambient Particles (experimental)", &s_pp.particles);
        if (s_pp.particles) {
            ImGui::TextUnformatted("Particle density");
            ImGui::SetNextItemWidth(right);
            moved |= ImGui::SliderFloat("##pp_density", &s_pp.density, 0.0f, 100.0f,
                                        "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
            done |= ImGui::IsItemDeactivatedAfterEdit();
        }
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Haze", &s_pp.haze);
    changed |= ImGui::Checkbox("Vignette", &s_pp.vignette);
    changed |= ImGui::Checkbox("Screen drops", &s_pp.drops);
    ImGui::TextUnformatted("CRT / scanlines");
    ImGui::SetNextItemWidth(right);
    changed |= ImGui::Combo("##pp_crt", &s_pp.crt, crt_names, 3);
    if (s_pp.crt) {
        ImGui::TextUnformatted("CRT strength");
        ImGui::SetNextItemWidth(right);
        moved |= ImGui::SliderFloat("##pp_crt_strength", &s_pp.crt_strength, 0.0f,
                                    100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
        done |= ImGui::IsItemDeactivatedAfterEdit();
    }
    if (changed || moved)
        pp_apply(&s_pp);
    if (ImGui::Checkbox("Save photo settings", &keep)) {
        if (keep)
            pp_save(&s_pp);
        else
            save("photo_picture", "");
    } else if (keep && (changed || done)) {
        pp_save(&s_pp);
    }
    ImGui::SetItemTooltip("Photo mode starts with these from now on, also after "
                          "a restart. Off: it starts from your settings.");
}

static void build_photo(unsigned w, unsigned h, bool *close)
{
    PhotoOptions *o = photo_options();
    float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    float right = -(ImGui::GetFrameHeight() + gap);
    char msg[160], v[32];
    bool b;

    (void)close;
    place_window(MENU_PHOTO, "photo_window", w, h, 22.0f, 0.0f);
    ImGui::Begin("Photo mode", NULL,
                 ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Take a photo"))
        overlay_photo_shot();
    /* Where photos go (user 2026-10-29): another folder, or Explorer on it. */
    take_picked_folder();
    ImGui::BeginDisabled(s_picking != 0);
    if (ImGui::Button("Change Photo Directory"))
        start_folder_job(true);
    ImGui::EndDisabled();
    {
        const char *open = "Open Photo Directory";
        float bw = ImGui::CalcTextSize(open).x
                 + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetContentRegionAvail().x
            >= ImGui::GetItemRectSize().x + gap * 2.0f + bw)
            ImGui::SameLine(0.0f, gap);
        if (ImGui::Button(open))
            start_folder_job(false);
    }
    if (settings_get("photo_folder")[0]) {
        ImGui::SameLine(0.0f, gap);
        if (reset_icon("##reset_folder",
                       "Back to the default (Photos in the save folder)"))
            save("photo_folder", "");
    }
    {
        wchar_t dir[MAX_PATH];
        char shown[MAX_PATH], line[MAX_PATH + 16];
        photo_dir(dir, MAX_PATH);
        if (WideCharToMultiByte(CP_UTF8, 0, dir, -1, shown, sizeof(shown),
                                NULL, NULL) > 0) {
            snprintf(line, sizeof(line), "Photos go to %s", shown);
            note(line);
        }
    }
    /* Under the buttons, so they stay put when it comes and goes. */
    if (shot_message_now(msg, sizeof(msg)))
        note(msg);

    ImGui::SeparatorText("Camera");
    ImGui::TextUnformatted("Zoom (field of view)");
    ImGui::SetNextItemWidth(right);
    ImGui::SliderFloat("##fov", &o->fov, 5.0f, 100.0f, "%.0f degrees",
                       ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine(0.0f, gap);
    snprintf(msg, sizeof(msg), "Back to the game's (%.0f degrees)",
             o->fov_game);
    if (reset_icon("##reset_fov", msg))
        o->fov = o->fov_game;
    ImGui::TextUnformatted("Roll");
    ImGui::SetNextItemWidth(right);
    ImGui::SliderAngle("##roll", &o->roll, -180.0f, 180.0f, "%.0f degrees",
                       ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine(0.0f, gap);
    if (reset_icon("##reset_roll", "Level (0 degrees)"))
        o->roll = 0.0f;
    ImGui::TextUnformatted("Fly speed");
    ImGui::SetNextItemWidth(right);
    ImGui::SliderFloat("##speed", &o->speed, 25.0f, 400.0f, "%.0f %%",
                       ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        snprintf(v, sizeof(v), "%.0f", o->speed);
        save("photo_speed", v);
    }
    ImGui::SameLine(0.0f, gap);
    if (reset_icon("##reset_speed", "Back to the default (100 %)")) {
        o->speed = 100.0f;
        save("photo_speed", "100");
    }
    if (ImGui::Button("Camera back to the start"))
        photo_ask_reset();

    ImGui::SeparatorText("Show");
    b = o->hud != 0;
    if (ImGui::Checkbox("HUD and speech bubbles", &b))
        o->hud = b;
    b = o->hide_conker != 0;
    if (ImGui::Checkbox("Hide Conker", &b))
        o->hide_conker = b;

    /* Off / the F2 choice ("updated" if that is off), photo mode only. */
    b = photo_cut_blur() >= 0 ? photo_cut_blur() != 0 : conker_CutsceneBlurOn();
    if (ImGui::Checkbox("Cutscene blur (the game's own)", &b))
        photo_toggle_cut_blur();

    ImGui::SeparatorText("Depth of field");
    b = o->dof != 0;
    if (ImGui::Checkbox("Depth of field (blur away from the focus)", &b))
        o->dof = b;
    ImGui::BeginDisabled(!o->dof);
    ImGui::TextUnformatted("Focus distance");
    ImGui::SetNextItemWidth(right);
    ImGui::SliderFloat("##focus", &o->focus, 10.0f, 20000.0f, "%.0f",
                       ImGuiSliderFlags_AlwaysClamp
                       | ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("Focus on Conker"))
        photo_ask_focus_conker();
    note("Game units: Conker is about 60 tall.");
    ImGui::TextUnformatted("Blur strength");
    ImGui::SetNextItemWidth(right);
    ImGui::SliderFloat("##blur", &o->blur, 0.0f, 100.0f, "%.0f %%",
                       ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        snprintf(v, sizeof(v), "%.0f", o->blur);
        save("photo_blur", v);
    }
    ImGui::SameLine(0.0f, gap);
    if (reset_icon("##reset_blur", "Back to the default (50 %)")) {
        o->blur = 50.0f;
        save("photo_blur", "50");
    }
    ImGui::EndDisabled();

    build_photo_picture(right, gap);

    if (ImGui::CollapsingHeader("Keys")) {
        if (ImGui::BeginTable("##photo_keys", 2,
                              ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingStretchProp)) {
            for (int i = PHOTO_FORWARD; i < PHOTO_ROWS; i++) {
                const ControlAction *a = controls_action(CONTROLS_PHOTO, i);
                const char *k = controls_key_label(controls_key(CONTROLS_PHOTO,
                                                                i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", a->label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s%s%s", k[0] ? k : "", k[0] ? " / " : "",
                                   a->button);
            }
            ImGui::EndTable();
        }
        note("F3 > Photo mode changes the keys.");
    }
    note("Start, Tab, B or Esc closes the panel.");
    remember_window("photo_window", w, h);
    ImGui::End();
}

/* ---- Report Bug (to-do 12) ------------------------------------------------- */

/* F10 or F2's "Report Bug..." (SPEC 9.2 "Report Bug"). report_begin freezes
 * the game as the pause key does where photo mode can (unless it is frozen
 * already) and report_end lets it go; the picture of the moment was grabbed
 * just before (report_grab, at the top of the hook: no menu on it). The
 * window takes a description and whether the saves go in; Create Report
 * hands them to a thread of its own (report_job: the PNG, bug_report_write,
 * bug_report_issue) so the picture keeps drawing, and the window cannot
 * close meanwhile. Then the zip's name, Open Folder (Explorer with it
 * selected), Open GitHub Issue (the prefilled new-issue page, its text put
 * on the clipboard first) and Close, each shell call on a thread of its
 * own too. Executor thread, except the jobs; their results come through
 * s_lock. */
enum { REPORT_EDIT, REPORT_WORKING, REPORT_DONE, REPORT_FAILED };
static volatile LONG s_report_stage;
static LONG s_report_shown = -1;  /* the stage the window last drew */
static bool s_report_up;          /* report_begin ran, report_end not yet */
static bool s_report_paused;      /* report_begin froze the game */
static bool s_report_saves = true;
static char s_report_desc[4096];  /* UTF-8; kept after Cancel, emptied once
                                   * a report was made */
/* The picture (BGR rows), read by the job while s_report_busy: freed only
 * when no job runs (the window cannot close then). */
static BYTE *s_report_px;
static unsigned s_report_w, s_report_h;
/* The job's results (s_lock): the zip's full path or why it failed; the
 * issue page and its text (malloc'd, NULL when they could not be made). */
static char s_report_zip[MAX_PATH * 2];
static char *s_report_url, *s_report_text;
static char s_report_note[200];   /* what the last button did */

static void report_grab(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                        ID3D11RenderTargetView *rtv)
{
    const char *why = NULL;

    if (s_report_busy)
        return;                     /* never: the window is up meanwhile */
    free(s_report_px);
    s_report_px = grab_back_buffer(dev, ctx, rtv, &s_report_w, &s_report_h,
                                   &why);
    if (!s_report_px)
        fprintf(stderr, "[REPORT] no picture: %s\n", why ? why : "?");
}

static void report_begin(void)
{
    s_report_up = true;
    s_report_shown = -1;
    s_report_unfocus = false;
    if (s_report_stage == REPORT_DONE)
        s_report_desc[0] = '\0';
    if (!s_report_busy)
        InterlockedExchange(&s_report_stage, REPORT_EDIT);
    s_report_saves = true;
    s_report_note[0] = '\0';
    /* Frozen as under P (to-do 117), where photo mode can freeze; refused
     * elsewhere, and the game runs on under the window as under F2. */
    if (!photo_frozen() && !s_want_pause) {
        want_pause(true);
        s_report_paused = true;
    }
    fprintf(stderr, "[REPORT] window open (%s; picture %ux%u%s)\n",
            s_report_by_key ? "its key" : "F2's button",
            s_report_px ? s_report_w : 0, s_report_px ? s_report_h : 0,
            s_report_px ? "" : ", none");
}

static void report_end(void)
{
    if (!s_report_up)
        return;
    s_report_up = false;
    if (!s_report_busy) {
        free(s_report_px);
        s_report_px = NULL;
    }
    if (s_report_paused) {
        s_report_paused = false;
        if (s_want_pause)
            want_pause(false);
    }
    fprintf(stderr, "[REPORT] window closed\n");
}

/* report.txt's game state lines. */
static void report_extra(char *out, size_t n)
{
    const char *audio = !decensor_available(0) ? "no patch"
        : strcmp(settings_get("uncensored_audio"), "0") ? "on" : "off";
    const char *text = !decensor_available(1) ? "no patch"
        : strcmp(settings_get("uncensored_text"), "0") ? "on" : "off";
    snprintf(out, n, "Uncensored audio: %s\r\nUncensored text: %s\r\n"
             "Debug mode: %s\r\n", audio, text,
             settings_debug() ? "on (--debug)" : "off");
}

struct ReportJob {
    const BYTE *px;               /* s_report_px, or NULL */
    unsigned w, h;
    char save[MAX_PATH * 2];
    char *desc;
    int saves;
    char extra[256];
};

static DWORD WINAPI report_job(void *arg)
{
    ReportJob *j = (ReportJob *)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    char shot[MAX_PATH * 2] = "", full[MAX_PATH * 2], zip[MAX_PATH * 2];
    char *url = NULL, *text = NULL;
    BugReport r;
    int rc;

    /* The picture as a PNG beside the reports; zipped, then deleted. */
    if (j->px) {
        wchar_t wpath[MAX_PATH * 2];
        HRESULT hr = E_FAIL;
        if (!GetFullPathNameA(j->save, sizeof(full), full, NULL))
            snprintf(full, sizeof(full), "%s", j->save);
        snprintf(shot, sizeof(shot), "%s\\reports", full);
        SHCreateDirectoryExA(NULL, shot, NULL);
        snprintf(shot, sizeof(shot), "%s\\reports\\screenshot_%lu.png", full,
                 (unsigned long)GetCurrentProcessId());
        if (MultiByteToWideChar(CP_ACP, 0, shot, -1, wpath, MAX_PATH * 2) > 0)
            hr = write_png(wpath, j->px, j->w, j->h);
        if (FAILED(hr)) {
            fprintf(stderr, "[REPORT] the picture could not be written: "
                    "0x%08lX\n", (unsigned long)hr);
            shot[0] = '\0';
        }
    }
    memset(&r, 0, sizeof(r));
    r.save_dir = j->save;
    r.description = j->desc;
    r.shot_png = shot[0] ? shot : NULL;
    r.include_saves = j->saves;
    r.origin = "F10 in the game";
    r.extra = j->extra;
    rc = bug_report_write(&r, zip, sizeof(zip));
    if (shot[0])
        DeleteFileA(shot);
    if (rc == 0 && bug_report_issue(j->desc, zip, &url, &text) != 0)
        fprintf(stderr, "[REPORT] no issue text (out of memory)\n");
    if (rc != 0)
        fprintf(stderr, "[REPORT] failed: %s\n", zip);
    else if (url)
        fprintf(stderr, "[REPORT] issue page: %u characters\n",
                (unsigned)strlen(url));

    EnterCriticalSection(&s_lock);
    snprintf(s_report_zip, sizeof(s_report_zip), "%s", zip);
    free(s_report_url);
    free(s_report_text);
    s_report_url = url;
    s_report_text = text;
    LeaveCriticalSection(&s_lock);
    InterlockedExchange(&s_report_stage, rc ? REPORT_FAILED : REPORT_DONE);
    InterlockedExchange(&s_report_busy, 0);
    free(j->desc);
    delete j;
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

static void report_create(void)
{
    ReportJob *j = new ReportJob();
    HANDLE t;

    j->px = s_report_px;
    j->w = s_report_w;
    j->h = s_report_h;
    snprintf(j->save, sizeof(j->save), "%s", settings_save_dir());
    j->desc = _strdup(s_report_desc);
    j->saves = s_report_saves ? 1 : 0;
    report_extra(j->extra, sizeof(j->extra));
    fprintf(stderr, "[REPORT] creating (description %u bytes, saves %s, "
            "picture %s)\n", (unsigned)strlen(s_report_desc),
            s_report_saves ? "in" : "out", s_report_px ? "in" : "none");
    InterlockedExchange(&s_report_busy, 1);
    InterlockedExchange(&s_report_stage, REPORT_WORKING);
    t = CreateThread(NULL, 0, report_job, j, 0, NULL);
    if (t) {
        CloseHandle(t);
        return;
    }
    free(j->desc);
    delete j;
    EnterCriticalSection(&s_lock);
    snprintf(s_report_zip, sizeof(s_report_zip), "no thread to write it");
    LeaveCriticalSection(&s_lock);
    InterlockedExchange(&s_report_stage, REPORT_FAILED);
    InterlockedExchange(&s_report_busy, 0);
}

static void report_open_issue(void)
{
    int rc;

    EnterCriticalSection(&s_lock);
    rc = bug_report_open_issue(s_report_url, s_report_text, s_hwnd);
    LeaveCriticalSection(&s_lock);
    snprintf(s_report_note, sizeof(s_report_note), "%s",
             rc == 0 ? "Opening the issue page in your browser; its text is on "
                       "the clipboard too."
                     : "The issue page could not be opened.");
}

/* The window, in the middle of the picture. On a stage's first frame the
 * keyboard / pad cursor goes to its main item: the description when the
 * key opened it (to type at once), else Create Report; Open GitHub Issue;
 * Back. */
static void build_report(unsigned w, unsigned h, bool *close)
{
    float px = ImGui::GetFontSize();
    float wide = px * 26.0f;
    LONG stage = s_report_stage;
    bool fresh = stage != s_report_shown;
    char zip[MAX_PATH * 2];

    s_report_shown = stage;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (s_focus) {
        ImGui::SetNextWindowFocus();
        s_focus = false;
    }
    ImGui::Begin("Report Bug", NULL,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wide);
    EnterCriticalSection(&s_lock);
    snprintf(zip, sizeof(zip), "%s", s_report_zip);
    LeaveCriticalSection(&s_lock);

    if (stage == REPORT_EDIT) {
        ImGui::TextUnformatted("What went wrong, and what were you doing? "
                               "(optional)");
        /* Esc / B in the box: it lets go, the text kept (drain_events). */
        if (s_report_unfocus) {
            ImGui::ClearActiveID();
            s_report_unfocus = false;
        }
        if (fresh && s_report_by_key)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##description", s_report_desc,
                                  sizeof(s_report_desc),
                                  ImVec2(wide, ImGui::GetTextLineHeight()
                                               * 8.0f));
        ImGui::Checkbox("Include save files", &s_report_saves);
        note("Makes a .zip to attach to a GitHub issue: this picture, the "
             "game's last logs, its settings, the saves when ticked and the "
             "PC's details (Windows, graphics card, processor). The Windows "
             "user name is left out.");
        if (fresh && !s_report_by_key)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Create Report"))
            report_create();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            *close = true;
    } else if (stage == REPORT_WORKING) {
        static const char *const dots[] = { "", ".", "..", "..." };
        ImGui::Text("Creating the report%s",
                    dots[(int)(ImGui::GetTime() * 3.0) % 4]);
    } else if (stage == REPORT_DONE) {
        const char *name = strrchr(zip, '\\');
        ImGui::Text("Report saved: %s", name ? name + 1 : zip);
        if (ImGui::Button("Open Folder"))
            bug_report_open_folder(zip);
        ImGui::SameLine();
        if (fresh)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Open GitHub Issue"))
            report_open_issue();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            *close = true;
        note("Open GitHub Issue opens a new issue with its text filled in "
             "and copies that text too: if the page comes up empty, paste "
             "it there (Ctrl+V). Then drag the zip into the issue. Posting "
             "needs a free GitHub account.");
        if (s_report_note[0])
            ImGui::TextUnformatted(s_report_note);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "The report could not be made:");
        ImGui::TextUnformatted(zip);
        if (fresh)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Back"))
            InterlockedExchange(&s_report_stage, REPORT_EDIT);
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            *close = true;
    }
    s_item_active = ImGui::IsAnyItemActive();
    ImGui::PopTextWrapPos();
    ImGui::End();
}

/* The pause key with the menu closed: a small note in the corner. It takes
 * no input and never the focus. Photo mode's note likewise, with its main
 * keys and the last photo's outcome. */
static void pause_label(unsigned h, bool photo)
{
    float m = h * 0.03f;
    float y = s_fps_bottom > 0.0f ? s_fps_bottom + h * 0.01f : m;
    ImGui::SetNextWindowPos(ImVec2(m, y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##paused", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                 | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoFocusOnAppearing
                 | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_AlwaysAutoResize);
    if (photo && !s_paused) {
        char msg[160];
        ImGui::Text("Photo mode   Photo: %s / A   Settings: %s / Start   "
                    "Leave: %s / Back + RB",
                    controls_key_label(controls_key(CONTROLS_PHOTO, PHOTO_TAKE)),
                    controls_key_label(controls_key(CONTROLS_PHOTO, PHOTO_PANEL)),
                    controls_key_label(controls_key(CONTROLS_PHOTO, PHOTO_LEAVE)));
        if (photo_world_resumed())
            ImGui::Text("The world is running   Pause it: %s / Back + A",
                        controls_key_label(controls_key(CONTROLS_PHOTO, PHOTO_RESUME)));
        if (shot_message_now(msg, sizeof(msg)))
            ImGui::TextUnformatted(msg);
    } else
        ImGui::Text("Paused (%s to resume)",
                    controls_key_label(controls_pause_key()));
    ImGui::End();
}

/* F1: the frame rate in the top left corner, like the pause label (which
 * then goes under it). Level 1 the fps alone, 2 with the frame times. */
static void fps_label(unsigned h)
{
    float m = h * 0.015f;
    ImGui::SetNextWindowPos(ImVec2(m, m), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                 | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoFocusOnAppearing
                 | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_AlwaysAutoResize);
    if (!s_fps_have)
        ImGui::TextUnformatted("-- fps");
    else if (s_fps == 1)
        ImGui::Text("%.0f fps", s_fps_rate);
    else
        ImGui::Text("%.1f fps  %.1f ms  (max %.1f)", s_fps_rate, s_fps_ms,
                    s_fps_long);
    s_fps_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
}

/* Conker's tail along the ring (to-do 16, the user's photo): the first
 * 10 % white like its tip, then 40 % yellow, the last 50 % orange, with a
 * short blend at each change. f = 0..1 round the circle; alpha 0..255. */
static ImU32 tail_colour(float f, int alpha = 255)
{
    static const float tip[3] = { 255, 246, 208 }, yel[3] = { 247, 210, 62 },
                       org[3] = { 238, 122, 18 };
    const float bl = 0.04f;
    const float *a = tip, *b = yel;
    float k;

    if (f < 0.1f - bl)
        k = 0.0f;
    else if (f < 0.1f + bl)
        k = (f - (0.1f - bl)) / (2.0f * bl);
    else if (f < 0.5f - bl)
        k = 1.0f;
    else {
        a = yel;
        b = org;
        k = f < 0.5f + bl ? (f - (0.5f - bl)) / (2.0f * bl) : 1.0f;
    }
    return IM_COL32((int)(a[0] + (b[0] - a[0]) * k),
                    (int)(a[1] + (b[1] - a[1]) * k),
                    (int)(a[2] + (b[2] - a[2]) * k), alpha);
}

/* Tail look (F2 Cutscenes "Tail-shaped ring", --debug only): fur strands along the ring, leaning back towards the tip as
 * on Conker's tail, longer outside than inside; each strand's length is
 * fixed by its number, so nothing shimmers. tw(f) = the stroke's width. */
static void skip_ring_fur(ImDrawList *dl, ImVec2 c, float r, float th, float p)
{
    const float pi = 3.14159265f;
    const int strands = 84;             /* a full circle, per side */
    const float half = pi / strands * 0.75f;

    for (int i = 0; i < strands; i++) {
        float f = (i + 0.5f) / strands;
        if (f > p)
            break;
        float rnd = (float)(((unsigned)i * 2654435761u >> 16) & 255) / 255.0f;
        float a = -pi * 0.5f + f * 2.0f * pi;
        float tw = th * (0.55f + 0.9f * f);
        ImU32 col = tail_colour(f);
        for (int side = 0; side < 2; side++) {
            float s = side ? -1.0f : 1.0f;
            float len = tw * (side ? 0.4f : 0.85f) * (0.6f + 0.7f * rnd);
            float r0 = r + s * tw * 0.4f, rt = r0 + s * len;
            float at = a - 0.8f * len / r;      /* lean back, towards the tip */
            dl->AddTriangleFilled(ImVec2(c.x + cosf(a - half) * r0, c.y + sinf(a - half) * r0),
                                  ImVec2(c.x + cosf(a + half) * r0, c.y + sinf(a + half) * r0),
                                  ImVec2(c.x + cosf(at) * rt, c.y + sinf(at) * rt), col);
        }
    }
}

/* The hold-to-skip ring (conker_SkipRing, recomp_manual.c): a hollow
 * circle in the bottom right corner that draws itself clockwise from
 * 12 o'clock as the skip button is held, with a soft glow of its own
 * colours fading out from it (user 2026-11-04: no dark edge). Sized by the
 * picture's height. */
/* One band of the ring, 12 o'clock to p round, as a single triangle strip
 * with a colour per corner (no joins to show): from d0 to d1 off the
 * ring's edge (outward, or inward with sign -1; the body from its middle),
 * alpha a0 to a1 across it. tail: the width grows from the tip. */
static void ring_band(ImDrawList *dl, ImVec2 c, float r, float th, float p,
                      bool tail, float sign, float d0, float d1, int a0, int a1)
{
    const float pi = 3.14159265f;
    const int segs = 120;               /* a full circle */
    int n = (int)(p * segs + 0.999f), i;
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

    if (n < 1)
        return;
    dl->PrimReserve(n * 6, (n + 1) * 2);
    unsigned base = dl->_VtxCurrentIdx;
    for (i = 0; i <= n; i++) {
        float f = i < n ? (float)i / segs : p;
        float a = -pi * 0.5f + f * 2.0f * pi;
        float half = (tail ? th * (0.55f + 0.9f * f) : th) * 0.5f;
        float ra = r + sign * (half + d0), rb = r + sign * (half + d1);
        if (d0 < 0.0f)                  /* the body: across its middle */
            ra = r - half, rb = r + half;
        dl->PrimWriteVtx(ImVec2(c.x + cosf(a) * ra, c.y + sinf(a) * ra), uv,
                         tail_colour(f, a0));
        dl->PrimWriteVtx(ImVec2(c.x + cosf(a) * rb, c.y + sinf(a) * rb), uv,
                         tail_colour(f, a1));
    }
    for (i = 0; i < n; i++) {
        unsigned v = base + (unsigned)i * 2;
        dl->PrimWriteIdx((ImDrawIdx)v);
        dl->PrimWriteIdx((ImDrawIdx)(v + 1));
        dl->PrimWriteIdx((ImDrawIdx)(v + 3));
        dl->PrimWriteIdx((ImDrawIdx)v);
        dl->PrimWriteIdx((ImDrawIdx)(v + 3));
        dl->PrimWriteIdx((ImDrawIdx)(v + 2));
    }
}

static void skip_ring(unsigned w, unsigned h, float p)
{
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float r = h * 0.03f, th = h * 0.009f, m = h * 0.05f, glow;
    ImVec2 c((float)w - m - r, (float)h - m - r);
    bool tail = settings_debug() && strcmp(settings_get("skip_ring_tail"), "0");

    if (th < 2.0f)
        th = 2.0f;
    glow = th * 0.8f;                   /* 1.6 was too wide (user 2026-11-04) */
    /* The soft edge: the ring's own colours fading out both ways (110 at
     * the ring was too strong, user 2026-11-04). */
    ring_band(dl, c, r, th, p, tail, 1.0f, 0.0f, glow, 50, 0);
    ring_band(dl, c, r, th, p, tail, -1.0f, 0.0f, glow * 0.8f, 50, 0);
    if (tail)
        skip_ring_fur(dl, c, r, th, p);
    ring_band(dl, c, r, th, p, tail, 1.0f, -1.0f, 0.0f, 255, 255);
}

/* --precompile-shaders: B on any controller stops the pass as Esc does
 * (user 2026-11-07: any controller here only; the menus and the quit
 * question stay slot 1's). A B already held when a controller is first
 * seen does not count. Executor thread, once a present. */
static void precompile_pad_stop(void)
{
    static WORD prev[XBOX_INPUT_HOST_PADS];
    static bool seen[XBOX_INPUT_HOST_PADS], stopped;
    XINPUT_STATE st;
    DWORD slot;

    if (stopped)
        return;
    for (slot = 0; slot < XBOX_INPUT_HOST_PADS; slot++) {
        WORD b;
        if (xbox_InputHostPadRead(slot, &st) != ERROR_SUCCESS) {
            seen[slot] = false;
            continue;
        }
        b = st.Gamepad.wButtons;
        if (seen[slot] && (b & XINPUT_GAMEPAD_B) && !(prev[slot] & XINPUT_GAMEPAD_B)) {
            stopped = true;
            fprintf(stderr, "[PRECOMPILE] B on controller %lu: stopped\n",
                    (unsigned long)slot + 1);
            precompile_stop();
            if (s_hwnd)
                PostMessageA(s_hwnd, WM_CLOSE, 0, 0);
            return;
        }
        seen[slot] = true;
        prev[slot] = b;
    }
}

/* --precompile-shaders (precompile.c): black over the game (the levels and
 * chapters it runs are not shown: no spoilers) and the pass's progress. */
static void precompile_screen(unsigned w, unsigned h)
{
    PrecompileStatus st;
    char line[160];

    precompile_status(&st);
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), ImVec2((float)w, (float)h), IM_COL32(0, 0, 0, 255));
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w * 0.7f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("##precompile", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                 | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings
                 | ImGuiWindowFlags_NoBackground);
    ImGui::TextWrapped("%s", st.resumed ? "Precompiling shaders (continuing "
                                          "where the last run stopped)"
                                        : "Precompiling shaders");
    ImGui::Spacing();
    ImGui::ProgressBar(st.progress, ImVec2(-1.0f, 0.0f));
    ImGui::Spacing();
    switch (st.stage) {
    case PRECOMPILE_BOOT:
        snprintf(line, sizeof line, st.resumed
                 ? "Starting the game, then skipping to where it stopped..."
                 : "Starting the game...");
        break;
    case PRECOMPILE_LEVELS:
        snprintf(line, sizeof line, "Levels: %d of %d", st.levels,
                 st.levels_total);
        break;
    case PRECOMPILE_CHAPTERS:
        snprintf(line, sizeof line, "Chapters: %d of %d (part %d of %d)%s",
                 st.chapter, st.chapters, st.task, st.tasks,
                 st.skipping ? ", skipping to where it stopped" : "");
        break;
    default:
        snprintf(line, sizeof line, "Finishing the last shaders...");
        break;
    }
    ImGui::TextUnformatted(line);
    snprintf(line, sizeof line, "Shaders compiled: %u    Time: %u:%02u",
             st.shaders, st.seconds / 60u, st.seconds % 60u);
    ImGui::TextUnformatted(line);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("The game runs every level and chapter behind this "
                       "screen with the sound off, so their shaders are ready "
                       "before you play. The game closes by itself when it is "
                       "done. Esc or B on a controller stops it; what is "
                       "compiled so far is kept, and the next run continues "
                       "from there.");
    ImGui::PopStyleColor();
    ImGui::End();
}

/* The menu, the pause label and the FPS counter, whichever are on. Nonzero
 * holds the flip: the game stays paused at this frame (F4). */
static int overlay_draw(void *dev_, void *ctx_, void *rtv_, unsigned w,
                        unsigned h)
{
    ID3D11Device *dev = (ID3D11Device *)dev_;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)ctx_;
    ID3D11RenderTargetView *rtv = (ID3D11RenderTargetView *)rtv_;
    ID3D11RenderTargetView *old_rtv = NULL;
    ID3D11DepthStencilView *old_dsv = NULL;
    LARGE_INTEGER now, freq;
    bool close = false;
    float ring, cover;

    InterlockedExchange(&s_bb_w, (LONG)w);
    InterlockedExchange(&s_bb_h, (LONG)h);
    if (!s_gpu_seen)
        InterlockedExchange(&s_gpu_seen, 1);

    if (!s_want_open && s_open)
        close_menu();
    follow_pause();
    follow_photo_picture();
    /* The keyboard and mouse are the game's with no menu up and the game
     * running. With a menu up the keys follow its focus as the pad does
     * (user 2026-10-28): a click outside it gives them back to the game;
     * the mouse stays free for the menu. */
    xbox_KbmSetActive(!s_paused && (!s_want_open || !s_pad_menu));
    xbox_KbmSetKeysOnly(s_want_open != 0);
    InterlockedExchange(&s_keys_to_game, s_want_open && s_open && !s_pad_menu);
    /* The photo first: the picture as it is, before any text of ours. */
    if (s_shot_want && InterlockedExchange(&s_shot_want, 0))
        take_shot(dev, ctx, rtv);
    /* Report Bug's picture likewise (F10 or F2's button asked for it). */
    if (s_report_shot_want && InterlockedExchange(&s_report_shot_want, 0))
        report_grab(dev, ctx, rtv);
    /* The photo panel goes with photo mode. */
    if (s_want_open == MENU_PHOTO && !photo_active())
        InterlockedCompareExchange(&s_want_open, MENU_NONE, MENU_PHOTO);
    if (!s_want_open && s_open)
        close_menu();
    /* A pre-compile pass: its screen only, nothing remembered. */
    const bool precompiling = precompile_on() != 0;
    if (!s_want_open && !precompiling)
        pad_opens_menu();
    if (!precompiling)
        remember_game_window();
    ring = conker_SkipRing();
    cover = conker_SkipCover();
    {
        float load = conker_LoadCover();    /* loading_screen = off */
        if (load > cover)
            cover = load;
    }
    if (!s_want_open && !s_paused && !s_fps && !photo_frozen() && ring <= 0.0f
        && cover <= 0.0f && !precompiling)
        return 0;
    if (!s_ready) {
        if (!init_imgui(dev, ctx, h)) {
            /* Leave F1, F2 and F4 dead rather than holding the game with
             * no menu. */
            xbox_SetGpuOverlay(NULL);
            xbox_SetWindowMessageHook(NULL);
            InterlockedExchange(&s_want_open, 0);
            InterlockedExchange(&s_want_pause, 0);
            InterlockedExchange(&s_want_fps, 0);
            follow_pause();
            return 0;
        }
        s_ready = true;
        QueryPerformanceCounter(&s_last_frame);
    }
    /* A new render resolution reached the back buffer: text and spacing
     * to the new height, the menu window back to its place. */
    if (h != s_style_h) {
        set_style(h);
        s_replace[MENU_SETTINGS] = s_replace[MENU_CONTROLS]
            = s_replace[MENU_PHOTO] = true;
    }
    if (s_want_open && !s_open)
        open_menu();
    else if (s_open && s_want_open != s_menu)
        switch_menu();

    ImGuiIO &io = ImGui::GetIO();
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    io.DeltaTime = (float)(now.QuadPart - s_last_frame.QuadPart)
                 / (float)freq.QuadPart;
    if (io.DeltaTime <= 0.0f || io.DeltaTime > 0.25f)
        io.DeltaTime = 1.0f / 60.0f;
    s_last_frame = now;
    io.DisplaySize = ImVec2((float)w, (float)h);

    if (s_open) {
        drain_events(&close);
        feed_pad(&close);
        /* Report Bug's window stays while its report is being written. */
        if (close && s_menu == MENU_REPORT && s_report_busy)
            close = false;
        if (close) {
            close_menu();
            if (!s_paused && !s_fps && !photo_frozen() && ring <= 0.0f
                && cover <= 0.0f)
                return 0;
            close = false;
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    s_popup_open = false;
    s_fps_bottom = 0.0f;
    if (s_fps)
        fps_label(h);
    /* After a skip of ours: black until what follows draws (recomp_manual.c
     * conker_SkipCover); under any menu. */
    if (cover > 0.0f)
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), ImVec2((float)w, (float)h),
            IM_COL32(0, 0, 0, (int)(cover * 255.0f)));
    if (ring > 0.0f)
        skip_ring(w, h, ring);
    if (precompiling) {
        precompile_pad_stop();
        precompile_screen(w, h);
    }
    if (s_open && s_menu == MENU_PHOTO)
        build_photo(w, h, &close);
    else if (s_open && s_menu == MENU_REPORT)
        build_report(w, h, &close);
    else if (s_open && s_menu == MENU_QUIT) {
        /* The question alone; its No (or Esc / B) closes it all. */
        ask_quit();
        if (!s_ask_quit)
            close = true;
    } else if (s_open)
        s_menu == MENU_CONTROLS ? build_controls(w, h, &close)
                                : build_menu(w, h, &close);
    else if (s_paused || photo_frozen())
        pause_label(h, photo_active() != 0);
    /* A click outside the menu takes its focus (ImGui's own rule), and the
     * pad goes with it. The quit question and Report Bug's window keep the
     * pad: they want an answer (the quit modal is not "focused" on its
     * first frame), and the game is frozen under the report. */
    if (s_open)
        pad_to_menu(s_menu == MENU_QUIT || s_menu == MENU_REPORT
                    || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow));
    ImGui::Render();

    /* The back end does not touch the render targets; we put back ours. */
    ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    ctx->OMSetRenderTargets(1, &rtv, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    if (old_rtv)
        old_rtv->Release();
    if (old_dsv)
        old_dsv->Release();

    /* A close from the menu's button: this frame still shows the menu; the
     * next present does not. */
    if (close)
        close_menu();
    return s_paused ? 1 : 0;
}

/* The back end's overlay hook: once per present, held ones too, executor
 * thread. Only this hook holds the flip, so a call after one that held is
 * the same frame again, not a present of the game's. */
static int overlay_frame(void *dev, void *ctx, void *rtv, unsigned w,
                         unsigned h)
{
    InterlockedExchange64(&s_last_present, (LONG64)GetTickCount64());
    follow_fps();
    fps_tick(!s_held);
    s_held = overlay_draw(dev, ctx, rtv, w, h) != 0;
    return s_held ? 1 : 0;
}

void overlay_init(void)
{
    InitializeCriticalSection(&s_lock);
    s_last_present = (LONG64)GetTickCount64();
    s_want_fps = settings_get("fps_counter")[0] - '0';
    xbox_SetWindowMessageHook(window_hook);
    xbox_SetGpuOverlay(overlay_frame);
}

/* --dump-settings (SPEC 9.2): the F2 and F3 menus as the launcher shows
 * them, as JSON to f: every settings.ini option ("options"), F2's entries
 * in order ("menu"), F3's ("controls": every tab in turn, "tab" entries
 * between them) and every key name with the label F3 shows ("keys"). The
 * menus are built once in an ImGui context of its own with no window or
 * renderer, at the defaults (settings_use_defaults) and without --debug, so
 * debug-only entries are left out. */
extern "C" void overlay_dump_settings(FILE *f)
{
    bool close = false;
    bool first = true;

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();

    fputs("{\n", f);
    settings_dump_options(f);
    fputs(",\n  \"menu\": [\n", f);
    s_dump = f;
    s_dump_first = true;
    build_menu(1920, 1080, &close);
    fputs("\n  ],\n  \"controls\": [\n", f);
    s_dump_first = true;
    build_controls(1920, 1080, &close);
    s_dump = NULL;
    fputs("\n  ],\n  \"keys\": [\n", f);
    /* Inputs as controls.h: virtual-key codes, the wheel 256 and 257. */
    for (int input = 0; input <= 257; input++) {
        const char *name = controls_key_name(input);
        if (!name[0])
            continue;
        fprintf(f, "%s    { \"name\": ", first ? "" : ",\n");
        first = false;
        settings_json_string(f, name);
        fputs(", \"label\": ", f);
        settings_json_string(f, controls_key_label(input));
        fputs(" }", f);
    }
    fputs("\n  ]\n}\n", f);
    fflush(f);

    ImGui::EndFrame();
    ImGui::DestroyContext();
}

/* --list-pads (SPEC 9.2): the eight controllers the Slots tab can pick,
 * by its values ("1"-"4" Windows' (XInput) controllers, "ps1"-"ps4"
 * PlayStation ones), named as F3 names them, and whether each is
 * connected now. The first look starts SDL's thread, which finds the
 * PlayStation pads a moment later; the second, after the toolkit's
 * one-second rescan, sees them. */
extern "C" void overlay_list_pads(FILE *f)
{
    char name[160];

    controls_pad_connected(0);
    Sleep(1500);
    fputs("{\n  \"pads\": [\n", f);
    for (int pad = 0; pad < CONTROLS_HOST_PADS; pad++) {
        pad_label(pad, name, sizeof(name));
        fprintf(f, "%s    { \"value\": \"%s%d\", \"name\": ", pad ? ",\n" : "",
                pad < CONTROLS_XINPUT_PADS ? "" : "ps",
                pad % CONTROLS_XINPUT_PADS + 1);
        settings_json_string(f, name);
        fprintf(f, ", \"connected\": %s }",
                controls_pad_connected(pad) ? "true" : "false");
    }
    fputs("\n  ]\n}\n", f);
    fflush(f);
}
