/**
 * "Update Cutscene" (Extras, to-do 47; SPEC 10.4; plan:
 * docs/research/r4-0.6.0-addons-plan.md section 5, timeline r3-gargoyle.md).
 * With Weapon = Bat the training's weapon scenes match the bat:
 *   E1 the weapon rack (TrainingCave 0x2E, scene 2): Conker takes the bat off
 *      the rack (user 2026-11-23): while the scene plays, the held-object name
 *      entry 0x281 of the pan prop (attachment 275, which the scene puts in
 *      his hand at tick 1046) names the bat's model, "BaseballBat" (the
 *      user's idea; attachments load their model by name when first drawn),
 *      and goes back at the scene's end. The pan's two sounds become the bat's
 *      (pan_unhook 0xCD8 at 1109 -> hung_cut_bat_out 0xCCC, pan_putaway 0xCD7
 *      at 1289 -> hung_cut_bat_in 0xCCB). The voice stops after the camera cut
 *      to the close-up (tick 1106), before "A frying pan!" (its last line,
 *      1141), and that line's subtitle (show 1148, hide 1247) is dropped.
 *   E3 the gargoyle (TrainingLevel 0x29, scene 11, then its part scene 9):
 *      starts at his "Ouch" (tick 2677, user 2026-11-23; first the uppercut's
 *      camera cut at 2600), the way the game starts a scene mid-timeline: its
 *      start [0x007655F4] and clock go there before the first events run, and
 *      the game's own catch-up runs every event up to there in one batch
 *      (cameras in order, so the uppercut camera is the one shown from the
 *      first frame; the weapon actions too: 0x6C sub 24 at 173 gives Conker
 *      the bat, 0x88 at 2454 draws it). The catch-up's sounds, subtitles and
 *      the hit's effect (action 0xA0 at 2614) are dropped; the speech start
 *      (tick 0) runs and its own seek (to its time, 0) is undone; the voice
 *      wave HUNG_NEW04 is served from the same point (2677 x 1,470 bytes,
 *      voice_wave_edit) and the lip base moves with it. Scene 9 ends after
 *      the boulder (user 2026-11-23): at its tick 294, before the camera cut
 *      to Conker and his own line (HUNG_NEW05 at 300).
 * With Weapon = Frying Pan (the pan in: addons_pan_on):
 *   E2 the gargoyle (the same scenes; the user's transcript 2026-11-02): his
 *      first line ("A frying pan. You stupid little t**t.", ticks 0-312),
 *      then straight on at the uppercut's camera cut (2600): at clock 312 the
 *      clock jumps to 2600 at the dispatcher's entry, last kept, so the batch
 *      is the game's own catch-up of the stretch (cameras, animations, the
 *      weapon draw 0x88 at 2454: the pan in his hand) without its sounds and
 *      subtitles; the voice wave HUNG_NEW04 is served without that stretch
 *      ([312, 2600) x 1,470 bytes) and the lip base moves 38.13 s. The pan's
 *      sounds (hit, out, in) are the Frying Pan's sound-table swaps
 *      (addons.c). Scene 9 ends after the boulder, as E3's.
 *
 * An early end: at the dispatcher's entry the clock goes to the scene's end
 * [0x0076F0E8 + slot] (last kept), the batch runs (last, end] as a catch-up
 * without its sounds, subtitles and speech (cameras, objects, actions run as
 * in the original), the scene's voice is stopped, and the next clock step
 * ends the scene as at its own end.
 *
 * How (Ghidra, plan 5.2): cutscene_batch_run 0x00055F20 runs each batched
 * event through cutscene_event_run 0x0005E240 (our wrapper drops, changes or
 * follows it); the dispatcher 0x00056050 runs the window (last, now]. A seek
 * after the speech start (the plan's first way) kept the scene's opening
 * camera track (one frame of it in play; the viewer's fade-in hid it). The
 * mid-timeline start: the clock step 0x000501F0 sets last to -1 when the
 * clock first leaves the start (while the dispatcher's flag [0x0076F0D8 +
 * slot] is 0), so the dispatcher runs (-1, now]; a dispatcher run before any
 * step runs (-100, start]. Both entries move the start, whichever comes
 * first; scene end (0x000507A0) puts the clock back to the start, so the
 * start goes back to 0 at our scene-end hook. Animations, looks and faces are
 * read by time every frame (no refire). The lip timeline goes live when the
 * voice really starts (0x0001B210: base -1.0 until the first step): the base
 * moves once it is live. A changed event (swap) has its code changed for its
 * one run and put back after.
 *
 * Latched at the scene start, per slot (an option changed mid-scene waits for
 * the next scene); ended at the row's end, on a skip, a level change or a new
 * scene. While the game skips ([0x00765601] = 2: only text runs) no timed
 * stop or early end. The cutscene viewer (to-do 99) plays through the same
 * functions, so the edits show there too (without scene 9: the viewer plays
 * one part). [CUTEDIT] one line per edit applied; RECOMP_CUTEDIT_LOG=1
 * (console log cutedit) adds the lip step and the end.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "recomp_types.h"
#include "addons.h"
#include "voice_pitch.h"
#include "dev_logs.h"
#include "cutscene_edits.h"

#define LEVEL        0x007FA1F8u
#define CUT_SEEK     0x0076F0A8u   /* -1, or the time the batch jumps to */
#define CUT_START    0x007655F4u   /* u16: the scene's start tick (0x0004F8A0) */
#define CUT_FLAG     0x0076F0D8u   /* + slot: the dispatcher has run (catch-up done) */
#define CUT_END      0x0076F0E8u   /* + slot * 4: the scene's last tick */
#define CUT_LAST     0x0076F0F0u   /* + slot * 4 */
#define CUT_NOW      0x0076F0F8u   /* + slot * 4: the scene clock (ticks, 60 a s) */
#define CUT_SECONDS  0x00847118u   /* float: the clock in seconds */
#define CUT_RATE     0x007F9F5Cu   /* float: ticks a second (60) */
#define CUT_SKIPPING 0x00765601u   /* 2 = a skip runs (only text events) */
#define VOICE_HANDLE 0x00657C54u   /* -1 = no speech */
#define VOICE_SOUND  0x00657C58u   /* the speech's sound */
#define LIP_LINE     0x00847054u   /* the lip timeline, 0 = none */
#define LIP_BASE     0x00847060u   /* float s; -1.0 until its first step */
#define LIP_TIME     0x007F9F4Cu   /* float s */
/* Held-object names (table 0x006A6108): entry 0x281 = the pan prop's model. */
#define NAME_PAN     0x006A6B0Cu
#define STR_PAN      0x005EA534u   /* "FryingPanBound" */
#define STR_BAT      0x005EA544u   /* "BaseballBat" (entry 0x280's) */

