/**
 * Photo mode (SPEC 10.1): the world frozen, the game still drawing every
 * frame, the picture taken from a free camera.
 *
 * The freeze is the game's own pause path. game_frame (0x002A4610) runs the
 * world update 0x002A56A0 (characters, cameras, the pause menu) only while
 * the pause flag [0x007FA275] is 0, then always draws (0x002A3FD0) and
 * swaps. Photo mode sets the flag at frame_timer_update (just before it is
 * read; dt is 0 while it is set) and puts it back at 0x002AF250, before
 * game_frame compares it with the wanted pause [0x007FA274]. That compare
 * is what pauses the game's sounds and starts the pause menu, so neither
 * happens; the pause screen, drawn whenever the flag is set, is skipped in
 * recomp_manual.c (pause_screen_draw 0x00204060). The sound is held by the
 * overlay, as for the F4 / P pause.
 *
 * The camera: while paused nothing publishes the view, so the draw keeps
 * the last one. At the top of the draw, viewport 0's render camera
 * ([[0x0084A13C] + 0x144]) gets ours as camera_publish_matrices
 * (0x000418F0) would write it: view at camera +0x10 and record +0x40,
 * the record's transposed copy at +0x100, the inverse (the camera's world
 * matrix) at camera +0x50 and record +0x80; then the game's projection
 * build 0x003FB030 (frustum planes from the new view, camera +0x114) and
 * the planes' copy to record +0x148. Matrices are D3D's rows, a point is
 * a row vector; the world matrix's rows are right, up, back (the camera
 * looks along minus row 2, right-handed) and position; y is up.
 *
 * Input (game thread, once a frame): the keys of F3's photo table through
 * the keyboard-and-mouse pad (controls_poll selects the empty photo profile,
 * so the game gets nothing; xbox_KbmHeld says what is held), the mouse's
 * movement handed over directly, and slot 1's controller read with
 * XInput while the toolkit keeps it from the game. Nothing while an
 * F2 / F3 menu is open: it has the pad and keys then.
 *
 * Zoom is the render camera's field of view (+0x104, degrees) written
 * before the projection build, and put back on leaving; the aspect (+0x108)
 * follows a render resolution changed while frozen (conker_AspectRefresh,
 * as the frozen world update would). The HUD (hidden
 * by default) is the game's two HUD draws, both skipped while the pause
 * flag is set: 0x00317780 inside 0x002A5DB0 (the flag cleared for that
 * call) and 0x002A5EB0 in the draw right after 0x002A60B0 (called again
 * from there, as the draw does); the weapon sight, an object of layer 3
 * drawn even while paused (0x0015CAF0), is skipped unless the HUD is
 * shown. Conker hidden: char_render_pass
 * (0x0007FB00) skipped for character id 1. Frame step: the flag left at
 * 0 until the draw, so the world update runs once. The world at normal speed
 * (to-do 31, frame step held or the resume key) is that, every frame, with no
 * menu up; the sound plays (photo_world_running), and a level change ends
 * photo mode: the camera belongs to the old level.
 *
 * Photos and the panel (overlay.cpp): the photo key asks the overlay for
 * the next present's picture; the panel key opens the photo panel, which
 * changes s_opt directly (no camera input meanwhile; keys still held when
 * it closes are not presses). Depth of field: recomp_manual.c's dof_update
 * hook (0x001C71A0, run by the draw for viewport 0 every frame, paused or
 * not) asks photo_dof and then draws a lens's blur focused s_opt.focus
 * away instead of the shot's. The camera's own near / far (+0xFC / +0x100)
 * read 0 in the field, so the cutscene depth of field's planes
 * [0x0076F7AC/B0] (10..150000) are used then. The focus starts on Conker:
 * his position (+0x78) along the view, 30 up.
 *
 * Debug trigger RECOMP_PHOTO_FILE=<path>: about twice a second the game
 * thread looks for the file; there = photo mode on, gone = off.
 *
 * The pause key (P, to-do 117, user 2026-11-06) is the same freeze without
 * photo mode's camera, keys, HUD hiding and panel (s_pause; photo_active
 * is photo mode itself, photo_frozen either): the game's view and HUD, its
 * sound paused, no input to it. Asked through overlay_pause_wanted; where
 * photo mode cannot freeze it is refused and does nothing. The photo key
 * while paused turns the pause into photo mode.
 */
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "photo_mode.h"
#include "controls.h"
#include "settings.h"

/* The toolkit's (xbox_kbm.h, xinput_device.c); not included: their Xbox
 * pad types clash with xinput.h. */
int  xbox_KbmHeld(unsigned input);
void xbox_KbmMouseDirect(int axes);
int  xbox_KbmTakeMouse(long *dx, long *dy);
void xbox_InputSetBlocked(int on);
int  xbox_InputHostPadOfPort(DWORD port);
DWORD xbox_InputHostPadRead(DWORD slot, XINPUT_STATE *st);
#define KBM_DIRECT_XY 3

extern void sub_003FB030(void);
extern void sub_003FB250(void);
extern void sub_002A5EB0(void);
extern void sub_00064A10(void);
extern void sub_00081190(void);
extern void sub_00068D10(void);
extern void sub_002C5070(void);

/* From xbox_kbm.h: the wheel as an input. */
#define KBM_WHEEL_UP   256
#define KBM_WHEEL_DOWN 257

#define DEG (3.14159265f / 180.0f)

/* Speeds: units a second (Conker is about 60 tall), degrees a second at
 * full stick, degrees a mouse count (mouse look's 0.09, RECOMP_KBM_MOUSE_DEG)
 * times single player's sensitivity. */
#define FLY_SPEED   300.0f
#define SLOW_TIMES  0.25f
#define FAST_TIMES  4.0f
#define TURN_SPEED  120.0f
#define ROLL_SPEED  60.0f
#define MOUSE_DEG   0.09f
/* Zoom: the field of view shrinks e times in ZOOM_RATE^-1 s held, by
 * WHEEL_TIMES a wheel notch; turning slows with it. */
