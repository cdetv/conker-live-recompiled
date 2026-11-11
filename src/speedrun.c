/**
 * The speedrun timer (to-do 154, SPEC 10.1): game time without loads, from a
 * new game: the total and the current chapter's part, shown by the overlay
 * (F2 Display "Speedrun Timer"). Counted on the game thread once a frame in
 * the story's levels (not the main menu, multiplayer, our freezes); the time a
 * load takes is left out (the load wrappers call speedrun_skip). A new game =
 * a story game started with no progress bit set; a saved game picked up goes
 * on from where the timer was (or starts at 0 if none ran this session, so a
 * chapter can be timed from the Chapters list as well).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "speedrun.h"
#include "edit_save.h"
#include "photo_mode.h"
#include "settings.h"

#define LEVEL     0x007FA1F8u
#define MULTI     0x007819D5u
#define PROGRESS  0x0084B290u       /* -> the progress bits */
#define PARTS     0x0074CD44u       /* per chapter: -> {u16 cash, u16 checkpoint bit} */
#define NPARTS    0x0074CD68u       /* per chapter: its part count (u8) */
#define CUTSCENE  0x0076F0E0u       /* u8: 1 while a cutscene plays (2 as one ends) */
#define FE_STATE  0x00776574u       /* the front end's state: 3 = the main menu, 0 in a game */

static volatile LONG s_running, s_skip;
static double s_total, s_part, s_last;    /* ms */
static volatile LONG s_chapter = -1, s_partno = -1;
static SRWLOCK s_lock = SRWLOCK_INIT;

static void lock(void)
{
    AcquireSRWLockExclusive(&s_lock);
}

static void unlock(void)
{
    ReleaseSRWLockExclusive(&s_lock);
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u;
}

/* The latest part whose checkpoint bit is set, as the slot screen counts it
 * (0x0035DE70). 0 when no progress is readable. */
static int current_part(int *chapter, int *part)
{
    uint32_t bits = MEM32(PROGRESS);
    int found = 0;

    if (!va_ok(bits))
        return 0;
    for (int c = 0; c < ES_CHAPTERS; c++) {
        uint32_t list = MEM32(PARTS + (uint32_t)c * 4u);
        int n = MEM8(NPARTS + (uint32_t)c);
        if (!va_ok(list))
            continue;
        for (int p = 0; p < n; p++) {
            unsigned bit = MEM16(list + (uint32_t)p * 4u + 2u);
            if (MEM8(bits + (bit >> 3)) & (1u << (bit & 7u))) {
                *chapter = c;
                *part = p;
                found = 1;
            }
        }
    }
    return found;
}

static int no_progress(void)
{
    uint32_t bits = MEM32(PROGRESS);
    if (!va_ok(bits))
        return 0;
    for (int i = 0; i < (ES_BITS + 7) / 8; i++)
        if (MEM8(bits + (uint32_t)i))
            return 0;
    return 1;
}

void speedrun_game_start(int level)
{
    if (level >= 0x4B || level == 0x1D) /* multiplayer, the main menu's level */
        return;
    lock();
    if (no_progress() || !s_running) {
        s_total = s_part = 0.0;
        s_chapter = s_partno = -1;
        fprintf(stderr, "[SPEEDRUN] %s at level 0x%02X: the timer starts at 0\n",
                no_progress() ? "new game" : "a game", level);
    }
    s_running = 1;
    s_skip = 1;
    unlock();
}

void speedrun_skip(void)
{
    InterlockedExchange(&s_skip, 1);
}

/* Time cutscenes (F2, under Speedrun Timer; user 2026-11-09): 1 = cutscenes
 * count (default), 0 = the timer stops while one plays. */
static volatile LONG s_cutscenes = -1;

void speedrun_set_cutscenes(int on)
{
    InterlockedExchange(&s_cutscenes, on != 0);
}

/* Why this frame is not counted, or NULL when it is. The main menu is the
 * front end's level 0x1D in front-end state 3: New Game's opening plays a scene
 * in that level too (Berri's room, state 0), and that is the game's (user
 * 2026-11-09: the timer stopped there; the scene byte reads 2, not 1, while a
 * scene is being skipped, so "no scene playing" was not enough). */
static const char *not_counted(void)
{
    const int scene = MEM8(CUTSCENE) == 1;

    if (s_cutscenes < 0) {
        const char *v = settings_get("speedrun_cutscenes");
        InterlockedExchange(&s_cutscenes, !(v && v[0] == '0'));
    }
    if (MEM8(MULTI))
        return "multiplayer";
    if (MEM32(LEVEL) == 0x1Du && MEM32(FE_STATE) == 3u)
        return "the main menu";
    if (scene && !s_cutscenes)
        return "a cutscene (Time cutscenes off)";
    if (photo_frozen())
        return "photo mode / pause";
    return NULL;
}

void speedrun_frame(void)
{
    static const char *last_why = "";
    const char *why;
    double t = now_ms(), dt;
    int c, p;

    if (!s_running)
        return;
    lock();
    dt = t - s_last;
    s_last = t;
    why = not_counted();
    if (why != last_why) {
        fprintf(stderr, "[SPEEDRUN] %s at %.2f s\n", why ? why : "counting",
                s_total / 1000.0);
        last_why = why;
    }
    if (InterlockedExchange(&s_skip, 0) || dt < 0.0 || dt > 1000.0 || why) {
        unlock();
        return;
    }
    s_total += dt;
    s_part += dt;
    if (!current_part(&c, &p)) {
        c = 0;                          /* a new game before its first checkpoint */
        p = 0;
    }
    if (c != s_chapter || p != s_partno) {
        if (s_chapter >= 0)
            fprintf(stderr, "[SPEEDRUN] %s %d done in %.2f s (total %.2f s)\n",
                    edit_save_chapter_name(s_chapter), s_partno + 1,
                    s_part / 1000.0, s_total / 1000.0);
        s_chapter = c;
        s_partno = p;
        s_part = 0.0;
    }
    unlock();
}

int conker_Speedrun(double *total_ms, double *part_ms, int *chapter, int *part)
{
    lock();
    *total_ms = s_total;
    *part_ms = s_part;
    *chapter = s_chapter;
    *part = s_partno;
    unlock();
    return s_running != 0;
}