typedef struct {
    int time;
    uint8_t cat;
    uint16_t code;
    uint8_t sub;
} EvMatch;

typedef struct {
    int time;
    uint8_t cat;
    uint16_t from, to;          /* the record's code (u16 at +2) for this run */
} EvSwap;

typedef struct {
    const char *name;
    uint32_t level, scene;
    int weapon;                 /* ADDON_WEAPON_* */
    unsigned voice;             /* the scene's speech sound */
    int stop_at;                /* stop the voice at this tick (-1: no) */
    int start_at;               /* start the scene here (0: at its own start) */
    float lip_shift;            /* seconds the lip base moves (after the voice starts) */
    const EvMatch *drop;        /* events not run (in the scene, or its start's catch-up) */
    int ndrop;
    const EvSwap *swap;         /* events run with another code */
    int nswap;
    int bat_prop;               /* the pan prop's model is the bat's while it plays */
    const char *wave_cue;       /* a cut voice wave (voice_wave_edit) */
    unsigned wave_len;
    const VoiceKeep *keep;
    int nkeep;
    int follow;                 /* the next part that keeps the edit (-1: none) */
    int follow_end_at;          /* the next part ends here (-1: at its own end) */
    int jump_at, jump_to;       /* at clock jump_at the scene goes on at jump_to (0: none) */
} Edit;

/* Rack scene line 2 ("A frying pan!"): its show and hide (cat 7 = text). */
static const EvMatch s_e1_drop[] = {
    { 1148, 0x07, 2, 0 },
    { 1247, 0x07, 0, 1 },
};

/* The bat's take-out / put-away sounds for the pan's. */
static const EvSwap s_e1_swap[] = {
    { 1109, 0x06, 0xCD8, 0xCCC },
    { 1289, 0x06, 0xCD7, 0xCCB },
};

/* The uppercut's hit effect (action 0xA0 on Conker). */
static const EvMatch s_e3_drop[] = {
    { 2614, 0x05, 0xA0, 0 },
};