#define ZOOM_RATE   0.8f
#define WHEEL_TIMES 0.9f
#define FOV_MIN     5.0f
#define FOV_MAX     100.0f
/* Frame step held this long: the world runs at normal speed until let go. */
#define STEP_HOLD_S 0.35f
/* Depth of field at 100 %: the blur radius (guest pixels) at twice the
 * focus distance; a lens's, so twice that far away and up to 2.5 times
 * nearer (dof_d3d11.c's cinematic style). */
#define DOF_MOST    10.0f
/* Focus with Conker not in view. */
#define DOF_FOCUS   200.0f

static volatile LONG s_frozen;  /* written by the game thread */
static volatile LONG s_freecam, s_fc_game, s_fc_ask;  /* photo_freecam_set */
static volatile LONG s_pause;   /* frozen by the pause key, not photo mode */
static int s_settle;            /* left by a key still held: its set stays */
static volatile LONG s_toggle;  /* asked by the window thread */
static int s_flag_set;          /* the pause flag is ours until frame end */
static uint8_t s_flag_saved;

/* The camera: where it started (the game's when frozen) and where it is. */
static int s_have_base;
static float s_base_pos[3], s_base_yaw, s_base_pitch;
static float s_pos[3], s_yaw, s_pitch;
static uint32_t s_cam;          /* the camera given our view */

/* Zoom, roll, HUD, Conker, speed and depth of field (the photo panel's). */
static PhotoOptions s_opt;
static volatile LONG s_ask_reset, s_ask_focus;   /* from the panel */
static int s_after_menu;        /* a menu was up: held keys are no presses */
static int s_step_asked, s_stepping;
static int s_step_held;         /* frame step held past STEP_HOLD_S (game thread) */
static volatile LONG s_resumed; /* the resume key's state */
static volatile LONG s_running; /* the world moves this frame */
static unsigned s_level;        /* the level photo mode started in */
static volatile LONG s_wheel[2];   /* notches up / down (window thread) */
static int s_wheel_rest;

static LARGE_INTEGER s_last;

int photo_frozen(void)
{
    return (int)s_frozen;
}

int photo_active(void)
{
    return s_frozen && !s_pause;
}

int photo_paused(void)
{
    return s_frozen && s_pause;
}

int photo_world_running(void)
{
    return s_frozen && !s_pause && s_running;
}

int photo_world_resumed(void)
{
    return s_frozen && !s_pause && s_resumed;
}

int photo_keys_wanted(void)
{
    return (s_frozen && !s_fc_game) || s_settle;
}

/* ---- freecam (--debug; to-do 33, SPEC 10.3) ------------------------------- */

/* Photo mode with the world running (to-do 31's way), no panel and no note: asked
 * from any thread, done at the next frame. The pad and keys belong to the camera or,
 * after the swap key (Back + X on the pad, s_fc_game), to Conker with the camera
 * staying where it is. The game's own pause ends it (photo mode can't start there). */
int photo_freecam(void)
{
    return s_frozen && s_freecam;
}

/* The fly camera's place while freecam is on (the console's tp cam, to-do
 * 155): 1 with it, 0 when freecam is off. */
int photo_freecam_pos(float *x, float *y, float *z)
{
    if (!photo_freecam())
        return 0;
    *x = s_pos[0];
    *y = s_pos[1];
    *z = s_pos[2];
    return 1;
}

int photo_freecam_game(void)
{
    return s_frozen && s_freecam && s_fc_game;
}

void photo_freecam_set(int on)
{
    InterlockedExchange(&s_fc_ask, on ? 1 : 2);
}

void photo_request_toggle(void)
{
    InterlockedExchange(&s_toggle, 1);
}

void photo_wheel(int delta)
{
    s_wheel_rest += delta;
    while (s_wheel_rest >= WHEEL_DELTA) {
        InterlockedIncrement(&s_wheel[0]);
        s_wheel_rest -= WHEEL_DELTA;
    }
    while (s_wheel_rest <= -WHEEL_DELTA) {
        InterlockedIncrement(&s_wheel[1]);
        s_wheel_rest += WHEEL_DELTA;
    }
}

/* The pause key's freeze keeps the HUD as the game draws it. */
int photo_hud_shown(void)
{
    return s_frozen && (s_pause || s_opt.hud);
}

int photo_hud_hidden(void)
{
    return s_frozen && !s_pause && !s_opt.hud;
}

int photo_hides_char_id(unsigned id)
{
    return s_frozen && s_opt.hide_conker && id == 1;
}

int photo_hides_char(unsigned index)
{
    uint32_t chars = MEM32(0x0084A5F8u);
    return chars && index < 0x81u
        && photo_hides_char_id(MEM8(chars + index * 0x6D0u + 0x10Bu));
}

PhotoOptions *photo_options(void)
{
    return &s_opt;
}

/* The cutscene blur in photo mode: -1 as the F2 menu says, else 0 off /
 * 1 on. Only while frozen; leaving puts the F2 setting back. */
static volatile LONG s_cut_blur = -1;

int photo_cut_blur(void)
{
    return s_frozen ? (int)s_cut_blur : -1;
}

void photo_toggle_cut_blur(void)
{
    int on = s_cut_blur >= 0 ? (int)s_cut_blur : conker_CutsceneBlurOn();
    InterlockedExchange(&s_cut_blur, !on);
    fprintf(stderr, "[PHOTO] cutscene blur %s\n", on ? "off" : "on");
}

void photo_set_cut_blur(int v)
{
    InterlockedExchange(&s_cut_blur, v);
}

void photo_ask_reset(void)
{
    InterlockedExchange(&s_ask_reset, 1);
}

