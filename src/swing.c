/**
 * The swing piece (Extras, 0.6.0; to-dos 42, 233, 234; SPEC 10.4 "Swing").
 * Plan: docs/research/r4-0.6.1-swing-plan.md (facts), r3-swing.md.
 *
 * The bat / frying pan combo (0x00329E80, from the bat / pan update
 * 0x0032A300): a press of the right trigger (an edge: held = one swing)
 * starts AttackPt1 (moving: RunAttackPt1) and opens a window on the
 * controller: ctl +0x560 counts down from (dead + open) / 60 s, ctl +0x564
 * holds open / 60. A press while +0x560 >= +0x564 is too early: the combo
 * ends with a 0.5 s lock (+0x55C). A press with 0 < +0x560 < +0x564 is
 * queued (+0x568) and starts the next part once the swing playing reaches
 * its table frame (Pt1 frame 17). Rows (table 0x0074A358): after press 1 the
 * window opens at 0.25 s and closes at 0.583 s, after press 2 at 0.2 /
 * 0.417 s, Pt3 ends the combo (+0x548 = 0, no window). A window that runs
 * out with the combo going (+0x548 > 0) locks 0.5 s too. +0x548 = combo
 * running, +0x54C = the next part's number (2 while Pt1 plays, 3 in Pt2).
 * Character state +0xA4: 0x836 FireHold, 0x836-0x83E stand / move with the
 * weapon out, 0x83F-0x841 AttackPt1-3, 0x847-0x849 RunAttackPt1-3.
 *
 * Quick Swing (Normal / Easy): while the right trigger is held, the trigger
 * the game sees is let up after each accepted press and pressed again the
 * moment the game can take the next part: Normal the first two parts (then
 * the game's own lock, then Pt1 again), Easy all three (then Pt1 at
 * FireHold). The player's own fresh press always passes (Normal + a press
 * in Pt2's window = the uppercut).
 * Forgiving Combo: a fresh press the game would refuse (too early, the
 * lock, a press queued, the last part's end) is kept from the game and
 * given to it as soon as it can take it (still held: let through; let go: a
 * pulse), at most FORGIVE_MS after the press, else dropped.
 * The Vault suit (kind 0x77, EndArena): punches 0x27A-0x27D chain only while
 * char +0x20B (a key event of the punch) or +0x214 (hits landed) is set;
 * Quick Swing presses again then: Normal two punches, Easy up to four (the
 * 3rd and 4th only come when the punches land, as the game decides).
 *
 * Decisions on the game thread once a frame (swing_frame: the game's fields,
 * in seconds and clip time, so any frame rate); the trigger is changed in
 * the toolkit's final pad hook (swing_final_hook), which sees what the game
 * gets with the keyboard merged in, so pad, keys, Classic and Modern share
 * one path. Times here are wall milliseconds (clock(), as the log).
 *
 * Fast Weapon Draw: the take-out / put-away request's speed (0x002F1790's
 * 5th word, re-sent with every re-ask; 0x002F1BD0's 2nd for a weapon drawn
 * while moving) times FAST_DRAW_X, for the request the draw is (ctl +0x1F8
 * 1 = coming out, 3 = going away; +0x474 = its animation), latched for the
 * rest of that animation (+0x1F8 turns 2 / 0 at the "in hand" event before
 * the clip ends). Key events are never skipped by a faster clip (0x002C4D00
 * walks every keyframe passed). Single player, the player only.
 *
 * Free Swing and the camera switch (part B): below, at free_frame and
 * view_frame. Their changes to the pad go in the same final hook, before
 * the right-trigger logic above (free_hook), so Quick Swing and Forgiving
 * Combo work on Free Swing's button too.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "controls.h"
#include "xbox_kbm.h"
#include "dev_logs.h"
#include "swing.h"

#define CAM0          0x008470DCu   /* camera 0; its character at +0x3E0 */
#define MP_FLAG       0x007819D5u   /* multiplayer */

#define KIND_BAT      0x75u
#define KIND_PAN      0x76u
#define KIND_SUIT     0x77u

#define ST_READY_LO   0x836u        /* FireHold */
#define ST_READY_HI   0x83Eu
#define ST_PT1        0x83Fu
#define ST_PT3        0x841u
#define ST_RUN_PT1    0x847u
#define ST_RUN_PT3    0x849u
#define ST_PUNCH1     0x27Au
#define ST_PUNCH4     0x27Du
#define ST_SUIT_DOWN_LO 0x289u      /* enviro_getup .. envirosweeptwitch: */
#define ST_SUIT_DOWN_HI 0x28Cu      /* knocked down by the alien */

#define PULSE_MS      80            /* a press held at least this long (Txukie's) */
#define GAP_MS        40            /* the trigger let up at least this long */
#define MIN_FRAMES    2             /* and for at least this many game frames */
#define MIN_GAP_MS    250           /* never two presses closer (breaks the combo) */
#define SUIT_GAP_MS   150           /* the suit's punches (no window to break) */
#define FORGIVE_MS    400           /* a kept press is dropped after this */
#define EARLY_MARGIN  0.017f        /* a frame into the window, to be safe */
#define END_MARGIN    0.05f         /* no press this close to the window's end */
#define FAST_DRAW_X   2.5f          /* 2.5x the game's (user 2026-11-23: 5.75x was too fast) */

static int va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u;
}

/* The same clock as the run log's other t= (attach_ms: wall ms since the
 * start), so [SWING] lines line up with [ANIMREQ]. */
static long now_ms(void)
{
    return (long)(clock() * 1000 / CLOCKS_PER_SEC);
}

static int log_on(void)
{
    return dev_log(DLOG_SWING);
}

/* ── settings ─────────────────────────────────────────────────────────── */

static volatile int g_quick = -1, g_forgive = -1, g_fast = -1;
static float g_fast_x = 0.0f;

static int quick_from(const char *v)
{
    if (v && !_stricmp(v, "normal"))
        return SWING_QUICK_NORMAL;
    if (v && !_stricmp(v, "easy"))
        return SWING_QUICK_EASY;
    return SWING_QUICK_OFF;
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int swing_quick(void)
{
    if (g_quick < 0)
        g_quick = quick_from(getenv("RECOMP_QUICK_SWING"));
    return g_quick;
}

void swing_set_quick(const char *value)
{
    static const char *const names[] = { "off", "normal", "easy" };
    int q = quick_from(value);
    if (q != g_quick)
        fprintf(stderr, "[SWING] quick swing %s\n", names[q]);
    g_quick = q;
}

int swing_forgiving(void)
{
    if (g_forgive < 0)
        g_forgive = env_on("RECOMP_FORGIVING_COMBO");
    return g_forgive;
}

void swing_set_forgiving(int on)
{
    if ((on != 0) != (g_forgive > 0))
        fprintf(stderr, "[SWING] forgiving combo %s\n", on ? "on" : "off");
    g_forgive = on != 0;
}

int swing_fast_draw(void)
{
    if (g_fast < 0)
        g_fast = env_on("RECOMP_FAST_DRAW");
    return g_fast;
}

void swing_set_fast_draw(int on)
{
    if ((on != 0) != (g_fast > 0))
        fprintf(stderr, "[SWING] fast weapon draw %s\n", on ? "on" : "off");
    g_fast = on != 0;
}

static volatile int g_free = -1, g_cswitch = -1;

int swing_free(void)
{
    if (g_free < 0)
        g_free = env_on("RECOMP_FREE_SWING");
    return g_free;
}

void swing_set_free(int on)
{
    if ((on != 0) != (g_free > 0))
        fprintf(stderr, "[SWING] free swing %s\n", on ? "on" : "off");
    g_free = on != 0;
}

int swing_camera_switch(void)
{
    if (g_cswitch < 0)
        g_cswitch = env_on("RECOMP_CAMERA_SWITCH");
    return g_cswitch;
}

void swing_set_camera_switch(int on)
{
    if ((on != 0) != (g_cswitch > 0))
        fprintf(stderr, "[SWING] camera switch %s\n", on ? "on" : "off");
    g_cswitch = on != 0;
}

int swing_zoom_reset_off(void)
{
    return swing_camera_switch();
}

static volatile int g_air = -1;

int swing_air(void)
{
    if (g_air < 0)
        g_air = env_on("RECOMP_AIR_SWING");
    return g_air;
}

void swing_set_air(int on)
{
    if ((on != 0) != (g_air > 0))
        fprintf(stderr, "[SWING] swing while jumping %s\n", on ? "on" : "off");
    g_air = on != 0;
}

/* RECOMP_FAST_DRAW_SPEED=<x>: the factor (debug; default FAST_DRAW_X). */
static float fast_x(void)
{
    if (g_fast_x <= 0.0f) {
        const char *e = getenv("RECOMP_FAST_DRAW_SPEED");
        float v = e && *e ? (float)atof(e) : 0.0f;
        g_fast_x = v >= 1.0f && v <= 20.0f ? v : FAST_DRAW_X;
    }
    return g_fast_x;
}

static uint32_t player(void)
{
    uint32_t cam = MEM32(CAM0);
    uint32_t c = va_ok(cam) ? MEM32(cam + 0x3E0u) : 0;
    return va_ok(c) ? c : 0;
}

/* ── Fast Weapon Draw ─────────────────────────────────────────────────── */

static uint32_t s_fd_index = ~0u;   /* the draw being sped up (game thread) */
static float s_fd_x = 1.0f;         /* its factor */

/* Free Swing's own take-out / put-away under way (free_frame): FS_DRAW_X
 * instead of Fast Weapon Draw's factor (user 2026-11-23: "swing the bat
 * instantly, ... instantly returned"), and its two sounds left out
 * (swing_mute_sound). */
static volatile LONG s_fs_quick;
#define FS_DRAW_X     30.0f         /* the bat's draw ~40 ms, its put-away ~25 ms */

float swing_draw_factor(uint32_t c, uint32_t idx, int layered)
{
    uint32_t ctl, d;
    int drawing, fs = s_fs_quick != 0;
    float x;

    if ((!swing_fast_draw() && !fs && s_fd_index == ~0u) || MEM8(MP_FLAG) || !c
        || c != player())
        return 1.0f;
    ctl = MEM32(c + 0x568u);
    if (!va_ok(ctl))
        return 1.0f;
    d = MEM32(ctl + 0x1F8u);
    drawing = (d == 1u || d == 3u) && idx == MEM32(ctl + 0x474u);
    x = fs ? FS_DRAW_X : swing_fast_draw() ? fast_x() : 1.0f;
    if (layered)
        return drawing ? x : 1.0f;
    if (drawing && idx != s_fd_index && x > 1.0f) {
        s_fd_index = idx;
        s_fd_x = x;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld %s draw: index 0x%X (%s, weapon 0x%02X) x%.2f\n",
                    now_ms(), fs ? "free swing" : "fast", idx,
                    d == 1u ? "coming out" : "going away", MEM32(ctl + 0xA0u) & 0x7Fu, x);
    } else if (!drawing && idx != s_fd_index) {
        s_fd_index = ~0u;
    }
    return idx == s_fd_index ? s_fd_x : 1.0f;
}

/* The sound key event (0x00305250) of character chr, sound-table entry
 * `entry`: 1 = leave it out (the bat's take-out / put-away sound during Free
 * Swing's own draw; the frying pan plays the bat's two in play). */
int swing_mute_sound(uint32_t chr, uint32_t entry)
{
    uint32_t name;
    if (!s_fs_quick || !chr || chr != player() || entry >= 0x2000u)
        return 0;
    name = MEM32(0x0065E060u + entry * 0x30u + 4u);
    if (!va_ok(name))
        return 0;
    if (!strcmp((const char *)&MEM8(name), "baseballbat_getout")
        || !strcmp((const char *)&MEM8(name), "baseballbat_putaway")) {
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld free swing: sound %s left out\n", now_ms(),
                    (const char *)&MEM8(name));
        return 1;
    }
    return 0;
}

/* ── the trigger: shared between the game thread and the pad hook ─────── */

enum { FORCE_NONE, FORCE_UP, FORCE_DOWN };

static volatile LONG s_charge;      /* the swing is ours (game thread) */
static volatile LONG s_block;       /* a fresh press now would be refused */
static volatile LONG s_force;       /* FORCE_*: up = let go, down = pressed */
static volatile LONG s_unblock;     /* give the kept press to the game */
static volatile LONG s_raw;         /* the trigger held (keys and pad) */
static volatile LONG s_kept;        /* the hook keeps a press back */
static volatile LONG s_kept_n;      /* presses kept back, counted */
static volatile LONG s_passed_n;    /* fresh presses let through, counted */
static volatile LONG s_pulse_until; /* our pulse (FORCE_DOWN) ends then, ms */

/* Free Swing and the camera switch (game thread -> pad hook). */
static volatile LONG s_fs_apply;    /* Free Swing applies: X is its button, kept from the game */
static volatile LONG s_fs_pad_x;    /* the controller's Reload button (pad_filter) */
static volatile LONG s_fs_key;      /* the Attack key, as Free Swing's button */
static volatile LONG s_fs_direct;   /* its button is the right trigger now (weapon in hand) */
static volatile LONG s_fs_b_until;  /* our B press ends then, ms */
static volatile LONG s_fs_rt_until; /* our right-trigger press ends then, ms */
static volatile LONG s_fs_player_b; /* B presses not ours (the player's, Modern's), counted */
static volatile LONG s_atk_rt;      /* the Attack key is the right trigger (Classic, out of the profile) */
static volatile LONG s_cs_ready;    /* the camera switch has the zoom button now */
static volatile LONG s_cs_toggles;  /* its presses, counted */
static volatile LONG s_view_normal = 1; /* the view picked: 1 = normal (the start), 0 = over the shoulder */
static volatile LONG s_lstick;      /* the left stick out of its dead zone */
static volatile LONG s_stick_rel;   /* its way from the camera, 0x10000 a turn (left +) */
static volatile LONG s_sh_rot;      /* shoulder view, weapon out: his facing - the camera's way, 0x10000 a turn */
static volatile LONG s_run_on;      /* run the way the stick points (normal view, weapon in hand) */
static volatile LONG s_stick_held;  /* the left stick out of its dead zone (while s_run_on) */
static volatile LONG s_stick_angle; /* its angle, 0x10000 a turn (Txukie's) */
static volatile LONG s_turn_calls;  /* the move turn hook's calls (log swing 2) */

#define STICK_DEAD     7849         /* XInput's left-stick dead zone */
#define DEG_TO_ANGLE   182.04444885f  /* 0x10000 / 360, as the game has it */
#define RAD_TO_ANGLE   10430.378f     /* 0x10000 / (2 pi) */

void swing_pad_x(int held)
{
    InterlockedExchange(&s_fs_pad_x, held != 0);
}

/* Free Swing's and the camera switch's changes to what the game gets: B and
 * the right trigger pressed for it, X kept from it, the zoom button taken by
 * the switch, the left stick straight ahead on the normal view (keys and pad
 * merged: W / A / S / D too). Before Quick Swing / Forgiving Combo, which
 * work on the right trigger this leaves. */
static void free_hook(XBOX_INPUT_STATE *st, long t)
{
    static int rth_was, rth_hide, b_was;
    XBOX_GAMEPAD *g = &st->Gamepad;
    BYTE *a = g->bAnalogButtons;
    int rth = (g->wButtons & XBOX_GAMEPAD_RIGHT_THUMB) != 0;
    int b = a[XBOX_BUTTON_B] > XBOX_ANALOG_BUTTON_THRESHOLD;

    /* The camera switch: the zoom button (right stick click, V) switches
     * the view; the game never sees that press. */
    if (s_cs_ready && rth && !rth_was) {
        InterlockedIncrement(&s_cs_toggles);
        rth_hide = 1;
    }
    if (!rth)
        rth_hide = 0;
    rth_was = rth;
    if (rth_hide)
        g->wButtons &= (WORD)~XBOX_GAMEPAD_RIGHT_THUMB;

    /* A B press of the player's (or Modern's): the weapon is theirs. */
    if (b && !b_was)
        InterlockedIncrement(&s_fs_player_b);
    b_was = b;
    if (s_fs_apply)
        a[XBOX_BUTTON_X] = 0;
    if (s_atk_rt)
        a[XBOX_BUTTON_RTRIGGER] = 255;
    if (s_fs_apply && s_fs_direct && (s_fs_pad_x || s_fs_key))
        a[XBOX_BUTTON_RTRIGGER] = 255;
    if (t < s_fs_rt_until)
        a[XBOX_BUTTON_RTRIGGER] = 255;
    if (t < s_fs_b_until)
        a[XBOX_BUTTON_B] = 255;

    /* Run, don't strafe (Txukie's): the game gets the stick straight ahead
     * (its length kept: walk / run), the move turn the way it points from
     * the camera (swing_heading). */
    {
        float x = g->sThumbLX, y = g->sThumbLY;
        float m = sqrtf(x * x + y * y);
        LONG rot = s_sh_rot;
        /* The shoulder view with nothing out needs nothing here: the game
         * gets the stick as it is and runs him the way it points from the
         * held camera. With the bat / pan out the game strafes from his
         * facing: while he turns to the camera's way (sh_frame), the stick
         * is turned by the difference, so he moves the way it points from
         * the camera (round 4). */
        InterlockedExchange(&s_lstick, m > (float)STICK_DEAD);
        if (m > (float)STICK_DEAD)          /* the weaponless strafe's clip pick */
            InterlockedExchange(&s_stick_rel, (LONG)(atan2f(-x, y) * RAD_TO_ANGLE));
        if (rot && m > (float)STICK_DEAD) {
            float a = atan2f(-x, y) - (float)rot * (6.28318530718f / 65536.0f);
            float mm = m > 32767.0f ? 32767.0f : m;
            g->sThumbLX = (SHORT)(-mm * sinf(a));
            g->sThumbLY = (SHORT)(mm * cosf(a));
        }
    }
    if (s_run_on) {
        float x = g->sThumbLX, y = g->sThumbLY, m = sqrtf(x * x + y * y);
        if (m > STICK_DEAD) {
            uint32_t ang = (uint32_t)(int32_t)(atan2f(-x, y) * RAD_TO_ANGLE) + 0x4000u;
            InterlockedExchange(&s_stick_angle, (LONG)(ang & 0xFFFFu));
            InterlockedExchange(&s_stick_held, 1);
            g->sThumbLX = 0;
            g->sThumbLY = (SHORT)(m > 32767.0f ? 32767.0f : m);
        } else {
            InterlockedExchange(&s_stick_held, 0);
        }
    } else {
        InterlockedExchange(&s_stick_held, 0);
    }
}

