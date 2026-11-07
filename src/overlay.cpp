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
 * frame rate, the dip fix, grass detail, screen haze, vignette and screen
 * drops, conker_SetFrameRate, conker_SetDipFix, conker_SetGrassFull,
 * conker_SetScreenHaze, conker_SetVignette, conker_SetScreenDrops,
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
#include "backends/imgui_impl_dx11.h"

#include "settings.h"
#include "controls.h"
#include "photo_mode.h"
#include "decensor.h"

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
void conker_SetGrassFull(int on);
void conker_SetPopinPart(int part, int on);
void conker_SetParticleDensity(float percent);
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
/* Which menu: none, F2's settings, F3's controls or photo mode's panel. */
enum { MENU_NONE, MENU_SETTINGS, MENU_CONTROLS, MENU_PHOTO };
static volatile LONG s_want_open;   /* F2/F3 state; the executor follows it */
static volatile LONG s_want_pause;  /* the pause key's state (P; F4 before
                                     * photo mode), likewise */
static volatile LONG s_want_fps;    /* F1 state (0-2, fps_counter), likewise */
static volatile LONG s_keys_to_game; /* a menu is up without the focus: keys
                                      * pass on to the game (executor sets) */
static HWND volatile s_hwnd;        /* the frame window */
/* Setting a key in F3 (executor; the window hook leaves the pause and
 * photo keys alone meanwhile): this set's action, else -1. */
static int s_capture_set = -1;
static volatile int s_capture_idx = -1;
static volatile LONG s_bb_w = 640, s_bb_h = 480;   /* back buffer size */

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

/* ---- window thread ---------------------------------------------------------- */

static int window_hook(void *hwnd, unsigned m, uintptr_t w, intptr_t l,
                       intptr_t *result)
{
    float x = 0, y = 0;

    s_hwnd = (HWND)hwnd;   /* for Close game */
    if (!s_gpu_seen)
        return 0;
    *result = 0;
    /* F2 the settings, F3 the controls; the open one's key closes it. */
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && (w == VK_F2 || w == VK_F3)) {
        LONG which = w == VK_F2 ? MENU_SETTINGS : MENU_CONTROLS;
        if (!(l & (1L << 30)))                      /* not auto-repeat */
            InterlockedExchange(&s_want_open,
                                s_want_open == which ? MENU_NONE : which);
        return 1;
    }
    /* The pause key (P) and the photo mode key (F4), from F3's photo
     * table, unless a key is being set (the press is the answer). Not
     * WM_SYSKEYDOWN: Alt+F4 still closes the window. */
    if (m == WM_KEYDOWN && !(s_want_open && s_capture_idx >= 0)
        && ((int)w == controls_pause_key()
            || (int)w == controls_photo_toggle_key())) {
        if (!(l & (1L << 30))) {
            if ((int)w == controls_pause_key())
                InterlockedXor(&s_want_pause, 1);
            else
                photo_request_toggle();
        }
        return 1;
    }
    /* Off, fps, fps and frame times, off again. */
    if (m == WM_KEYDOWN && w == VK_F1) {
        if (!(l & (1L << 30)))
            InterlockedExchange(&s_want_fps, (s_want_fps + 1) % 3);
        return 1;
    }
    /* Photo mode's zoom: the wheel kept from the game's keys. */
    if (m == WM_MOUSEWHEEL && !s_want_open && photo_frozen()) {
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
static bool s_focus;          /* give the menu the focus next frame */
static WORD s_pad_prev;       /* pad buttons last frame, for presses */
static LARGE_INTEGER s_last_frame;
static char s_error[160];     /* last save failure, shown in the menu */
static ImVec2 s_win_pos, s_win_size;   /* the menu's window last frame */
static bool s_win_dragged;    /* moved or resized with the mouse down */
static unsigned s_style_h;    /* picture height the style was made for */
static bool s_replace[4];     /* put a menu window back from settings.ini */
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
    return ImGuiKey_None;
}

/* The first connected pad, as the toolkit picks player 1: an Xbox pad
 * (XInput) or a PlayStation pad (SDL3), in XInput's layout either way. */