void photo_ask_focus_conker(void)
{
    InterlockedExchange(&s_ask_focus, 1);
}

/* The camera's near and far planes (+0xFC, +0x100, as the projection build
 * reads them), else the cutscene depth of field's [0x0076F7AC/B0]. */
static void planes(float *cn, float *cf)
{
    *cn = s_cam ? MEMF(s_cam + 0xFCu) : 0.0f;
    *cf = s_cam ? MEMF(s_cam + 0x100u) : 0.0f;
    if (!(*cn > 0.0f && *cf > *cn * 2.0f)) {
        *cn = MEMF(0x0076F7ACu);
        *cf = MEMF(0x0076F7B0u);
    }
}

int photo_dof(float *focus, float *blur, float *cn, float *cf)
{
    if (!s_frozen || !s_have_base || !s_opt.dof || s_opt.blur <= 0.0f)
        return 0;
    planes(cn, cf);
    if (!(*cn > 0.0f && *cf > *cn))
        return 0;
    *focus = s_opt.focus;
    *blur = s_opt.blur * 0.01f * DOF_MOST;
    return 1;
}

/* Conker (character id 1) from the camera: the distance along the view to
 * his middle (position +0x78, about 30 above his feet), as the depth
 * buffer measures it. 0 when he is not there or behind the camera. */
static float conker_distance(void)
{
    uint32_t chars = MEM32(0x0084A5F8u), c;
    float f[3], d[3], dist;
    int i;

    if (!chars)
        return 0.0f;
    for (i = 0, c = chars; i < 0x81; i++, c += 0x6D0u)
        if (MEM32(c + 0x64u) && MEM8(c + 0x10Bu) == 1)
            break;
    if (i == 0x81)
        return 0.0f;
    f[0] = sinf(s_yaw) * cosf(s_pitch);
    f[1] = sinf(s_pitch);
    f[2] = cosf(s_yaw) * cosf(s_pitch);
    d[0] = MEMF(c + 0x78u) - s_pos[0];
    d[1] = MEMF(c + 0x7Cu) + 30.0f - s_pos[1];
    d[2] = MEMF(c + 0x80u) - s_pos[2];
    dist = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    return dist > 10.0f ? dist : 0.0f;
}

static void focus_on_conker(void)
{
    float d = conker_distance();
    if (d > 0.0f) {
        s_opt.focus = d;
        fprintf(stderr, "[PHOTO] focus on Conker: %.0f\n", d);
    } else {
        fprintf(stderr, "[PHOTO] focus on Conker: not in view\n");
    }
}

static void reset_camera(void)
{
    memcpy(s_pos, s_base_pos, sizeof(s_pos));
    s_yaw = s_base_yaw;
    s_pitch = s_base_pitch;
    s_opt.roll = 0.0f;
    s_opt.fov = s_opt.fov_game;
    fprintf(stderr, "[PHOTO] camera back to the start\n");
}

static int debug_poll(void)
{
    static const char *path;
    static int checked, want;
    static unsigned n;

    if (!checked) {
        checked = 1;
        path = getenv("RECOMP_PHOTO_FILE");
        if (path && !*path)
            path = NULL;
    }
    if (path && !(n++ % 15))
        want = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    return path ? want : -1;
}

/* ---- the controller ------------------------------------------------------ */

typedef struct {
    WORD buttons;
    float lx, ly, rx, ry, lt, rt;
} Pad;

static float stick(SHORT v)
{
    float f = v / 32767.0f, a = fabsf(f);
    if (a < 0.24f)
        return 0.0f;
    a = (a - 0.24f) / 0.76f;
    if (a > 1.0f)
        a = 1.0f;
    return f < 0 ? -a : a;
}

static float trigger(BYTE v)
{
    return v < 30 ? 0.0f : (v - 30) / 225.0f;
}

/* Slot 1's controller (F3 Slots: game port 0), an Xbox pad or a
 * PlayStation pad, in XInput's layout; none when slot 1 is "No input". The
 * first connected pad until 2026-11-05 flew the camera with every slot
 * empty (to-do 68). The toolkit never asks an empty slot (slow in XInput). */
static int read_pad(Pad *p)
{
    XINPUT_STATE st;
    int slot = xbox_InputHostPadOfPort(0);

    memset(p, 0, sizeof(*p));
    if (slot < 0 || xbox_InputHostPadRead((DWORD)slot, &st) != ERROR_SUCCESS)
        return 0;
    p->buttons = st.Gamepad.wButtons;
    p->lx = stick(st.Gamepad.sThumbLX);
    p->ly = stick(st.Gamepad.sThumbLY);
    p->rx = stick(st.Gamepad.sThumbRX);
    p->ry = stick(st.Gamepad.sThumbRY);
    p->lt = trigger(st.Gamepad.bLeftTrigger);
    p->rt = trigger(st.Gamepad.bRightTrigger);
    return 1;
}

/* ---- on and off ---------------------------------------------------------- */

/* Single player (gameplay, cutscenes) and the front end: one viewport, the
 * game not paused, not multiplayer or a menu inside a level. */
static int can_enter(void)
{
    int set = controls_mode();
    return MEM32(0x0084A144u) == 0 && !MEM8(0x007FA274u) && !MEM8(0x007FA275u)
        && set != CONTROLS_MP && set != CONTROLS_MULE_GUN
        && set != CONTROLS_PAUSE && set != CONTROLS_MENU_GAME;
}

static void build_projection(uint32_t cam);

