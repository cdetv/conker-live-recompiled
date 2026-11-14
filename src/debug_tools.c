/**
 * The --debug tools of the 0.3.0 second round (SPEC 10.3; to-dos 155, 156,
 * 158, 159): where Conker is and putting him somewhere (tp, spots), setting
 * his energy / lives / money / air and killing him, the characters near him,
 * and the jukebox's play / stop. The console's commands (console.c) and the
 * Debug menu (overlay.cpp) call these from the executor; what changes the
 * game is asked for here and done on the game thread by debug_tools_frame
 * (from recomp_manual.c's cheats_frame, the end of a frame).
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "debug_tools.h"

#define CHARS      0x0084A5F8u      /* the character records, 0x6D0 each */
#define NCHARS   0x81u
#define CHAR_SIZE  0x6D0u
#define LEVEL      0x007FA1F8u
#define MULTI      0x007819D5u      /* nonzero in multiplayer */
#define TYPE_NAMES 0x006A58F0u      /* character type names, 0x205 of them */
#define STAT_LIVES (0x008761F0u + 8u)    /* s8 */
#define STAT_CASH  (0x008761F0u + 0xCu)  /* u32 */
#define SAVE_CASH  0x0084B24Cu           /* the copy kept over level loads */
#define C_ENERGY   0x3AEu           /* u16, full = 6 */
#define C_AIR      0x1B8u           /* u16: 60 Hz ticks under water */
#define C_FALL     0x3B8u           /* the height a fall began at */
#define AIR_TICKS  4320             /* 72 s, then he starts to drown */
#define SOUNDS     0x0065E064u      /* the sound table: 0x30 each, +0 name */
#define SOUND_N    3545

static int va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u;
}

/* The main menu, not its level warped into (to-do 165): level 0x1D with the
 * front end running (its state 0x00776574 is 0 once a game runs, also in
 * New Game's scenes in the bar and in a warp to "MainMenu"). */
int conker_OnMainMenu(void)
{
    return MEM32(LEVEL) == 0x1Du && MEM32(0x00776574u) != 0;
}

static uint32_t player(void)
{
    uint32_t c = MEM32(CHARS);
    if (!va_ok(c) || conker_OnMainMenu())
        return 0;
    return c;
}

int conker_Level(void)
{
    return (int)(MEM32(LEVEL) & 0xFFu);
}

int conker_PlayerPos(float *x, float *y, float *z)
{
    uint32_t c = player();
    if (!c)
        return 0;
    *x = MEMF(c + 0x78u);
    *y = MEMF(c + 0x7Cu);
    *z = MEMF(c + 0x80u);
    return 1;
}

/* ---- tp: put him there over a few frames (his next step undid a single
 * write), speeds cleared, his fall starting there (no fall damage). ---- */
#define TP_FRAMES 10
static volatile LONG s_tp_ask;
static float s_tp[3];
static int s_tp_left;

void conker_Teleport(float x, float y, float z)
{
    s_tp[0] = x;
    s_tp[1] = y;
    s_tp[2] = z;
    InterlockedExchange(&s_tp_ask, 1);
}

static void tp_frame(void)
{
    uint32_t c = player();

    if (InterlockedExchange(&s_tp_ask, 0)) {
        s_tp_left = TP_FRAMES;
        fprintf(stderr, "[TP] to %.0f %.0f %.0f\n", s_tp[0], s_tp[1], s_tp[2]);
    }
    if (s_tp_left <= 0 || !c)
        return;
    s_tp_left--;
    MEMF(c + 0x78u) = MEMF(c + 0x30u) = s_tp[0];
    MEMF(c + 0x7Cu) = MEMF(c + 0x34u) = s_tp[1];
    MEMF(c + 0x80u) = MEMF(c + 0x38u) = s_tp[2];
    MEMF(c + 0xD0u) = 0.0f;             /* vertical speed */
    MEMF(c + 0xDCu) = 0.0f;
    MEMF(c + 0x110u) = 0.0f;            /* speed */
    MEMF(c + C_FALL) = s_tp[1];
}

/* ---- set / kill ---- */
static volatile LONG s_set_what = -1, s_set_value, s_kill_ask;

int conker_SetValue(int what, int value)
{
    if (!player() || MEM8(MULTI))
        return 0;
    InterlockedExchange(&s_set_value, value);
    InterlockedExchange(&s_set_what, what);
    return 1;
}

int conker_Kill(void)
{
    if (!player() || MEM8(MULTI))
        return 0;
    InterlockedExchange(&s_kill_ask, 1);
    return 1;
}

extern void sub_002CCD90(void);

static void set_frame(void)
{
    LONG what = InterlockedExchange(&s_set_what, -1);
    int v = (int)s_set_value;
    uint32_t c = player();

    if (what >= 0 && c) {
        switch (what) {
        case DBG_SET_ENERGY: MEM16(c + C_ENERGY) = (uint16_t)v; break;
        case DBG_SET_LIVES:  MEM8(STAT_LIVES) = (uint8_t)v; break;
        case DBG_SET_CASH:   MEM32(STAT_CASH) = (uint32_t)v; MEM32(SAVE_CASH) = (uint32_t)v; break;
        case DBG_SET_OXYGEN: MEM16(c + C_AIR) = (uint16_t)((72 - v) * 60); break;
        }
        fprintf(stderr, "[SET] %ld = %d\n", what, v);
    }
    if (InterlockedExchange(&s_kill_ask, 0) && c) {
        /* As the game's own instant kills do: player_died (eax = the
         * character index) takes a life and reloads at the checkpoint. */
        uint32_t r_eax = g_eax, r_esp = g_esp;
        g_eax = 0;
        g_esp -= 4;
        MEM32(g_esp) = 0;               /* return address, popped by the callee */
        sub_002CCD90();
        g_eax = r_eax;
        g_esp = r_esp;
        fprintf(stderr, "[SET] kill: player_died\n");
    }
}

/* ---- peek / poke (to-do 172) ---- */
int xbox_GuestReadable(uint32_t va, uint32_t n);

int conker_Peek(uint32_t va, void *out, unsigned n)
{
    if (!n || !xbox_GuestReadable(va, n))
        return 0;
    memcpy(out, (const void *)XBOX_PTR(va), n);
    return 1;
}

int conker_Poke(uint32_t va, const void *in, unsigned n)
{
    if (!n || n > 8 || !xbox_GuestReadable(va, n))
        return 0;
    memcpy((void *)XBOX_PTR(va), in, n);
    return 1;
}

/* ---- story flags by name (to-do 170) ----
 * The game's 238 tasks (task table 0x0059E9A0, also the warp's checkpoints): a
 * task's name is at [0x0074D00C + id * 8], done = its progress bit
 * [[0x0084B290] + id / 8] and its state byte 0x00801910 + id = 3 (as
 * 0x0035DD50 / 0x0035DE10 mark one; 0 is the wiped state, 0x0035DF40). The
 * level's tasks are loaded at its load, so a change shows from the next load.
 * Asked from any thread, done on the game thread. */
#define PROGRESS    0x0084B290u
#define TASK_STATE  0x00801910u
#define TASK_NAMES  0x0074D00Cu
#define TASKS_N     238

static volatile LONG s_flag_ask[TASKS_N];  /* 0 none, 1 off, 2 on */
static volatile LONG s_flag_any;

int conker_FlagCount(void)
{
    return TASKS_N;
}

const char *conker_FlagName(int id)
{
    uint32_t p;

    if (id < 0 || id >= TASKS_N)
        return NULL;
    p = MEM32(TASK_NAMES + (uint32_t)id * 8u);
    return va_ok(p) ? (const char *)XBOX_PTR(p) : NULL;
}

int conker_Flag(int id, int *state)
{
    uint32_t bits = MEM32(PROGRESS);

    if (id < 0 || id >= TASKS_N || !va_ok(bits) || MEM8(MULTI))
        return -1;
    if (state)
        *state = MEM8(TASK_STATE + (uint32_t)id);
    return (MEM8(bits + (uint32_t)id / 8u) >> (id & 7)) & 1;
}

int conker_FlagSet(int id, int on)
{
    if (conker_Flag(id, NULL) < 0)
        return 0;
    InterlockedExchange(&s_flag_ask[id], on ? 2 : 1);
    InterlockedExchange(&s_flag_any, 1);
    return 1;
}