static bool read_pad(XINPUT_STATE *st)
{
    int pad = xbox_InputFirstHostPad();
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

static void open_menu(void)
{
    XINPUT_STATE pad;
    read_pad(&pad);
    s_pad_prev = pad.Gamepad.wButtons;   /* a held button is not a press */
    xbox_InputSetBlocked(1);
    s_pad_menu = true;
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    s_error[0] = '\0';
    s_focus = true;
    s_ask_quit = false;
    s_open = true;
    s_menu = (int)s_want_open;
    s_capture_idx = -1;
    s_ask_reset = -1;
    fprintf(stderr, "[OVERLAY] opened%s\n",
            s_menu == MENU_CONTROLS ? " (controls)"
            : s_menu == MENU_PHOTO ? " (photo panel)" : "");
}

/* F3 with F2's menu open, or the other way round: the other menu, as if
 * just opened, the pad staying where it is. */
static void switch_menu(void)
{
    s_menu = (int)s_want_open;
    s_error[0] = '\0';
    s_focus = true;
    s_ask_quit = false;
    s_capture_idx = -1;
    s_ask_reset = -1;
    fprintf(stderr, "[OVERLAY] switched to the %s menu\n",
            s_menu == MENU_CONTROLS ? "controls"
            : s_menu == MENU_PHOTO ? "photo" : "settings");
}

static void close_menu(void)
{
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
    bool paused = s_want_pause != 0;
    bool hold_apu = (paused || photo_frozen())
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
        case WM_CHAR:
            io.AddInputCharacter((unsigned)e.w);
            break;
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
};
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
    apply_picture();
}

/* The name above; the slider the window's full width but for its reset
 * icon (back to neutral, the original) at the end of the line. fmt shows
 * the value and rounds it as it shows it; tip is the icon's. */
static void slider(const char *label, const char *key, float lo, float hi,
                   const char *neutral, const char *fmt, const char *tip)
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
        fprintf(s_dump, ", \"min\": %g, \"max\": %g, \"neutral\": ", lo, hi);
        settings_json_string(s_dump, neutral);
        fprintf(s_dump, ", \"decimals\": %d, \"unit\": ", f ? atoi(f + 1) : 0);
        settings_json_string(s_dump, unit);
        fputs(", \"reset_tip\": ", s_dump);
        settings_json_string(s_dump, tip);
        fputs(" }", s_dump);
    }
    snprintf(id, sizeof(id), "##%s", key);
    ImGui::TextUnformatted(label);
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