static void enter(void)
{
    InterlockedExchange(&s_pause, 0);   /* also from the pause key's freeze */
    InterlockedExchange(&s_frozen, 1);
    s_have_base = 0;
    s_opt.hud = s_opt.hide_conker = 0;
    s_opt.dof = 0;
    s_opt.focus = 0.0f;             /* set with the camera, take_base */
    s_opt.speed = (float)atof(settings_get("photo_speed"));
    s_opt.blur = (float)atof(settings_get("photo_blur"));
    if (s_opt.speed < 25.0f || s_opt.speed > 400.0f)
        s_opt.speed = 100.0f;
    InterlockedExchange(&s_ask_reset, 0);
    InterlockedExchange(&s_ask_focus, 0);
    InterlockedExchange(&s_cut_blur, -1);
    s_after_menu = 0;
    s_step_asked = s_stepping = s_step_held = 0;
    InterlockedExchange(&s_resumed, 0);
    InterlockedExchange(&s_running, 0);
    s_level = MEM8(0x007FA1F8u);
    InterlockedExchange(&s_wheel[0], 0);
    InterlockedExchange(&s_wheel[1], 0);
    xbox_InputSetBlocked(1);
    QueryPerformanceCounter(&s_last);
    fprintf(stderr, "[PHOTO] on (level 0x%02X)\n", MEM8(0x007FA1F8u));
}

/* The pause key (P; to-do 117): the same freeze, the game's own camera,
 * HUD and pause of its sounds, and none of photo mode's keys; the game
 * gets no input (the empty photo profile, the pad blocked). */
static void enter_pause(void)
{
    InterlockedExchange(&s_pause, 1);
    s_have_base = 0;
    s_opt.hud = s_opt.hide_conker = s_opt.dof = 0;
    InterlockedExchange(&s_cut_blur, -1);
    s_step_asked = s_stepping = s_step_held = 0;
    InterlockedExchange(&s_resumed, 0);
    InterlockedExchange(&s_running, 0);
    InterlockedExchange(&s_frozen, 1);
    xbox_InputSetBlocked(1);
    fprintf(stderr, "[PAUSE] on (level 0x%02X)\n", MEM8(0x007FA1F8u));
}

static void leave(void)
{
    int was_pause = (int)s_pause;
    InterlockedExchange(&s_frozen, 0);
    InterlockedExchange(&s_freecam, 0);
    InterlockedExchange(&s_fc_game, 0);
    InterlockedExchange(&s_pause, 0);
    InterlockedExchange(&s_resumed, 0);
    InterlockedExchange(&s_running, 0);
    InterlockedExchange(&s_cut_blur, -1);     /* the F2 setting again */
    /* The game's field of view back (it may not set it every frame). */
    if (s_have_base && s_cam && MEMF(s_cam + 0x104u) != s_opt.fov_game) {
        MEMF(s_cam + 0x104u) = s_opt.fov_game;
        build_projection(s_cam);
    }
    s_have_base = 0;
    xbox_InputSetBlocked(overlay_pad_menu());
    fprintf(stderr, was_pause ? "[PAUSE] off\n" : "[PHOTO] off\n");
}

static const char *not_here_why(void)
{
    return MEM32(0x0084A144u) ? "split screen"
         : MEM8(0x007FA274u) ? "paused" : "multiplayer or a menu";
}

/* ---- each frame ---------------------------------------------------------- */

static int held(int row)
{
    int k = controls_key(CONTROLS_PHOTO, row);
    return k > 0 && xbox_KbmHeld((unsigned)k);
}

/* Noclip (recomp_manual.c): the fly controls with the game running; nothing while
 * a menu has the pad or the keys. Speed in units a second. */
int overlay_fly_input_free(void);

void photo_fly_input(float *fwd, float *side, float *vert, float *speed)
{
    Pad p;
    /* Photo mode's speed setting; s_opt holds it only once photo mode has been on. */
    float pct = (float)atof(settings_get("photo_speed"));
    float s = FLY_SPEED * (pct > 0.0f ? pct : 100.0f) * 0.01f;

    *fwd = *side = *vert = 0.0f;
    if (!overlay_fly_input_free()) {
        *speed = s;
        return;
    }
    read_pad(&p);
    *fwd = (float)(held(PHOTO_FORWARD) - held(PHOTO_BACK)) + p.ly;
    *side = (float)(held(PHOTO_RIGHT) - held(PHOTO_LEFT)) + p.lx;
    *vert = (float)(held(PHOTO_UP) - held(PHOTO_DOWN)) + p.rt - p.lt;
    if (held(PHOTO_SLOW) || (p.buttons & XINPUT_GAMEPAD_RIGHT_THUMB))
        s *= SLOW_TIMES;
    if (held(PHOTO_FAST) || (p.buttons & XINPUT_GAMEPAD_LEFT_THUMB))
        s *= FAST_TIMES;
    *speed = s;
}

/* The fast-fly key is down (noclip's tail spins fast with it). */
int photo_fly_fast(void)
{
    Pad p;

    if (!overlay_fly_input_free())
        return 0;
    read_pad(&p);
    return held(PHOTO_FAST) || (p.buttons & XINPUT_GAMEPAD_LEFT_THUMB);
}

/* Wheel notches since the last frame on a row's key, if it is the wheel. */
static int notches(int row, const int *wheel)
{
    int k = controls_key(CONTROLS_PHOTO, row);
    return k == KBM_WHEEL_UP ? wheel[0] : k == KBM_WHEEL_DOWN ? wheel[1] : 0;
}

/* Fly, look and roll by the keys, the mouse and the pad; the one-press
 * actions on the press. */