static int swing_final_hook(DWORD port, XBOX_INPUT_STATE *st)
{
    static int raw_was, kept, swallow;
    static long fresh_until;
    BYTE *rt;
    int raw, rising, pulsing;
    long t;

    if (port != 0)
        return 0;
    free_hook(st, now_ms());
    rt = &st->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    raw = *rt > XBOX_ANALOG_BUTTON_THRESHOLD;
    rising = raw && !raw_was;
    raw_was = raw;
    InterlockedExchange(&s_raw, raw);
    if (!raw)
        kept = swallow = 0;
    if (!s_charge) {
        /* Let go: a press kept back, or held while we had the trigger up,
         * stays from the game until it is let go (no press out of it). */
        InterlockedExchange(&s_kept, 0);
        fresh_until = 0;
        if (kept || swallow)
            *rt = 0;
        return 0;
    }
    t = now_ms();
    /* Our pulse ends on time here, not at the game thread's next frame (a
     * press right after it is the player's own). A press during it is part
     * of it: the trigger stays down (up and down again would be a second,
     * too-early press). */
    pulsing = s_force == FORCE_DOWN && t < s_pulse_until;
    if (rising && !pulsing) {
        if (s_block) {
            kept = 1;
            InterlockedIncrement(&s_kept_n);
        } else {
            fresh_until = t + PULSE_MS;
            InterlockedIncrement(&s_passed_n);
        }
    }
    if (InterlockedExchange(&s_unblock, 0) && kept) {
        kept = 0;
        fresh_until = t + PULSE_MS;
    }
    InterlockedExchange(&s_kept, kept);
    if (kept)
        *rt = 0;
    else if (pulsing)
        *rt = 255;                          /* our pulse: down to its end */
    else if (t < fresh_until)
        ;                                   /* the player's press, as it is */
    else if (s_force == FORCE_UP)
        *rt = 0;
    swallow = raw && *rt == 0;
    return 0;
}

void swing_install(void)
{
    xbox_InputSetPadFinal(swing_final_hook);
}

/* ── Layered draws (round 3, user 2026-11-24) ─────────────────────────────
 * The bat / pan update asks every take-out and put-away full body (TakeOut
 * 0x834 / PutBack 0x835): a running Conker stops for it, and after a
 * put-away the game asks "stance" (a full stop) before the run starts again
 * from rest (Free Swing's put-away after a running swing: the user's
 * "momentum killed"); during a take-out he doesn't turn either. The game has
 * a layered way too (0x00319BC0 / 0x0031A0D0 with "layered" set: the clip on
 * the right arm, slot 3, bone list 0x0074A0A0), which no weapon update asks
 * (all 27 push 0). Asked here while Conker moves (the left stick out of its
 * dead zone, on the floor) and Free Swing's own draw is under way or the
 * camera switch is on. RECOMP_SWING_LAYERED=0: never (A/B), =2: every draw
 * of the player's bat / pan, standing too (tests).
 * The draw's key events "in hand" / "away" (0x00309B30 / 0x00309B90: ctl
 * +0x1F8 1 -> 2 / -> 0, the model words copied) are watched: when the game
 * hasn't run them by LAY_EVENT_AT of the clip, lay_frame runs them itself
 * (logged either way). */
#define LAY_OUT_CLIP   1.47f        /* TakeOut, clip seconds at speed 1 (1.13 s at 1.3) */
#define LAY_OUT_EVENT  0.60f        /* its "in hand", a share of the clip (0.7 s of 1.15 s, measured) */
#define LAY_AWAY_CLIP  0.45f        /* PutBack (0.25 s at 1.82, measured) */
#define LAY_AWAY_EVENT 0.95f        /* its "away": at the end */
#define LAY_PICK_MS    300          /* the clip's end event this long after its end at most */

static struct {
    uint32_t chr, idx;
    int out;                        /* +0x1F8 it set: 1 coming out, 3 going away */
    int on;                         /* waiting for its event */
    int marked;                     /* the game marked it done (+0x1F8) */
    int repicked;                   /* a put-away's movement re-picked by us */
    long t0, due, end;
    float speed;
    float carry;                    /* a put-away's: the weapon run's speed (ctl +0x390) */
} s_lay;

/* After a layered put-away the plain movement (0x002D5B30) picks stand or
 * run from his speed +0x110, which the weapon's run leaves at 0 (it keeps
 * its own, ctl +0x390): it asked "stance" and the run built up again from
 * rest (~1 s, +0.5 a frame). The weapon run's speed goes to +0x110 the
 * moment the weapon is away, while the stick is held (same units: ~33 at a
 * full run). */
static void lay_carry(uint32_t chr, long t)
{
    float v = MEMF(chr + 0x110u);
    if (s_lay.out != 3 || s_lay.carry <= v || !s_lstick)
        return;
    MEMF(chr + 0x110u) = s_lay.carry;
    /* +0x118: the speed the stick asks (35 at a full push, 0 while the
     * weapon runs); 0 with +0x110 up = a skid / stance pick that frame. */
    if (MEMF(chr + 0x118u) < s_lay.carry)
        MEMF(chr + 0x118u) = s_lay.carry;
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld layered put-away: his speed %.2f -> %.2f (the run goes on)\n",
                t, v, s_lay.carry);
}

static int lay_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_SWING_LAYERED");
        m = e && *e ? atoi(e) : 1;
    }
    return m;
}

static int noclip_now(long t);

int swing_layered(uint32_t chr, int out)
{
    int m = lay_mode();
    (void)out;
    if (!m || !chr || chr != player() || MEM8(MP_FLAG))
        return 0;
    if (MEMF(chr + 0xDCu) != 0.0f)
        return swing_air_draw(chr);     /* Free Swing's in the air: on the arm, the jump goes on */
    if (noclip_now(now_ms()))
        return s_fs_quick != 0;         /* Free Swing's in noclip: on the arm, the spin pose stays */
    if (m == 2)
        return 1;
    return (s_fs_quick || swing_camera_switch()) && s_lstick;
}

void swing_layer_started(uint32_t chr, uint32_t idx, float speed)
{
    uint32_t ctl, d;
    if (!chr || chr != player())
        return;
    ctl = MEM32(chr + 0x568u);
    if (!va_ok(ctl))
        return;
    d = MEM32(ctl + 0x1F8u);
    if ((d != 1u && d != 3u) || MEM32(ctl + 0x474u) != idx)
        return;
    s_lay.chr = chr;
    s_lay.idx = idx;
    s_lay.out = (int)d;
    s_lay.on = 1;
    s_lay.t0 = now_ms();
    s_lay.speed = speed > 0.01f ? speed : 1.0f;
    s_lay.carry = d == 3u ? MEMF(ctl + 0x390u) : 0.0f;
    s_lay.marked = 0;
    s_lay.repicked = 0;
    s_lay.due =s_lay.t0 + (long)((d == 1u ? LAY_OUT_CLIP * LAY_OUT_EVENT
                                           : LAY_AWAY_CLIP * LAY_AWAY_EVENT)
                                  / s_lay.speed * 1000.0f);
    s_lay.end = s_lay.t0 + (long)((d == 1u ? LAY_OUT_CLIP : LAY_AWAY_CLIP) / s_lay.speed
                                  * 1000.0f) + LAY_PICK_MS;
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld layered %s: index 0x%X speed %.2f (state 0x%X; "
                "event due in %ld ms)\n", s_lay.t0, d == 1u ? "take-out" : "put-away", idx,
                s_lay.speed, MEM32(chr + 0xA4u), s_lay.due - s_lay.t0);
}

/* Once a frame while a layered draw is under way. The game's single-player
 * weapon step (0x00300740) marks a draw done (+0x1F8 1 -> 2, 3 -> 0) at
 * once when the body isn't playing the draw's clip (+0xA4 != +0x3DC), as
 * here: the arm's clip plays on, but "in hand" (0x00309B30) then finds
 * +0x1F8 2 and leaves the model words alone (the hand's model +0x2FC
 * stayed the old one). They are switched here at the clip's own moment
 * (LAY_OUT_EVENT / LAY_AWAY_EVENT); +0x1F8 too if nothing marked it. */
static void lay_frame(long t)
{
    uint32_t ctl, d, want;
    const char *what;

    if (!s_lay.on)
        return;
    what = s_lay.out == 1 ? "take-out" : "put-away";
    want = s_lay.out == 1 ? 2u : 0u;
    ctl = s_lay.chr == player() ? MEM32(s_lay.chr + 0x568u) : 0;
    if (!va_ok(ctl)) {
        s_lay.on = 0;
        return;
    }
    d = MEM32(ctl + 0x1F8u);
    if (d != (uint32_t)s_lay.out && d != want) {
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld layered %s: something else after %ld ms (+1F8 %u, "
                    "state 0x%X)\n", t, what, t - s_lay.t0, d, MEM32(s_lay.chr + 0xA4u));
        s_lay.on = 0;
        return;
    }
    if (d == want && !s_lay.marked) {
        s_lay.marked = 1;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld layered %s: the game marked it done after %ld ms "
                    "(state 0x%X)\n", t, what, t - s_lay.t0, MEM32(s_lay.chr + 0xA4u));
        if (s_lay.out == 3)
            lay_carry(s_lay.chr, t);
    }
    if (t < s_lay.due)
        return;
    if (d == (uint32_t)s_lay.out) {
        MEM32(ctl + 0x1F8u) = want;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld layered %s: marked done by us after %ld ms\n", t,
                    what, t - s_lay.t0);
        if (s_lay.out == 3)
            lay_carry(s_lay.chr, t);
    }
    /* The arm slot's last animation (ctl +0x45C, slot 3) reads as stopped:
     * the bat update's gate (0x0031A010) refuses a swing while it names the
     * take-out or put-away (it held a layered draw's first swing ~0.2 s). */
    if (s_lay.out == 1 && MEM32(ctl + 0x45Cu) == s_lay.idx) {
        MEM32(ctl + 0x45Cu) = 0xFFFFFFFFu;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld layered %s: arm slot marked stopped\n", t, what);
    }
    /* The models, as 0x00309B30 ("in hand") / 0x00309B90 ("away") do. */
    if (MEM32(ctl + 0x2FCu) != MEM32(ctl + 0x2F8u) || MEM32(ctl + 0x304u) != MEM32(ctl + 0x300u)) {
        MEM32(ctl + 0x2FCu) = MEM32(ctl + 0x2F8u);
        MEM32(ctl + 0x304u) = MEM32(ctl + 0x300u);
        MEM8(ctl + 0xB0u) = MEM8(ctl + 0xB1u);
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld layered %s: the hand's model switched after %ld ms\n",
                    t, what, t - s_lay.t0);
    }
    s_lay.on = 0;
}

/* After a layered put-away is marked done, until its clip's end (+ a little):
 * his speed +0x110 kept at the carried run speed at least, each frame, while
 * the stick is held. A put-away at the game's speed (1.82: 0.25 s) is marked
 * done at once, and the frame after, his speed was 0 again (zeroed in that
 * frame's update) and the plain run built up from rest under the arm's clip. */
static void lay_carry_frame(long t)
{
    uint32_t chr = s_lay.chr, ctl;
    if (s_lay.out != 3 || !s_lay.marked || t > s_lay.end || !chr || chr != player())
        return;
    ctl = MEM32(chr + 0x568u);
    if (!va_ok(ctl) || MEM32(ctl + 0xA0u) != 0u || MEMF(chr + 0xDCu) != 0.0f)
        return;
    if (s_lay.carry >= 1.0f && s_lstick && MEMF(chr + 0x110u) < 1.0f)
        lay_carry(chr, t);              /* zeroed (not the game's own slowing) */
    /* The body still in a bat animation (the clip's end event didn't come):
     * the movement re-picked once, as that event would (the plain run). */
    if (!s_lay.repicked && MEM32(chr + 0xA4u) >= 0x834u && MEM32(chr + 0xA4u) <= 0x849u) {
        uint32_t st = MEM32(chr + 0xA4u);
        s_lay.repicked = 1;
        swing_move_pick(chr, s_lay.carry);
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld layered put-away: body 0x%X -> 0x%X (re-picked at "
                    "speed %.2f)\n", t, st, MEM32(chr + 0xA4u), s_lay.carry);
    }
}

/* The draw clip's end event (0x0030AA60) re-picks his movement: 0x002D5B30
 * with no wanted speed, from +0x110 alone, which the weapon's run left at 0:
 * "stance" (a stop; at speed a skid). Right after a layered put-away, the
 * stick held: the run's speed as both (round 3: Free Swing's put-away while
 * running keeps him running). 1 = *want set. */
int swing_pick_speed(uint32_t chr, float *want)
{
    long t = now_ms();
    float v;
    if (!chr || chr != player() || s_lay.chr != chr || s_lay.out != 3 || s_lay.carry < 1.0f
        || !s_lstick || t < s_lay.t0 || t > s_lay.end)
        return 0;
    v = MEMF(chr + 0x110u);
    /* Only what was zeroed (his speed) or not given (the wanted speed): a
     * lower speed of the game's own (a wall, a slope) stays. */
    if (v >= 1.0f && *want >= 1.0f)
        return 0;
    if (v < 1.0f)
        MEMF(chr + 0x110u) = s_lay.carry;
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld layered put-away: his move re-picked at speed %.2f "
                "(his %.2f, wanted %.2f)\n", t, s_lay.carry, v, *want);
    if (*want < 1.0f)
        *want = s_lay.carry;
    return 1;
}

/* ── the game thread ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t chr, ctl, st;
    int kind;                       /* KIND_* of what is in hand, 0 none */
    int out;                        /* +0xA0: what B is doing (also drawing, going away) */
    int step, next, queued, air;
    int air_ok;                     /* in the air, and Swing While Jumping lets it swing */
    float lock, win, mark;
} Look;

static void look(Look *L, uint32_t chr)
{
    uint32_t ctl = MEM32(chr + 0x568u);
    memset(L, 0, sizeof *L);
    L->chr = chr;
    L->ctl = ctl;
    L->st = MEM32(chr + 0xA4u);
    L->air = MEMF(chr + 0xDCu) != 0.0f;
    if (!va_ok(ctl))
        return;
    L->out = (int)(MEM32(ctl + 0xA0u) & 0x7Fu);
    if (MEM32(ctl + 0x1F8u) == 2u)
        L->kind = L->out;
    L->step = (int)MEM32(ctl + 0x548u);
    L->next = (int)MEM32(ctl + 0x54Cu);
    L->lock = MEMF(ctl + 0x55Cu);
    L->win = MEMF(ctl + 0x560u);
    L->mark = MEMF(ctl + 0x564u);
    L->queued = MEM8(ctl + 0x568u) != 0;
    L->air_ok = L->air && swing_air_gate(chr);
}

static int swinging(uint32_t st)
{
    return (st >= ST_PT1 && st <= ST_PT3) || (st >= ST_RUN_PT1 && st <= ST_RUN_PT3);
}

static int ready(uint32_t st)
{
    return st >= ST_READY_LO && st <= ST_READY_HI;
}

static int air_ready(uint32_t st);

/* The game would start a new combo (Pt1) on a press now (in the air: when
 * Swing While Jumping lets it, from a jump). */
static int can_start(const Look *L)
{
    return (!L->air || L->air_ok) && L->lock == 0.0f && !L->queued
        && L->step < 1 && (ready(L->st) || (L->air_ok && air_ready(L->st)));
}

/* The game would take the next part on a press now. */
static int can_chain(const Look *L)
{
    return (!L->air || L->air_ok) && L->lock == 0.0f && !L->queued && L->step >= 1
        && L->win > END_MARGIN && L->win < L->mark - EARLY_MARGIN;
}

/* A fresh press now would be refused or break the combo. */
static int would_refuse(const Look *L)
{
    if (L->air && !L->air_ok)
        return 0;                       /* the game ignores it; ours neither */
    if (L->lock > 0.0f || L->queued)
        return 1;                       /* a press while queued restarts at Pt1 */
    if (L->step >= 1 && L->win > 0.0f)
        return L->win >= L->mark - EARLY_MARGIN;
    /* A swing playing with no window (Pt3, or one whose window ran out):
     * the bat update doesn't even ask the combo (0x0032A4BF). */
    return swinging(L->st);
}

