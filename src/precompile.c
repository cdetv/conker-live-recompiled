/*
 * Shader pre-compile pass (to-do 25, SPEC 10.1 item 1): conker.exe
 * --precompile-shaders, started from the launcher's Files tab (never by
 * itself: user 2026-11-07).
 *
 * A shader is compiled the first time the game draws with it and kept in
 * the save folder's shader_cache; until it is ready that draw goes a slower
 * way (vertex programs on the CPU, combiner setups through the one general
 * pixel shader), which is the first-time-in-an-area slowdown of issue #11.
 * Nothing derived from the game may ship with the port (SPEC 3.3), so the
 * cache can only be filled on the player's own PC. This pass does it ahead
 * of play with the game's own soak test (recomp_manual.c soak_poll; Rare's
 * test in the retail game, conker-notes 2026-10-23): every map, then every
 * task of every chapter (Conker warped to each), while the level-change
 * memory scans pre-compile every vertex program found (RECOMP_VSH_PRECOMPILE)
 * and whatever is drawn compiles its combiner setups.
 *
 * The soak test waits about 5 s of game time at each step; here a step ends
 * once it has been on screen MIN_STEP_MS and every compile queue has been
 * empty for IDLE_MS (soak timer 0x00771136 set past its limit), so the pass
 * takes ~8 minutes (2026-11-07), not the soak test's ~45, and makes nearly
 * everything a full-length soak run makes. A cover hides the levels (no
 * spoilers; overlay.cpp precompile_screen), the sound is muted
 * (RECOMP_APU_MUTE), the controllers and keyboard are taken from the game,
 * and the result goes to shader_cache\precompiled.ini for the launcher:
 * version, date, done or stopped, counts. Saves are never written: the soak
 * test runs from the front end with no game loaded (checked: an empty
 * saves folder after a full pass).
 *
 * Stopped (Esc, the window closed): the file also says where (the map, or
 * the chapter and task), and the next run of the same version continues
 * there (user 2026-11-07). The soak test takes its first mode, map and
 * chapter from three globals (SOAK_CFG_*, read when it starts), so the
 * resumed run starts in that map or chapter directly; inside a chapter it
 * races through the tasks before the stopped one (no wait at all).
 *
 * Finished (user 2026-11-07): .vso / .cso files older than the run's start
 * that nothing used are deleted. "Used" = a scan found it, a draw loaded it
 * or drew it from the startup load (the toolkit sets its write time:
 * RECOMP_SHADER_CACHE_TOUCH); the startup load of every .vso is not a use.
 * A resumed run counts from the first run's start, so what the earlier,
 * stopped runs used stays. A stopped run deletes nothing.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "precompile.h"

#ifndef CONKER_VERSION_STR
#define CONKER_VERSION_STR "?"
#endif

extern void conker_StartSoak(void);
extern int nv2a_vsh_prescan_busy(void);
extern void nv2a_vsh_gpu_precompile_stats(uint32_t *compiled, uint32_t *had,
                                          uint32_t *failed, uint32_t *waiting);
extern void nv2a_vsh_gpu_draw_stats(uint32_t *compiled, uint32_t *failed,
                                    uint32_t *queued, double *ms);
extern uint32_t nv2a_d3d11_ps_spec_pending(uint32_t *compiled);
extern void xbox_InputSetBlocked(int on);

/* The soak test's state (soak_test_update 0x000A54B0). */
#define SOAK_MODE    0x0077111Cu  /* 0 idle, 1 start, 2 maps, 3 chapters,
                                     4 scenes, 5 loop */
#define SOAK_SUB     0x0077111Du  /* 0 begin a step, 1 waiting */
#define SOAK_MAP     0x0077111Eu
#define SOAK_CHAPTER 0x00771121u  /* 0-8; 9 once the last one is done */
#define SOAK_TASK    0x00771122u
#define SOAK_TASKS   0x00771123u
#define SOAK_TIMER   0x00771136u  /* u16 ticks; the step ends past
                                     [0x007F9F5C] x 5 */