static void steer(const Pad *p, WORD pressed)
{
    static int was[PHOTO_ROWS];
    static int back_alone;       /* Back down, nothing with it since */
    static int step_was;
    static LARGE_INTEGER step_since;
    int now[PHOTO_ROWS] = { 0 }, i, inv, wheel[2], zoom, step, back_down;
    LARGE_INTEGER t, f;
    float dt, fwd, side, vert, speed, sy, cy, sp, cp, sx_m, sy_m, look;
    long dx = 0, dy = 0;

    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    dt = (float)(t.QuadPart - s_last.QuadPart) / (float)f.QuadPart;
    s_last = t;
    if (dt > 0.1f)
        dt = 0.1f;

    for (i = PHOTO_FORWARD; i < PHOTO_ROWS; i++)
        now[i] = held(i);
    wheel[0] = (int)InterlockedExchange(&s_wheel[0], 0);
    wheel[1] = (int)InterlockedExchange(&s_wheel[1], 0);
    /* A notch on a one-press key is a press. */
    for (i = PHOTO_TAKE; i < PHOTO_ROWS; i++)
        if (notches(i, wheel)) {
            now[i] = 1;
            was[i] = 0;
        }
    /* Back from a menu (F2, F3, the photo panel): keys held through it and
     * the mouse moved over it do nothing. */
    step = now[PHOTO_STEP] || (p->buttons & XINPUT_GAMEPAD_DPAD_RIGHT);
    if (s_after_menu) {
        s_after_menu = 0;
        memcpy(was, now, sizeof(was));
        step_was = step;
        back_alone = 0;
        xbox_KbmMouseDirect(KBM_DIRECT_XY);
        xbox_KbmTakeMouse(&dx, &dy);
        return;
    }

    /* Back alone resets on letting go; with RB (White) it was the toggle,
     * with LB (Black) the F2 menu (overlay.cpp), with A the resume. Back +
     * Start does nothing here: it is kept for a controller menu (user
     * 2026-11-08). */
    back_down = (p->buttons & XINPUT_GAMEPAD_BACK) != 0;
    if (pressed & XINPUT_GAMEPAD_BACK)
        back_alone = 1;
    if (p->buttons & (XINPUT_GAMEPAD_RIGHT_SHOULDER
                      | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_START
                      | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X))
        back_alone = 0;
    if (!(p->buttons & XINPUT_GAMEPAD_BACK) && back_alone) {
        back_alone = 0;
        now[PHOTO_RESET] = 1;          /* a press */
        was[PHOTO_RESET] = 0;
    }

    /* On the pad Back + RB leaves (photo_frame_begin); B is the
     * cutscene blur's (user 2026-10-29). */
    if (now[PHOTO_LEAVE] && !was[PHOTO_LEAVE]) {
        memcpy(was, now, sizeof(was));
        /* Esc would reach the game as its pause key: the photo keys stay
         * in use until it is let go (the pad waits in the toolkit). */
        s_settle = now[PHOTO_LEAVE];
        leave();
        return;
    }
    if (now[PHOTO_RESET] && !was[PHOTO_RESET])
        reset_camera();
    if ((now[PHOTO_TAKE] && !was[PHOTO_TAKE])
        || ((pressed & XINPUT_GAMEPAD_A) && !back_down))
        overlay_photo_shot();
    if ((now[PHOTO_CUT_BLUR] && !was[PHOTO_CUT_BLUR])
        || (pressed & XINPUT_GAMEPAD_B))
        photo_toggle_cut_blur();
    if (!s_freecam && ((now[PHOTO_RESUME] && !was[PHOTO_RESUME])
                       || ((pressed & XINPUT_GAMEPAD_A) && back_down)))
        InterlockedExchange(&s_resumed, !s_resumed);
    if (!s_freecam && ((now[PHOTO_PANEL] && !was[PHOTO_PANEL])
                       || ((pressed & XINPUT_GAMEPAD_START) && !back_down))) {
        memcpy(was, now, sizeof(was));
        step_was = step;
        overlay_photo_panel();
        return;
    }
    if ((now[PHOTO_HUD] && !was[PHOTO_HUD]) || (pressed & XINPUT_GAMEPAD_Y)) {
        s_opt.hud = !s_opt.hud;
        fprintf(stderr, "[PHOTO] HUD %s\n", s_opt.hud ? "shown" : "hidden");
    }
    if ((now[PHOTO_CONKER] && !was[PHOTO_CONKER])
        || ((pressed & XINPUT_GAMEPAD_X) && !(s_freecam && back_down))) {
        s_opt.hide_conker = !s_opt.hide_conker;
        fprintf(stderr, "[PHOTO] Conker %s\n", s_opt.hide_conker ? "hidden" : "shown");
    }
    /* A press is one frame; held a moment, the world runs until let go. */
    if (step && !step_was) {
        s_step_asked = 1;
        step_since = t;
    }
    s_step_held = step && step_was
        && (float)(t.QuadPart - step_since.QuadPart) / (float)f.QuadPart
           >= STEP_HOLD_S;
    step_was = step;
    memcpy(was, now, sizeof(was));

    /* Zoom: held keys and the D-pad smoothly, wheel notches a step each. */
    zoom = (now[PHOTO_ZOOM_IN] || (p->buttons & XINPUT_GAMEPAD_DPAD_UP))
        - (now[PHOTO_ZOOM_OUT] || (p->buttons & XINPUT_GAMEPAD_DPAD_DOWN));
    s_opt.fov *= expf(-(float)zoom * ZOOM_RATE * dt);
    s_opt.fov *= powf(WHEEL_TIMES, (float)(notches(PHOTO_ZOOM_IN, wheel)
                                       - notches(PHOTO_ZOOM_OUT, wheel)));
    if (s_opt.fov < FOV_MIN)
        s_opt.fov = FOV_MIN;
    if (s_opt.fov > FOV_MAX)
        s_opt.fov = FOV_MAX;
    look = s_opt.fov_game > 1.0f ? s_opt.fov / s_opt.fov_game : 1.0f;

    fwd = (float)(now[PHOTO_FORWARD] - now[PHOTO_BACK]) + p->ly;
    side = (float)(now[PHOTO_RIGHT] - now[PHOTO_LEFT]) + p->lx;
    vert = (float)(now[PHOTO_UP] - now[PHOTO_DOWN]) + p->rt - p->lt;
    speed = FLY_SPEED * s_opt.speed * 0.01f;
    if (now[PHOTO_SLOW] || (p->buttons & XINPUT_GAMEPAD_RIGHT_THUMB))
        speed *= SLOW_TIMES;
    if (now[PHOTO_FAST] || (p->buttons & XINPUT_GAMEPAD_LEFT_THUMB))
        speed *= FAST_TIMES;
    sy = sinf(s_yaw);
    cy = cosf(s_yaw);
    sp = sinf(s_pitch);
    cp = cosf(s_pitch);
    /* Forward along the view (sin yaw cos pitch, sin pitch, cos yaw cos
     * pitch), sideways level (right = forward x up = -cos yaw, 0, sin
     * yaw), up and down straight. */
    s_pos[0] += (sy * cp * fwd - cy * side) * speed * dt;
    s_pos[1] += (sp * fwd + vert) * speed * dt;
    s_pos[2] += (cy * cp * fwd + sy * side) * speed * dt;

    xbox_KbmMouseDirect(KBM_DIRECT_XY);
    if (!xbox_KbmTakeMouse(&dx, &dy))
        dx = dy = 0;
    controls_mouse(CONTROLS_STORY, &sx_m, &sy_m, &inv);
    /* More yaw turns left. */
    s_yaw -= (p->rx * TURN_SPEED * dt + dx * MOUSE_DEG * sx_m) * look * DEG;
    s_pitch += (p->ry * TURN_SPEED * dt
                - (inv ? -1.0f : 1.0f) * dy * MOUSE_DEG * sy_m) * look * DEG;
    if (s_pitch > 89.0f * DEG)
        s_pitch = 89.0f * DEG;
    if (s_pitch < -89.0f * DEG)
        s_pitch = -89.0f * DEG;
    /* RB is the toggle's while Back is down, LB the F2 menu's. */
    s_opt.roll += ((float)(now[PHOTO_ROLL_LEFT]
                       || ((p->buttons & XINPUT_GAMEPAD_LEFT_SHOULDER)
                           && !(p->buttons & XINPUT_GAMEPAD_BACK)))
               - (float)(now[PHOTO_ROLL_RIGHT]
                         || ((p->buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                             && !(p->buttons & XINPUT_GAMEPAD_BACK))))
              * ROLL_SPEED * dt * DEG;
    if (s_opt.roll > 180.0f * DEG)          /* the panel shows -180..180 */
        s_opt.roll -= 360.0f * DEG;
    if (s_opt.roll < -180.0f * DEG)
        s_opt.roll += 360.0f * DEG;
}

/* The pause flag set until frame end. A frame that returned early (before
 * the draw) left it set: keep the value saved then. */
static void hold_flag(void)
{
    if (!s_flag_set) {
        s_flag_saved = MEM8(0x007FA275u);
        s_flag_set = 1;
    }
    MEM8(0x007FA275u) = 1;
}

void photo_frame_begin(void)
{
    static WORD prev;
    static int file_was = -1;
    Pad p;
    WORD pressed, combo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_RIGHT_SHOULDER;
    int want, file = debug_poll();
    int menu = overlay_menu_open();
    unsigned age;

    if (s_settle && !held(PHOTO_LEAVE))
        s_settle = 0;
    s_stepping = 0;             /* a step whose draw was skipped */
    read_pad(&p);
    pressed = (WORD)(p.buttons & ~prev);
    prev = p.buttons;

    want = InterlockedExchange(&s_toggle, 0)
        || (!menu && (p.buttons & combo) == combo && (pressed & combo));
    if (file >= 0 && file != file_was) {
        if (file != (int)s_frozen)
            want = 1;
        file_was = file;
    }
    if (want) {
        if (s_frozen && s_pause) {      /* paused by P: photo mode from here */
            overlay_pause_refused();
            enter();
        } else if (s_frozen)
            leave();
        else if (can_enter())
            enter();
        else
            fprintf(stderr, "[PHOTO] not here (level 0x%02X, %s)\n",
                    MEM8(0x007FA1F8u), not_here_why());
        pressed = 0;
    }
    /* The pause key (overlay.cpp): asked while the game ran frames. A press
     * during a load or a movie (no game frames) is too old by the next one
     * and does nothing, as where photo mode cannot freeze (user
     * 2026-11-06); in photo mode it does nothing either. */
    if (overlay_pause_wanted(&age)) {
        if (s_frozen && !s_pause) {
            fprintf(stderr, "[PAUSE] not in photo mode\n");
            overlay_pause_refused();
        } else if (!s_frozen) {
            if (age <= 1000u && can_enter()) {
                enter_pause();
            } else {
                fprintf(stderr, "[PAUSE] not here (level 0x%02X, %s)\n",
                        MEM8(0x007FA1F8u),
                        age > 1000u ? "no game frames" : not_here_why());
                overlay_pause_refused();
            }
        }
    } else if (s_frozen && s_pause) {
        leave();
    }
    /* Freecam (to-do 33). */
    {
        static int swap_was;
        LONG ask = InterlockedExchange(&s_fc_ask, 0);
        int swap = held(PHOTO_FREECAM_SWAP)
                   || (p.buttons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_X))
                      == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_X);

        if (ask == 1 && !s_frozen) {
            if (can_enter()) {
                enter();
                InterlockedExchange(&s_freecam, 1);
                InterlockedExchange(&s_resumed, 1);
                s_opt.hud = 1;
                fprintf(stderr, "[FREECAM] on\n");
            } else {
                fprintf(stderr, "[FREECAM] not here (level 0x%02X, %s)\n",
                        MEM8(0x007FA1F8u), not_here_why());
            }
        } else if (ask == 2 && s_freecam) {
            leave();
            fprintf(stderr, "[FREECAM] off\n");
        }
        if (s_freecam && MEM8(0x007FA274u)) {   /* the game's own pause */
            leave();
            fprintf(stderr, "[FREECAM] off: the game paused\n");
        }
        if (s_freecam && swap && !swap_was && !menu) {
            InterlockedExchange(&s_fc_game, !s_fc_game);
            xbox_InputSetBlocked(s_fc_game ? overlay_pad_menu() : 1);
            fprintf(stderr, "[FREECAM] the pad and keys: %s\n",
                    s_fc_game ? "Conker" : "the camera");
        }
        swap_was = swap;
    }
    if (!s_frozen)
        return;
    if (s_pause) {                      /* no camera, keys or frame step */
        QueryPerformanceCounter(&s_last);
        hold_flag();
        return;
    }
    if (s_have_base && InterlockedExchange(&s_ask_reset, 0))
        reset_camera();
    if (s_have_base && InterlockedExchange(&s_ask_focus, 0))
        focus_on_conker();
    s_step_held = 0;                    /* steer says so again */
    if (!menu && s_have_base && !s_fc_game) {
        steer(&p, pressed);
    } else {
        QueryPerformanceCounter(&s_last);
        if (menu)
            s_after_menu = 1;
    }
    if (!s_frozen)
        return;
    /* The world moved into another level (only it can, here): the camera and
     * the views saved here belong to the old one. */
    if (MEM8(0x007FA1F8u) != s_level) {
        fprintf(stderr, "[PHOTO] another level (0x%02X): leaving\n",
                MEM8(0x007FA1F8u));
        leave();
        return;
    }
    {
        /* Not under a menu (the panel, F2, Report Bug): those hold it, but for
         * freecam, where the game runs under the menus as without it. */
        LONG run = s_freecam || (!menu && (s_resumed || s_step_held));
        if (run != s_running) {
            InterlockedExchange(&s_running, run);
            fprintf(stderr, "[PHOTO] world %s\n", run ? "running" : "frozen");
        }
    }
    /* Frame step: the world update runs this frame; the flag is set at the
     * draw instead, so the frame is drawn as a frozen one (no HUD). */
    if ((s_step_asked || s_running) && !s_flag_set) {
        if (!s_running)
            fprintf(stderr, "[PHOTO] one frame on\n");
        s_step_asked = 0;
        s_stepping = 1;
        return;
    }
    hold_flag();
}