/* HUNG_NEW04 (cut_hungover, 4,500,414 bytes, 16-bit mono 44.1 kHz = 1,470
 * bytes a tick): from tick 2677 on. */
#define E3_START 2677
static const VoiceKeep s_e3_keep[] = { { E3_START * 1470u, 4500414u } };

/* E2 (Pan): the gargoyle's first line ("A frying pan. You stupid little
 * t**t.", voice 0-5.12 s, subtitles 5-70 and 134-254), then the uppercut's
 * camera cut (2600): the voice without [312, 2600) x 1,470 bytes. */
#define E2_CUT  312
#define E2_TO   2600
static const VoiceKeep s_e2_keep[] = { { 0u, E2_CUT * 1470u }, { E2_TO * 1470u, 4500414u } };

static const Edit s_edits[] = {
    { "E1 weapon rack", 0x2Eu, 2u, ADDON_WEAPON_BAT, 0xC58u, 1110, 0, 0.0f,
      s_e1_drop, 2, s_e1_swap, 2, 1, NULL, 0, NULL, 0, -1, -1, 0, 0 },
    { "E3 gargoyle", 0x29u, 11u, ADDON_WEAPON_BAT, 0xC5Bu, -1, E3_START, E3_START / 60.0f,
      s_e3_drop, 1, NULL, 0, 0, "hung_new04", 4500414u, s_e3_keep, 1, 9, 294, 0, 0 },
    { "E2 gargoyle (pan)", 0x29u, 11u, ADDON_WEAPON_PAN, 0xC5Bu, -1, 0, (E2_TO - E2_CUT) / 60.0f,
      NULL, 0, NULL, 0, 0, "hung_new04", 4500414u, s_e2_keep, 2, 9, 294, E2_CUT, E2_TO },
};
#define EDITS ((int)(sizeof(s_edits) / sizeof(s_edits[0])))

typedef struct {
    int row;                    /* -1: none */
    int phase;                  /* 0 its scene, 1 its next part due, 2 in it */
    uint32_t level;
    int stopped;
    int started;                /* start_at: 1 moved, 0 not yet */
    uint16_t start_was;
    uint16_t start_set;         /* the start we wrote (put back at the end), 0: none */
    int prop;                   /* bat_prop: the name entry is ours */
    int dropped, reported;      /* catch-up events dropped */
    int ending;                 /* an early end's / a jump's catch-up: from this tick on */
    int ending_to;              /* ... up to this tick (0: to the end) */
    int jumped;
    int lip_pending, lip_wait;
} Latch;

static Latch s_latch[2] = { { -1 }, { -1 } };

/* The event changed for its run (put back in cutedits_ran). */
static uint32_t s_swap_rec;
static uint16_t s_swap_code;

static int va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u;
}

static int log_on(void)
{
    return dev_log(DLOG_CUTEDIT);
}

static int skipping(void)
{
    return MEM8(CUT_SKIPPING) == 2;
}

static int is_text(uint8_t cat)
{
    return cat == 0x07 || cat == 0x0E || cat == 0x0F || cat == 0x10 || cat == 0x12;
}

static int is_sound(uint8_t cat)
{
    return cat == 0x06 || cat == 0x0A;
}

static int str_is(uint32_t va, const char *s)
{
    for (; *s; s++, va++)
        if (MEM8(va) != (uint8_t)*s)
            return 0;
    return MEM8(va) == 0;
}

static void edit_end(uint32_t slot, const char *why, int stop)
{
    Latch *L = &s_latch[slot];
    const Edit *e;

    if (L->row < 0)
        return;
    e = &s_edits[L->row];
    /* The scene end puts the clock back to the start (0x0005083A): as the
     * original's. */
    if (L->start_set && MEM16(CUT_START) == L->start_set)
        MEM16(CUT_START) = L->start_was;
    L->start_set = 0;
    /* The pan prop's own model name back (a prop made meanwhile keeps the
     * bat it loaded). */
    if (L->prop && MEM32(NAME_PAN) == STR_BAT)
        MEM32(NAME_PAN) = STR_PAN;
    /* A cut voice still running (silence past its kept part). */
    if (stop && e->wave_cue && conker_voice_stop(e->voice))
        fprintf(stderr, "[CUTEDIT] %s: voice stopped (%s)\n", e->name, why);
    if (e->wave_cue)
        voice_wave_edit_clear();
    if (log_on())
        fprintf(stderr, "[CUTEDIT] %s: ended (%s)\n", e->name, why);
    L->row = -1;
}