#define SOAK_LOOPS   0x00771118u
#define LEVEL        0x007FA1F8u
/* What it starts with (mode 1): mode (retail 2), the first map, and for
 * mode 3 the first chapter (its chapter byte starts one below). */
#define SOAK_CFG_MODE    0x006A7FF8u
#define SOAK_CFG_MAP     0x0084728Cu
#define SOAK_CFG_CHAPTER 0x00847290u

#define LEVELS_TOTAL   47         /* the map pass: soak_next_map's steps */
#define CHAPTERS_TOTAL 9
#define LEVEL_SHARE    0.5f       /* of the progress bar: the map pass took
                                     half of a run (242 of 490 s) */
#define MIN_STEP_MS    2500u      /* on screen at least this long: 97 % of a
                                     step's combiner setups show by 3 s */
#define IDLE_MS        500u       /* every queue empty this long */
#define FINISH_MAX_MS  60000u     /* the last compiles, at most */

/* Where a stopped run was: PRECOMPILE_LEVELS (map, levels done counting
 * it) or PRECOMPILE_CHAPTERS (chapter 0-9, task); 0 = start over. */
typedef struct {
    int stage, map, levels, chapter, task;
} Resume;

static int s_on;
static char s_result[MAX_PATH];   /* shader_cache\precompiled.ini */
static char s_dir[MAX_PATH];      /* shader_cache */
static CRITICAL_SECTION s_cs;     /* s_st */
static PrecompileStatus s_st;
static ULONGLONG s_t0;
static volatile LONG s_written;   /* the result file, once */
static Resume s_resume;           /* from the last, stopped run */
static ULONGLONG s_started;       /* FILETIME of the first run of a series */
static unsigned s_prev_seconds;   /* the stopped runs' time before this one */

static ULONGLONG now_ms(void)
{
    return GetTickCount64();
}

static ULONGLONG now_filetime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

void precompile_enable(void)
{
    s_on = 1;
    InitializeCriticalSection(&s_cs);
    s_t0 = now_ms();
    s_started = now_filetime();
    /* Before settings.ini is read, so these win over its options. Small
     * and windowed: the picture is covered anyway, and a shader's cache key
     * does not depend on the render size. */
    _putenv_s("RECOMP_APU_MUTE", "1");
    _putenv_s("RECOMP_RENDER_RES", "640x480");
    _putenv_s("RECOMP_RENDER_SCALE", "100");
    _putenv_s("RECOMP_DISPLAY_MODE", "windowed");
    _putenv_s("RECOMP_FRAME_RATE", "30");
    _putenv_s("RECOMP_VSH_PRECOMPILE", "1");
    _putenv_s("RECOMP_LOADSCREEN", "off");
    _putenv_s("RECOMP_SHADER_CACHE_TOUCH", "1");
}

int precompile_on(void)
{
    return s_on;
}

/* The last run's precompiled.ini: a stopped run of this version is
 * continued (where it was, its start time and its seconds). */