void photo_frame_end(void)
{
    if (!s_flag_set)
        return;
    MEM8(0x007FA275u) = s_flag_saved;
    s_flag_set = 0;
}

/* ---- the view ------------------------------------------------------------ */

static void put_matrix(uint32_t va, const float *m)
{
    memcpy((void *)XBOX_PTR(va), m, 64);
}

/* A guest function with no stack arguments, given eax, ecx and esi, with
 * the guest registers as they were. */
static void guest_call(void (*fn)(void), uint32_t eax, uint32_t ecx,
                       uint32_t esi)
{
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_ebx = g_ebx;
    uint32_t r_esi = g_esi, r_edi = g_edi, r_esp = g_esp;

    g_eax = eax;
    g_ecx = ecx;
    g_esi = esi;
    g_esp -= 4;
    MEM32(g_esp) = 0;               /* return address, popped by the callee */
    fn();
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi; g_esp = r_esp;
}

/* A cdecl guest function with one stack argument. */
static void guest_call_arg(void (*fn)(void), uint32_t arg)
{
    uint32_t r_esp = g_esp;

    g_esp -= 4;
    MEM32(g_esp) = arg;
    guest_call(fn, g_eax, g_ecx, g_esi);
    g_esp = r_esp;
}

/* The game's projection build for camera cam (fastcall, ecx). */
static void build_projection(uint32_t cam)
{
    guest_call(sub_003FB030, g_eax, cam, g_esi);
}