typedef struct {
    int on;
    long last_press;                    /* our press or the player's, ms */
    long down_until;                    /* a press held down until */
    long up_since;                      /* the trigger let up since (0: not) */
    unsigned up_frames;
    long kept_since;                    /* a kept press waiting (0: none) */
    LONG kept_seen, passed_seen;
    int raw_was;
    uint32_t chained;                   /* suit: the punch we pressed after */
    uint32_t log_st;
    int log_step, log_next;
} Core;

static Core s_core;
static unsigned s_frame;

static void set_force(int f)
{
    InterlockedExchange(&s_force, f);
}

static void let_go(const char *why)
{
    if (s_core.on && log_on())
        fprintf(stderr, "[SWING] t=%ld off: %s\n", now_ms(), why);
    InterlockedExchange(&s_charge, 0);
    InterlockedExchange(&s_block, 0);
    set_force(FORCE_NONE);
    memset(&s_core, 0, sizeof s_core);
    s_core.kept_seen = s_kept_n;
    s_core.passed_seen = s_passed_n;
}

static void press_now(Core *C, long t, const char *what, const Look *L)
{
    set_force(FORCE_NONE);              /* the held trigger goes down: a press */
    C->last_press = t;
    C->down_until = t + PULSE_MS;
    C->up_since = 0;
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld quick: %s (state 0x%X step %d next %d "
                "window %.3f mark %.3f)\n", t, what, L->st, L->step, L->next,
                L->win, L->mark);
}

/* Hold the trigger up while waiting; a press when the moment comes. */
static void quick_wait(Core *C, long t, int now_ok, long min_gap, const char *what,
                       const Look *L)
{
    if (t < C->down_until) {
        set_force(FORCE_NONE);
        return;
    }
    if (!C->up_since) {
        C->up_since = t;
        C->up_frames = 0;
    }
    C->up_frames++;
    if (now_ok && t - C->up_since >= GAP_MS && C->up_frames >= MIN_FRAMES
        && t - C->last_press >= min_gap) {
        press_now(C, t, what, L);
        return;
    }
    set_force(FORCE_UP);
}

static void log_state(Core *C, const Look *L, long t)
{
    if (!log_on() || (L->st == C->log_st && L->step == C->log_step
                      && L->next == C->log_next))
        return;
    fprintf(stderr, "[SWING] t=%ld state 0x%X step %d next %d window %.3f mark %.3f "
            "lock %.3f queued %d air %d | +20B %u +214 %u\n", t, L->st, L->step, L->next,
            L->win, L->mark, L->lock, L->queued, L->air, MEM8(L->chr + 0x20Bu),
            MEM8(L->chr + 0x214u));
    C->log_st = L->st;
    C->log_step = L->step;
    C->log_next = L->next;
}

/* ── Free Swing (to-do 42; Txukie's Swing Button, MIT) ────────────────────
 * Where B takes out the bat or frying pan (the level's weapon, no B pad;
 * story, on foot), the controller's Reload button (the game's X after the
 * binds) and the keyboard's Attack key (Modern: while not aiming) take it
 * out and swing: a press with nothing out sends B, the press (up to three)
 * is kept and given to the game as a right-trigger press the moment it can
 * take it (the first when the weapon is in hand and ready, the next ones in
 * the combo's window), after that the button is the right trigger itself
 * (Quick Swing / Forgiving Combo work on it). A weapon it took out goes away
 * again (B) as soon as the swinging is over (FS_AWAY_MS), unless the player
 * pressed B meanwhile (theirs then). Its take-out and put-away play FS_DRAW_X
 * as fast, without their sounds (user 2026-11-23: instant, silent), and the
 * camera stays the free one meanwhile (s_fs_cam; with Camera Switch on, the
 * view picked). X is Reload elsewhere; in Classic the Attack key left the
 * story set's profile while Free Swing is on (controls.c) and is the right
 * trigger here wherever Free Swing doesn't apply (guns, the tank, B pads). */
#define FS_LIFE_MS    1200          /* a kept press is dropped after this (Txukie's) */
#define FS_READY_MS   2500          /* no weapon after this: B did something else */
#define FS_RETRY_MS   200           /* the first swing again if not taken by then */
#define FS_B_RETRY_MS 300           /* B again if nothing started coming out by then */
#define FS_CHAIN_MS   300           /* from one kept press to the next (Txukie's) */
#define FS_AWAY_MS    80            /* the swinging over this long: away (user: at once) */

/* The weapon out is Free Swing's (from its take-out until it is away): the
 * camera stays as it was (swing_camera_normal). */
static volatile LONG s_fs_cam;

typedef struct {
    int trig_was, auto_, want_out, waiting, pending, await, rt_was, stowing, held_sent;
    int atk_latched;                    /* the Attack key held since before play */
    long last_press, wait_since, sent, busy;
    LONG player_b_seen;
} Free;

static Free s_free;

static void free_reset(Free *F, int trig)
{
    LONG pb = s_fs_player_b;
    int lat = F->atk_latched;
    memset(F, 0, sizeof *F);
    F->trig_was = trig;                 /* a press held across: not a press */
    F->player_b_seen = pb;
    F->atk_latched = lat;
}

/* What B does here: the kind player_b_kind_update (0x002F48A0) handed the B
 * handler last frame (it keeps it nowhere: the zone +0x97, else the level's
 * weapon +0xAC, else the bat when player_has_bat says so (Extras' Weapon
 * too), the frying pan in the training levels; the Windmill's bat is that
 * fallback, +0xAC 0). Not called with nothing to do: 0 then. */
static int s_bk_kind;
static unsigned s_bk_frame;

static int melee(int k)
{
    return k == (int)KIND_BAT || k == (int)KIND_PAN;
}

/* The body in the bat's own moves (the frying pan plays the bat's clips):
 * TakeOut 0x834 .. RunAttackPt3 0x849, Idle1 0x875. Not: the plain run on
 * which a draw on the arm plays. */
static int melee_body(uint32_t st)
{
    return (st >= 0x834u && st <= 0x849u) || st == 0x875u;
}

void swing_b_kind(uint32_t chr, int kind)
{
    if (!chr || chr != player())
        return;
    s_bk_kind = kind & 0x7F;
    s_bk_frame = s_frame;
}

static int b_kind(void)
{
    return s_frame - s_bk_frame <= 1u ? s_bk_kind : 0;
}

static void free_log(long t, const char *what, const Look *L)
{
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld free swing: %s (state 0x%X out 0x%02X +1F8 %u pending %d; "
                "combo step %d lock %.2f window %.2f air %d/%d)\n",
                t, what, L->st, L->out, va_ok(L->ctl) ? MEM32(L->ctl + 0x1F8u) : 0u,
                s_free.pending, L->step, L->lock, L->win, L->air, L->air_ok);
}

/* ── Swing While Jumping (to-do 48; user 2026-11-24) ──────────────────────
 * Option air_swing (F2 Extras > Swing, off by default): with the bat or
 * frying pan in hand the attack swings during a jump too (the combo, Quick
 * Swing and Forgiving Combo as on the floor), and Free Swing takes the
 * weapon out, swings and puts it away in the air. The jump keeps its arc
 * (user 2026-11-16 / 24: no paused fall); nothing here touches his fall.
 *  - The swing: the bat / pan update (0x0032A300) refuses a swing off the
 *    floor ([chr+0xDC], the height above it, must be 0 at 0x0032A502, right
 *    before the combo 0x00329E80). Its wrapper (recomp_manual.c) shows the
 *    height as 0 from 0x0031A520's return (the last call before the test;
 *    single player: it returns at once) until the combo has run, when
 *    swing_air_gate says so: the player, the bat / pan in hand, in the air
 *    in one of the jumps (air_state).
 *  - Its end: the combo's last swing asks FireHold (standing); in the air
 *    the bat's JumpPt2 (falling) is asked instead (swing_air_anim), which
 *    lands in JumpPt3 as the game's own bat jump. The same for a ready
 *    pose asked in the air while the weapon is out (Free Swing's weapon
 *    comes out in a plain jump: the bat update asks FireHold for it).
 *  - His legs: the swing clips stand; in the air his legs keep the jump's
 *    pose of the moment before the swing (swing_air_pose, below).
 *  - Free Swing's draw and put-away in the air: 0x003197F0 refuses them off
 *    the floor (+0xDC != 0) or rising (+0xD0 > 0); its wrapper hides both
 *    for the call while s_air_draw runs (swing_air_draw; Free Swing's own B,
 *    asked on the arm: swing_layered). */
#define ST_JUMP1      0x844u        /* the bat's JumpPt1 (rising) */
#define ST_JUMP2      0x845u        /* JumpPt2 (falling) */
#define ST_JUMP3      0x846u        /* JumpPt3 (landing) */
#define ST_PLAIN_JUMP 0x32u         /* jump_straight (nothing out), the whole jump */
#define ST_PLAIN_FALL 0x38u         /* jump_pt2 (after the tail spin), falling */
#define ST_HOVER      0xE9u         /* tailhover: the tail spin */
#define AIR_DRAW_MS   400           /* Free Swing's B in the air: its draw's start passes */
#define AIR_DRAW_END_MS 200         /* a draw's end event this soon after a swing started */

static volatile LONG s_air_draw;    /* until then (ms) */
static int s_hover_swing;           /* a swing started in the tail spin (air_frame) */
static int s_jump_swing;            /* a jump taken during a floor swing (swing_air_pose's legs) */

/* The bat / pan in hand or on its way in / out (+0x1F8: 1 draw, 2 in hand,
 * 3 put-away). */
static int weapon_held(uint32_t ctl)
{
    return melee((int)(MEM32(ctl + 0xA0u) & 0x7Fu)) || MEM32(ctl + 0x1F8u) != 0u;
}

/* Nothing out, in the air: the plain jump, the tail spin, the fall after it. */
static int plain_air(uint32_t st)
{
    return st == ST_PLAIN_JUMP || st == ST_HOVER || st == ST_PLAIN_FALL;
}

/* The states a swing may start from in the air: the bat's own (its jump,
 * a ready pose, a swing), the plain ones (Free Swing's weapon just out). */
static int air_state(uint32_t st)
{
    return melee_body(st) || plain_air(st);
}

/* A swing can start from it in the air (as ready() on the floor). */
static int air_ready(uint32_t st)
{
    return st == ST_JUMP1 || st == ST_JUMP2 || plain_air(st);
}

/* Free Swing takes the weapon out in the air from here. */
static int air_take(const Look *L)
{
    return swing_air() && L->air && plain_air(L->st);
}

/* Free Swing's weapon may go away in the air (the swing over: the bat's
 * fall after it, or a jump it never swung in). */
static int air_away_ok(const Look *L)
{
    return swing_air() && L->air && (L->st == ST_JUMP2 || L->st == ST_JUMP1
                                     || plain_air(L->st));
}

static void air_draw_now(long t, const char *what)
{
    InterlockedExchange(&s_air_draw, t + AIR_DRAW_MS);
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld air: free swing's %s in the air\n", t, what);
}

int swing_air_gate(uint32_t chr)
{
    static uint32_t refused = ~0u;
    uint32_t ctl, st;
    if (!swing_air() || !chr || chr != player() || MEM8(MP_FLAG) || MEM8(chr + 0x1B5u)
        || MEMF(chr + 0xDCu) == 0.0f)
        return 0;
    ctl = MEM32(chr + 0x568u);
    if (!va_ok(ctl) || MEM32(ctl + 0x1F8u) != 2u || !melee((int)(MEM32(ctl + 0xA0u) & 0x7Fu)))
        return 0;
    st = MEM32(chr + 0xA4u);
    if (!air_state(st)) {
        if (st != refused && log_on())
            fprintf(stderr, "[SWING] t=%ld air: no swing from state 0x%X\n", now_ms(), st);
        refused = st;
        return 0;
    }
    refused = ~0u;
    return 1;
}

int swing_air_draw(uint32_t chr)
{
    return swing_air() && chr && chr == player() && !MEM8(MP_FLAG)
        && now_ms() < (long)s_air_draw
        && (MEMF(chr + 0xDCu) != 0.0f || MEMF(chr + 0xD0u) > 0.0f);
}

int swing_air_weapon(uint32_t chr)
{
    uint32_t ctl;
    if (!swing_air() || !chr || chr != player() || MEM8(MP_FLAG))
        return 0;
    ctl = MEM32(chr + 0x568u);
    if (!va_ok(ctl))
        return 0;
    return melee((int)(MEM32(ctl + 0xA0u) & 0x7Fu)) || MEM32(ctl + 0x1F8u) != 0u
        || now_ms() < (long)s_air_draw;
}

#define AIR_END_BLEND 4.0f          /* the game's own blend for a swing ending in the air */

int swing_air_anim(uint32_t chr, uint32_t idx, uint32_t from, uint32_t *idx2, float *blend)
{
    uint32_t st, ctl;
    static long sw_t0;
    if (!swing_air() || !chr || chr != player() || MEM8(MP_FLAG) || MEM8(chr + 0x1B5u))
        return 0;
    st = MEM32(chr + 0xA4u);
    ctl = MEM32(chr + 0x568u);
    if (swinging(idx) && idx != st)
        sw_t0 = now_ms();               /* a swing starts (or the next part) */
    /* Landing (floor_contact, 0x002EC0F0 from 0x002ECEB7) during a swing:
     * after the bat's own jump the game asks no landing clip then and the
     * swing plays out on the floor; after a plain jump (Free Swing's weapon
     * came out in it) it asked JumpPt3 over the swing. The swing goes on. */
    if (from == 0x002ECEB7u && swinging(st) && idx != st) {
        *idx2 = st;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: landed during swing 0x%X (0x%X asked): the "
                    "swing goes on\n", now_ms(), st, idx);
        return 1;
    }
    /* A jump pressed during a swing (its start, 0x002F39A9, asks the bat's
     * JumpPt1 over it: the swing was cut, user 2026-11-24): the swing goes
     * on, the jump's arc is the game's; it ends in the air as any air swing
     * (the bat's fall, the landing, the combo's next press). */
    if (from == 0x002F39A9u && swinging(st) && idx != st && !swinging(idx)) {
        *idx2 = st;
        s_jump_swing = 1;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: jump during swing 0x%X (0x%X asked): the swing "
                    "goes on\n", now_ms(), st, idx);
        return 1;
    }
    /* Noclip: the B / weapon step (run there for Free Swing) asks "stance"
     * once the weapon is away; the flight's spin pose stays. */
    if (noclip_now(now_ms()) && idx == 15u && from == 0x00310756u && idx != st) {
        *idx2 = st;
        return 1;
    }
    /* The tail spin back after Free Swing's put-away in the air: the
     * put-away's end event asks the plain fall over it a frame later (the
     * spin's own code asks the spin again the next frame: a flicker). */
    if (from == 0x0030AB3Au && st == ST_HOVER && idx != st && now_ms() < (long)s_air_draw) {
        *idx2 = st;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: a draw's end asked 0x%X in the tail spin: the "
                    "spin goes on\n", now_ms(), idx);
        return 1;
    }
    /* Noclip (height 0: the floor's re-pick): a swing's end asked the bat's
     * FireHold (upright, a snap before the flight's pose came back), from
     * the B / weapon step's pose pick (0x00310521, noclip runs the step) or
     * the clip's end event: the spin's pose at once. Not a draw's end just
     * after the swing began. */
    if (noclip_now(now_ms()) && (from == 0x0030AB3Au || from == 0x00310521u)
        && swinging(st) && idx != st
        && !swinging(idx) && now_ms() - sw_t0 >= AIR_DRAW_END_MS) {
        *idx2 = ST_HOVER;
        /* The step's pick blends in 8 (the tail spin's end event: 4): the
         * swing clip's own upright recovery showed through the longer blend. */
        if (blend && *blend > AIR_END_BLEND)
            *blend = AIR_END_BLEND;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: swing 0x%X over in noclip: the flight's pose, "
                    "not 0x%X\n", now_ms(), st, idx);
        return 1;
    }
    if (MEMF(chr + 0xDCu) <= 0.0f)
        return 0;
    /* A clip's end event (0x0030AA60) in the air asks the weapon's fall:
     * right for the swing's own end (its clip's end, 0.4 s on at least),
     * but Free Swing's draw on the arm ends a frame after its swing started
     * (on the floor the same event re-picks the movement, which leaves a
     * swing alone): the swing goes on then. */
    if (from == 0x0030AB3Au && swinging(st) && idx != st) {
        long since = now_ms() - sw_t0;
        /* The swing's own end in the tail spin: the bat's fall (JumpPt2,
         * upright with the bat) showed until Free Swing's put-away brought
         * the spin back (~0.1 s, a snap): the spin's pose at once; air_frame
         * holds its sink until the weapon is away. */
        if (s_hover_swing && idx == ST_JUMP2 && !(since >= 0 && since < AIR_DRAW_END_MS)) {
            *idx2 = ST_HOVER;
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld air: swing 0x%X over in the tail spin: the spin's "
                        "pose, not JumpPt2\n", now_ms(), st);
            return 1;
        }
        if (since >= 0 && since < AIR_DRAW_END_MS) {
            *idx2 = st;
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld air: a draw's end asked 0x%X %ld ms into swing "
                        "0x%X: the swing goes on\n", now_ms(), idx, since, st);
            return 1;
        }
        return 0;
    }
    if (!ready(idx))
        return 0;
    if (!swinging(st) && !(plain_air(st) && va_ok(ctl)
                           && melee((int)(MEM32(ctl + 0xA0u) & 0x7Fu))))
        return 0;
    *idx2 = ST_JUMP2;
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld air: 0x%X asked in the air from 0x%X: the bat's "
                "JumpPt2 instead (height %.1f)\n", now_ms(), idx, st, MEMF(chr + 0xDCu));
    return 1;
}