static void read_previous(void)
{
    char line[256], stage[32] = "", version[64] = "", result[64] = "";
    unsigned long long started = 0;
    Resume r = { 0 };
    unsigned seconds = 0;
    FILE *f;

    if (fopen_s(&f, s_result, "r") || !f)
        return;
    while (fgets(line, sizeof line, f)) {
        char key[64], val[160];
        if (line[0] == ';' || sscanf_s(line, " %63[^= ] = %159[^\r\n]", key,
                                       (unsigned)sizeof key, val,
                                       (unsigned)sizeof val) != 2)
            continue;
        if (!strcmp(key, "version"))
            strcpy_s(version, sizeof version, val);
        else if (!strcmp(key, "result"))
            strcpy_s(result, sizeof result, val);
        else if (!strcmp(key, "resume"))
            strcpy_s(stage, sizeof stage, val);
        else if (!strcmp(key, "resume_map"))
            r.map = atoi(val);
        else if (!strcmp(key, "resume_levels"))
            r.levels = atoi(val);
        else if (!strcmp(key, "resume_chapter"))
            r.chapter = atoi(val);
        else if (!strcmp(key, "resume_task"))
            r.task = atoi(val);
        else if (!strcmp(key, "started"))
            started = _strtoui64(val, NULL, 10);
        else if (!strcmp(key, "seconds"))
            seconds = (unsigned)strtoul(val, NULL, 10);
    }
    fclose(f);
    if (strcmp(result, "stopped") || strcmp(version, CONKER_VERSION_STR))
        return;
    if (!strcmp(stage, "levels"))
        r.stage = PRECOMPILE_LEVELS;
    else if (!strcmp(stage, "chapters"))
        r.stage = PRECOMPILE_CHAPTERS;
    if (!r.stage || r.map < 0 || r.map > 255 || r.chapter < 0
        || r.chapter > CHAPTERS_TOTAL || r.task < 0 || r.task > 255)
        return;
    s_resume = r;
    if (started && started < s_started)
        s_started = started;
    s_prev_seconds = seconds;
    s_st.resumed = 1;
    if (r.stage == PRECOMPILE_LEVELS)
        fprintf(stderr, "[PRECOMPILE] continuing a stopped run at map %d "
                "(level %d of %d)\n", r.map, r.levels, LEVELS_TOTAL);
    else
        fprintf(stderr, "[PRECOMPILE] continuing a stopped run at chapter "
                "%d, task %d\n", r.chapter + 1, r.task + 1);
}

void precompile_set_cache_dir(const char *dir)
{
    if (!s_on)
        return;
    if (!dir || !dir[0]) {
        fprintf(stderr, "[PRECOMPILE] no shader cache (RECOMP_VSH_GPU_CACHE"
                " off): nothing would be kept\n");
        return;
    }
    CreateDirectoryA(dir, NULL);
    strcpy_s(s_dir, MAX_PATH, dir);
    sprintf_s(s_result, MAX_PATH, "%s\\precompiled.ini", dir);
    fprintf(stderr, "[PRECOMPILE] shader pre-compile pass (version %s), "
            "result in %s\n", CONKER_VERSION_STR, s_result);
    read_previous();
}

/* A cache file's name: 16 hex digits and .vso or .cso, nothing else (a
 * pattern like *.vso can also match longer names through 8.3 names). */
static int cache_name(const char *name, const char *ext)
{
    size_t n = strlen(name), i;
    if (n != 20 || _stricmp(name + 16, ext))
        return 0;
    for (i = 0; i < 16; i++)
        if (!strchr("0123456789ABCDEFabcdef", name[i]))
            return 0;
    return 1;
}

/* The cache files of one kind; older_than nonzero: delete those last
 * written before it instead (returns how many went). */