static void flag_frame(void)
{
    uint32_t bits = MEM32(PROGRESS);

    if (!InterlockedExchange(&s_flag_any, 0))
        return;
    for (int id = 0; id < TASKS_N; id++) {
        LONG ask = InterlockedExchange(&s_flag_ask[id], 0);
        uint32_t b = bits + (uint32_t)id / 8u;
        if (!ask || !va_ok(bits))
            continue;
        if (ask == 2) {
            MEM8(b) |= (uint8_t)(1u << (id & 7));
            MEM8(TASK_STATE + (uint32_t)id) = 3;
        } else {
            MEM8(b) &= (uint8_t)~(1u << (id & 7));
            MEM8(TASK_STATE + (uint32_t)id) = 0;
        }
        fprintf(stderr, "[FLAG] %d %s %s\n", id, conker_FlagName(id) ? conker_FlagName(id) : "?",
                ask == 2 ? "on" : "off");
    }
}

/* ---- characters ---- */
static uint32_t char_at(int n)
{
    uint32_t base = MEM32(CHARS), c;

    if (!va_ok(base) || n < 0 || (uint32_t)n >= NCHARS)
        return 0;
    c = base + (uint32_t)n * CHAR_SIZE;
    return MEM8(c + 0x64u) ? c : 0;
}

int conker_CharPos(int n, float *x, float *y, float *z)
{
    uint32_t c = n > 0 ? char_at(n) : 0;

    if (!c)
        return 0;
    *x = MEMF(c + 0x78u);
    *y = MEMF(c + 0x7Cu);
    *z = MEMF(c + 0x80u);
    return 1;
}

static int char_cmp(const void *a, const void *b)
{
    float d = ((const ConkerChar *)a)->dist - ((const ConkerChar *)b)->dist;
    return d < 0.0f ? -1 : d > 0.0f;
}

int conker_CharsNear(ConkerChar *out, int max)
{
    static ConkerChar all[NCHARS];
    float px, py, pz;
    int n = 0;

    if (!conker_PlayerPos(&px, &py, &pz))
        return -1;
    for (int i = 1; i < (int)NCHARS; i++) {
        uint32_t c = char_at(i), type, name;
        float dx, dy, dz;
        if (!c)
            continue;
        type = MEM32(c + 0x68u);
        name = type < 0x205u ? MEM32(TYPE_NAMES + type * 4u) : 0;
        all[n].index = i;
        if (va_ok(name))
            strncpy_s(all[n].name, sizeof all[n].name,
                      (const char *)XBOX_PTR(name), _TRUNCATE);
        else
            snprintf(all[n].name, sizeof all[n].name, "type %u", type);
        dx = MEMF(c + 0x78u) - px;
        dy = MEMF(c + 0x7Cu) - py;
        dz = MEMF(c + 0x80u) - pz;
        all[n].dist = sqrtf(dx * dx + dy * dy + dz * dz);
        all[n].energy = (int)MEM16(c + C_ENERGY);
        n++;
    }
    qsort(all, (size_t)n, sizeof all[0], char_cmp);
    if (n > max)
        n = max;
    memcpy(out, all, (size_t)n * sizeof all[0]);
    return n;
}

/* ---- the jukebox (to-do 159) ----
 * The game's sound table (0x0065E064, 0x30 bytes a sound: +0 its name, +0xC
 * its number), read while the game runs: no names in our files. Played as the
 * console's cue does (0x00022D90: ecx = the number, eax = edx = -1; stack the
 * manager [[0x00847024]+0x40], 4, 0, 0, -1, -1, 0, 0; ret 32), which answers
 * the emitter's index in the manager's container ([manager+4], 128 slots);
 * stopped through the container's RemoveEmitter 0x0002E660 (eax = index;
 * stack container, 0, 0; ret 12), only while that slot still holds the
 * emitter we started (a finished sound's slot is used again). */
int conker_SoundCount(void)
{
    return SOUND_N;
}

const char *conker_SoundName(int i, int *number)
{
    uint32_t e = SOUNDS + (uint32_t)i * 0x30u, name;

    if (i < 0 || i >= SOUND_N)
        return NULL;
    name = MEM32(e);
    if (number)
        *number = (int)MEM32(e + 0xCu);
    return va_ok(name) ? (const char *)XBOX_PTR(name) : NULL;
}

static volatile LONG s_jb_play = -1, s_jb_stop;
static int s_jb_index = -1;
static uint32_t s_jb_emitter;

void conker_JukeboxPlay(int number)
{
    InterlockedExchange(&s_jb_play, number);
}

void conker_JukeboxStop(void)
{
    InterlockedExchange(&s_jb_stop, 1);
}

extern void sub_00022D90(void);
extern void sub_0002E660(void);

static uint32_t jb_manager(void)
{
    uint32_t snd = MEM32(0x00847024u);
    return va_ok(snd) && va_ok(MEM32(snd + 0x40u)) ? MEM32(snd + 0x40u) : 0;
}

static uint32_t jb_slot(uint32_t man, int index)
{
    uint32_t cont = MEM32(man + 4u);
    if (!va_ok(cont) || !va_ok(MEM32(cont)) || index < 0 || index >= 128)
        return 0;
    return MEM32(cont) + (uint32_t)index * 4u;
}