/* The tail spin's sink during a swing started in it (Free Swing in the
 * hover): the spin holds his fall at its own speed ([chr+0xD0], -8.5 a tick
 * measured; 0 with the Infinite Tail Spin cheat's A held), a swing falls
 * freely (-16.5 within 0.3 s). That speed is held while the swing, and the
 * bat's fall after it, play in the air, until the spin (or the plain fall)
 * is back or he lands. Noclip: the flight marks itself every frame
 * (swing_air_noclip); his legs count as in the air then. */
static float s_hover_v;
static long s_hover_t = -100000;
static volatile LONG s_noclip_t = -100000;

static int noclip_now(long t)
{
    return t - (long)s_noclip_t < 150;
}

/* The spin's time: jump_air_update counts it in ctl +0x45 (30 Hz frames,
 * 0x002F4731) and ends it past 0x3D with A held (0x12 let go), the tail's
 * speed in char +0x120. After a swing in it the game starts a fresh spin
 * (the counter from 0): its count and speed at the swing are put back on
 * the resumed spin, plus the frames the swing took, so a swing never makes
 * the spin last longer than the game's (it extended it by ~1 s). */
static uint8_t s_spin_count;
static float s_spin_speed;
static int s_spin_frames, s_spin_restore;

static void air_frame(uint32_t chr, const Look *L, long t)
{
    uint32_t ctl = L->ctl;
    if (!swing_air() || !chr || chr != player() || MEM8(MP_FLAG) || MEM8(chr + 0x1B5u)
        || MEMF(chr + 0xDCu) <= 0.0f || !va_ok(ctl)) {
        if (s_hover_swing && log_on())
            fprintf(stderr, "[SWING] t=%ld air: the spin's sink let go (landed)\n", t);
        s_hover_swing = 0;
        s_spin_restore = 0;
        return;
    }
    /* The swing over, its weapon still in hand (Free Swing puts it away when
     * the combo's window ends): the spin's pose already (swing_air_anim), its
     * sink still held, the spin's own code idle while a weapon is out. */
    if (L->st == ST_HOVER && s_hover_swing && weapon_held(ctl)) {
        if (s_spin_restore)
            s_spin_frames += (int)MEM8(0x007FA250u);
        if (MEMF(chr + 0xD0u) < s_hover_v)
            MEMF(chr + 0xD0u) = s_hover_v;
        return;
    }
    if (L->st == ST_HOVER) {
        if (s_spin_restore) {
            uint32_t c = (uint32_t)s_spin_count + (uint32_t)s_spin_frames;
            s_spin_restore = 0;
            if (c > 0xFEu)
                c = 0xFEu;
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld air: the spin goes on from count %u (was %u at the "
                        "swing, +%d frames; the game's fresh spin %u), tail speed %.2f\n", t, c,
                        s_spin_count, s_spin_frames, MEM8(ctl + 0x45u), s_spin_speed);
            MEM8(ctl + 0x45u) = (uint8_t)c;
            MEMF(chr + 0x120u) = s_spin_speed;
        }
        s_hover_v = MEMF(chr + 0xD0u);
        s_hover_t = t;
        s_spin_count = MEM8(ctl + 0x45u);
        s_spin_speed = MEMF(chr + 0x120u);
        s_hover_swing = 0;
        return;
    }
    if (s_spin_restore)
        s_spin_frames += (int)MEM8(0x007FA250u);    /* 30 Hz frames this frame */
    if (!s_hover_swing && swinging(L->st) && t - s_hover_t < 200) {
        s_hover_swing = 1;
        s_spin_restore = 1;
        s_spin_frames = 0;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: a swing in the tail spin: its sink %.2f held "
                    "(spin count %u)\n", t, s_hover_v, s_spin_count);
    }
    if (s_hover_swing && !swinging(L->st) && L->st != ST_JUMP2) {
        s_hover_swing = 0;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: the spin's sink let go (state 0x%X)\n", t, L->st);
    }
    if (s_hover_swing && MEMF(chr + 0xD0u) < s_hover_v)
        MEMF(chr + 0xD0u) = s_hover_v;
}

int swing_air_noclip(uint32_t chr)
{
    uint32_t ctl, st;
    long t;
    int r = 0;
    if (!swing_air() || !chr || chr != player() || MEM8(MP_FLAG))
        return 0;
    t = now_ms();
    InterlockedExchange(&s_noclip_t, t);
    ctl = MEM32(chr + 0x568u);
    st = MEM32(chr + 0xA4u);
    if (!va_ok(ctl))
        return 0;
    /* The B / weapon step with Free Swing on (it also says what B does
     * here, which Free Swing needs before a press: swing_b_kind). With
     * nothing out it asks "stance" now and then (0x00310756): kept off in
     * swing_air_anim. */
    if (swing_free())
        r |= 1;
    /* A swing playing: his animation is its. Only then: with the weapon in
     * hand between swings (or on its way in / out, layered on the arm) the
     * flight's spin pose goes on; waiting for the whole time it was out
     * showed the bat's upright pose between the swing and the put-away. */
    if (swinging(st))
        r |= 2;
    /* The movement left out also counted the combo's lock and window down
     * (0x002D5540, by the frame's seconds [0x007FA21C]): a swing's lock
     * stayed and the next press never came. */
    {
        /* Once a frame: noclip_fly asks again after the step (its second
         * call ran the window down twice as fast: the combo's next press
         * came too late at the ground's timing). */
        static uint32_t last_fc = ~0u;
        uint32_t fc = MEM32(0x007F9F60u);
        float dt = fc != last_fc ? MEMF(0x007FA21Cu) : 0.0f;
        static const uint32_t offs[2] = { 0x55Cu, 0x560u };
        int i;
        last_fc = fc;
        for (i = 0; i < 2 && dt > 0.0f && dt < 1.0f; i++) {
            float v = MEMF(ctl + offs[i]);
            if (v > 0.0f)
                MEMF(ctl + offs[i]) = v - dt > 0.0f ? v - dt : 0.0f;
        }
    }
    return r;
}

static void free_frame(int set, uint32_t chr, int foot, const Look *L, long t,
                       int atk_key, int aiming)
{
    Free *F = &s_free;
    int on = swing_free(), modern = controls_modern(NULL, NULL, NULL);
    int apply, key_trig, trig, rising, inhand, story = set == CONTROLS_STORY;

    if (controls_keyboard_port() != 0)
        atk_key = 0;
    /* A key held as play starts (a cutscene skipped with it): not a press
     * until let go, as controls.c's latch for the profile's keys. */
    if (!story)
        F->atk_latched = atk_key;
    else if (!atk_key)
        F->atk_latched = 0;
    if (F->atk_latched)
        atk_key = 0;
    key_trig = atk_key && !(modern && aiming);

    /* Free Swing's weapon away (or none came out): its camera and quick
     * draw end. */
    if (!L->out && !F->waiting && !F->want_out && t >= s_fs_b_until) {
        if (s_fs_cam || s_fs_quick) {
            InterlockedExchange(&s_fs_cam, 0);
            InterlockedExchange(&s_fs_quick, 0);
        }
        F->stowing = 0;
    }

    if (set == CONTROLS_PAUSE || set == CONTROLS_PHOTO) {
        /* Kept as it is (whose the weapon is) until play goes on. */
        F->trig_was = s_fs_pad_x || key_trig;
        InterlockedExchange(&s_fs_apply, 0);
        InterlockedExchange(&s_fs_key, 0);
        InterlockedExchange(&s_fs_direct, 0);
        InterlockedExchange(&s_atk_rt, 0);
        return;
    }
    apply = on && story && chr && foot && !MEM8(MP_FLAG) && chr == player() && va_ok(L->ctl);
    if (apply) {
        /* B takes out the bat or frying pan here (or one is out: the game
         * puts away what B no longer takes out, so it is B's weapon). */
        /* Nothing for B for a moment (a skid, a jump: player_b_kind_update's
         * fallback wants Conker settled) right after it was the bat or pan:
         * still here (a press then waits for the floor). */
        static long melee_seen = -100000;
        int bk = b_kind();
        if (melee(bk))
            melee_seen = t;
        apply = L->out ? melee(L->out) : melee(bk) || (!bk && t - melee_seen < 1500);
    }
    {
        static int was = -1;
        if (apply != was && log_on())
            fprintf(stderr, "[SWING] t=%ld free swing %s here (B kind 0x%02X, out 0x%02X, "
                    "set %d)\n", t, apply ? "applies" : "doesn't apply", b_kind(), L->out, set);
        was = apply;
    }
    InterlockedExchange(&s_atk_rt, on && !modern && story && atk_key && !apply);
    if (!apply) {
        if (F->auto_ || F->pending || F->waiting || F->want_out)
            free_log(t, "off (not where it applies)", L);
        free_reset(F, s_fs_pad_x || key_trig);
        InterlockedExchange(&s_fs_apply, 0);
        InterlockedExchange(&s_fs_key, 0);
        InterlockedExchange(&s_fs_direct, 0);
        return;
    }
    InterlockedExchange(&s_fs_apply, 1);
    InterlockedExchange(&s_fs_key, key_trig);
    trig = s_fs_pad_x || key_trig;
    rising = trig && !F->trig_was;
    F->trig_was = trig;
    inhand = L->kind == (int)KIND_BAT || L->kind == (int)KIND_PAN;

    if (s_fs_player_b != F->player_b_seen) {
        F->player_b_seen = s_fs_player_b;
        if (F->auto_)
            free_log(t, "B pressed: the weapon is the player's", L);
        F->auto_ = 0;
        F->want_out = 0;
    }
    if (!trig)
        F->held_sent = 0;
    if (rising) {
        F->last_press = t;
        if (!L->out && !F->waiting && !F->want_out) {
            F->want_out = 1;
            F->auto_ = 1;
            F->pending = 1;
            F->await = 0;
            free_log(t, "press: take out", L);
        } else {
            /* Every press is kept and given at its moment (the combo's
             * window: with the quick draw a tap during a swing came too
             * early and broke the combo). */
            if (F->pending < 3)
                F->pending++;
            free_log(t, "press kept", L);
        }
    }
    /* Kept presses with nothing out (it went away meanwhile): out again. */
    if (!L->out && F->pending && !F->want_out && !F->waiting && t >= s_fs_b_until) {
        F->want_out = 1;
        F->auto_ = 1;
    }
    /* Take out: B, once on the floor. */
    if (F->want_out) {
        if (t - F->last_press > FS_LIFE_MS) {
            free_log(t, "press dropped (no take out)", L);
            free_reset(F, trig);
        } else if (!L->out && (!L->air || air_take(L)) && t >= s_fs_b_until
                   && L->st != 0x834u && L->st != 0x835u) {   /* not while it goes away */
            if (L->air)
                air_draw_now(t, "take-out");
            /* Its last weapon went away inside the combo's window (the
             * swinging over, the window still open): the combo stayed
             * "running" (+0x548), and with the weapon back the window ran
             * out and the game locked swings for 0.5 s (to-do 48's tests:
             * the next Free Swing waited half a second). A fresh weapon,
             * a fresh combo. */
            if (MEM32(L->ctl + 0x548u) || MEMF(L->ctl + 0x55Cu) > 0.0f) {
                free_log(t, "the last combo's step / lock cleared", L);
                MEM32(L->ctl + 0x548u) = 0;
                MEM32(L->ctl + 0x54Cu) = 0;
                MEMF(L->ctl + 0x55Cu) = 0.0f;
                MEMF(L->ctl + 0x560u) = 0.0f;
                MEM8(L->ctl + 0x568u) = 0;
            }
            InterlockedExchange(&s_fs_b_until, t + PULSE_MS);
            InterlockedExchange(&s_fs_quick, 1);
            InterlockedExchange(&s_fs_cam, 1);
            F->want_out = 0;
            F->waiting = 1;
            F->wait_since = t;
            free_log(t, "B: take out", L);
        }
    }
    if (F->waiting) {
        if (inhand) {
            F->waiting = 0;
            F->last_press = t;          /* kept presses live on from here */
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld free swing: in hand after %ld ms\n", t,
                        t - F->wait_since);
        } else if (!L->out && t - F->wait_since >= FS_B_RETRY_MS && t >= s_fs_b_until) {
            /* B not taken (an animation the game finished first): again
             * while the press lives. */
            F->waiting = 0;
            if (t - F->last_press <= FS_LIFE_MS) {
                F->want_out = 1;
                free_log(t, "B not taken: again", L);
            } else {
                free_log(t, "no weapon came out: dropped", L);
                free_reset(F, trig);
            }
        } else if (t - F->wait_since > FS_READY_MS) {
            free_log(t, "no weapon came out: dropped", L);
            free_reset(F, trig);
        }
    }
    /* The first swing: retried if the game didn't take it. */
    if (F->await) {
        if (swinging(L->st)) {
            F->await = 0;
        } else if (t - F->sent >= FS_RETRY_MS) {
            F->await = 0;
            if (t - F->last_press <= FS_LIFE_MS && F->pending < 3) {
                F->pending++;
                free_log(t, "swing not taken: again", L);
            }
        }
    }
    /* Kept presses, each when the game can take it. */
    /* In the air / noclip its draw is on the arm: the first swing waits
     * until that is over (lay_frame frees the arm slot the bat update's gate
     * reads; at 120 fps a press a frame before was refused and the retry
     * came 0.2 s later). */
    if (dev_log(DLOG_SWING) >= 2 && F->pending && inhand && !F->await) {
        static long last;
        if (t - last >= 100) {
            last = t;
            free_log(t, "a kept press waits", L);
        }
    }
    if (F->pending && !F->want_out && !F->waiting && !F->await && inhand
        && t >= s_fs_rt_until && !(s_lay.on && (L->air || noclip_now(t)))) {
        if (t - F->last_press > FS_LIFE_MS) {
            free_log(t, "kept presses dropped", L);
            F->pending = 0;
        } else if (can_start(L)) {
            /* A press of PULSE_MS at least (one shorter than a game frame
             * is missed); a button still held stays down after it. */
            InterlockedExchange(&s_fs_rt_until, t + PULSE_MS);
            F->pending--;
            F->await = 1;
            F->sent = t;
            F->held_sent = trig;
            /* Presses kept through the draw live on from the first
             * swing (a 1.15 s draw ate the third tap's life). */
            F->last_press = t;
            free_log(t, trig ? "swing (button held)" : "swing", L);
        } else if (can_chain(L) && L->next <= 3 && t - F->sent >= FS_CHAIN_MS) {
            InterlockedExchange(&s_fs_rt_until, t + PULSE_MS);
            F->pending--;
            F->sent = t;
            F->held_sent = trig;
            free_log(t, L->next == 2 ? "Pt2" : "Pt3", L);
        }
    }
    /* The button held after its press went: the right trigger stays down
     * (Quick Swing's hold); otherwise only our presses reach the game. */
    InterlockedExchange(&s_fs_direct, inhand && F->held_sent && !F->pending
                                      && !F->waiting && !F->want_out);
    /* The quick take-out ends with its clip (the weapon is in hand before). */
    if (s_fs_quick && inhand && ready(L->st) && !F->stowing)
        InterlockedExchange(&s_fs_quick, 0);
    /* Goes away by itself after a moment without a press or a swing (the
     * right trigger's presses too, refused ones included: the combo's lock). */
    {
        int rt = (int)s_raw;
        if (rt && !F->rt_was)
            F->last_press = t;
        F->rt_was = rt;
        /* Noclip: the flight's pose after a swing (swing_air_anim) rests too. */
        int rest = ready(L->st) || air_away_ok(L) || (noclip_now(t) && L->st == ST_HOVER);
        if (L->out && (swinging(L->st) || trig || rt || F->pending || F->await || F->waiting
                       || !rest || t < s_fs_rt_until))
            F->busy = t;
    }
    if (F->auto_ && inhand && !F->pending && !F->await && t - F->busy >= FS_AWAY_MS
        && t - F->last_press >= FS_AWAY_MS && t >= s_fs_b_until) {
        if (L->air)
            air_draw_now(t, "put-away");
        InterlockedExchange(&s_fs_b_until, t + PULSE_MS);
        InterlockedExchange(&s_fs_quick, 1);
        F->auto_ = 0;
        F->stowing = 1;
        free_log(t, "B: put away (swing over)", L);
    }
    if (!L->out && !F->pending && !F->want_out && !F->waiting && t >= s_fs_b_until)
        F->auto_ = 0;
}