void cutedits_scene_start(uint32_t level, uint32_t scene, uint32_t slot, int next_part)
{
    Latch *L;
    int weapon;

    if (slot > 1)
        return;
    L = &s_latch[slot];
    if (L->row >= 0) {
        const Edit *e = &s_edits[L->row];
        if (next_part && L->phase == 1 && level == e->level && (int)scene == e->follow) {
            L->phase = 2;
            L->ending = 0;
            L->dropped = L->reported = 0;
            if (log_on())
                fprintf(stderr, "[CUTEDIT] %s: its next part, scene %u\n", e->name, scene);
            return;
        }
        edit_end(slot, "another scene started", 1);
    }
    if (!addons_update_cutscenes())
        return;
    weapon = addons_weapon();
    /* Pan's edit only with the pan in (no pan file: the bat stays, the scene too). */
    if (weapon == ADDON_WEAPON_PAN && addons_pan_on() != 1)
        weapon = ADDON_WEAPON_BOTH;
    for (int i = 0; i < EDITS; i++) {
        const Edit *e = &s_edits[i];
        char why[160] = "";
        if (e->level != level || e->scene != scene || e->weapon != weapon)
            continue;
        if (e->wave_cue && !voice_wave_edit(e->wave_cue, e->wave_len, e->keep, e->nkeep,
                                            why, (int)sizeof why)) {
            fprintf(stderr, "[CUTEDIT] %s left out: %s\n", e->name, why);
            return;
        }
        memset(L, 0, sizeof *L);
        L->row = i;
        L->level = level;
        if (e->bat_prop) {
            if (MEM32(NAME_PAN) == STR_PAN && str_is(STR_PAN, "FryingPanBound")
                && str_is(STR_BAT, "BaseballBat")) {
                MEM32(NAME_PAN) = STR_BAT;
                L->prop = 1;
                snprintf(why, sizeof why, "the pan prop shows the bat");
            } else
                snprintf(why, sizeof why, "held-object names not as expected: the pan stays");
        }
        fprintf(stderr, "[CUTEDIT] %s: on (level %02X scene %u slot %u)%s%s\n", e->name,
                level, scene, slot, why[0] ? "; " : "", why);
        return;
    }
}

void cutedits_scene_end(uint32_t slot, uint32_t next_level, int next_scene)
{
    Latch *L;
    const Edit *e;

    if (slot > 1 || (L = &s_latch[slot])->row < 0)
        return;
    e = &s_edits[L->row];
    if (L->phase == 0 && e->follow >= 0 && next_level == e->level && next_scene == e->follow) {
        if (L->start_set && MEM16(CUT_START) == L->start_set)
            MEM16(CUT_START) = L->start_was;
        L->start_set = 0;
        L->phase = 1;
        return;
    }
    edit_end(slot, "its end", 1);
}

void cutedits_skip(void)
{
    for (uint32_t s = 0; s < 2; s++)
        edit_end(s, "skipped", 1);
}

/* The scene's start moved to start_at before its first events run (the
 * clock step's or the dispatcher's entry, whichever comes first): as
 * 0x0004F8A0 does for a scene whose start event says so. Too late (the
 * dispatcher already ran, or the clock left the start): the edit is left out
 * (its voice cut would not fit). */
static void start_move(uint32_t slot)
{
    Latch *L = &s_latch[slot];
    const Edit *e;
    uint16_t start;

    if (L->row < 0 || L->phase != 0 || L->started)
        return;
    e = &s_edits[L->row];
    if (e->start_at <= 0)
        return;
    start = MEM16(CUT_START);
    if (MEM8(CUT_FLAG + slot) || (int)MEM32(CUT_NOW + slot * 4u) != (int)start) {
        L->started = -1;
        fprintf(stderr, "[CUTEDIT] %s left out: the scene had begun (clock %d, start %u)\n",
                e->name, (int)MEM32(CUT_NOW + slot * 4u), start);
        edit_end(slot, "too late", 0);
        return;
    }
    L->started = 1;
    L->start_was = start;
    L->start_set = (uint16_t)e->start_at;
    MEM16(CUT_START) = (uint16_t)e->start_at;
    MEM32(CUT_NOW + slot * 4u) = (uint32_t)e->start_at;
    MEMF(CUT_SECONDS) = (float)e->start_at / (float)(int)MEMF(CUT_RATE);
    fprintf(stderr, "[CUTEDIT] %s: scene starts at tick %d (last %d)\n", e->name, e->start_at,
            (int)MEM32(CUT_LAST + slot * 4u));
}