static unsigned scan_files(const char *ext, ULONGLONG older_than)
{
    char pattern[MAX_PATH + 16], path[MAX_PATH + 32];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    unsigned n = 0;

    sprintf_s(pattern, sizeof pattern, "%s\\*%s", s_dir, ext);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        ULONGLONG written = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32
                          | fd.ftLastWriteTime.dwLowDateTime;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || !cache_name(fd.cFileName, ext))
            continue;
        if (!older_than) {
            n++;
        } else if (written < older_than) {
            sprintf_s(path, sizeof path, "%s\\%s", s_dir, fd.cFileName);
            if (DeleteFileA(path))
                n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* shader_cache\precompiled.ini: what the launcher's Files tab shows, and
 * where a stopped run continues. deleted: stale files removed (done). */
static void write_result(const char *result, unsigned deleted)
{
    char tmp[MAX_PATH + 8];
    PrecompileStatus st;
    Resume r = { 0 };
    FILE *f;
    time_t t = time(NULL);
    struct tm lt;
    int stopped = !strcmp(result, "stopped");

    if (!s_on || !s_result[0] || InterlockedExchange(&s_written, 1))
        return;
    precompile_status(&st);
    if (stopped) {
        if (st.stage == PRECOMPILE_LEVELS) {
            r.stage = PRECOMPILE_LEVELS;
            r.map = MEM8(SOAK_MAP);
            r.levels = st.levels;
        } else if (st.stage == PRECOMPILE_CHAPTERS && !st.skipping) {
            r.stage = PRECOMPILE_CHAPTERS;
            r.chapter = MEM8(SOAK_CHAPTER);
            r.task = MEM8(SOAK_TASK);
        } else if (st.stage >= PRECOMPILE_FINISH) {
            r.stage = PRECOMPILE_CHAPTERS;      /* nothing left but the end */
            r.chapter = CHAPTERS_TOTAL;
        } else {
            r = s_resume;       /* not started yet: the same place again */
        }
    }
    localtime_s(&lt, &t);
    sprintf_s(tmp, sizeof tmp, "%s.tmp", s_result);
    if (fopen_s(&f, tmp, "w") || !f)
        return;
    fprintf(f, "; Written by conker.exe --precompile-shaders (the launcher's "
               "Files tab).\n");
    fprintf(f, "version = %s\n", CONKER_VERSION_STR);
    fprintf(f, "date = %04d-%02d-%02d %02d:%02d\n", lt.tm_year + 1900,
            lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    fprintf(f, "result = %s\n", result);
    fprintf(f, "levels = %d\n", st.levels);
    fprintf(f, "chapters = %d\n", st.stage >= PRECOMPILE_FINISH
                                  ? CHAPTERS_TOTAL
                                  : st.chapter > 0 ? st.chapter - 1 : 0);
    fprintf(f, "seconds = %u\n", s_prev_seconds + st.seconds);
    fprintf(f, "compiled = %u\n", st.shaders);
    fprintf(f, "vertex_shaders = %u\n", scan_files(".vso", 0));
    fprintf(f, "other_shaders = %u\n", scan_files(".cso", 0));
    if (!stopped)
        fprintf(f, "deleted = %u\n", deleted);
    /* The first run's start: a continued run keeps it, so its clean-up
     * spares what the earlier runs used. */
    fprintf(f, "started = %llu\n", s_started);
    if (r.stage == PRECOMPILE_LEVELS)
        fprintf(f, "resume = levels\nresume_map = %d\nresume_levels = %d\n",
                r.map, r.levels);
    else if (r.stage == PRECOMPILE_CHAPTERS)
        fprintf(f, "resume = chapters\nresume_chapter = %d\n"
                   "resume_task = %d\n", r.chapter, r.task);
    if (fclose(f) || !MoveFileExA(tmp, s_result, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
    fprintf(stderr, "[PRECOMPILE] %s after %u s (%u in all): %d levels, "
            "chapter %d, %u shaders compiled, %u stale deleted; %s\n", result,
            st.seconds, s_prev_seconds + st.seconds, st.levels, st.chapter,
            st.shaders, deleted, s_result);
}

void precompile_stop(void)
{
    if (s_on && s_st.stage != PRECOMPILE_DONE)
        write_result("stopped", 0);
}

void precompile_status(PrecompileStatus *st)
{
    if (!s_on) {
        memset(st, 0, sizeof *st);
        return;
    }
    EnterCriticalSection(&s_cs);
    *st = s_st;
    LeaveCriticalSection(&s_cs);
    st->seconds = (unsigned)((now_ms() - s_t0) / 1000u);
}

/* Shaders compiled this run, and whether anything is still on its way:
 * a scan not done, programs waiting for the pre-compiler or the drawing
 * path, combiner setups not ready. */
static int compilers_busy(unsigned *made)
{
    uint32_t pc, had, failed, waiting, dc, dfail, queued, psc, pending;
    double ms;
    int busy = nv2a_vsh_prescan_busy();

    nv2a_vsh_gpu_precompile_stats(&pc, &had, &failed, &waiting);
    nv2a_vsh_gpu_draw_stats(&dc, &dfail, &queued, &ms);
    pending = nv2a_d3d11_ps_spec_pending(&psc);
    *made = pc + dc + psc;
    return busy || waiting || queued || pending;
}

void precompile_poll(void)
{
    static int asked, last_map = -1, cfg_set;
    static uint8_t cfg[3];              /* the soak test's own start values */
    static uint32_t step_id = 0xFFFFFFFFu, level = 0xFFFFFFFFu, loops;
    static ULONGLONG step_t, idle_t, finish_t;
    PrecompileStatus st;
    uint8_t mode;
    unsigned made;
    int busy;
    ULONGLONG t;

    if (!s_on)
        return;
    xbox_InputSetBlocked(1);            /* every frame: menus give it back */
    if (!asked) {
        asked = 1;
        loops = MEM32(SOAK_LOOPS);
        if (s_resume.stage) {
            /* Start where the stopped run was: the soak test reads these
             * when it starts; put back once it has. */
            cfg[0] = MEM8(SOAK_CFG_MODE);
            cfg[1] = MEM8(SOAK_CFG_MAP);
            cfg[2] = MEM8(SOAK_CFG_CHAPTER);
            cfg_set = 1;
            if (s_resume.stage == PRECOMPILE_LEVELS) {
                MEM8(SOAK_CFG_MODE) = 2;
                MEM8(SOAK_CFG_MAP) = (uint8_t)s_resume.map;
                s_st.levels = s_resume.levels - 1;
            } else {
                MEM8(SOAK_CFG_MODE) = 3;
                MEM8(SOAK_CFG_CHAPTER) = (uint8_t)s_resume.chapter;
                s_st.levels = LEVELS_TOTAL;
            }
        }
        conker_StartSoak();             /* on the front end, at once */
    }
    t = now_ms();
    busy = compilers_busy(&made);
    if (busy)
        idle_t = t;
    EnterCriticalSection(&s_cs);
    s_st.shaders = made;
    st = s_st;
    LeaveCriticalSection(&s_cs);
    mode = MEM8(SOAK_MODE);

    switch (st.stage) {
    case PRECOMPILE_BOOT:
        if (mode >= 2 || MEM32(SOAK_LOOPS) != loops) {
            if (cfg_set) {
                MEM8(SOAK_CFG_MODE) = cfg[0];
                MEM8(SOAK_CFG_MAP) = cfg[1];
                MEM8(SOAK_CFG_CHAPTER) = cfg[2];
                cfg_set = 0;
            }
            st.stage = mode == 2 ? PRECOMPILE_LEVELS : PRECOMPILE_CHAPTERS;
            fprintf(stderr, "[PRECOMPILE] t=%llu s: %s pass%s\n",
                    (t - s_t0) / 1000u, mode == 2 ? "map" : "chapter",
                    s_resume.stage ? " (continued)" : "");
        }
        break;
    case PRECOMPILE_LEVELS:
    case PRECOMPILE_CHAPTERS:
        if (mode == 3 && st.stage == PRECOMPILE_LEVELS) {
            st.stage = PRECOMPILE_CHAPTERS;
            fprintf(stderr, "[PRECOMPILE] t=%llu s: chapter pass (%d levels"
                    " done)\n", (t - s_t0) / 1000u, st.levels);
        }
        if (mode >= 4 || mode == 1 || mode == 0
            || MEM32(SOAK_LOOPS) != loops) {
            /* The chapters are done (the scene step has nothing in the
             * retail game): stop the soak test, wait for the last ones. */
            MEM8(SOAK_MODE) = 0;
            st.stage = PRECOMPILE_FINISH;
            st.skipping = 0;
            finish_t = t;
            fprintf(stderr, "[PRECOMPILE] t=%llu s: passes done, %u "
                    "compiled\n", (t - s_t0) / 1000u, made);
            break;
        }
        if (mode == 2 && MEM8(SOAK_MAP) != last_map) {
            last_map = MEM8(SOAK_MAP);
            if (st.levels < LEVELS_TOTAL)   /* a hand-edited resume point */
                st.levels++;
        }
        /* The chapter byte is 0xFF for a frame as the chapter pass begins. */
        if (mode == 3 && MEM8(SOAK_CHAPTER) < CHAPTERS_TOTAL) {
            if (st.chapter != MEM8(SOAK_CHAPTER) + 1)
                fprintf(stderr, "[PRECOMPILE] t=%llu s: chapter %d of %d (%d"
                        " parts)\n", (t - s_t0) / 1000u,
                        MEM8(SOAK_CHAPTER) + 1, CHAPTERS_TOTAL,
                        MEM8(SOAK_TASKS));
            st.chapter = MEM8(SOAK_CHAPTER) + 1;
            st.task = MEM8(SOAK_TASK) + 1;
            st.tasks = MEM8(SOAK_TASKS);
            if (st.task > st.tasks)
                st.task = st.tasks;
            /* Continued: the tasks before the stopped one were done. */
            {
                int was = st.skipping;
                st.skipping = s_resume.stage == PRECOMPILE_CHAPTERS
                              && MEM8(SOAK_CHAPTER) == s_resume.chapter
                              && MEM8(SOAK_TASK) < s_resume.task;
                if (was && !st.skipping)
                    fprintf(stderr, "[PRECOMPILE] t=%llu s: caught up at "
                            "chapter %d, task %d\n", (t - s_t0) / 1000u,
                            MEM8(SOAK_CHAPTER) + 1, MEM8(SOAK_TASK) + 1);
            }
        }
        /* A step is a map, or a chapter's task; a level change (its load
         * has finished, the scan is asked for) starts its time again. */
        {
            uint32_t id = (uint32_t)mode << 24 | (uint32_t)MEM8(SOAK_MAP) << 16
                        | (uint32_t)MEM8(SOAK_CHAPTER) << 8 | MEM8(SOAK_TASK);
            if (id != step_id || MEM32(LEVEL) != level) {
                step_id = id;
                level = MEM32(LEVEL);
                step_t = t;
                idle_t = t;
            }
        }
        if (MEM8(SOAK_SUB) == 1 && MEM16(SOAK_TIMER) < 0x7FFFu
            && (st.skipping
                || (t - step_t >= MIN_STEP_MS && t - idle_t >= IDLE_MS)))
            MEM16(SOAK_TIMER) = 0x7FFFu;    /* past any limit: next step */
        break;
    case PRECOMPILE_FINISH:
        if ((!busy && t - idle_t >= IDLE_MS) || t - finish_t >= FINISH_MAX_MS) {
            unsigned deleted;
            st.stage = PRECOMPILE_DONE;
            EnterCriticalSection(&s_cs);
            s_st = st;
            LeaveCriticalSection(&s_cs);
            /* What nothing used since the first run's start is stale. */
            deleted = scan_files(".vso", s_started)
                    + scan_files(".cso", s_started);
            write_result(busy ? "done (some still compiling)" : "done",
                         deleted);
            fflush(stderr);
            ExitProcess(0);
        }
        break;
    }

    st.levels_total = LEVELS_TOTAL;
    st.chapters = CHAPTERS_TOTAL;
    if (st.stage <= PRECOMPILE_LEVELS)
        st.progress = LEVEL_SHARE * (st.levels > 0 ? st.levels - 1 : 0)
                    / LEVELS_TOTAL;
    else if (st.stage == PRECOMPILE_CHAPTERS)
        st.progress = LEVEL_SHARE + (1.0f - LEVEL_SHARE)
                    * ((st.chapter - 1)
                       + (st.tasks ? (float)(st.task - 1) / st.tasks : 0.0f))
                    / CHAPTERS_TOTAL;
    else
        st.progress = 1.0f;
    if (st.progress < 0.0f)
        st.progress = 0.0f;
    EnterCriticalSection(&s_cs);
    st.shaders = s_st.shaders;
    s_st = st;
    LeaveCriticalSection(&s_cs);
}