static void build_menu(unsigned w, unsigned h, bool *close)
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
    static const char *const scale_values[] = {
        "50", "67", "75", "100", "125", "150", "200",
    };
    static const char *const scale_names[] = {
        "50 % (fastest, softest)", "67 %", "75 %", "100 % (default)",
        "125 %", "150 %", "200 % (sharpest, slowest)",
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
    static const char *const mix_values[] = {
        "xemu", "speakers", "evenodd", "front",
    };
    static const char *const mix_names[] = {
        "xemu (default)", "speakers (wider stereo)", "evenodd (debug)",
        "front (debug)",
    };

    /* 24 wide: "Ambient Particles (experimental)", indented, is the widest
     * line (clipped at 22, 2026-10-31). */
    place_window(MENU_SETTINGS, "menu_window", w, h, 24.0f, 0.0f);
    ImGui::Begin("Settings", NULL,
                 ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoSavedSettings);
    {
        bool paused = s_paused;
        char label[64];
        snprintf(label, sizeof(label), "Pause the game (%s)",
                 controls_key_label(controls_pause_key()));
        if (ImGui::Checkbox(label, &paused))
            InterlockedExchange(&s_want_pause, paused ? 1 : 0);
    }
    if (!s_pad_menu)
        note("Controller: the game. Click this menu (or F2 twice) to use it.");

    section("Display");
    choice("Display mode", "display_mode", mode_values, mode_names,
           (int)(sizeof(mode_values) / sizeof(mode_values[0])), true);
    choice("Render resolution", "render_resolution", res_values, res_names,
           (int)(sizeof(res_values) / sizeof(res_values[0])), true);
    choice("Render scale", "render_scale", scale_values, scale_names,
           (int)(sizeof(scale_values) / sizeof(scale_values[0])), true);
    {
        /* The size the game is drawn at, and when the toolkit capped it,
         * the percent that is. */
        unsigned dw, dh;
        int pct = xbox_RenderScaleSize(&dw, &dh);
        char buf[128];
        if (pct < atoi(settings_get("render_scale")))
            snprintf(buf, sizeof(buf), "    Drawn at %ux%u: capped at %d %%,"
                     " the largest the renderer takes here", dw, dh, pct);
        else
            snprintf(buf, sizeof(buf), "    Drawn at %ux%u", dw, dh);
        note(buf);
    }
    choice("Upscaler (render scale below 100 %)", "upscaler", up_values,
           up_names, (int)(sizeof(up_values) / sizeof(up_values[0])), true);
    choice("Texture filtering", "texture_filter", filter_values, filter_names,
           (int)(sizeof(filter_values) / sizeof(filter_values[0])), true);
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
    choice("Frame rate", "frame_rate", fps_values, fps_names,
           settings_debug() ? 6 : 4, true);
    /* The dip fix at 30 is on by default and a debug choice (user
     * 2026-10-22); 60 and above always have it. */
    if (settings_debug() && !strcmp(settings_get("frame_rate"), "30")) {
        checkbox("Keep speed in slowdowns (debug)", "slowdown_fix", true);
        note("    Off = original (runs fast when the frame rate dips)");
    }
    checkbox("Main Menu Speed", "main_menu_speed", true,
             "Above 60 fps the main menu turns between its boards and zooms "
             "into them at the original speed (off: faster, twice as fast at "
             "120)");
    checkbox("Sub-menu Speed", "submenu_speed", true,
             "Above 60 fps game select and the chapter list fade at the "
             "original speed (off: faster, twice as fast at 120)");
    /* On by default, a debug choice (user 2026-11-04). */
    if (settings_debug())
        checkbox("Physics at 30 Hz (debug)", "physics_30hz", true,
                 "Above 30 fps tails, ragdolls and cloth move at the original "
                 "speed (off: faster, 4x as fast at 120)");
    choice("VSync", "vsync", vsync_values, vsync_names,
           (int)(sizeof(vsync_values) / sizeof(vsync_values[0])), true);

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

    section("Picture");
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
    checkbox("Haze", "screen_haze", true,
             "The mist or colour wash in some levels. Off = a clear view "
             "(e.g. no green gas in Chemical Warfare)");
    checkbox("Vignette", "vignette", true,
             "The dark corners round the picture in some levels. Off = "
             "the picture as bright at the edges as in the middle");
    checkbox("Screen drops", "screen_drops", true,
             "Water and blood drops on the screen and the streaks they "
             "leave. Off = a clean screen");
    checkbox("Underwater Overlay", "underwater_overlay", true,
             "The shimmering haze round Conker under water, which shows as "
             "a band at eye level. Off = a clear view under water");
    /* Shown while the overlay is on, as the pop-in parts (user
     * 2026-11-03). */
    if (s_dump || strcmp(settings_get("underwater_overlay"), "0")) {
        ImGui::Indent();
        s_dump_parent = "underwater_overlay";
        checkbox("Poo Fix", "poo_fix", true,
                 "Under the poo after the Bull fight, the haze is brown to "
                 "match instead of a blue band. Off = the original. Normal "
                 "water is never changed");
        s_dump_parent = NULL;
        ImGui::Unindent();
    }
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

    section("Sound");
    /* evenodd and front are for testing: listed only with --debug. */
    choice("Mix", "mix", mix_values, mix_names, settings_debug() ? 4 : 2,
           false);
    checkbox("3D sound (HRTF)", "hrtf", false);
    checkbox("Silence sound while paused", "pause_silence", true);

    /* RECOMP_LANGUAGE: the toolkit's answer to XGetLanguage, read once. */
    section("Language");
    static const char *const lang_values[] = { "english", "french", "german",
                                               "italian", "spanish",
                                               "japanese" };
    static const char *const lang_names[] = { "English", "French", "German",
                                              "Italian", "Spanish",
                                              "Japanese" };
    choice("Text and menus", "language", lang_values, lang_names,
           (int)(sizeof(lang_values) / sizeof(lang_values[0])), false);
    note("    Voices are English in every language");

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

    if (settings_debug()) {
        section("Debug");
        note("Started with --debug: debug choices are shown.");
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
        if (set == CONTROLS_PHOTO && i <= PHOTO_PAUSE)
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
            /* Photo mode's two "anywhere" keys clash with all its rows. */
            for (int j = 0; j < n && input > 0; j++)
                if (j != i && controls_key(set, j) == input
                    && (same_section(controls_action(set, j)->section,
                                     a->section)
                        || (set == CONTROLS_PHOTO
                            && (i <= PHOTO_PAUSE || j <= PHOTO_PAUSE)))
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

static DWORD WINAPI write_shot(void *arg)
{
    Shot *s = (Shot *)arg;
    IWICImagingFactory *fac = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    UINT stride = s->w * 3;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    bool made = false;
    char msg[160];

    if (SUCCEEDED(hr))
        hr = fac->CreateStream(&st);
    if (SUCCEEDED(hr))
        hr = st->InitializeFromFilename(s->path, GENERIC_WRITE);
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
        hr = fr->SetSize(s->w, s->h);
    if (SUCCEEDED(hr))
        hr = fr->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR)
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = fr->WritePixels(s->h, stride, stride * s->h, s->bgr);
    if (SUCCEEDED(hr))
        hr = fr->Commit();
    if (SUCCEEDED(hr))
        hr = enc->Commit();
    release(fr);
    release(enc);
    release(st);
    release(fac);
    if (FAILED(hr) && made)
        DeleteFileW(s->path);       /* no half-written file left behind */
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

static void take_shot(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                      ID3D11RenderTargetView *rtv)
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *bb = NULL, *flat = NULL, *stage = NULL, *src;
    D3D11_TEXTURE2D_DESC d, sd;
    D3D11_MAPPED_SUBRESOURCE m;
    const char *why = NULL;
    Shot *s = NULL;
    int order;          /* 0 BGRA, 1 RGBA, 2 RGB10A2 */

    rtv->GetResource(&res);
    if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(&bb)))) {
        why = "no back buffer";
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
        why = "back buffer format not handled";
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
            why = "no resolve texture";
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
        why = "no staging texture";
        goto done;
    }
    ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, src, 0, NULL);
    if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        why = "the copy could not be read";
        goto done;
    }
    s = new Shot();
    s->w = d.Width;
    s->h = d.Height;
    s->bgr = (BYTE *)malloc((size_t)s->w * s->h * 3);
    if (s->bgr) {
        for (unsigned y = 0; y < s->h; y++) {
            const BYTE *in = (const BYTE *)m.pData + (size_t)y * m.RowPitch;
            BYTE *out = s->bgr + (size_t)y * s->w * 3;
            for (unsigned x = 0; x < s->w; x++, in += 4, out += 3) {
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
    if (!s->bgr) {
        why = "out of memory";
        delete s;
        goto done;
    }
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
done:
    release(stage);
    release(flat);
    release(bb);
    release(res);
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
    /* The photo panel goes with photo mode. */
    if (s_want_open == MENU_PHOTO && !photo_frozen())
        InterlockedCompareExchange(&s_want_open, MENU_NONE, MENU_PHOTO);
    if (!s_want_open && s_open)
        close_menu();
    if (!s_want_open)
        pad_opens_menu();
    ring = conker_SkipRing();
    cover = conker_SkipCover();
    if (!s_want_open && !s_paused && !s_fps && !photo_frozen() && ring <= 0.0f
        && cover <= 0.0f)
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
    if (s_open && s_menu == MENU_PHOTO)
        build_photo(w, h, &close);
    else if (s_open)
        s_menu == MENU_CONTROLS ? build_controls(w, h, &close)
                                : build_menu(w, h, &close);
    else if (s_paused || photo_frozen())
        pause_label(h, photo_frozen() != 0);
    /* A click outside the menu takes its focus (ImGui's own rule), and the
     * pad goes with it. */
    if (s_open)
        pad_to_menu(ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow));
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
    follow_fps();
    fps_tick(!s_held);
    s_held = overlay_draw(dev, ctx, rtv, w, h) != 0;
    return s_held ? 1 : 0;
}

void overlay_init(void)
{
    InitializeCriticalSection(&s_lock);
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