void cutedits_clock(uint32_t slot)
{
    if (slot <= 1)
        start_move(slot);
}

int cutedits_drop(uint32_t slot, uint32_t rec, int time)
{
    Latch *L;
    const Edit *e;
    uint8_t cat;

    if (slot > 1 || (L = &s_latch[slot])->row < 0 || !va_ok(rec))
        return 0;
    e = &s_edits[L->row];
    cat = MEM8(rec);
    /* An early end's or a jump's catch-up: its sounds, subtitles and speech. */
    if (L->ending && time >= L->ending && (!L->ending_to || time < L->ending_to)
        && (is_sound(cat) || is_text(cat) || cat == 0x0B || cat == 0x13)) {
        L->dropped++;
        return 1;
    }
    if (L->phase != 0 || L->ending)     /* past a jump: the rest as in the original */
        return 0;
    /* The catch-up of a moved start: its sounds and subtitles (as a skip). */
    if (L->started > 0 && time < e->start_at && (is_sound(cat) || is_text(cat))) {
        L->dropped++;
        return 1;
    }
    for (int i = 0; i < e->ndrop; i++) {
        const EvMatch *d = &e->drop[i];
        if (d->time == time && cat == d->cat && MEM16(rec + 2) == d->code
            && MEM8(rec + 4) == d->sub) {
            fprintf(stderr, "[CUTEDIT] %s: event dropped (tick %d, cat %02X code %u sub %u)\n",
                    e->name, time, d->cat, d->code, d->sub);
            return 1;
        }
    }
    for (int i = 0; i < e->nswap; i++) {
        const EvSwap *w = &e->swap[i];
        if (w->time == time && cat == w->cat && MEM16(rec + 2) == w->from) {
            s_swap_rec = rec;
            s_swap_code = w->from;
            MEM16(rec + 2) = w->to;
            fprintf(stderr, "[CUTEDIT] %s: event changed (tick %d, cat %02X code 0x%X -> 0x%X)\n",
                    e->name, time, cat, w->from, w->to);
            return 0;
        }
    }
    return 0;
}

void cutedits_ran(uint32_t slot, uint32_t rec, int time)
{
    Latch *L;
    const Edit *e;
    uint8_t cat;

    if (s_swap_rec && s_swap_rec == rec) {
        MEM16(rec + 2) = s_swap_code;
        s_swap_rec = 0;
    }
    if (slot > 1 || (L = &s_latch[slot])->row < 0 || L->phase != 0 || L->started <= 0
        || !va_ok(rec))
        return;
    e = &s_edits[L->row];
    cat = MEM8(rec);
    if ((cat != 0x13 && cat != 0x0B) || MEM16(rec + 2) != e->voice || MEM8(rec + 4) != 0
        || time >= e->start_at)
        return;
    /* The speech start wrote its time into the seek (to keep the clock with
     * the voice): here that would jump the clock back to it. */
    if ((int)MEM32(CUT_SEEK) == time)
        MEM32(CUT_SEEK) = 0xFFFFFFFFu;
    L->lip_pending = e->lip_shift != 0.0f;
    fprintf(stderr, "[CUTEDIT] %s: speech start (tick %d) in the catch-up, voice from %.3f s\n",
            e->name, time, (float)e->start_at / 60.0f);
}

/* A jump inside the scene (E2): the clock to jump_to (plus what it ran past
 * jump_at, as the voice did), last kept, so this frame's dispatcher runs are
 * the game's own catch-up of (last, jump_to]: cameras, animations and the
 * weapon draw in order, without the sounds and subtitles (cutedits_drop). The
 * voice's wave has the same stretch cut out (voice_wave_edit), its lip base
 * moves with it (cutedits_frame).
 * At the frame's first dispatcher call (mode 1, category 0x11 events only):
 * world_update 0x002A56C6.. runs the clock step, dispatcher mode 1, the track
 * evaluation 0x00050D10 (every track's keys at now: cameras' eye and target,
 * props), dispatcher mode 0 (the batch), then 0x00052B50 hands the active
 * track to the camera. A jump at the mode-0 call came after the evaluation:
 * the first frame showed the bridge camera's track at the old time, before
 * its first key (its side view from tick 620). The start moves too: the
 * mode-0 call's per-frame animation lookup (0x0005633A) snaps a character's
 * animation whose event is at or before the start, else blends from the pose
 * before the jump. */