static void jukebox_frame(void)
{
    LONG play = InterlockedExchange(&s_jb_play, -1);
    uint32_t man = jb_manager(), slot;
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_esp = g_esp;

    if (!man)
        return;
    if ((InterlockedExchange(&s_jb_stop, 0) || play >= 0) && s_jb_index >= 0) {
        slot = jb_slot(man, s_jb_index);
        if (slot && MEM32(slot) == s_jb_emitter && s_jb_emitter) {
            g_esp -= 4; MEM32(g_esp) = 0;
            g_esp -= 4; MEM32(g_esp) = 0;
            g_esp -= 4; MEM32(g_esp) = MEM32(man + 4u);
            g_esp -= 4; MEM32(g_esp) = 0;   /* return address */
            g_eax = (uint32_t)s_jb_index;
            sub_0002E660();
            fprintf(stderr, "[JUKEBOX] stopped (emitter %d)\n", s_jb_index);
        }
        s_jb_index = -1;
        s_jb_emitter = 0;
        g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_esp = r_esp;
    }
    if (play >= 0) {
        static const uint32_t after[6] = { 4, 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
        g_esp -= 4; MEM32(g_esp) = 0;
        for (int i = 5; i >= 0; i--) {
            g_esp -= 4;
            MEM32(g_esp) = after[i];
        }
        g_esp -= 4; MEM32(g_esp) = man;
        g_esp -= 4; MEM32(g_esp) = 0;       /* return address */
        g_ecx = (uint32_t)play;
        g_eax = 0xFFFFFFFFu;
        g_edx = 0xFFFFFFFFu;
        sub_00022D90();
        s_jb_index = (int32_t)g_eax;
        slot = s_jb_index >= 0 ? jb_slot(man, s_jb_index) : 0;
        s_jb_emitter = slot ? MEM32(slot) : 0;
        fprintf(stderr, "[JUKEBOX] sound 0x%lX started (emitter %d)\n", play, s_jb_index);
        g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_esp = r_esp;
    }
}

/* ---- the jukebox's tunes (to-do 159) ----
 * The music manager plays sequences ("tunes", AD_SEQ_*), not sounds: the tune
 * table 0x00658FC8 (0x2C a tune, 0x119 of them, 0 = none: +4 its cue, -1 =
 * none, +0x10 its name) is the game's own lookup (ad_musiclookup_getTuneInfo
 * 0x0001D490). A tune starts through CAudioMusicManager::OldPlaySequence
 * 0x0001FC70 (eax = the new tune; stack the manager [[0x00847024]+0x3C], the
 * old tune, the method; ret 12), as a cutscene's "start tune" event does
 * (0x0005CD50). Methods (names at 0x0065C7B4): 0 immediate, 3 fade in new
 * only, 4 crossfade and stop old, 5 fade out old before new, 6 crossfade and
 * pause old. A tune whose music the level hasn't loaded fails at once
 * (audioSoundBankGetCueEx: XACT error 317), so the old music is stopped only
 * once ours plays: ours starts with old = 0, immediate; the manager's playing
 * list ([mus+4]: nodes +0 tune, +0xC next; tune +0 its number, +0x14 state,
 * 1 = playing) says when; then the tune it replaces (the first playing one
 * that isn't ambience, AD_SEQ_ambi_*) stops (OldStopSequence 0x0001FEB0: eax
 * = method, stack the manager, the tune; ret 8). Not playing within
 * TUNE_WAIT_MS = can't play here: ours stopped, the music left as it was.
 * Stop crossfades the replaced tune back (method 4, old = ours). A level
 * change forgets it all (the load stops every tune). */
#define TUNES   0x00658FC8u
#define TUNE_N  0x119
#define TUNE_WAIT_MS 4000u

int conker_TuneCount(void)
{
    return TUNE_N;
}

const char *conker_TuneName(int i)
{
    uint32_t e = TUNES + (uint32_t)i * 0x2Cu, name;
    if (i < 1 || i >= TUNE_N || (int32_t)MEM32(e + 4u) < 0)
        return NULL;
    name = MEM32(e + 0x10u);
    return va_ok(name) ? (const char *)XBOX_PTR(name) : NULL;
}

static volatile LONG s_tune_play = -1, s_tune_stop;
static volatile LONG s_tune_ours;       /* the jukebox's tune, 0 = none */
static volatile LONG s_tune_state;      /* TUNE_* (debug_tools.h) */
static volatile LONG s_tune_failed;     /* the last tune that couldn't play */
static volatile LONG s_tune_loading;    /* the tune whose music bank loads, 0 = none */
static int s_tune_was;                  /* the music it replaced, 0 = none */
static int s_tune_prev;                 /* the jukebox's tune before (to stop) */
static uint32_t s_tune_level;
static ULONGLONG s_tune_until;
static uint8_t s_tune_bad[TUNE_N];      /* didn't play in s_tune_bad_level */
static volatile LONG s_tune_bad_level = -1;

int conker_TuneBad(int i)
{
    return i > 0 && i < TUNE_N && s_tune_bad_level == (LONG)(MEM32(LEVEL) & 0xFFu)
           && s_tune_bad[i];
}

void conker_TunePlay(int number)
{
    InterlockedExchange(&s_tune_play, number);
}

void conker_TuneStop(void)
{
    InterlockedExchange(&s_tune_stop, 1);
}

int conker_TunePlaying(void)
{
    return (int)s_tune_ours;
}

int conker_TuneStatus(int *failed)
{
    if (failed)
        *failed = (int)s_tune_failed;
    return s_tune_loading ? TUNE_LOADING : (int)s_tune_state;
}

extern void sub_0001FC70(void);
extern void sub_0001FEB0(void);

static uint32_t tune_manager(void)
{
    uint32_t snd = MEM32(0x00847024u);
    return va_ok(snd) && va_ok(MEM32(snd + 0x3Cu)) ? MEM32(snd + 0x3Cu) : 0;
}

/* A tune's state in the playing list (+0x14; 1 = playing), -1 = not there. */
static int tune_state(uint32_t mus, int id)
{
    uint32_t head = MEM32(mus + 4u), node;
    int n = 0;
    if (!va_ok(head))
        return -1;
    for (node = MEM32(head + 0xCu); va_ok(node) && node != head && n < 16;
         node = MEM32(node + 0xCu), n++) {
        uint32_t t = MEM32(node);
        if (va_ok(t) && (int)MEM32(t) == id)
            return (int)MEM8(t + 0x14u);
    }
    return -1;
}

/* The playing music (not ambience) that isn't `not`, 0 = none. */
static int tune_music_now(uint32_t mus, int not)
{
    uint32_t head = MEM32(mus + 4u), node;
    int n = 0;
    if (!va_ok(head))
        return 0;
    for (node = MEM32(head + 0xCu); va_ok(node) && node != head && n < 16;
         node = MEM32(node + 0xCu), n++) {
        uint32_t t = MEM32(node);
        int id = va_ok(t) ? (int)MEM32(t) : 0;
        const char *name = conker_TuneName(id);
        if (name && id != not && _strnicmp(name, "AD_SEQ_ambi", 11))
            return id;
    }
    return 0;
}

static void tune_call_play(uint32_t mus, int tune, int old, uint32_t method)
{
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_esp = g_esp;
    g_esp -= 4; MEM32(g_esp) = method;
    g_esp -= 4; MEM32(g_esp) = (uint32_t)old;
    g_esp -= 4; MEM32(g_esp) = mus;
    g_esp -= 4; MEM32(g_esp) = 0;       /* return address */
    g_eax = (uint32_t)tune;
    sub_0001FC70();
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_esp = r_esp;
}

static void tune_call_stop(uint32_t mus, int tune)
{
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_esp = g_esp;
    g_esp -= 4; MEM32(g_esp) = (uint32_t)tune;
    g_esp -= 4; MEM32(g_esp) = mus;
    g_esp -= 4; MEM32(g_esp) = 0;       /* return address */
    g_eax = 0;                          /* immediate */
    sub_0001FEB0();
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_esp = r_esp;
}

/* ---- the tunes' music banks (to-do 189) ----
 * A tune's cue lives in a sound bank (tune +0, the manager's array [mgr+0])
 * at the XACT index CUE_INDEX[cue]; its waves in wave banks. The chapters'
 * main music banks stay loaded; the looping ones (m_*_lp, am_*_lp) and the
 * multiplayer ones (multi_*) only while a level registers them, so another
 * level's tune can't prepare (XACT error 317). Wave bank objects ([mgr+4], 76):
 * +4 path, +0x14 loaded for good, +0x15 primed, +0x18 register count, +0x20
 * wanted, +0x21 to free, +0x22 no priming. A level's music block (0x00017A40)
 * raises a count and sets "wanted"; audio_update (0x00017CF0) registers wanted
 * banks (0x00017C30, manager +0xC = banks pending) and frees the ones
 * 0x00017B40 marked (count 0, primed; manager +8), waiting while a cue still
 * plays on one. The jukebox does the same for one bank: count + 1 to load it,
 * - 1 and 0x00017B40 to let it go; a level load's "every count - 1" frees it
 * anyway. Which bank a tune needs: s_tune_banks (the scan below found it). */
#define SNDMGR     0x00847024u
#define WAVEBANKS  76
#define BANK_PRIME_MS 10000u

extern void sub_00017B40(void);     /* marks count-0 banks to free: ecx = mgr */
extern void sub_00402A70(void);     /* unregisters a wave bank: eax = &handle */

static uint32_t snd_manager(void)
{
    uint32_t m = MEM32(SNDMGR);
    return va_ok(m) && va_ok(MEM32(m)) && va_ok(MEM32(m + 4u)) ? m : 0;
}

static uint32_t wave_bank(uint32_t m, int i)
{
    uint32_t b = (i >= 0 && i < WAVEBANKS) ? MEM32(MEM32(m + 4u) + (uint32_t)i * 4u) : 0;
    return va_ok(b) ? b : 0;
}

/* The bank's file name without folder and ".xwb" ("m_batstower_lp"). */
static const char *bank_name(uint32_t b, char *out, size_t n)
{
    uint32_t p = MEM32(b + 4u);
    const char *s = va_ok(p) ? (const char *)XBOX_PTR(p) : "", *slash = strrchr(s, '\\');
    size_t len;
    if (slash)
        s = slash + 1;
    len = strcspn(s, ".");
    if (len >= n)
        len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

static int bank_find(uint32_t m, const char *name)
{
    char nm[48];
    for (int i = 0; i < WAVEBANKS; i++) {
        uint32_t b = wave_bank(m, i);
        if (b && !_stricmp(bank_name(b, nm, sizeof nm), name))
            return i;
    }
    return -1;
}

/* One more register count (0x00017A40's step for one bank). */
static void bank_add(uint32_t m, uint32_t b)
{
    MEM32(b + 0x18u) += 1u;
    if (!MEM8(b + 0x15u) && !MEM8(b + 0x22u) && !MEM8(b + 0x20u)) {
        MEM8(b + 0x20u) = 1;
        MEM32(m + 0xCu) += 1u;
    }
}

/* One count less; at 0 the next audio updates free it (once no cue plays on it). */
static void bank_release(uint32_t m, uint32_t b)
{
    uint32_t r_ecx = g_ecx;
    if ((int32_t)MEM32(b + 0x18u) > 0)
        MEM32(b + 0x18u) -= 1u;
    if (MEM32(b + 0x18u))
        return;
    if (MEM8(b + 0x20u) && !MEM8(b + 0x15u)) {     /* not registered yet: not wanted */
        MEM8(b + 0x20u) = 0;
        if ((int32_t)MEM32(m + 0xCu) > 0)
            MEM32(m + 0xCu) -= 1u;
    }
    g_ecx = m;
    sub_00017B40();
    g_ecx = r_ecx;
}

/* A bank at count 0 unloaded now (as 0x00017EF0 does, without its "nothing
 * of this kind plays" wait: the music and the level's sounds always play;
 * XACT itself refuses while a cue uses the bank). 1 = gone. */
static int bank_unregister(uint32_t b)
{
    uint32_t r_eax = g_eax;
    if (!MEM8(b + 0x15u) || MEM8(b + 0x14u) || MEM32(b + 0x18u))
        return !MEM8(b + 0x15u);
    if (MEM32(b + 0x1Cu)) {
        g_eax = b + 0x1Cu;
        sub_00402A70();
        g_eax = r_eax;
    }
    if (MEM32(b + 0x1Cu))
        return 0;
    MEM8(b + 0x15u) = 0;
    MEM8(b + 0x21u) = 0;
    return 1;
}

/* Every tune in the playing list stopped (the scan's start). */
static void tune_stop_all(void)
{
    uint32_t mus = tune_manager(), head = mus ? MEM32(mus + 4u) : 0, node;
    int ids[16], n = 0;
    if (!va_ok(head))
        return;
    for (node = MEM32(head + 0xCu); va_ok(node) && node != head && n < 16; node = MEM32(node + 0xCu)) {
        uint32_t t = MEM32(node);
        if (va_ok(t) && conker_TuneName((int)MEM32(t)))
            ids[n++] = (int)MEM32(t);
    }
    for (int i = 0; i < n; i++)
        tune_call_stop(mus, ids[i]);
}

/* ---- the one-time scan (RECOMP_TUNE_SCAN=1; to-do 189) ----
 * In each level, 8 s after it loads: every tune stopped, then each tune tried
 * as it is (started; a tune whose music isn't loaded never enters the playing
 * list, one that can play does within a few frames and is stopped at once).
 * Then each bank the level hasn't registered (on the disc, not loaded for
 * good, not one the game copies to the hard disk first: +0x2C = 1), in turn:
 * loaded, primed, 2 s for XACT to link it, the tunes that couldn't play tried
 * again, unloaded (bank_unregister: the game's own free waits while any tune
 * of the bank's kind plays). One extra bank at a time: with many loaded at
 * once nothing plays (XACT's limits). A first version prepared each tune's cue
 * itself (CAudioCue::GetCue on a stand-in, then audioSoundBankFreeCue): XACT
 * never destroyed those cues and after a few hundred some tunes couldn't play
 * at all. At the end the tunes that played at the start are tried again (a
 * check line for any that no longer do), then one [TUNESCAN] line per tune
 * ("plays here" = a bank the level has or one loaded for good); the table
 * s_tune_banks is made from two levels' lists. */
enum { SC_OFF, SC_WAIT, SC_BASE, SC_NEXT, SC_PRIME, SC_TEST, SC_FREE, SC_CHECK, SC_DONE };
#define SC_SETTLE_MS 2000u
#define SC_ENTER_FRAMES 6           /* not in the playing list by then: can't play */
#define SC_PLAY_FRAMES 60           /* in it but not playing by then: can't either */
static int s_sc = -1, s_sc_bank = -1, s_sc_cur, s_sc_trying, s_sc_frames, s_sc_n0, s_sc_n1;
static uint32_t s_sc_level;
static ULONGLONG s_sc_t, s_sc_settle_at;
static int8_t s_sc_base[TUNE_N];            /* -1 no cue, 0 can't play here, 1 can */
static int8_t s_sc_found[TUNE_N];           /* the bank it plays with, -1 none */
static int s_sc_enter[SC_ENTER_FRAMES + 1]; /* frames to enter the list, played ones */

static int scan_candidate(uint32_t b)
{
    uint32_t p = MEM32(b + 4u);
    if (!va_ok(p) || !strstr((const char *)XBOX_PTR(p), "dvddata"))
        return 0;                   /* the Z: (hard disk) sound banks: sounds, not tunes */
    return !MEM8(b + 0x14u) && !MEM8(b + 0x22u) && MEM32(b + 0x2Cu) != 1u
           && !MEM32(b + 0x18u) && !MEM8(b + 0x15u);
}

/* Tune s_sc_cur tried, a frame at a time: -1 still trying, 1 played, 0 not. */
static int scan_try(uint32_t mus)
{
    int st;
    if (!s_sc_trying) {
        tune_call_play(mus, s_sc_cur, 0, 0);
        s_sc_trying = s_sc_cur;
        s_sc_frames = 0;
        return -1;
    }
    st = tune_state(mus, s_sc_trying);
    s_sc_frames++;
    if (st == 1) {
        tune_call_stop(mus, s_sc_trying);
        s_sc_trying = 0;
        return 1;
    }
    if (st >= 0) {
        if (s_sc_frames <= SC_ENTER_FRAMES)
            s_sc_enter[s_sc_frames]++;
        if (s_sc_frames < SC_PLAY_FRAMES)
            return -1;
        tune_call_stop(mus, s_sc_trying);
        fprintf(stderr, "[TUNESCAN] tune %d in the list, not playing after %d frames\n",
                s_sc_trying, s_sc_frames);
        s_sc_trying = 0;
        return 0;
    }
    if (s_sc_frames < SC_ENTER_FRAMES)
        return -1;
    s_sc_trying = 0;
    return 0;
}

/* The next tune to try from s_sc_cur on: all (base < 0 = untried) or those
 * that can't play yet; TUNE_N = none left. */
static int scan_next_tune(int from, int all)
{
    for (int i = from; i < TUNE_N; i++)
        if (conker_TuneName(i) && (all ? 1 : (s_sc_base[i] == 0 && s_sc_found[i] < 0)))
            return i;
    return TUNE_N;
}

static void scan_report(uint32_t m)
{
    char nm[48];
    int i, n[3] = { 0 };
    for (i = 1; i < TUNE_N; i++) {
        if (s_sc_base[i] < 0)
            continue;
        if (s_sc_found[i] >= 0)
            n[1]++;
        else
            n[s_sc_base[i] ? 0 : 2]++;
        fprintf(stderr, "[TUNESCAN] tune %d %s: %s\n", i, conker_TuneName(i),
                s_sc_found[i] >= 0 ? bank_name(wave_bank(m, s_sc_found[i]), nm, sizeof nm)
                : s_sc_base[i] ? "plays here" : "not found");
    }
    fprintf(stderr, "[TUNESCAN] frames to enter the list (played ones):");
    for (i = 1; i <= SC_ENTER_FRAMES; i++)
        fprintf(stderr, " %d:%d", i, s_sc_enter[i]);
    fprintf(stderr, "\n[TUNESCAN] level 0x%02X done: %d play here, %d need a bank, %d not found\n",
            s_sc_level & 0xFFu, n[0], n[1], n[2]);
}

static void scan_frame(void)
{
    uint32_t m = snd_manager(), mus = tune_manager(), level = MEM32(LEVEL), b;
    char nm[48];
    int i, r;
    ULONGLONG now = GetTickCount64();
    if (s_sc < 0) {
        const char *e = getenv("RECOMP_TUNE_SCAN");
        s_sc = (e && *e == '1') ? SC_WAIT : SC_OFF;
        s_sc_t = now;
        s_sc_level = level;
    }
    if (s_sc == SC_OFF || !m || !mus)
        return;
    if (s_sc == SC_DONE || s_sc == SC_WAIT) {   /* each level gets its own */
        if (level != s_sc_level || level == 0x1Du) {
            s_sc = SC_WAIT;
            s_sc_level = level;
            s_sc_t = now;
            return;
        }
        if (s_sc == SC_DONE || now - s_sc_t < 8000u)
            return;
        tune_stop_all();
        for (i = 1; i < TUNE_N; i++) {
            s_sc_base[i] = -1;
            s_sc_found[i] = -1;
        }
        memset(s_sc_enter, 0, sizeof s_sc_enter);
        s_sc_trying = 0;
        s_sc_cur = scan_next_tune(1, 1);
        s_sc_n0 = s_sc_n1 = 0;
        s_sc = SC_BASE;
        return;
    }
    if (level != s_sc_level) {
        fprintf(stderr, "[TUNESCAN] level changed: stopped\n");
        s_sc = SC_WAIT;
        s_sc_level = level;
        s_sc_t = now;
        return;
    }
    if (s_sc == SC_BASE) {                      /* what plays as it is */
        if (s_sc_cur < TUNE_N) {
            if ((r = scan_try(mus)) < 0)
                return;
            s_sc_base[s_sc_cur] = (int8_t)r;
            s_sc_n0 += !r;
            s_sc_n1 += r;
            s_sc_cur = scan_next_tune(s_sc_cur + 1, 1);
            return;
        }
        fprintf(stderr, "[TUNESCAN] level 0x%02X: %d tunes play, %d don't\n",
                level & 0xFFu, s_sc_n1, s_sc_n0);
        s_sc_bank = -1;
        s_sc = SC_NEXT;
        return;
    }
    if (s_sc == SC_NEXT) {
        for (i = s_sc_bank + 1; i < WAVEBANKS; i++)
            if ((b = wave_bank(m, i)) && scan_candidate(b))
                break;
        if (i >= WAVEBANKS || scan_next_tune(1, 0) >= TUNE_N) {
            s_sc_cur = scan_next_tune(1, 1);
            s_sc = SC_CHECK;
            return;
        }
        s_sc_bank = i;
        bank_add(m, b);
        s_sc_t = now;
        s_sc_settle_at = 0;
        s_sc = SC_PRIME;
        return;
    }
    if (s_sc == SC_CHECK) {                     /* the same as at the start? */
        while (s_sc_cur < TUNE_N && s_sc_base[s_sc_cur] != 1)
            s_sc_cur = scan_next_tune(s_sc_cur + 1, 1);
        if (s_sc_cur < TUNE_N) {
            if ((r = scan_try(mus)) < 0)
                return;
            if (!r)
                fprintf(stderr, "[TUNESCAN] check: tune %d no longer plays\n", s_sc_cur);
            s_sc_cur = scan_next_tune(s_sc_cur + 1, 1);
            return;
        }
        scan_report(m);
        s_sc = SC_DONE;
        return;
    }
    b = wave_bank(m, s_sc_bank);
    if (s_sc == SC_PRIME) {
        if (!MEM8(b + 0x15u)) {
            if (now - s_sc_t > BANK_PRIME_MS) {
                fprintf(stderr, "[TUNESCAN] bank %s didn't prime\n", bank_name(b, nm, sizeof nm));
                bank_release(m, b);
                s_sc_t = now;
                s_sc = SC_FREE;
            }
            return;
        }
        if (!s_sc_settle_at) {                  /* XACT links it a moment later */
            s_sc_settle_at = now;
            return;
        }
        if (now - s_sc_settle_at < SC_SETTLE_MS)
            return;
        s_sc_cur = scan_next_tune(1, 0);
        s_sc_n1 = 0;
        s_sc = SC_TEST;
        return;
    }
    if (s_sc == SC_TEST) {
        if (s_sc_cur < TUNE_N) {
            if ((r = scan_try(mus)) < 0)
                return;
            if (r) {
                s_sc_found[s_sc_cur] = (int8_t)s_sc_bank;
                s_sc_n1++;
            }
            s_sc_cur = scan_next_tune(s_sc_cur + 1, 0);
            return;
        }
        fprintf(stderr, "[TUNESCAN] bank %d %s: primed in %llu ms, %d tunes play with it\n",
                s_sc_bank, bank_name(b, nm, sizeof nm), s_sc_settle_at - s_sc_t, s_sc_n1);
        bank_release(m, b);
        s_sc_t = now;
        s_sc = SC_FREE;
        return;
    }
    if (s_sc == SC_FREE) {
        if (MEM8(b + 0x15u) && !bank_unregister(b) && now - s_sc_t < 10000u)
            return;
        if (MEM8(b + 0x15u))
            fprintf(stderr, "[TUNESCAN] bank %s still loaded after 10 s\n", bank_name(b, nm, sizeof nm));
        s_sc = SC_NEXT;
    }
}

/* ---- the jukebox loads a tune's music (to-do 189) ----
 * A tune in a bank this level hasn't loaded: the bank gets one more register
 * count (bank_add), and once it is primed and 2 s on (XACT links it a moment
 * later) the tune starts as usual. The bank goes again (bank_release, then
 * bank_unregister once the tune that used it has left the playing list) when
 * the jukebox's tune stops, fails or is replaced by one that doesn't need it.
 * A level change forgets it all: the load's "every count - 1" and its frees
 * take care of the counts. One extra bank at a time (XACT's limits: with
 * many loaded at once nothing plays). The tune -> bank table below comes from
 * the RECOMP_TUNE_SCAN runs in the field and in Harbour (It's War), merged
 * by _work\j189\merge.py: 142 tunes, + Sloprano's 14 (to-do 193: loadable once
 * PooBoss copied their bank to the hard disk). Not in it: multiplayer and two
 * It's War ambiences, and eight tunes that sit in the list without playing:
 * conker_TuneWhere greys them where they can't play. */
#define TUNE_SETTLE_MS 2000u
#define TUNE_LOAD_MS   10000u
static const struct { uint16_t tune; const char *bank; } s_tune_banks[] = {
    {   1, "m_windy02_lp" },    /* newwind */
    {   2, "m_windy03_lp" },    /* sadbees */
    {   3, "m_windy03_lp" },    /* badbees1 */
    {   9, "m_ugabuga_lp" },    /* bronto */
    {  10, "m_windy03_lp" },    /* pissing */
    {  11, "am_batstower_lp" }, /* boileramb */
    {  13, "m_batstower_lp" },  /* dogfish1 */
    {  15, "m_batstower_lp" },  /* dogtower */
    {  16, "am_barnboys_lp" },  /* drips */
    {  19, "am_itswar_lp_X" },  /* warambi */
    {  25, "am_ugabuga_lp" },   /* crowd */
    {  27, "m_ugabuga_lp" },    /* ugarena1 */
    {  31, "am_general_lp" },   /* death */
    {  33, "m_itswar_lp" },     /* harbour */
    {  34, "m_frontend_lp" },   /* pub */
    {  44, "m_itswar_lp_X" },   /* bunker2 */
    {  45, "m_itswar_lp_X" },   /* bunkstab1 */
    {  48, "m_itswar_lp_X" },   /* bunkstab4 */
    {  49, "m_itswar_lp_X" },   /* bunkstab5 */
    {  53, "m_windy03_lp" },    /* poo */
    {  54, "am_heist_lp" },     /* ambi_credits */
    {  55, "m_batstower_lp" },  /* drunk1 */
    {  57, "m_batstower_lp" },  /* bullFloose */
    {  58, "m_batstower_lp" },  /* bullFeat1 */
    {  59, "m_batstower_lp" },  /* bullFeat2 */
    {  61, "m_ugabuga_lp" },    /* rocksolid2 */
    {  63, "am_hungover_lp" },  /* birdys */
    {  64, "m_itswar_lp" },     /* lavarace */
    {  65, "m_ugabuga_lp" },    /* lavachase */
    {  70, "am_windy_lp" },     /* windnight */
    {  75, "am_sloprano_lp" },  /* pooambi */
    {  76, "am_barnboys_lp" },  /* metstress */
    {  79, "m_itswar_lp_X" },   /* leccy1 */
    {  84, "am_ugabuga_lp" },   /* dinoambi */
    {  88, "m_ugabuga_lp" },    /* uggadino */
    {  89, "am_spooky_lp" },    /* horrorambi */
    {  90, "m_spooky_lp" },     /* conkbat */
    {  91, "m_spooky_lp" },     /* insidehouse */
    {  93, "m_spooky_lp" },     /* zombatk */
    {  94, "m_ugabuga_lp" },    /* bombrun */
    {  96, "am_windy_lp" },     /* bullshitambi */
    {  97, "am_ugabuga_lp" },   /* slimeambi */
    {  98, "m_windy03_lp" },    /* bullshit1 */
    { 101, "m_itswar_lp" },     /* subshoot */
    { 102, "m_itswar_lp" },     /* warboss */
    { 103, "m_barnboys_lp" },   /* bighayfight */
    { 104, "m_barnboys_lp" },   /* terminboss */
    { 110, "am_general_lp" },   /* labambi */
    { 111, "m_hungover_lp" },   /* birdytoon */
    { 119, "m_barnboys_lp" },   /* frankin2 */
    { 121, "m_frontend_lp" },   /* berrikid */
    { 128, "m_ugabuga_lp" },    /* weasboss */
    { 130, "m_ugabuga_lp" },    /* byebyerapt */
    { 135, "m_itswar_lp" },     /* harboureel */
    { 136, "m_itswar_lp" },     /* warreturn */
    { 142, "m_heist_lp" },      /* ending3 */
    { 146, "am_heist_lp" },     /* vaultambi */
    { 149, "multi_towers" },    /* mpArtifact */
    { 150, "m_barnboys_lp" },   /* barnoutside */
    { 151, "m_windy02_lp" },    /* happybees */
    { 152, "m_batstower_lp" },  /* backing */
    { 154, "am_hungover_lp" },  /* ambi_hungover_waterlaps */
    { 157, "am_windy_lp" },     /* ambi_windy_forest */
    { 158, "multi_towers" },    /* ambi_multi_alien_towers_wind */
    { 159, "am_barnboys_lp" },  /* ambi_barn_interior */
    { 160, "am_spooky_lp" },    /* ambi_spooky_interior */
    { 164, "am_hungover_lp" },  /* ambi_hungover_rain */
    { 165, "am_windy_lp" },     /* ambi_poo_exterior */
    { 166, "am_windy_lp" },     /* ambi_barn_wind */
    { 168, "am_itswar_lp" },    /* ambi_war_acidtunnel */
    { 169, "am_itswar_lp_X" },  /* ambi_war_battle */
    { 171, "m_frontend_lp" },   /* frontend_multi_phaseA */
    { 172, "m_frontend_lp" },   /* frontend_multi_phaseB */
    { 173, "m_frontend_lp" },   /* frontend_multi_phaseC */
    { 188, "multi_beach" },     /* ambi_multi_beach_head */
    { 189, "multi_doon" },      /* ambi_multi_doom_wind */
    { 190, "am_hungover_lp" },  /* ambi_hungover_cave */
    { 195, "am_itswar_lp_X" },  /* ambi_war_metalhits */
    { 201, "m_windy03_lp" },    /* badbees2_loop */
    { 202, "m_barnboys_lp" },   /* inbarn4_loop */
    { 203, "multi_bridge" },    /* ambi_multi_war */
    { 204, "multi_bridge" },    /* multi_bridge_level_01 */
    { 205, "m_ugabuga_lp" },    /* ugabuga_rocksolid2_A */
    { 206, "multi_beach" },     /* multi_beachdead_intro */
    { 207, "multi_beach" },     /* multi_beachdead_main_phase1 */
    { 208, "multi_beach" },     /* multi_beachdead_main_phase2 */
    { 209, "multi_beach" },     /* multi_beachdead_main_phase3 */
    { 215, "multi_doon" },      /* multi_alien_b */
    { 216, "multi_doon" },      /* multi_doon02 */
    { 217, "multi_doon" },      /* multi_doon03 */
    { 218, "multi_doon" },      /* multi_doon04 */
    { 219, "multi_spamono" },   /* ambi_multi_spamono */
    { 221, "multi_castle" },    /* ambi_multi_castle */
    { 222, "multi_ditch" },     /* ambi_multi_ditch */
    { 223, "multi_fortress" },  /* ambi_multi_fortress */
    { 226, "multi_bridge" },    /* ambi_multi_toth_spawnroom */
    { 227, "m_heist_lp" },      /* heist_inter_banklob_1 */
    { 228, "m_heist_lp" },      /* heist_inter_banklob_2 */
    { 229, "m_heist_lp" },      /* heist_inter_banklob_3 */
    { 230, "m_heist_lp" },      /* heist_inter_banklob_4 */
    { 231, "m_heist_lp" },      /* heist_inter_banklob_button */
    { 232, "m_heist_lp" },      /* heist_inter_banklob_cutscene */
    { 233, "am_barnboys_lp" },  /* ambi_barn_exterior */
    { 234, "am_barnboys_lp" },  /* ambi_barn_wind_exterior */
    { 235, "multi_castle" },    /* multi_castle_phase_one */
    { 236, "multi_castle" },    /* multi_castle_phase_two */
    { 237, "m_windy02_lp" },    /* music_windy_bee_cavern */
    { 238, "am_itswar_lp" },    /* ambi_war_harbour */
    { 239, "m_batstower_lp" },  /* windy_dfzone */
    { 240, "m_batstower_lp" },  /* windy_keepmon_loop */
    { 242, "am_windy_lp" },     /* ambi_intro_rain_windy */
    { 243, "am_windy_lp" },     /* ambi_inside_waspsnest */
    { 244, "multi_ditch" },     /* multi_the_ditch */
    { 245, "m_barnboys_lp" },   /* windy_wires_1 */
    { 246, "m_barnboys_lp" },   /* windy_wires_1a */
    { 247, "m_barnboys_lp" },   /* windy_wires_1b */
    { 248, "m_barnboys_lp" },   /* windy_wires_2 */
    { 249, "m_barnboys_lp" },   /* windy_wires_2a */
    { 250, "m_barnboys_lp" },   /* windy_wires_2b */
    { 251, "m_barnboys_lp" },   /* windy_wires_3 */
    { 252, "m_barnboys_lp" },   /* windy_wires_3a */
    { 253, "m_barnboys_lp" },   /* windy_wires_3b */
    { 254, "am_windy_lp" },     /* ambi_poo_cabin */
    { 255, "m_ugabuga_lp" },    /* bronto_lowpass */
    { 256, "am_ugabuga_lp" },   /* ambi_lava_pool */
    { 257, "am_ugabuga_lp" },   /* ambi_lava_pool_close */
    { 259, "m_ugabuga_lp" },    /* bomb_run_1 */
    { 261, "m_ugabuga_lp" },    /* bomb_run_3 */
    { 262, "m_ugabuga_lp" },    /* bomb_run_4 */
    { 263, "m_ugabuga_lp" },    /* bomb_run_5 */
    { 264, "m_ugabuga_lp" },    /* bomb_run_6 */
    { 265, "m_itswar_lp" },     /* warreturn_1 */
    { 266, "m_itswar_lp" },     /* warreturn_2 */
    { 267, "m_itswar_lp" },     /* warreturn_3 */
    { 268, "m_itswar_lp" },     /* warreturn_4 */
    { 269, "am_barnboys_lp" },  /* ambi_batstower_wind */
    { 270, "m_windy03_lp" },    /* badbees2_batstower */
    { 274, "m_batstower_lp" },  /* windy_pissing_batstower */
    { 275, "am_heist_lp" },     /* ambi_heist_airlock */
    { 277, "am_itswar_lp" },    /* ambi_plane_drone */
    { 278, "am_general_lp" },   /* ambi_throne_room */
    { 279, "m_frontend_lp" },   /* frontend_stats_tune */
    /* Sloprano's poo song (to-do 193): its bank only loads once PooBoss has copied
     * it to the hard disk (+0x2C 1 -> 2); after that it loads in any level (tested
     * in PooCabin). Before, tune_bank_of refuses it and conker_TuneWhere greys it. */
    {  73, "m_sloprano_lp" },   /* pooend1 */
    {  74, "m_sloprano_lp" },   /* pooend2 */
    { 176, "m_sloprano_lp" },   /* sloprano_verse01_intro */
    { 177, "m_sloprano_lp" },   /* sloprano_verse01_loop */
    { 178, "m_sloprano_lp" },   /* sloprano_verse02_intro */
    { 179, "m_sloprano_lp" },   /* sloprano_verse02_loop */
    { 180, "m_sloprano_lp" },   /* sloprano_verse03_intro */
    { 181, "m_sloprano_lp" },   /* sloprano_verse03_loop */
    { 182, "m_sloprano_lp" },   /* sloprano_pooarpegio */
    { 183, "m_sloprano_lp" },   /* sloprano_poocoda */
    { 184, "m_sloprano_lp" },   /* sloprano_poocoda_loop */
    { 185, "m_sloprano_lp" },   /* sloprano_verse01_loop_B */
    { 186, "m_sloprano_lp" },   /* sloprano_verse02_loop_B */
    { 187, "m_sloprano_lp" },   /* sloprano_verse03_loop_B */
};
static LONG s_load_was_state;
static int s_load_bank = -1;
static ULONGLONG s_load_at, s_load_primed;
static int s_bank_ours = -1, s_bank_prev = -1;  /* banks added for ours / the one before */
static struct { int bank, tune; } s_bank_free[4];  /* let go, unloaded once the tune is gone */
static int s_nbank_free;

int conker_TuneLoading(void)
{
    return (int)s_tune_loading;
}

/* The bank the table gives the tune, -1 = none (or one the jukebox can't load:
 * Sloprano's loops are copied to the hard disk first, +0x2C = 1). */
static int tune_bank_of(uint32_t m, int tune)
{
    for (size_t k = 0; k < sizeof s_tune_banks / sizeof s_tune_banks[0]; k++)
        if (s_tune_banks[k].tune == tune) {
            int i = bank_find(m, s_tune_banks[k].bank);
            uint32_t b = wave_bank(m, i);
            return b && MEM32(b + 0x2Cu) != 1u && !MEM8(b + 0x22u) && !MEM8(b + 0x14u) ? i : -1;
        }
    return -1;
}

/* ---- tunes that can't play here (to-do 193) ----
 * Greyed in the jukebox and refused by the console's tune, with where they do
 * play. Found by trying each in the field, Harbour, PooBoss, PooCabin, UBend and
 * a Beach Dead bot match, with banks added by hand (conker-notes 2026-11-12):
 * - never: sit in the music manager's list without playing anywhere;
 * - multiplayer: only in a match (Beach Dead played 167, 220, 224, 225; the alien
 *   winds not there, nor with any multi_* bank added; a story level can't play
 *   them even with multi_beach and sfx_looped_mult loaded);
 * - It's War: two banks each, both loaded only in its levels (the jukebox adds
 *   one; user 2026-11-12: grey elsewhere rather than load two);
 * - Sloprano's poo song: see s_tune_banks (any level once PooBoss copied it). */
static const uint16_t s_tune_never[] = { 78, 153, 161, 174, 175, 198, 260, 280 };
static const uint16_t s_tune_multi[] = {
    155, 156, 163, 167, 210, 211, 212, 213, 214, 220, 224, 225
};
static const struct { uint16_t tune; const char *bank[2]; } s_tune_war[] = {
    { 147, { "am_itswar_lp", "am_itswar_lp_X" } },  /* warbossamb */
    { 193, { "am_itswar_lp_X", "sfx_lv_itswar" } },  /* ambi_war_beachrun */
};

/* Registered by the level (or by us) and primed: its cues play. */
static int bank_up(uint32_t m, const char *name)
{
    uint32_t b = wave_bank(m, bank_find(m, name));
    return b && MEM8(b + 0x15u) && (MEM32(b + 0x18u) || MEM8(b + 0x14u));
}

const char *conker_TuneWhere(int i)
{
    uint32_t m = snd_manager();

    for (size_t k = 0; k < sizeof s_tune_never / sizeof s_tune_never[0]; k++)
        if (s_tune_never[k] == i)
            return "Never plays: it doesn't start in any level";
    for (size_t k = 0; k < sizeof s_tune_multi / sizeof s_tune_multi[0]; k++)
        if (s_tune_multi[k] == i)
            return MEM8(MULTI) ? NULL : "Multiplayer: plays only in a multiplayer match";
    if (!m)
        return NULL;
    for (size_t k = 0; k < sizeof s_tune_war / sizeof s_tune_war[0]; k++)
        if (s_tune_war[k].tune == i)
            return bank_up(m, s_tune_war[k].bank[0]) && bank_up(m, s_tune_war[k].bank[1])
                       ? NULL : "Plays only in It's War's levels, such as Harbour";
    for (size_t k = 0; k < sizeof s_tune_banks / sizeof s_tune_banks[0]; k++)
        if (s_tune_banks[k].tune == i) {
            uint32_t b;
            if (_stricmp(s_tune_banks[k].bank, "m_sloprano_lp"))
                return NULL;
            b = wave_bank(m, bank_find(m, s_tune_banks[k].bank));
            return !b || MEM32(b + 0x2Cu) != 1u || bank_up(m, s_tune_banks[k].bank) ? NULL
                   : "Plays in PooBoss (the Great Mighty Poo); after a visit there, "
                     "in any level until the game closes";
        }
    return NULL;
}

static void tune_let_go(uint32_t m, int bank, int tune)
{
    char nm[48];
    uint32_t b = wave_bank(m, bank);
    if (!b)
        return;
    bank_release(m, b);
    fprintf(stderr, "[JUKEBOX] bank %s let go (count %d)\n", bank_name(b, nm, sizeof nm),
            (int)MEM32(b + 0x18u));
    for (int k = 0; k < s_nbank_free; k++)
        if (s_bank_free[k].bank == bank) {
            s_bank_free[k].tune = tune;
            return;
        }
    if (s_nbank_free < 4) {
        s_bank_free[s_nbank_free].bank = bank;
        s_bank_free[s_nbank_free++].tune = tune;
    }
}

/* Banks let go: unloaded once their tune has left the playing list (the
 * game's own free waits until no tune of the bank's kind plays at all). */
static void tune_free_frame(uint32_t m, uint32_t mus)
{
    char nm[48];
    for (int k = 0; k < s_nbank_free; k++) {
        uint32_t b = wave_bank(m, s_bank_free[k].bank);
        int gone = !b || MEM32(b + 0x18u) || !MEM8(b + 0x15u);   /* in use again, or freed */
        if (!gone && (s_bank_free[k].tune <= 0 || tune_state(mus, s_bank_free[k].tune) < 0)
            && bank_unregister(b)) {
            fprintf(stderr, "[JUKEBOX] bank %s unloaded\n", bank_name(b, nm, sizeof nm));
            gone = 1;
        }
        if (gone)
            s_bank_free[k--] = s_bank_free[--s_nbank_free];
    }
}

static void tune_forget(void)
{
    InterlockedExchange(&s_tune_ours, 0);
    InterlockedExchange(&s_tune_state, TUNE_NONE);
    InterlockedExchange(&s_tune_loading, 0);
    s_tune_was = s_tune_prev = 0;
    s_bank_ours = s_bank_prev = s_load_bank = -1;
    s_nbank_free = 0;
}

static void tune_mark_bad(uint32_t level, int tune)
{
    InterlockedExchange(&s_tune_failed, tune);
    if (s_tune_bad_level != (LONG)(level & 0xFFu)) {
        memset(s_tune_bad, 0, sizeof s_tune_bad);
        InterlockedExchange(&s_tune_bad_level, (LONG)(level & 0xFFu));
    }
    s_tune_bad[tune] = 1;
}

/* Starts `play` (its bank, if one was added for it, in `bank`). */
static void tune_start(uint32_t m, uint32_t mus, int play, int bank, uint32_t level)
{
    int ours = (int)s_tune_ours;
    if (ours == play) {                 /* ours ended: the same again */
    } else if (ours && s_tune_state == TUNE_STARTING) {
        tune_call_stop(mus, ours);      /* never played; the one before stays */
        if (s_bank_ours >= 0 && s_bank_ours != bank && s_bank_ours != s_bank_prev)
            tune_let_go(m, s_bank_ours, ours);
    } else if (ours) {                  /* ours goes once the new one plays */
        s_tune_prev = ours;
        s_bank_prev = s_bank_ours != bank ? s_bank_ours : -1;
    } else {
        s_tune_was = tune_music_now(mus, play);
    }
    s_bank_ours = bank;
    tune_call_play(mus, play, 0, 0);
    fprintf(stderr, "[JUKEBOX] tune %d %s asked for (then stops %d)\n", play,
            conker_TuneName(play), ours ? ours : s_tune_was);
    InterlockedExchange(&s_tune_ours, play);
    InterlockedExchange(&s_tune_state, TUNE_STARTING);
    s_tune_level = level;
    s_tune_until = GetTickCount64() + TUNE_WAIT_MS;
}

/* The loading tune's bank primed and settled: it starts (1); or it didn't prime. */
static int tune_load_frame(uint32_t m, uint32_t mus, uint32_t level)
{
    char nm[48];
    uint32_t b = wave_bank(m, s_load_bank);
    int play = (int)s_tune_loading;
    ULONGLONG now = GetTickCount64();
    if (b && MEM8(b + 0x15u)) {
        if (!s_load_primed) {
            s_load_primed = now;
            fprintf(stderr, "[JUKEBOX] bank %s loaded in %llu ms\n", bank_name(b, nm, sizeof nm),
                    now - s_load_at);
        }
        if (now - s_load_primed < TUNE_SETTLE_MS)
            return 0;
        InterlockedExchange(&s_tune_loading, 0);
        tune_start(m, mus, play, s_load_bank, level);
        s_load_bank = -1;
        return 1;
    }
    if (now - s_load_at < TUNE_LOAD_MS)
        return 0;
    fprintf(stderr, "[JUKEBOX] tune %d: bank %s didn't load\n", play, b ? bank_name(b, nm, sizeof nm) : "?");
    tune_let_go(m, s_load_bank, 0);
    s_load_bank = -1;
    InterlockedExchange(&s_tune_loading, 0);
    tune_mark_bad(level, play);
    return 0;
}

static void tune_frame(void)
{
    LONG play = InterlockedExchange(&s_tune_play, -1);
    LONG stop = InterlockedExchange(&s_tune_stop, 0);
    uint32_t m = snd_manager(), mus = tune_manager(), level = MEM32(LEVEL);
    int ours = (int)s_tune_ours, bank;

    if (!m || !mus || ((ours || s_tune_loading) && level != s_tune_level)) {
        if (ours || s_tune_loading)
            fprintf(stderr, "[JUKEBOX] tune %d forgotten (level change)\n",
                    ours ? ours : (int)s_tune_loading);
        tune_forget();
        return;
    }
    tune_free_frame(m, mus);
    if (play > 0 && play < TUNE_N && conker_TuneName((int)play)
        && ((int)play != ours || s_tune_state == TUNE_ENDED) && (LONG)play != s_tune_loading) {
        if (s_tune_loading) {           /* the one loading isn't wanted any more */
            tune_let_go(m, s_load_bank, 0);
            s_load_bank = -1;
            InterlockedExchange(&s_tune_loading, 0);
        }
        bank = tune_bank_of(m, (int)play);
        if (bank >= 0 && bank != s_bank_ours && bank != s_bank_prev) {
            uint32_t b = wave_bank(m, bank);
            if (!MEM8(b + 0x15u) || !MEM32(b + 0x18u)) {    /* this level hasn't it */
                char nm[48];
                int primed = MEM8(b + 0x15u);               /* let go, not yet unloaded */
                bank_add(m, b);
                s_load_bank = bank;
                s_load_at = GetTickCount64();
                s_load_primed = primed ? s_load_at - TUNE_SETTLE_MS : 0;
                s_tune_level = level;
                InterlockedExchange(&s_tune_loading, play);
                fprintf(stderr, "[JUKEBOX] tune %ld %s: loading bank %s\n", play,
                        conker_TuneName((int)play), bank_name(b, nm, sizeof nm));
                return;
            }
            bank = -1;                  /* the level's own: nothing to let go later */
        }
        tune_start(m, mus, (int)play, bank, level);
        return;
    }
    if (stop && s_tune_loading) {       /* Stop while loading: no tune, no bank */
        fprintf(stderr, "[JUKEBOX] tune %ld stopped while its bank loaded\n", s_tune_loading);
        tune_let_go(m, s_load_bank, 0);
        s_load_bank = -1;
        InterlockedExchange(&s_tune_loading, 0);
    }
    if (s_tune_loading && tune_load_frame(m, mus, level))
        return;                         /* started: next frame looks at it */
    if (stop && ours) {
        if (s_tune_state == TUNE_STARTING) {
            tune_call_stop(mus, ours);
            if (s_bank_ours >= 0 && s_bank_ours != s_bank_prev)
                tune_let_go(m, s_bank_ours, ours);
            s_bank_ours = s_bank_prev;
            s_bank_prev = -1;
            ours = s_tune_prev;         /* the one before keeps playing */
            if (!ours) {
                fprintf(stderr, "[JUKEBOX] tune %ld stopped before it played\n", s_tune_ours);
                InterlockedExchange(&s_tune_ours, 0);
                InterlockedExchange(&s_tune_state, TUNE_NONE);
                s_tune_was = s_tune_prev = 0;
                return;
            }
        }
        if (s_tune_was > 0) {
            tune_call_play(mus, s_tune_was, ours, 4);
            fprintf(stderr, "[JUKEBOX] tune %d stopped, %d back\n", ours, s_tune_was);
        } else {
            tune_call_stop(mus, ours);
            fprintf(stderr, "[JUKEBOX] tune %d stopped\n", ours);
        }
        if (s_bank_ours >= 0)
            tune_let_go(m, s_bank_ours, ours);
        if (s_bank_prev >= 0 && s_bank_prev != s_bank_ours)
            tune_let_go(m, s_bank_prev, s_tune_prev);
        InterlockedExchange(&s_tune_ours, 0);
        InterlockedExchange(&s_tune_state, TUNE_NONE);
        s_tune_was = s_tune_prev = 0;
        s_bank_ours = s_bank_prev = -1;
        return;
    }
    if (s_tune_state == TUNE_STARTING) {
        int st = tune_state(mus, ours);
        if (st == 1) {
            int old = s_tune_prev ? s_tune_prev : s_tune_was;
            if (old)
                tune_call_stop(mus, old);
            fprintf(stderr, "[JUKEBOX] tune %d playing; %d stopped\n", ours, old);
            if (s_bank_prev >= 0 && s_bank_prev != s_bank_ours)
                tune_let_go(m, s_bank_prev, s_tune_prev);
            s_tune_prev = 0;
            s_bank_prev = -1;
            InterlockedExchange(&s_tune_state, TUNE_PLAYING);
        } else if (GetTickCount64() > s_tune_until) {
            if (st >= 0)
                tune_call_stop(mus, ours);
            fprintf(stderr, "[JUKEBOX] tune %d can't play in this level (state %d): the music "
                    "stays as it was\n", ours, st);
            tune_mark_bad(level, ours);
            if (s_bank_ours >= 0 && s_bank_ours != s_bank_prev)
                tune_let_go(m, s_bank_ours, ours);
            if (s_tune_prev) {          /* the jukebox's previous tune still plays */
                InterlockedExchange(&s_tune_ours, s_tune_prev);
                InterlockedExchange(&s_tune_state, TUNE_PLAYING);
                s_bank_ours = s_bank_prev;
                s_tune_prev = 0;
                s_bank_prev = -1;
            } else {
                InterlockedExchange(&s_tune_ours, 0);
                InterlockedExchange(&s_tune_state, TUNE_NONE);
                s_tune_was = 0;
                s_bank_ours = -1;
            }
        }
    } else if (s_tune_state == TUNE_PLAYING && tune_state(mus, ours) < 0) {
        fprintf(stderr, "[JUKEBOX] tune %d ended\n", ours);
        InterlockedExchange(&s_tune_state, TUNE_ENDED);
    }
}

/* ---- wave banks for the cheat sounds (to-do 199, SPEC 10.3) ----
 * A cue whose wave bank the screen hasn't loaded is silent: lightbulb2 is in
 * sfx_X, which only levels register (not the main menu), war_conker_gunout in
 * It's War's sfx_lv_itswar, the Mouse's mice in hd_sfx (the effects every
 * level registers; to-do 212). As the jukebox does for a tune: count + 1 to load,
 * - 1 and an unregister to let go; after priming XACT needs ~2 s more. A level
 * load's "every count - 1" takes ours away too, so a level change forgets it. */
static struct {
    const char *name;
    int bank;                   /* -2 = not looked up yet */
    int ours;                   /* 1 added by us, 2 let go, unload pending */
    uint32_t level;
    ULONGLONG primed_at;
} s_sb[SOUND_BANK_N] = { { "sfx_X", -2 }, { "sfx_lv_itswar", -2 }, { "hd_sfx", -2 } };

int conker_SoundBank(int which, int want)
{
    uint32_t m = snd_manager(), b, level = MEM32(LEVEL);
    ULONGLONG now = GetTickCount64();
    char nm[48];

    if (!m || which < 0 || which >= SOUND_BANK_N)
        return 0;
    if (s_sb[which].bank == -2)
        s_sb[which].bank = bank_find(m, s_sb[which].name);
    b = wave_bank(m, s_sb[which].bank);
    if (!b)
        return 0;
    if (s_sb[which].ours && s_sb[which].level != level) {
        fprintf(stderr, "[CHEATSND] bank %s forgotten (level change)\n", s_sb[which].name);
        s_sb[which].ours = 0;
    }
    if (want) {
        if (s_sb[which].ours != 1) {
            if (s_sb[which].ours == 0 && MEM8(b + 0x15u) && (MEM32(b + 0x18u) || MEM8(b + 0x14u)))
                return 1;               /* the level's own */
            bank_add(m, b);
            s_sb[which].ours = 1;
            s_sb[which].level = level;
            s_sb[which].primed_at = MEM8(b + 0x15u) ? now - TUNE_SETTLE_MS : 0;
            fprintf(stderr, "[CHEATSND] loading bank %s\n", bank_name(b, nm, sizeof nm));
        }
        if (!MEM8(b + 0x15u))
            return 0;
        if (!s_sb[which].primed_at) {
            s_sb[which].primed_at = now;
            fprintf(stderr, "[CHEATSND] bank %s loaded\n", s_sb[which].name);
        }
        return now - s_sb[which].primed_at >= TUNE_SETTLE_MS;
    }
    if (s_sb[which].ours == 1) {
        bank_release(m, b);
        s_sb[which].ours = 2;
    }
    if (s_sb[which].ours == 2 && (MEM32(b + 0x18u) || bank_unregister(b))) {
        fprintf(stderr, "[CHEATSND] bank %s let go%s\n", s_sb[which].name,
                MEM32(b + 0x18u) ? " (still in use)" : ", unloaded");
        s_sb[which].ours = 0;
    }
    return 0;
}

/* ---- the frame ---- */
void debug_tools_frame(void)
{
    tp_frame();
    set_frame();
    flag_frame();
    jukebox_frame();
    tune_frame();
    scan_frame();
}