/* After 0x002A60B0: the HUD block the draw runs next only while the pause
 * flag is 0, for the one viewport: its camera made current
 * (camera_set_transform_viewport, esi), the view copied to 0x0081BD30,
 * then 0x002A5EB0 for player [0x007F9FF8] (eax). */
void photo_hud_draw(void)
{
    uint32_t rec, cam, player;

    if (!photo_hud_shown() || MEM32(0x0084A144u) != 0)
        return;
    rec = MEM32(0x0084A13Cu);
    cam = rec ? MEM32(rec + 0x144u) : 0;
    if (!cam)
        return;
    MEM32(0x0084B868u) = cam;
    guest_call(sub_003FB250, g_eax, g_ecx, cam);
    memcpy((void *)XBOX_PTR(0x0081BD30u), (const void *)XBOX_PTR(cam + 0x10u), 64);
    MEM16(0x0081BD26u) = 0;
    player = MEM8(0x007F9FF8u);
    MEM32(0x0084A148u) = player;
    guest_call(sub_002A5EB0, player, g_ecx, g_esi);
}

/* The start: the game's camera as it was drawn last (its world matrix,
 * the view's inverse, at camera +0x50; it looks along minus row 2); no
 * roll. */
static void take_base(uint32_t cam)
{
    float w[16], len;

    memcpy(w, (const void *)XBOX_PTR(cam + 0x50u), 64);
    len = sqrtf(w[8] * w[8] + w[9] * w[9] + w[10] * w[10]);
    if (len < 1e-6f)
        len = 1.0f;
    s_base_pos[0] = w[12];
    s_base_pos[1] = w[13];
    s_base_pos[2] = w[14];
    s_base_yaw = atan2f(-w[8], -w[10]);
    s_base_pitch = asinf(-w[9] / len);
    memcpy(s_pos, s_base_pos, sizeof(s_pos));
    s_yaw = s_base_yaw;
    s_pitch = s_base_pitch;
    s_opt.roll = 0.0f;
    s_opt.fov_game = s_opt.fov = MEMF(cam + 0x104u);
    s_cam = cam;
    s_have_base = 1;
    s_opt.focus = conker_distance();
    if (s_opt.focus <= 0.0f)
        s_opt.focus = DOF_FOCUS;
    fprintf(stderr, "[PHOTO] start at %.1f %.1f %.1f, yaw %.1f pitch %.1f, "
            "fov %.1f, planes %g..%g (cutscene's %g..%g), focus %.0f\n",
            s_pos[0], s_pos[1], s_pos[2], s_yaw / DEG, s_pitch / DEG,
            MEMF(cam + 0x104u), MEMF(cam + 0xFCu), MEMF(cam + 0x100u),
            MEMF(0x0076F7ACu), MEMF(0x0076F7B0u), s_opt.focus);
}