static void jump_check(uint32_t slot)
{
    Latch *L = &s_latch[slot];
    const Edit *e;
    int now, to;

    if (L->row < 0 || L->phase != 0 || L->jumped || L->ending || skipping())
        return;
    e = &s_edits[L->row];
    now = (int)MEM32(CUT_NOW + slot * 4u);
    if (e->jump_at <= 0 || now < e->jump_at)
        return;
    to = e->jump_to + (now - e->jump_at);
    L->jumped = 1;
    L->start_was = MEM16(CUT_START);
    L->start_set = (uint16_t)e->jump_to;
    MEM16(CUT_START) = (uint16_t)e->jump_to;
    L->ending = (int)MEM32(CUT_LAST + slot * 4u) + 1;
    L->ending_to = e->jump_to;
    L->lip_pending = e->lip_shift != 0.0f;
    MEM32(CUT_NOW + slot * 4u) = (uint32_t)to;
    MEMF(CUT_SECONDS) = (float)to / (float)(int)MEMF(CUT_RATE);
    fprintf(stderr, "[CUTEDIT] %s: jumps at tick %d to %d (catch-up from %d)\n", e->name,
            now, to, L->ending);
}

void cutedits_dispatch(uint32_t mode, uint32_t slot)
{
    Latch *L;
    const Edit *e;
    int now, end;

    if (slot > 1)
        return;
    if (mode == 1) {
        jump_check(slot);
        return;
    }
    if (mode != 0)
        return;
    start_move(slot);
    if ((L = &s_latch[slot])->row < 0 || L->ending || skipping())
        return;
    e = &s_edits[L->row];
    now = (int)MEM32(CUT_NOW + slot * 4u);
    if (L->phase == 0 && e->stop_at >= 0 && !L->stopped && now >= e->stop_at) {
        L->stopped = 1;
        fprintf(stderr, "[CUTEDIT] %s: voice %s at tick %d\n", e->name,
                conker_voice_stop(e->voice) ? "stopped" : "not playing", now);
        return;
    }
    /* The next part ends early: the clock to its end, the rest as a catch-up. */
    if (L->phase != 2 || e->follow_end_at < 0 || now < e->follow_end_at)
        return;
    end = (int)MEM32(CUT_END + slot * 4u);
    if (end <= now)
        return;
    L->ending = (int)MEM32(CUT_LAST + slot * 4u) + 1;
    L->ending_to = 0;
    MEM32(CUT_NOW + slot * 4u) = (uint32_t)end;
    MEMF(CUT_SECONDS) = (float)end / (float)(int)MEMF(CUT_RATE);
    fprintf(stderr, "[CUTEDIT] %s: scene %d ends at tick %d (jumps to its end, %d)%s\n",
            e->name, e->follow, now, end, conker_voice_stop(e->voice) ? ", voice stopped" : "");
}

void cutedits_frame(void)
{
    for (uint32_t s = 0; s < 2; s++) {
        Latch *L = &s_latch[s];
        const Edit *e;

        if (L->row < 0)
            continue;
        e = &s_edits[L->row];
        if (MEM32(LEVEL) != L->level) {
            edit_end(s, "level change", 0);
            continue;
        }
        if (L->dropped && !L->reported && MEM8(CUT_FLAG + s)) {
            L->reported = 1;
            fprintf(stderr, "[CUTEDIT] %s: catch-up ran, %d sounds / subtitles / speech "
                    "dropped\n", e->name, L->dropped);
        }
        if (L->lip_pending) {
            if (MEM32(LIP_LINE) && (int32_t)MEM32(VOICE_HANDLE) != -1
                && MEM32(VOICE_SOUND) == e->voice) {
                float base = MEMF(LIP_BASE), was = base;
                base = base == -1.0f ? MEMF(LIP_TIME) - e->lip_shift : base - e->lip_shift;
                MEMF(LIP_BASE) = base;
                L->lip_pending = 0;
                if (log_on())
                    fprintf(stderr, "[CUTEDIT] %s: lip base %.3f -> %.3f (lip time %.3f) after "
                            "%d frames\n", e->name, was, base, MEMF(LIP_TIME), L->lip_wait);
            } else if (++L->lip_wait > 600) {
                L->lip_pending = 0;
                fprintf(stderr, "[CUTEDIT] %s: the voice never started: lip base left\n", e->name);
            }
        }
    }
}