/* ── The camera switch (to-do 42; user 2026-11-23, second round) ──────────
 * Option on: the zoom button (right stick click / V) switches between the
 * normal (Unlocked) camera and over the shoulder, on foot in play with
 * nothing out or the bat / frying pan out (also coming out or going away);
 * drawing a weapon never changes the view (normal at the start, then the one
 * picked last). A gun keeps the game's own view and button.
 *  - Normal view, bat / pan out: camera_update doesn't see the camera's
 *    "aiming" byte (+0, set every frame by 0x00301410 while a weapon is out;
 *    nothing else of the weapon changes), so the camera does what it does
 *    without a weapon: no over-the-shoulder push (0x0003CE30), the right
 *    stick turns the camera (0x002F99A0 on its own look record, then
 *    cam_stick_look), the Unlocked Camera's tilt and zoom. Conker's own aim
 *    (0x002F99A0 on his aim data) doesn't get the right stick (it turned
 *    him), and he runs the way the stick points from the camera instead of
 *    strafing (the left stick straight ahead + the move turn's heading,
 *    Txukie's). Free Swing's weapon gets this view with the option off too
 *    (user: "complete camera control ... throughout Conker's swing").
 *    His turn to the way pushed is the game's own turn rate (turn_toward;
 *    round 4, user 2026-11-24: it snapped at once).
 *  - Over the shoulder (built on the normal camera; the game has no such
 *    view without a weapon, its LT head-cam is first person): framed as the
 *    bat's aiming view (look-at point beside his head, SH_SIDE units to the
 *    right, the camera SH_DIST back and level), up / down tilt, the wheel
 *    zooms (the Unlocked Camera's weapon zoom, so drawing keeps the
 *    framing). The camera's heading is held (s_sh_yaw): only the right
 *    stick / mouse turn it, and Conker with it at once. With the bat / pan
 *    out he faces that way and the game strafes (round 3: in place of the
 *    game's aiming view). With nothing out he strafes (round 5, "The
 *    weaponless strafe" below: the bat's side clips, his body drawn facing
 *    the view while the plain movement goes the way the stick points), and
 *    the camera never swings round behind him by itself (round 4). */
static LONG s_toggles_seen;

#define SH_DIST       320.0f        /* the bat's aiming distance (target +0x26C) */
#define SH_SIDE       45.0f         /* its look-at point beside him */
#define SH_PITCH      -6.0f         /* the wanted pitch when it comes in */
#define SH_EASE       8.0f          /* framing in / out, per second (e-folds) */
#define SH_FOLLOW     14.0f         /* the camera easing to its heading, per second */

static volatile LONG s_shoulder;    /* the shoulder view wanted (game thread) */
static float s_sh_amt;              /* its framing, eased 0..1 (camera thread = game thread) */
static long s_sh_ms;                /* last camera_update of camera 0, ms */

/* The shoulder view's headings, in the camera heading's degrees (the camera
 * behind him has yaw +0x60 = this; +0x170 = this x DEG_TO_ANGLE + 0x4000).
 * s_sh_yaw: the camera's, from his facing when the view comes in, then
 * turned only by the mouse / right stick. s_sh_head: his. With the bat /
 * pan out it is ours: s_sh_yaw (set at once when drawn out of a strafe;
 * turned to it at the game's rate after a turn the game made itself),
 * written to his facing every frame (a change of +0x170 the game made
 * itself past SH_ADOPT degrees, a ledge or a hit, is taken over, the
 * camera's heading with it). With nothing out it is the game's (read each
 * frame; the strafe sets it to the plain movement's heading), turned with
 * the camera by the mouse / right stick. */
#define SH_ADOPT      30.0f
static int s_sh_on;                 /* s_sh_head is his */
static float s_sh_head;
static uint16_t s_sh_wrote;         /* +0x170 as written last */
static int s_sh_yaw_on;             /* s_sh_yaw holds the camera's heading */
static float s_sh_yaw;

/* The camera heading the game moves Conker by (as 0x002D3370 reads it):
 * camera +0x3C8 degrees, or its controller's +0x5C when that holds 999. */
static int cam_heading(uint32_t chr, float *deg)
{
    uint32_t cam = MEM32(chr + 0x564u), view;
    float d;
    if (!va_ok(cam))
        return 0;
    d = MEMF(cam + 0x3C8u);
    if (d == 999.0f) {
        view = MEM32(cam + 4u);
        if (!va_ok(view))
            return 0;
        d = MEMF(view + 0x5Cu);
    }
    if (!isfinite(d))
        return 0;
    *deg = d;
    return 1;
}