void photo_render_begin(void)
{
    uint32_t rec, cam;
    float w[16], v[16], t[16];
    float f[3], r[3], u[3], cr, sr;
    int i, j;
    uint8_t cut;
    uint32_t chars, act;

    /* One viewport only; the pause key's freeze keeps the game's view. */
    if (!s_frozen || s_pause || MEM32(0x0084A144u) != 0)
        return;
    if (s_stepping) {                   /* the world has moved a frame */
        s_stepping = 0;
        hold_flag();
    }
    rec = MEM32(0x0084A13Cu);
    cam = rec ? MEM32(rec + 0x144u) : 0;
    if (!cam)
        return;
    if (!s_have_base)
        take_base(cam);
    if (cam != s_cam) {                 /* a new camera: its own view back */
        MEMF(s_cam + 0x104u) = s_opt.fov_game;
        s_opt.fov_game = MEMF(cam + 0x104u);
        s_cam = cam;
    }

    /* Forward from yaw and pitch, right level (forward x up), up = right
     * x forward; then the roll turns right and up about forward (more
     * roll: up leans left, anticlockwise). Row 2 is minus forward. */
    f[0] = sinf(s_yaw) * cosf(s_pitch);
    f[1] = sinf(s_pitch);
    f[2] = cosf(s_yaw) * cosf(s_pitch);
    r[0] = -cosf(s_yaw);
    r[1] = 0.0f;
    r[2] = sinf(s_yaw);
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    cr = cosf(s_opt.roll);
    sr = sinf(s_opt.roll);
    for (i = 0; i < 3; i++) {
        w[i] = r[i] * cr + u[i] * sr;
        w[4 + i] = u[i] * cr - r[i] * sr;
        w[8 + i] = -f[i];
        w[12 + i] = s_pos[i];
    }
    w[3] = w[7] = w[11] = 0.0f;
    w[15] = 1.0f;

    /* Rigid inverse: the 3x3 transposed, the position through it negated. */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            v[4 * i + j] = w[4 * j + i];
        v[4 * i + 3] = 0.0f;
    }
    for (j = 0; j < 3; j++)
        v[12 + j] = -(w[12] * w[4 * j] + w[13] * w[4 * j + 1]
                      + w[14] * w[4 * j + 2]);
    v[15] = 1.0f;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[4 * i + j] = v[4 * j + i];

    put_matrix(cam + 0x10u, v);
    put_matrix(rec + 0x40u, v);
    put_matrix(rec + 0x100u, t);
    put_matrix(cam + 0x50u, w);
    put_matrix(rec + 0x80u, w);
    MEMF(cam + 0x104u) = s_opt.fov;
    conker_AspectRefresh(cam);
    build_projection(cam);
    memcpy((void *)XBOX_PTR(rec + 0x148u), (const void *)XBOX_PTR(cam + 0x114u),
           0x2A * 4);

    /* What is drawn is chosen by world_update, from the view of its time:
     * the level's geometry by 0x00064A10 (eax = viewport; from 0x00064B00,
     * its handlers from the table at 0x008281C8: 0x003FDCA0 takes the render
     * camera's +0x50), characters by 0x00081190 (hidden bits +0x16E per
     * viewport), scenery by 0x00068D10(0) (each viewport's draw list); the
     * last two test the frustum planes and a GPU occlusion box (0x00313C20,
     * visible while any of its last 4 results saw pixels). Run again from
     * ours, as world_update would at this point of the frame. A character
     * out of view fades (+0xB8 per viewport, 0x19 a frame) only in
     * world_update, so one hidden when the freeze began stays at 0 even when
     * the pass shows it: the viewport's camera cut byte [0x00762E40 + n] makes
     * the fade and the occlusion box take effect at once, as after a cut. */
    cut = MEM8(0x00762E40u);
    MEM8(0x00762E40u) = 1;
    if (MEM32(0x0076FE00u) != 0 && MEM32(0x0076F79Cu) == 0)
        guest_call(sub_00064A10, 0, g_ecx, g_esi);
    guest_call(sub_00081190, g_eax, g_ecx, g_esi);
    guest_call_arg(sub_00068D10, 0);
    MEM8(0x00762E40u) = cut;

    /* A character hidden in every viewport gets no pose in world_update's
     * animation step (0x002C5500 moves its bone buffer +0x3C8 to the spare
     * +0x3CC), and the draw (0x0007F4D0) skips one with a skeleton +0x3C4
     * but no +0x3C8. One now in our view gets its pose from its animation
     * as it stands: 0x002C5070 (eax = character) takes the spare buffer. */
    chars = MEM32(0x0084A5F8u);
    act = MEM32(0x0084A5FCu);
    for (i = 0; chars && act && i < 0x80; i++) {
        uint32_t c = chars + (uint32_t)i * 0x6D0u;

        if (MEM8(act + (uint32_t)i) && (MEM8(c + 0x16Eu) & 1) == 0
            && MEM32(c + 0x3C4u) && !MEM32(c + 0x3C8u) && MEM32(c + 0x3CCu)
            && !MEM32(c + 0x3D8u))
            guest_call(sub_002C5070, c, g_ecx, g_esi);
    }
}