static float wrap180(float d)
{
    d = fmodf(d, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

/* The normal camera of camera 0 as play has it: single player, no cutscene,
 * plain follow mode (1), no zone camera (+0x8C4 / +0x8C5). */
static int plain_camera(uint32_t cam)
{
    uint32_t ctl;
    if (!cam || cam != MEM32(CAM0) || MEM8(MP_FLAG) || MEM8(0x0076F0E0u) == 1)
        return 0;
    ctl = MEM32(cam + 4u);
    return va_ok(ctl) && MEM32(cam + 0xC0u) == 1u && !MEM8(ctl + 0x8C4u) && !MEM8(ctl + 0x8C5u);
}

/* The free view wanted with a bat / pan out: the switch's normal view, or
 * Free Swing's weapon with the switch off. */
static int free_view(void)
{
    return swing_camera_switch() ? s_view_normal != 0 : s_fs_cam != 0;
}

static float wrap360(float d)
{
    d = fmodf(d, 360.0f);
    return d < 0.0f ? d + 360.0f : d;
}

/* +0x170 of heading h (degrees, the camera heading's sense) and back. */
static uint16_t head_angle(float h)
{
    return (uint16_t)((int32_t)(wrap360(h) * DEG_TO_ANGLE) + 0x4000);
}

static float angle_head(uint16_t a)
{
    return wrap360((float)(uint16_t)(a - 0x4000u) * (360.0f / 65536.0f));
}

/* His facing from s_sh_head: +0x170 (now), +0x174 (where he turns to),
 * +0x114 degrees ((+0x170 + 0x4000) as cam_aim_update keeps it). */
static void sh_apply(uint32_t chr)
{
    uint16_t a = head_angle(s_sh_head);
    MEM16(chr + 0x174u) = a;
    MEM16(chr + 0x170u) = a;
    MEMF(chr + 0x114u) = (float)(int16_t)(uint16_t)(a + 0x4000u) * (360.0f / 65536.0f);
    s_sh_wrote = a;
}

#define DT_TICKS      0x007FDD28u   /* s16: the frame's length, 100 = 1/30 s */

static float frame_ticks(void)
{
    float ticks = (float)(int16_t)MEM16(DT_TICKS);
    return ticks > 0.0f && ticks <= 400.0f ? ticks : 100.0f;
}

/* The speed the plain movement would have now: +0x110 builds up by 1 a 30
 * fps frame while the stick is held (to 35, ~1.2 s) and falls by 2.5 when
 * let go; the weapon's run gets to full speed at once, so its turn from
 * standing would be slower than the plain one (game thread, once a
 * frame). */
static float s_ramp;

static void ramp_frame(void)
{
    float k = frame_ticks() / 100.0f;
    if (s_lstick)
        s_ramp = s_ramp + k > 35.0f ? 35.0f : s_ramp + k;
    else
        s_ramp = s_ramp - 2.5f * k < 0.0f ? 0.0f : s_ramp - 2.5f * k;
}

/* His speed for the turn rate, in the game's 30 fps units: +0x110 (the
 * plain movement; ~35 at a full run), or the weapon's run's own (ctl
 * +0x390, ~33), which leaves +0x110 at 0; at most the plain movement's
 * build-up (s_ramp). */
static float move_speed(uint32_t chr)
{
    uint32_t ctl = MEM32(chr + 0x568u);
    float s = MEMF(chr + 0x110u), w = va_ok(ctl) ? MEMF(ctl + 0x390u) : 0.0f;
    if (!isfinite(s))
        s = 0.0f;
    if (isfinite(w) && w > s)
        s = w;
    return s < s_ramp ? s : s_ramp;
}

/* The game's own turn of Conker to the way he is pushed (0x002DF610, the
 * plain movement; round 4, user 2026-11-24: with the weapon out "he changes
 * directions instantly"): +0x170 goes toward the wanted heading by c x 256
 * x ticks / 100 (1/65536 turns), ticks = [0x007FDD28] (100 a 30 fps frame,
 * 50 at 60), c = 25 below speed 5 (from standing: ~1000 deg/s), else
 * (64 - speed) / 4 (a full run, ~33: 7, ~250 deg/s). Past 112.5 deg at
 * speed the game skids round with c = 12 (no skid with a weapon: just that
 * rate). Exact here (the game steps whole 256s, a little slower above 30
 * fps). */
static uint16_t turn_toward(uint16_t cur, uint16_t want, float speed)
{
    int diff = (int)(int16_t)(uint16_t)(want - cur);
    int ad = diff < 0 ? -diff : diff;
    float step;
    int c;

    c = speed >= 5.0f ? (int)((64.0f - speed) * 0.25f) : 25;
    if (ad > 0x5000 && speed > 3.0f)
        c = 12;
    if (c < 1)
        c = 1;
    step = (float)c * 2.56f * frame_ticks();
    if (step >= (float)ad)
        return want;
    return (uint16_t)(cur + (diff < 0 ? -(int)step : (int)step));
}

/* The weaponless strafe (round 5, user 2026-11-24: "use conkers strafe
 * animation from when he has the weapon out, but just not use the weapon
 * draw/holding part"). Over the shoulder with nothing out, on the ground,
 * not swimming (s_sx): while the stick is held the game runs him straight
 * the way it points from the camera (swing_heading sets his facing to the
 * plain movement's heading: no turn), his body is drawn facing the view
 * (+0x114 at his matrix build, swing_strafe_draw, put back at the next
 * poll) and the plain run / walk shows the bat's side-run / side-walk /
 * run-back clip (swing_strafe_anim: the name swapped, the plain index and
 * state kept). Let go: he stops at once (speed 0, no skid) facing the view.
 * A weapon drawn meanwhile finds him facing the view and the bat's own
 * strafe goes on (his arm-only draw on the plain run keeps the swapped
 * clip). */
static int s_sx;                    /* the strafe applies (game thread) */
static int s_sx_moved;              /* he strafed last frame */
static int s_sx_air;                /* in the air after a strafe */

static void sx_stop(uint32_t chr, long t)
{
    MEMF(chr + 0x110u) = 0.0f;
    s_sh_head = s_sh_yaw;
    sh_apply(chr);
    if (log_on())
        fprintf(stderr, "[SWING] t=%ld strafe: let go, stopped facing the view (%.1f)\n", t, s_sh_yaw);
}

/* The shoulder view's headings, once a frame (game thread). */
static void sh_frame(uint32_t chr, const Look *L, long t)
{
    uint16_t cur;
    int sx_was = s_sx;

    if (!s_shoulder || !chr || chr != player()) {
        if ((s_sh_on || s_sh_yaw_on) && log_on())
            fprintf(stderr, "[SWING] t=%ld shoulder heading: let go\n", t);
        s_sh_on = 0;
        s_sh_yaw_on = 0;
        s_sx = s_sx_moved = s_sx_air = 0;
        InterlockedExchange(&s_sh_rot, 0);
        return;
    }
    cur = MEM16(chr + 0x170u);
    if (!s_sh_yaw_on) {
        s_sh_yaw = angle_head(cur);
        s_sh_yaw_on = 1;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld shoulder view: camera heading %.1f (his)\n", t, s_sh_yaw);
    }
    {
        int plain = !L->out && !MEM8(chr + 0x1B5u), ground = MEMF(chr + 0xDCu) == 0.0f;
        s_sx = plain && ground;
        /* A jump out of a strafe: drawn facing the view in the air too. */
        s_sx_air = plain && !ground && (s_sx_moved || s_sx_air);
        if (s_sx && s_lstick)
            s_sx_moved = 1;
        else if (s_sx && s_sx_moved) {
            s_sx_moved = 0;
            sx_stop(chr, t);
        } else if (!s_sx && !s_sx_air)
            s_sx_moved = 0;
    }
    if (!L->out) {
        /* Nothing out: his heading is the game's (it runs him the way the
         * stick points from the camera). */
        s_sh_head = angle_head(MEM16(chr + 0x170u));
        s_sh_wrote = MEM16(chr + 0x170u);
        s_sh_on = 1;
        InterlockedExchange(&s_sh_rot, 0);
        return;
    }
    if (sx_was || s_sx_moved) {
        /* Drawn while he strafed: he is drawn facing the view already. */
        s_sh_head = s_sh_yaw;
        s_sh_wrote = head_angle(s_sh_yaw);
        s_sh_on = 1;
        s_sx_moved = 0;
        cur = s_sh_wrote;
    }
    if (!s_sh_on || fabsf(wrap180(angle_head(cur) - angle_head(s_sh_wrote))) > SH_ADOPT) {
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld shoulder heading: %s %.1f\n", t,
                    s_sh_on ? "the game turned him, taken over at" : "his, from", angle_head(cur));
        s_sh_head = angle_head(cur);
        if (s_sh_on)
            s_sh_yaw = s_sh_head;           /* the camera goes round with him */
        s_sh_on = 1;
    }
    /* The bat / pan out: he faces the camera's way (turning to it at the
     * game's rate when the weapon came out while he ran another way; the
     * stick turned meanwhile, free_hook). */
    s_sh_head = angle_head(turn_toward(head_angle(s_sh_head), head_angle(s_sh_yaw),
                                       move_speed(chr)));
    sh_apply(chr);
    InterlockedExchange(&s_sh_rot, (LONG)(int16_t)(uint16_t)(head_angle(s_sh_head) - head_angle(s_sh_yaw)));
}

static void view_frame(int set, uint32_t chr, int foot, const Look *L, long t)
{
    int on = swing_camera_switch();
    int play = set == CONTROLS_STORY && chr && foot && !MEM8(MP_FLAG) && chr == player();
    int here = on && play && (!L->out || melee(L->out));
    uint32_t cam = MEM32(CAM0);
    LONG n = s_cs_toggles;

    InterlockedExchange(&s_cs_ready, here);
    if (n != s_toggles_seen) {
        int k = (int)(n - s_toggles_seen);
        s_toggles_seen = n;
        if (here && (k & 1)) {
            InterlockedExchange(&s_view_normal, !s_view_normal);
            fprintf(stderr, "[SWING] t=%ld camera switch: %s\n", t,
                    s_view_normal ? "normal camera" : "over the shoulder");
        }
    }
    /* The shoulder view (round 3: also with the bat / pan out, in place of
     * the game's aiming view). */
    InterlockedExchange(&s_shoulder, here && !s_view_normal && plain_camera(cam));
    /* Run, don't strafe: with the bat / pan in hand, and while it comes out
     * (round 3: he keeps turning to the camera during the draw). Also during
     * a full-body TakeOut (a draw from standing): it plays on the spot while
     * the stick rests (nothing changes then), but the stick pushed during it
     * ran him straight ahead the way he faced until the draw ended (user
     * 2026-11-24): now he runs the way it points from the camera at once. Not
     * over the shoulder: there he faces the view and strafes (sh_frame; round 5). */
    InterlockedExchange(&s_run_on, play && free_view() && melee(L->out));
    sh_frame(chr, L, t);
}

/* The camera's aiming byte hidden (camera_update): the bat / pan out with
 * the switch on (either view: the shoulder view replaces the game's aiming
 * view, user 2026-11-24), or Free Swing's weapon with it off. */
int swing_camera_normal(uint32_t cam)
{
    uint32_t chr, ctl;

    if (!(swing_camera_switch() || s_fs_cam) || !cam || cam != MEM32(CAM0) || MEM8(MP_FLAG)
        || MEM8(cam) != 1u)
        return 0;
    chr = MEM32(cam + 0x3E0u);
    ctl = va_ok(chr) ? MEM32(chr + 0x568u) : 0;
    return va_ok(ctl) && melee((int)(MEM32(ctl + 0xA0u) & 0x7Fu));
}

/* ── The weaponless shoulder view: the camera's hooks ─────────────────── */

int swing_shoulder(uint32_t cam)
{
    return s_shoulder && plain_camera(cam);
}

/* camera_update of camera 0 (game thread): the framing eased in / out. */
void swing_shoulder_tick(uint32_t cam)
{
    long now = now_ms();
    float dt, want;

    if (!cam || cam != MEM32(CAM0))
        return;
    dt = s_sh_ms && now > s_sh_ms ? (float)(now - s_sh_ms) / 1000.0f : 0.0f;
    if (dt > 0.25f)
        dt = 0.25f;
    s_sh_ms = now;
    want = swing_shoulder(cam) ? 1.0f : 0.0f;
    if (want > 0.0f && s_sh_amt == 0.0f) {
        uint32_t ctl = MEM32(cam + 4u);
        if (va_ok(ctl))
            MEMF(ctl + 0x8ECu) = SH_PITCH;   /* level, as the aiming view */
    }
    s_sh_amt += (want - s_sh_amt) * (1.0f - expf(-SH_EASE * dt));
    if (s_sh_amt < 0.002f && want == 0.0f)
        s_sh_amt = 0.0f;
    if (s_sh_amt > 0.998f && want > 0.0f)
        s_sh_amt = 1.0f;
}

float swing_shoulder_amount(void)
{
    return s_sh_amt;
}

/* Turns the camera's heading and Conker by d degrees (his facing +0x114 and
 * the u16 pair +0x170 / +0x174, as cam_aim_update writes them): the mouse /
 * right stick. With nothing out from his facing as the game has it now. */
static void turn_conker(uint32_t chr, float d)
{
    if (d > -0.001f && d < 0.001f)
        return;
    if (!s_sh_yaw_on) {                 /* before the first frame of the view */
        s_sh_yaw = angle_head(MEM16(chr + 0x170u));
        s_sh_yaw_on = 1;
    }
    s_sh_yaw = wrap360(s_sh_yaw + d);
    if (!s_sh_on || MEM16(chr + 0x170u) != s_sh_wrote)
        s_sh_head = angle_head(MEM16(chr + 0x170u));
    s_sh_on = 1;
    s_sh_head = wrap360(s_sh_head + d);
    sh_apply(chr);
}

/* After cam_stick_look (y0 = the yaw +0x60 before it, dt seconds; stick =
 * the right stick turned it): that turn is the camera heading's and
 * Conker's; the camera (controller +0x60, the normal view's yaw source, and
 * +0x5C) eases to the held heading from where it is (in at the view's
 * start; else it stays: the camera never swings round behind him by
 * itself, round 4). */
void swing_shoulder_follow(uint32_t cam, float y0, float dt, int stick)
{
    uint32_t ctl = MEM32(cam + 4u), chr = MEM32(cam + 0x3E0u);
    float y, d;

    if (!va_ok(ctl) || !va_ok(chr) || chr != player())
        return;
    y = MEMF(ctl + 0x60u);
    d = wrap180(y - y0);
    if (stick)                          /* only a turn the stick made */
        turn_conker(chr, d);
    else
        y = y0;
    if (!s_sh_yaw_on) {
        s_sh_yaw = angle_head(MEM16(chr + 0x170u));
        s_sh_yaw_on = 1;
    }
    d = wrap180(s_sh_yaw - y);
    y = wrap360(y + d * (1.0f - expf(-SH_FOLLOW * (dt > 0.0f ? dt : 1.0f / 60.0f))));
    MEMF(ctl + 0x60u) = y;
    MEMF(ctl + 0x5Cu) = y;
}

/* The mouse's turn in the shoulder view (look_turn): the camera's and
 * Conker's at once. 1 = taken. */
int swing_shoulder_mouse(uint32_t cam, float d)
{
    uint32_t chr, ctl;
    if (!swing_shoulder(cam))
        return 0;
    chr = MEM32(cam + 0x3E0u);
    ctl = MEM32(cam + 4u);
    if (va_ok(chr) && chr == player()) {
        turn_conker(chr, d);
        if (va_ok(ctl)) {
            float y = wrap360(MEMF(ctl + 0x60u) + d);
            MEMF(ctl + 0x60u) = y;
            MEMF(ctl + 0x5Cu) = y;
        }
    }
    return 1;
}

/* cam_ctl_ease_caller: the target distance and height for the framing
 * (zoom = the Unlocked Camera's weapon zoom). */
void swing_shoulder_ease(float *dist, float *height, float zoom)
{
    float a = s_sh_amt;
    if (a <= 0.0f)
        return;
    *dist += (SH_DIST * zoom - *dist) * a;
    *height *= 1.0f - a;
}

/* camera_update_view_matrix: the look-at point and the camera moved
 * SH_SIDE to the right (the aiming view's side, measured: eye +0x330,
 * look-at +0x2F4). *dx, *dz = the move, 0 = none. */
void swing_shoulder_side(uint32_t cam, float *dx, float *dz)
{
    float fx, fz, m;
    *dx = *dz = 0.0f;
    if (s_sh_amt <= 0.0f || !cam || cam != MEM32(CAM0))
        return;
    fx = MEMF(cam + 0x2F4u) - MEMF(cam + 0x330u);
    fz = MEMF(cam + 0x2FCu) - MEMF(cam + 0x338u);
    m = sqrtf(fx * fx + fz * fz);
    if (m < 1.0f)
        return;
    *dx = -fz / m * SH_SIDE * s_sh_amt;
    *dz = fx / m * SH_SIDE * s_sh_amt;
}

int swing_aim_still(uint32_t aimd)
{
    uint32_t cam = MEM32(CAM0), chr, ctl;

    if (!aimd || !va_ok(cam) || !swing_camera_normal(cam))
        return 0;
    chr = MEM32(cam + 0x3E0u);
    ctl = MEM32(chr + 0x568u);
    return MEM32(ctl + 0x4D4u) == aimd;
}

/* The move turn (0x002DEF80) of the player on the normal view, the stick
 * held: the heading = the camera's heading (+0x3C8 degrees, or its
 * controller's +0x5C when that holds 999, as 0x002D3370 reads it) + the
 * stick's angle (Txukie's conker_swing_heading); his facing turns to it at
 * the game's own rate (turn_toward; round 4: Txukie's set it at once) and
 * he runs the way he faces (the stick straight ahead). */
int swing_heading(uint32_t chr, uint32_t *heading)
{
    uint16_t now;
    float deg;

    if (chr && chr == player())
        s_turn_calls++;
    /* The weaponless strafe: he goes straight the way the plain movement
     * asks (*heading: the camera + the stick, the game's own), no turn. */
    if (s_sx && s_lstick && chr && chr == player()) {
        float rel = (float)(int16_t)(uint16_t)s_stick_rel * (360.0f / 65536.0f);
        MEM16(chr + 0x170u) = (uint16_t)*heading;
        MEM16(chr + 0x174u) = (uint16_t)*heading;
        /* Sideways or back: at the asked speed at once, as the bat's strafe
         * (the plain run builds up over ~1 s; +0x118 = the stick's asked
         * speed, set just before this call). */
        if ((rel > 45.0f || rel < -45.0f) && MEMF(chr + 0x118u) > MEMF(chr + 0x110u))
            MEMF(chr + 0x110u) = MEMF(chr + 0x118u);
        return 0;
    }
    if (!s_run_on || !s_stick_held || !chr || chr != player())
        return 0;
    if (!cam_heading(chr, &deg))
        return 0;
    *heading = ((uint32_t)(int32_t)(deg * DEG_TO_ANGLE) + (uint32_t)s_stick_angle) & 0xFFFFu;
    now = turn_toward(MEM16(chr + 0x170u), (uint16_t)*heading, move_speed(chr));
    MEM16(chr + 0x170u) = now;
    MEM16(chr + 0x174u) = now;
    return 1;
}

/* The weaponless strafe's look (sh_frame "The weaponless strafe"). His
 * matrix build (0x000808F0 -> 0x0029CBF0) turns his body by +0x114
 * (degrees; the plain movement makes it from his facing each frame): set
 * to the view's way while he strafes, put back at the next poll. */
static float s_sx_114;
static uint32_t s_sx_chr;
static int s_sx_saved;
static uint32_t s_sx_clip;          /* the bat clip shown in place of the plain one, 0 = none */

/* Tests (round 7): RECOMP_SX_ARM (0 / 1 / 2, swing_arm_pose), RECOMP_SX_ARM_MIRROR
 * (x / y / z / none), RECOMP_SX_DRAW_TURN=<degrees> (his drawn body turned that
 * much more while he strafes, so the shoulder view sees his arms from the side
 * or the front), and RECOMP_SX_TEST_FILE=<file> holding "<turn> [mode]
 * [mirror]", read again every 300 ms (every case in one run). */
static float s_sx_turn;
static int s_arm_mode = -1, s_arm_mirror = 1;

static int mirror_from(const char *e)
{
    return !e || !*e || *e == 'x' ? 1 : *e == 'y' ? 2 : *e == 'z' ? 3 : 0;
}

static void sx_test_settings(void)
{
    static long next;
    static const char *file;
    long t = now_ms();
    FILE *f;
    char m[16] = "";
    float turn = 0.0f;
    int mode = 0, got;
    if (s_arm_mode < 0) {
        const char *e = getenv("RECOMP_SX_ARM");
        s_arm_mode = e && *e >= '0' && *e <= '2' ? *e - '0' : 2;
        s_arm_mirror = mirror_from(getenv("RECOMP_SX_ARM_MIRROR"));
        e = getenv("RECOMP_SX_DRAW_TURN");
        s_sx_turn = e && *e ? (float)atof(e) : 0.0f;
        file = getenv("RECOMP_SX_TEST_FILE");
        if (file && !*file)
            file = NULL;
    }
    if (!file || t < next)
        return;
    next = t + 300;
    if (fopen_s(&f, file, "r") || !f)
        return;
    got = fscanf_s(f, "%f %d %15s", &turn, &mode, m, (unsigned)sizeof m);
    fclose(f);
    if (got >= 1 && isfinite(turn))
        s_sx_turn = turn;
    if (got >= 2 && mode >= 0 && mode <= 2)
        s_arm_mode = mode;
    if (got >= 3)
        s_arm_mirror = mirror_from(m);
}

void swing_strafe_draw(uint32_t chr)
{
    uint16_t a;
    uint32_t st;
    if (s_sx_saved || !chr || chr != player())
        return;
    st = MEM32(chr + 0xA4u);
    /* Strafing on the ground (the plain sneak / walk / run, or pushing a
     * wall: shown as a side-run in place), or in the air after a strafe. */
    if (!(s_sx && s_lstick && (st <= 2u || st == 0x7Au)) && !s_sx_air)
        return;
    a = head_angle(s_sh_yaw);
    s_sx_chr = chr;
    s_sx_114 = MEMF(chr + 0x114u);
    s_sx_saved = 1;
    MEMF(chr + 0x114u) = (float)(int16_t)(uint16_t)(a + 0x4000u) * (360.0f / 65536.0f) + s_sx_turn;
}

static void sx_restore(void)
{
    if (!s_sx_saved)
        return;
    s_sx_saved = 0;
    MEMF(s_sx_chr + 0x114u) = s_sx_114;
}

/* char_set_animation (0x002F1790) of the player asking the plain sneak /
 * walk / run (0 / 1 / 2) while he strafes: the bat's clip for the way the
 * stick points from the camera (left / right = side-run or side-walk, back
 * = run-back or walk-back; ahead = the plain clip). *name = that clip's name
 * (0 = keep the asked one); *force = the clip changes under the same index,
 * so the game must take it as a new request (it keeps the playing clip for
 * a re-ask). The bat's clips are in Conker's own package (Anims\Conker):
 * every level has them. */
int swing_strafe_anim(uint32_t chr, uint32_t idx, float speed, uint32_t *name, int *force)
{
    uint32_t want = 0, tbl, ctl;
    int out;
    *name = 0;
    *force = 0;
    if (!chr || chr != player() || (idx > 2u && idx != 0x7Au))
        return 0;
    (void)speed;
    /* Let go: the plain clip is not asked back (stance or a stop comes next;
     * a frame of the forward run would flash). */
    if (!s_lstick)
        return 0;
    ctl = MEM32(chr + 0x568u);
    out = va_ok(ctl) ? (int)(MEM32(ctl + 0xA0u) & 0x7Fu) : 0;
    /* Read now, not at the poll: a landing asks the run in the same frame. */
    if (s_shoulder && !MEM8(chr + 0x1B5u) && (!out || !melee_body(MEM32(chr + 0xA4u)))) {
        float rel = (float)(int16_t)(uint16_t)s_stick_rel * (360.0f / 65536.0f);
        int run = idx != 1u && idx != 0u;           /* run, or a wall push (in place) */
        if (rel > 45.0f && rel < 135.0f)
            want = run ? 2107u : 2105u;             /* SideRunL / SideWalkL */
        else if (rel < -45.0f && rel > -135.0f)
            want = run ? 2108u : 2106u;             /* SideRunR / SideWalkR */
        else if (rel >= 135.0f || rel <= -135.0f)
            want = run ? 2110u : 2109u;             /* RunBack / WalkBack */
    }
    if (want) {
        tbl = va_ok(MEM32(chr + 0x74u)) ? MEM32(MEM32(chr + 0x74u) + 0x50u) : 0;
        if (!va_ok(tbl) || !va_ok(MEM32(tbl + want * 12u)))
            want = 0;
        else
            *name = MEM32(tbl + want * 12u);
    }
    if (want != s_sx_clip) {
        *force = 1;
        if (log_on())
            fprintf(stderr, "[SWING] strafe clip: %u -> %u (asked %u)\n", s_sx_clip, want, idx);
        s_sx_clip = want;
    }
    return *name || *force;
}

/* The strafe's right arm (rounds 6-7, user 2026-11-24: in the bat's side /
 * back clips "he holds his right arm as if he's holding the bat"; wanted:
 * both arms alike, best the right arm copying the left arm's motion). The
 * pose the game mixes for him (0x00036230 over the list [chr+0x3C4]+4; 10
 * floats a bone: rotation x y z w, offset, scale) is changed right after the
 * mix, before 0x00036C00 builds his bone matrices from it (swing_arm_pose,
 * from the wrapper sub_00036230): each right-arm bone (r_shoulder, r_elbow,
 * r_wrist, r_fingers, r_thumb; found by name in his skeleton) gets its left
 * twin's rotation mirrored, faded in and out with s_arm_w (ARM_FADE).
 * RECOMP_SX_ARM: 0 = the clip's own arm, 1 = the left arm's pose of the
 * same moment, 2 (default) = of half a cycle before (arms alternate as in a
 * run; from a history of this clip's frames, the same moment until half a
 * cycle is known). RECOMP_SX_ARM_MIRROR = x / y / z / none: the mirror of a
 * rotation (x = across the body's side-to-side axis).
 * Round 6's arm layer (the plain run's arm on the game's arm slot 3) did
 * nothing until a weapon had been drawn while moving: its bone list
 * 0x0074A0A0 is filled only by the layered take-out / put-away (0x00319B50),
 * and once filled the run's arm (its own 0.95 s cycle over the side clip's
 * 0.53 s) swung far higher than the left arm (the user's shot 3).
 * Steps aside while the game's own arm layer plays (a draw / put-away on
 * the arm: ctl +0x1F8 1 / 3, or a slot-3 layer with weight) and from a B
 * press on (its draw takes the arm). */
#define ARM_FADE      0.12f
#define ARM_HIST      96
static float s_arm_w;               /* how much of the mirrored arm shows, 0..1 */

/* Some layer of the game's own on his arm (slot 3) with weight and not
 * fading out, or a draw / put-away going (ctl +0x1F8 1 / 3). A layered
 * put-away holds its end pose ~0.45 s, then fades out over 0.25 s (+0x1C4
 * < 0): ours fades in over that fade, not after it (the bat-arm pose came
 * back between them for ~0.5 s). */
static int arm_busy(uint32_t chr)
{
    uint32_t ctl = MEM32(chr + 0x568u), inst = MEM32(chr + 0x3C4u), n;
    int guard = 0;
    if (va_ok(ctl) && (MEM32(ctl + 0x1F8u) == 1u || MEM32(ctl + 0x1F8u) == 3u))
        return 1;
    if (!va_ok(inst))
        return 0;
    for (n = MEM32(inst + 8u); n && va_ok(n) && guard < 64; n = MEM32(n + 4u), guard++)
        if (MEM32(n + 8u) == 3u && MEMF(n + 0x1C8u) * MEMF(n + 0x1C0u) > 0.02f && MEMF(n + 0x1C4u) >= 0.0f)
            return 1;
    return 0;
}

static void arm_frame(uint32_t chr, long t)
{
    static LONG b_seen;
    static long hold_until, last_t;
    static int was;
    uint32_t st = chr ? MEM32(chr + 0xA4u) : ~0u;
    LONG b = s_fs_player_b;
    float dt = last_t ? (float)(t - last_t) * 0.001f : 0.0f;
    int want = 0;

    sx_test_settings();
    last_t = t;
    if (dt < 0.0f || dt > 0.1f)
        dt = 0.1f;
    /* B (the player's, or Free Swing's own): its draw takes the arm (ours
     * fades out over the draw's own blend-in, no snap). */
    if (b != b_seen || t < s_fs_b_until) {
        b_seen = b;
        if (s_arm_w > 0.0f && t >= hold_until && log_on())
            fprintf(stderr, "[SWING] t=%ld strafe arm: off for a draw\n", t);
        hold_until = t + 150;
    }
    if (s_arm_mode && s_sx && s_lstick && chr && chr == player() && (st <= 2u || st == 0x7Au)
        && s_sx_clip >= 2105u && s_sx_clip <= 2110u && t >= hold_until && !arm_busy(chr))
        want = 1;
    if (want != was && log_on())
        fprintf(stderr, "[SWING] t=%ld strafe arm: %s (clip %u, mode %d)\n", t,
                want ? "the left arm's, mirrored" : "let go", s_sx_clip, s_arm_mode);
    was = want;
    if (want) {
        s_arm_w += dt / ARM_FADE;
        if (s_arm_w > 1.0f)
            s_arm_w = 1.0f;
    } else {
        s_arm_w -= dt / ARM_FADE;
        if (s_arm_w < 0.0f)
            s_arm_w = 0.0f;
    }
}

/* The arm bones by name in a character's skeleton ([[[chr+0x4B0]+0x24]+8]
 * +0x10: count, then 12-byte entries {name, 0, bone}; as 0x00319B50 looks
 * up the right arm's), cached per skeleton. */
static const char *const s_arm_names[2][5] = {
    { "r_shoulder", "r_elbow", "r_wrist", "r_fingers", "r_thumb" },
    { "l_shoulder", "l_elbow", "l_wrist", "l_fingers", "l_thumb" },
};
static uint32_t s_arm_skel;
static int s_arm_bone[2][5], s_arm_bones_ok;

static int guest_streq(uint32_t va, const char *s)
{
    for (;; va++, s++) {
        if (MEM8(va) != (uint8_t)*s)
            return 0;
        if (!*s)
            return 1;
    }
}

static int arm_bones(uint32_t chr)
{
    uint32_t a = MEM32(chr + 0x4B0u), t = 0, e, n, i, nm;
    int s, k;
    if (va_ok(a) && va_ok(a = MEM32(a + 0x24u)) && va_ok(a = MEM32(a + 8u)))
        t = MEM32(a + 0x10u);
    if (!va_ok(t))
        return 0;
    if (t == s_arm_skel)
        return s_arm_bones_ok;
    s_arm_skel = t;
    s_arm_bones_ok = 0;
    n = MEM32(t);
    e = MEM32(t + 4u);
    if (n > 4096u || !va_ok(e))
        return 0;
    memset(s_arm_bone, 0xFF, sizeof s_arm_bone);
    for (i = 0; i < n; i++) {
        nm = MEM32(e + i * 12u);
        if (!va_ok(nm) || MEM8(nm + 1u) != '_')
            continue;
        for (s = 0; s < 2; s++)
            for (k = 0; k < 5; k++)
                if (s_arm_bone[s][k] < 0 && guest_streq(nm, s_arm_names[s][k]))
                    s_arm_bone[s][k] = (int)(MEM32(e + i * 12u + 8u) & 0xFFFFu);
    }
    s_arm_bones_ok = 1;
    for (s = 0; s < 2; s++)
        for (k = 0; k < 5; k++)
            if (s_arm_bone[s][k] < 0 || s_arm_bone[s][k] >= 128)
                s_arm_bones_ok = 0;
    if (log_on())
        fprintf(stderr, "[SWING] arm bones (skeleton %08X): right %d %d %d %d %d, left %d %d %d %d %d%s\n", t,
                s_arm_bone[0][0], s_arm_bone[0][1], s_arm_bone[0][2], s_arm_bone[0][3], s_arm_bone[0][4],
                s_arm_bone[1][0], s_arm_bone[1][1], s_arm_bone[1][2], s_arm_bone[1][3], s_arm_bone[1][4],
                s_arm_bones_ok ? "" : " (not all found: no mirrored arm)");
    return s_arm_bones_ok;
}

/* A history of the left arm's rotations over the body clip's cycle (mode
 * 2): the phase unwrapped (+1 a cycle), newest last. */
typedef struct {
    float u;
    float q[5][4];
} ArmSample;
static ArmSample s_hist[ARM_HIST];
static int s_hist_n, s_hist_at;
static float s_hist_u, s_hist_p, s_hist_k;
static long s_hist_kt;
static uint32_t s_hist_clip;

static void q_norm(float *q)
{
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    int i;
    if (l < 1e-6f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (i = 0; i < 4; i++)
        q[i] /= l;
}

/* out = a (1 - k) + b k, b's sign matched to a's, normalised. */
static void q_nlerp(const float *a, const float *b, float k, float *out)
{
    float s = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f ? -1.0f : 1.0f;
    int i;
    for (i = 0; i < 4; i++)
        out[i] = a[i] * (1.0f - k) + s * b[i] * k;
    q_norm(out);
}

/* The left arm half a cycle before (or after: the same pose) from the
 * history: 1 = found (interpolated between the two samples around it). */
static int arm_hist_find(float q[5][4])
{
    int i, j, s;
    for (i = 0; i < s_hist_n - 1; i++) {
        const ArmSample *b = &s_hist[(s_hist_at - 1 - i + 2 * ARM_HIST) % ARM_HIST];
        const ArmSample *a = &s_hist[(s_hist_at - 2 - i + 2 * ARM_HIST) % ARM_HIST];
        for (s = -1; s <= 1; s += 2) {
            float want = s_hist_u + 0.5f * (float)s, lo = a->u < b->u ? a->u : b->u;
            float hi = a->u < b->u ? b->u : a->u, f;
            if (want < lo || want > hi || hi - lo > 0.25f)
                continue;
            f = hi > lo ? (want - a->u) / (b->u - a->u) : 0.0f;
            for (j = 0; j < 5; j++)
                q_nlerp(a->q[j], b->q[j], f, q[j]);
            return 1;
        }
    }
    return 0;
}

static void arm_hist_add(uint32_t clip, float p, float q[5][4])
{
    static long last;
    long t = now_ms();
    float d;
    int j;
    /* A new clip, or a gap (a new strafe of the same clip): start again. */
    if (clip != s_hist_clip || !isfinite(p) || t - last > 100 || t < last) {
        s_hist_clip = clip;
        s_hist_n = s_hist_at = 0;
        s_hist_u = p;
    } else {
        d = p - s_hist_p;
        if (d == 0.0f)
            return;                     /* the same frame mixed again */
        if (d < -0.5f)
            d += 1.0f;
        else if (d > 0.5f)
            d -= 1.0f;
        s_hist_u += d;
    }
    last = t;
    s_hist_p = p;
    s_hist[s_hist_at].u = s_hist_u;
    for (j = 0; j < 5; j++)
        memcpy(s_hist[s_hist_at].q[j], q[j], sizeof q[j]);
    s_hist_at = (s_hist_at + 1) % ARM_HIST;
    if (s_hist_n < ARM_HIST)
        s_hist_n++;
}

void swing_arm_pose(uint32_t list, uint32_t pose)
{
    uint32_t chr = player(), arr, stride, n, body = 0;
    float L[5][4], R[5][4], H[5][4], p = 0.0f;
    int k, j, have = 0, dbg = dev_log(DLOG_SWING) >= 2;
    uint32_t guard = 0;

    if ((s_arm_w <= 0.0f && !dbg) || !chr || !va_ok(MEM32(chr + 0x3C4u))
        || list != MEM32(chr + 0x3C4u) + 4u || !va_ok(pose) || !arm_bones(chr))
        return;
    arr = MEM32(pose + 4u);
    stride = MEM32(pose + 0x168u);
    if (!va_ok(arr) || stride < 4u || stride > 32u)
        return;
    /* The body clip: the slot-0 layer mixed last (the newer one of a
     * cross-fade). Its +0xDC = the cycle's phase, 0..1. */
    for (n = MEM32(list + 4u); n && va_ok(n) && guard < 64; n = MEM32(n + 4u), guard++)
        if (MEM32(n + 8u) == 0u)
            body = n;
    if (body)
        p = MEMF(body + 0xDCu);
    for (k = 0; k < 5; k++) {
        uint32_t ra = arr + ((uint32_t)s_arm_bone[0][k] * stride) * 4u;
        uint32_t la = arr + ((uint32_t)s_arm_bone[1][k] * stride) * 4u;
        for (j = 0; j < 4; j++) {
            R[k][j] = MEMF(ra + 4u * (uint32_t)j);
            L[k][j] = MEMF(la + 4u * (uint32_t)j);
        }
    }
    if (dbg) {
        /* log swing 2: both arms every mix (the mirror's calibration on the
         * plain run: tools _work\s061\r7cal.py). */
        char nm[48] = "-";
        uint32_t s = body ? MEM32(body + 0xCu) : 0u, i, last = 0, prev = 0;
        /* "aid\anim\...\<clip>\default.rba": the clip's folder name */
        for (i = 0; va_ok(s) && i < 160u && MEM8(s + i); i++)
            if (MEM8(s + i) == '\\') {
                prev = last;
                last = i + 1u;
            }
        if (va_ok(s) && last > prev) {
            for (i = 0; i + 1u < sizeof nm && prev + i + 1u < last; i++)
                nm[i] = (char)MEM8(s + prev + i);
            nm[i] = 0;
        }
        fprintf(stderr, "[ARMQ] t=%ld clip %s p %.4f w %.2f", now_ms(), nm, p, s_arm_w);
        for (k = 0; k < 5; k++)
            fprintf(stderr, " L%d %.4f %.4f %.4f %.4f", k, L[k][0], L[k][1], L[k][2], L[k][3]);
        for (k = 0; k < 5; k++)
            fprintf(stderr, " R%d %.4f %.4f %.4f %.4f", k, R[k][0], R[k][1], R[k][2], R[k][3]);
        fputc('\n', stderr);
    }
    if (body)
        arm_hist_add(MEM32(body + 0x10u), p, L);
    if (s_arm_w <= 0.0f)
        return;
    if (s_arm_mode == 2)
        have = arm_hist_find(H);
    /* Half a cycle of this clip known only after a while (a start, a change
     * of clip): the same moment's pose until then, eased over. */
    {
        long tn = now_ms();
        float step = (float)(tn - s_hist_kt) * 0.001f / ARM_FADE;
        s_hist_kt = tn;
        if (step < 0.0f || step > 1.0f)
            step = 1.0f;
        s_hist_k = have ? (s_hist_k + step > 1.0f ? 1.0f : s_hist_k + step) : 0.0f;
    }
    for (k = 0; k < 5; k++) {
        float m[4], out[4], q[4];
        uint32_t ra = arr + ((uint32_t)s_arm_bone[0][k] * stride) * 4u;
        if (have)
            q_nlerp(L[k], H[k], s_hist_k, q);
        else
            memcpy(q, L[k], sizeof q);
        /* A mirror across the plane whose normal is the axis: that axis'
         * part kept, the other two negated. */
        m[0] = q[0];
        m[1] = q[1];
        m[2] = q[2];
        m[3] = q[3];
        if (s_arm_mirror == 1) {
            m[1] = -q[1];
            m[2] = -q[2];
        } else if (s_arm_mirror == 2) {
            m[0] = -q[0];
            m[2] = -q[2];
        } else if (s_arm_mirror == 3) {
            m[0] = -q[0];
            m[1] = -q[1];
        }
        q_nlerp(R[k], m, s_arm_w, out);
        for (j = 0; j < 4; j++)
            MEMF(ra + 4u * (uint32_t)j) = out[j];
    }
}

/* Swing While Jumping's legs (to-do 48): the swing clips stand, so in the
 * air his legs take the jump's pose of the frame before the swing started
 * (the mixed pose, kept every frame he is in the air and not swinging),
 * eased in over AIR_FADE_IN and out over AIR_FADE_OUT when he lands or the
 * swing ends (the bat's fall clip takes over then). The bones: the game's
 * lower-body list (0x0074818C, the one its walking legs layer uses: CENT1,
 * body, hips, knees, ankles, toes; by name in the skeleton). RECOMP_AIR_LEGS:
 * 0 = the swing's own legs, 1 = hips down only, 2 = the game's list
 * (default).
 * His tail in the tail spin (a swing started in it, or in noclip's spin
 * pose): the swing clip holds the tail up, so it stopped spinning; the
 * spin's tail (tail_1..4) is recorded over the spin clip's cycle (by its
 * phase, the slot-0 node's +0xDC) and played on at the spin's own rate
 * during the swing (RECOMP_AIR_TAIL=0: the swing's tail). */
#define AIR_FADE_IN   0.08f
#define AIR_FADE_OUT  0.15f
#define LEG_N         10
#define TAIL_N        4
#define BONE_N        (LEG_N + TAIL_N)
#define TAIL_HIST     96
static const char *const s_leg_names[BONE_N] = {
    "CENT1", "body", "l_hip", "r_hip", "l_knee", "r_knee", "l_ankle", "r_ankle", "l_toe", "r_toe",
    "tail_1", "tail_2", "tail_3", "tail_4",
};
static uint32_t s_leg_skel;
static int s_leg_bone[BONE_N], s_leg_ok;
static float s_leg_pose[LEG_N][10];
/* The legs near a jump's top (the last rising frame of the plain jump or the
 * bat's JumpPt1): a jump taken during a floor swing has no jump pose of its
 * own to hold (the swing goes on instead), so it borrows this one. */
static float s_jl_pose[LEG_N][10];
static int s_jl_have;
static int s_leg_have, s_leg_mode = -1, s_leg_was, s_tail_mode = -1, s_tail_was;
static float s_leg_w, s_tail_w;
static long s_leg_t;

typedef struct {
    float p;                        /* the spin clip's phase, 0..1 */
    float q[TAIL_N][4];
} TailSample;
static TailSample s_th[TAIL_HIST];
static int s_th_n, s_th_at;
static float s_th_p = -1.0f, s_th_rate, s_tail_pv, s_tail_per;
static long s_th_t;

static int leg_bones(uint32_t chr)
{
    uint32_t a = MEM32(chr + 0x4B0u), t = 0, e, n, i, nm;
    int k;
    if (va_ok(a) && va_ok(a = MEM32(a + 0x24u)) && va_ok(a = MEM32(a + 8u)))
        t = MEM32(a + 0x10u);
    if (!va_ok(t))
        return 0;
    if (t == s_leg_skel)
        return s_leg_ok;
    s_leg_skel = t;
    s_leg_ok = 0;
    n = MEM32(t);
    e = MEM32(t + 4u);
    if (n > 4096u || !va_ok(e))
        return 0;
    for (k = 0; k < BONE_N; k++)
        s_leg_bone[k] = -1;
    for (i = 0; i < n; i++) {
        nm = MEM32(e + i * 12u);
        if (!va_ok(nm))
            continue;
        for (k = 0; k < BONE_N; k++)
            if (s_leg_bone[k] < 0 && guest_streq(nm, s_leg_names[k]))
                s_leg_bone[k] = (int)(MEM32(e + i * 12u + 8u) & 0xFFFFu);
    }
    s_leg_ok = 1;
    for (k = 0; k < BONE_N; k++)
        if (s_leg_bone[k] < 0 || s_leg_bone[k] >= 128)
            s_leg_ok = 0;
    s_th_n = s_th_at = 0;
    if (log_on())
        fprintf(stderr, "[SWING] leg bones (skeleton %08X): %d %d %d %d %d %d %d %d %d %d, tail %d %d "
                "%d %d%s\n", t, s_leg_bone[0], s_leg_bone[1], s_leg_bone[2], s_leg_bone[3],
                s_leg_bone[4], s_leg_bone[5], s_leg_bone[6], s_leg_bone[7], s_leg_bone[8],
                s_leg_bone[9], s_leg_bone[10], s_leg_bone[11], s_leg_bone[12], s_leg_bone[13],
                s_leg_ok ? "" : " (not all found: the swing's own legs in the air)");
    return s_leg_ok;
}

/* The slot-0 node mixed last (the body clip): its phase, -1 = none. */
static float body_phase(uint32_t list)
{
    uint32_t n, body = 0;
    int guard = 0;
    for (n = MEM32(list + 4u); n && va_ok(n) && guard < 64; n = MEM32(n + 4u), guard++)
        if (MEM32(n + 8u) == 0u)
            body = n;
    return body ? MEMF(body + 0xDCu) : -1.0f;
}

static void tail_record(uint32_t arr, uint32_t stride, float p, long t)
{
    TailSample *s;
    int k, j;
    if (p < 0.0f || p > 1.0f || p == s_th_p)
        return;
    if (s_th_p >= 0.0f && t > s_th_t && t - s_th_t < 100) {
        float d = p - s_th_p, r;
        if (d < -0.5f)
            d += 1.0f;
        r = d * 1000.0f / (float)(t - s_th_t);      /* cycles a second */
        if (r > 0.0f && r < 20.0f)
            s_th_rate = s_th_rate > 0.0f ? s_th_rate * 0.8f + r * 0.2f : r;
    } else {
        s_th_n = s_th_at = 0;                       /* a new spin */
    }
    s_th_p = p;
    s_th_t = t;
    if (dev_log(DLOG_SWING) >= 2)
        fprintf(stderr, "[AIRTAIL] t=%ld record p %.3f rate %.2f q0 %.3f %.3f %.3f %.3f\n", t, p,
                s_th_rate, MEMF(arr + (uint32_t)s_leg_bone[LEG_N] * stride * 4u),
                MEMF(arr + (uint32_t)s_leg_bone[LEG_N] * stride * 4u + 4u),
                MEMF(arr + (uint32_t)s_leg_bone[LEG_N] * stride * 4u + 8u),
                MEMF(arr + (uint32_t)s_leg_bone[LEG_N] * stride * 4u + 12u));
    s = &s_th[s_th_at];
    s->p = p;
    for (k = 0; k < TAIL_N; k++) {
        uint32_t b = arr + ((uint32_t)s_leg_bone[LEG_N + k] * stride) * 4u;
        for (j = 0; j < 4; j++)
            s->q[k][j] = MEMF(b + 4u * (uint32_t)j);
    }
    s_th_at = (s_th_at + 1) % TAIL_HIST;
    if (s_th_n < TAIL_HIST)
        s_th_n++;
}

/* The tail at phase pv from the newest recorded cycle (the two samples
 * around it); 0 = not enough recorded. */
static int tail_at(float pv, float q[TAIL_N][4])
{
    int i, k;
    for (i = 0; i < s_th_n - 1; i++) {
        const TailSample *b = &s_th[(s_th_at - 1 - i + 2 * TAIL_HIST) % TAIL_HIST];
        const TailSample *a = &s_th[(s_th_at - 2 - i + 2 * TAIL_HIST) % TAIL_HIST];
        float lo = a->p, hi = b->p, x = pv, f;
        if (hi < lo) {                  /* the pair wraps past 1 */
            hi += 1.0f;
            if (x < lo)
                x += 1.0f;
        }
        if (x < lo || x > hi || hi - lo > 0.3f)
            continue;
        f = hi > lo ? (x - lo) / (hi - lo) : 0.0f;
        for (k = 0; k < TAIL_N; k++)
            q_nlerp(a->q[k], b->q[k], f, q[k]);
        return 1;
    }
    return 0;
}

/* One turn of the tail in the recording, in the spin clip's phase: the
 * newest sample's tail_1 matched against the older ones (the spin clip is
 * several seconds long and turns the tail about every 0.15 of it). 0 = not
 * found (less than a turn recorded). */
static float tail_period(void)
{
    const TailSample *n = &s_th[(s_th_at - 1 + TAIL_HIST) % TAIL_HIST];
    float best = 0.0f, per = 0.0f;
    int i;
    for (i = 1; i < s_th_n; i++) {
        const TailSample *a = &s_th[(s_th_at - 1 - i + 2 * TAIL_HIST) % TAIL_HIST];
        float d = n->p - a->p, dot;
        if (d < 0.0f)
            d += 1.0f;
        if (d < 0.04f)
            continue;
        if (d > 0.4f)
            break;
        dot = fabsf(n->q[0][0] * a->q[0][0] + n->q[0][1] * a->q[0][1] + n->q[0][2] * a->q[0][2]
                    + n->q[0][3] * a->q[0][3]);
        if (dot > best) {
            best = dot;
            per = d;
        }
    }
    return best > 0.97f ? per : 0.0f;
}

void swing_air_pose(uint32_t list, uint32_t pose)
{
    uint32_t chr = player(), arr, stride, st, nf;
    long t = now_ms();
    float dt, p;
    int k, j, air, want, first, nc, tail;

    if (s_leg_mode < 0) {
        const char *e = getenv("RECOMP_AIR_LEGS");
        s_leg_mode = e && *e ? atoi(e) : 2;
        e = getenv("RECOMP_AIR_TAIL");
        s_tail_mode = !(e && *e == '0');
    }
    /* Every character's mix comes here: only the player's counts. */
    if (!chr || !va_ok(MEM32(chr + 0x3C4u)) || list != MEM32(chr + 0x3C4u) + 4u)
        return;
    if (!swing_air() || MEM8(MP_FLAG) || !va_ok(pose) || !leg_bones(chr)) {
        s_leg_have = 0;
        s_leg_w = s_tail_w = 0.0f;
        return;
    }
    arr = MEM32(pose + 4u);
    stride = MEM32(pose + 0x168u);
    if (!va_ok(arr) || stride < 4u || stride > 32u)
        return;
    nf = stride < 10u ? stride : 10u;
    /* Game time (the eases match the mixer's blends, also under timescale /
     * Bullet time); real time when the game's frame seconds are unusable. */
    dt = MEMF(0x007FA21Cu);
    if (!(dt > 0.0f && dt <= 0.1f)) {
        dt = s_leg_t ? (float)(t - s_leg_t) * 0.001f : 0.0f;
        if (dt < 0.0f || dt > 0.1f)
            dt = 0.1f;
    }
    s_leg_t = t;
    st = MEM32(chr + 0xA4u);
    nc = noclip_now(t);
    air = (MEMF(chr + 0xDCu) > 0.0f || nc) && !MEM8(chr + 0x1B5u);
    first = s_leg_mode == 1 ? 2 : 0;
    if (!air || !swinging(st))
        s_jump_swing = 0;
    if (s_jump_swing && !s_leg_have && s_jl_have) {
        memcpy(s_leg_pose, s_jl_pose, sizeof s_leg_pose);
        s_leg_have = 1;
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld air: a jump during the swing: the legs of a jump's top\n", t);
    }
    /* Noclip: the weapon's poses between the spin's (its draw's FireHold). */
    want = s_leg_mode && air && s_leg_have && (swinging(st) || (nc && st != ST_HOVER));
    if (want != s_leg_was && log_on())
        fprintf(stderr, "[SWING] t=%ld air: legs %s (state 0x%X height %.1f)\n", t,
                want ? "from the jump" : "let go", st, MEMF(chr + 0xDCu));
    s_leg_was = want;
    p = body_phase(list);
    if (!air) {
        if (s_leg_w <= 0.0f)
            s_leg_have = 0;             /* landed: kept for the ease out only */
    } else if (!swinging(st) && !(nc && st != ST_HOVER) && s_leg_w <= 0.0f) {
        /* The jump's pose now (before any of ours): kept for a swing. Not
         * while a swing's legs ease out: the first frame after the swing is
         * the swing clip's own upright end (the mixer blends from it), and
         * taken as the held pose it left nothing to ease from (a one-frame
         * snap upright in the tail spin and noclip). */
        for (k = first; k < LEG_N; k++) {
            uint32_t b = arr + ((uint32_t)s_leg_bone[k] * stride) * 4u;
            for (j = 0; j < (int)nf; j++)
                s_leg_pose[k][j] = MEMF(b + 4u * (uint32_t)j);
        }
        s_leg_have = 1;
        if ((st == ST_JUMP1 || st == ST_PLAIN_JUMP) && MEMF(chr + 0xD0u) > 0.0f) {
            memcpy(s_jl_pose, s_leg_pose, sizeof s_jl_pose);
            s_jl_have = 1;
        }
        if (st == ST_HOVER)
            tail_record(arr, stride, p, t);
    }
    /* The spin's tail through a swing in it (and the bat's fall after it). */
    tail = s_tail_mode && air && s_th_n >= 8 && s_th_rate > 0.0f
        && (s_tail_was || t - s_th_t < 2000)        /* a spin just before it */
        && ((s_hover_swing && (swinging(st) || st == ST_JUMP2)) || (nc && st != ST_HOVER));
    if (tail && !s_tail_was) {
        s_tail_per = tail_period();
        s_tail_pv = 0.0f;
        if (s_tail_per <= 0.0f)
            tail = 0;                   /* less than a turn recorded: the swing's tail */
    }
    if (tail != s_tail_was && log_on())
        fprintf(stderr, "[SWING] t=%ld air: tail %s (state 0x%X, %d samples, %.2f cycles a s, "
                "a turn %.3f)\n", t, tail ? "spins on" : "let go", st, s_th_n, s_th_rate,
                s_tail_per);
    s_tail_was = tail;
    for (k = 0; k < 2; k++) {
        float *w = k ? &s_tail_w : &s_leg_w;
        int on = k ? tail : want;
        *w += (on ? 1.0f : -1.0f) * dt / (on ? AIR_FADE_IN : AIR_FADE_OUT);
        if (*w > 1.0f)
            *w = 1.0f;
        if (*w < 0.0f)
            *w = 0.0f;
    }
    if (s_leg_w > 0.0f && s_leg_have) {
        for (k = first; k < LEG_N; k++) {
            uint32_t b = arr + ((uint32_t)s_leg_bone[k] * stride) * 4u;
            float q[4], cur[4];
            for (j = 0; j < 4; j++)
                cur[j] = MEMF(b + 4u * (uint32_t)j);
            q_nlerp(cur, s_leg_pose[k], s_leg_w, q);
            for (j = 0; j < 4; j++)
                MEMF(b + 4u * (uint32_t)j) = q[j];
            for (j = 4; j < 7 && j < (int)nf; j++)       /* the offset; the scale stays */
                MEMF(b + 4u * (uint32_t)j) += (s_leg_pose[k][j] - MEMF(b + 4u * (uint32_t)j)) * s_leg_w;
        }
    }
    if (s_tail_w > 0.0f) {
        float q[TAIL_N][4];
        float at = 0.0f;
        if (tail)
            s_tail_pv += s_th_rate * dt;    /* the spin clip's phase gone by since the swing */
        if (s_tail_per > 0.0f) {
            at = s_th_p - s_tail_per + fmodf(s_tail_pv, s_tail_per);
            if (at < 0.0f)
                at += 1.0f;
        }
        /* The tail's physics chain draws it unless char +0x542 bit 0 says
         * the animation drives it (0x0008EBC0 sets it from the state: the
         * spin's in its list, a swing isn't; 0x0008F2B0 reads it): set
         * while ours plays, the chain eases back in after (its 30 frames). */
        MEM8(chr + 0x542u) |= 1u;
        if (dev_log(DLOG_SWING) >= 2)
            fprintf(stderr, "[AIRTAIL] t=%ld pv %.3f w %.2f rate %.2f samples %d found %d\n", t,
                    at, s_tail_w, s_th_rate, s_th_n, s_tail_per > 0.0f && tail_at(at, q));
        if (s_tail_per > 0.0f && tail_at(at, q))
            for (k = 0; k < TAIL_N; k++) {
                uint32_t b = arr + ((uint32_t)s_leg_bone[LEG_N + k] * stride) * 4u;
                float cur[4], o[4];
                for (j = 0; j < 4; j++)
                    cur[j] = MEMF(b + 4u * (uint32_t)j);
                q_nlerp(cur, q[k], s_tail_w, o);
                for (j = 0; j < 4; j++)
                    MEMF(b + 4u * (uint32_t)j) = o[j];
            }
    }
}

void swing_frame(int set, uint32_t chr, int foot, int atk_key, int aiming)
{
    Core *C = &s_core;
    int quick = swing_quick(), forgive = swing_forgiving();
    int suit, raw, fresh;
    long t = now_ms();
    Look L;

    sx_restore();
    s_frame++;
    lay_frame(t);
    lay_carry_frame(t);
    if (chr)
        look(&L, chr);
    else
        memset(&L, 0, sizeof L);
    air_frame(chr, &L, t);
    free_frame(set, chr, foot, &L, t, atk_key, aiming);
    ramp_frame();
    view_frame(set, chr, foot, &L, t);
    arm_frame(chr, t);
    /* log swing 2: the player every frame (round 3's checks: turning and
     * speed through a draw, the shoulder view's heading). */
    if (dev_log(DLOG_SWING) >= 2 && chr && va_ok(L.ctl)) {
        uint32_t cam = MEM32(CAM0), cc = va_ok(cam) ? MEM32(cam + 4u) : 0;
        fprintf(stderr, "[SWTRACE] t=%ld st 0x%X out 0x%02X d %u head %.1f cam %.1f mode %X "
                "byte %u pos %.1f %.1f %.1f slot3 %d stick %d tk %d yaw %ld sh %d %.2f run %d "
                "v110 %.2f v118 %.2f c390 %.2f r474 %d r478 %d r3dc %d ang %ld held %d turns %ld\n",
                t, L.st, L.out, MEM32(L.ctl + 0x1F8u), angle_head(MEM16(chr + 0x170u)),
                va_ok(cc) ? MEMF(cc + 0x60u) : 0.0f, va_ok(cam) ? MEM32(cam + 0xC0u) : 0u,
                va_ok(cam) ? MEM8(cam) : 0u, MEMF(chr + 0x78u), MEMF(chr + 0x7Cu),
                MEMF(chr + 0x80u), (int)MEM32(L.ctl + 0x45Cu), (int)s_lstick, (int)(int16_t)MEM16(DT_TICKS),
                s_sh_yaw_on ? (long)s_sh_yaw : -1L, (int)s_shoulder, s_sh_amt, (int)s_run_on, MEMF(chr + 0x110u),
                MEMF(chr + 0x118u), MEMF(L.ctl + 0x390u), (int)MEM32(L.ctl + 0x474u),
                (int)MEM32(L.ctl + 0x478u), (int)MEM32(L.ctl + 0x3DCu),
                (long)s_stick_angle, (int)s_stick_held, (long)s_turn_calls);
    }
    if (!quick && !forgive) {
        if (C->on)
            let_go("options off");
        return;
    }
    if ((set != CONTROLS_STORY && set != CONTROLS_VAULT_SUIT) || !chr || !foot
        || MEM8(MP_FLAG) || chr != player()) {
        if (C->on)
            let_go("not on foot in play");
        return;
    }
    suit = set == CONTROLS_VAULT_SUIT;
    /* The suit's set says the suit (in_vault_suit); its punches are not a
     * drawn weapon. */
    if (suit ? !quick : (L.kind != (int)KIND_BAT && L.kind != (int)KIND_PAN)) {
        if (C->on)
            let_go("no bat or frying pan in hand");
        return;
    }
    if (!C->on) {
        C->on = 1;
        C->kept_seen = s_kept_n;
        C->passed_seen = s_passed_n;
        C->raw_was = (int)s_raw;
        InterlockedExchange(&s_charge, 1);
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld on: %s, quick %d forgiving %d\n", t,
                    suit ? "Vault suit" : L.kind == (int)KIND_PAN ? "frying pan" : "bat",
                    quick, forgive);
    }
    log_state(C, &L, t);

    raw = (int)s_raw;
    /* The player's own fresh presses (let through by the hook). */
    fresh = s_passed_n != C->passed_seen;
    if (fresh) {
        C->passed_seen = s_passed_n;
        C->last_press = t;
        C->down_until = t + PULSE_MS;
        C->up_since = 0;
        C->kept_since = 0;              /* a new press stands for a kept one */
        if (log_on())
            fprintf(stderr, "[SWING] t=%ld press (player) state 0x%X step %d next %d "
                    "window %.3f\n", t, L.st, L.step, L.next, L.win);
    }

    /* Forgiving Combo: a press the hook kept back. */
    if (!suit && s_kept_n != C->kept_seen) {
        C->kept_seen = s_kept_n;
        if (!C->kept_since) {
            C->kept_since = t;
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld kept: %s (state 0x%X step %d window %.3f "
                        "mark %.3f lock %.3f)\n", t,
                        L.lock > 0.0f ? "locked" : L.queued ? "one queued"
                        : L.step >= 1 && L.win > 0.0f ? "too early" : "swing ending",
                        L.st, L.step, L.win, L.mark, L.lock);
        }
    }
    if (C->kept_since) {
        if (t - C->kept_since > FORGIVE_MS) {
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld kept press dropped after %ld ms\n", t,
                        t - C->kept_since);
            C->kept_since = 0;
        } else if (can_chain(&L) || can_start(&L)) {
            if (s_kept && raw) {
                InterlockedExchange(&s_unblock, 1);
                set_force(FORCE_NONE);
            } else {
                InterlockedExchange(&s_pulse_until, t + PULSE_MS);
                set_force(FORCE_DOWN);
            }
            if (log_on())
                fprintf(stderr, "[SWING] t=%ld kept press sent after %ld ms (%s; window "
                        "%.3f mark %.3f)\n", t, t - C->kept_since,
                        s_kept && raw ? "still held" : "a pulse", L.win, L.mark);
            C->kept_since = 0;
            C->last_press = t;
            C->down_until = t + PULSE_MS;
            C->up_since = 0;
        } else {
            set_force(s_kept ? FORCE_NONE : FORCE_UP);
        }
    } else if (s_force == FORCE_DOWN && t >= C->down_until) {
        set_force(FORCE_NONE);              /* the pulse is over */
    }
    InterlockedExchange(&s_block, forgive && !suit && would_refuse(&L));

    if (C->kept_since || s_force == FORCE_DOWN)
        goto done;
    if (!quick || !raw || s_kept) {
        if (s_force == FORCE_UP)
            set_force(FORCE_NONE);
        C->up_since = 0;
        goto done;
    }

    if (suit) {
        /* Punch n (1-4); the next one only while the game chains (+0x20B,
         * a key event of the punch, or +0x214, hits landed). Normal: two
         * punches. Easy: up to four, pressed at each chain moment; the game
         * itself chains the 3rd and 4th only when the punches land, so a miss
         * gives two punches as the original (user 2026-11-23). Nothing while
         * he is knocked down or getting up. */
        uint32_t st = L.st;
        int n = st >= ST_PUNCH1 && st <= ST_PUNCH4 ? (int)(st - ST_PUNCH1) + 1 : 0;
        int easy = quick == SWING_QUICK_EASY;
        int max = easy ? 4 : 2;
        int landed = MEM8(chr + 0x214u) != 0;   /* a byte: hits landed */
        int chain = MEM8(chr + 0x20Bu) || landed;
        if (st >= ST_SUIT_DOWN_LO && st <= ST_SUIT_DOWN_HI) {
            C->chained = 0;
            quick_wait(C, t, 0, SUIT_GAP_MS, "", &L);
        } else if (!n) {
            C->chained = 0;
            quick_wait(C, t, 1, SUIT_GAP_MS, "punch 1", &L);
        } else if (n < max && chain && C->chained != st) {
            quick_wait(C, t, 1, SUIT_GAP_MS, n == 1 ? "punch 2" : n == 2 ? "punch 3" : "punch 4", &L);
            if (C->last_press == t)
                C->chained = st;            /* one press per punch */
        } else {
            quick_wait(C, t, 0, SUIT_GAP_MS, "", &L);
        }
    } else {
        int max = quick == SWING_QUICK_EASY ? 3 : 2;
        if (can_start(&L))
            quick_wait(C, t, 1, MIN_GAP_MS, "Pt1", &L);
        else if (can_chain(&L) && L.next <= max)
            quick_wait(C, t, 1, MIN_GAP_MS, L.next == 2 ? "Pt2" : "Pt3", &L);
        else
            quick_wait(C, t, 0, MIN_GAP_MS, "", &L);
    }
done:
    C->raw_was = raw;
}
