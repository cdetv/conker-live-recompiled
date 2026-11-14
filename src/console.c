/**
 * The debug console (--debug only; to-do 14, SPEC 10.3). See console.h.
 *
 * Every command is a row of s_commands: a later feature (warp, the cheats)
 * adds a row and a function. Commands run on the executor thread, inside
 * overlay.cpp's drawing of the console, so one that touches the game only
 * sets something the game thread reads (conker_SetDebugPage, ...).
 *
 * The log: what con_out detailed and full show is read back from the file the
 * game's stdout and stderr already go to (run.ps1's run log, or the log of its
 * own in <save dir>\logs; a second reader, so nothing changes in how the game
 * writes it, and a crash loses nothing). Output that is no file (a pipe, a
 * console window) has no log to show: the console says so. The game's own
 * lines arrive a frame later and only while the console is open; on opening
 * it reads back at most LOG_BACKLOG of what it missed.
 *
 * Commands and their replies also go to the log as [CONSOLE] lines (a Report
 * Bug zip shows what was done), which the reader then leaves out.
 */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <io.h>

#include "cheats.h"
#include "console.h"
#include "warp.h"
#include "edit_save.h"
#include "photo_mode.h"
#include "settings.h"
#include "controls.h"
#include "debug_tools.h"
#include "cmd_pipe.h"
#include "dev_logs.h"

int xbox_PadLiveLine(const char *line);
long overlay_dev_shot(const char *path, char *why, size_t why_size);
int overlay_dev_shot_result(long seq, char *msg, size_t size);
double overlay_fps_now(void);
void conker_PlayCue(int cue);
void conker_ImpAnim(int anim);
void overlay_set_metrics(int level);
int overlay_metrics(void);
void conker_StartSoak(void);
int conker_SoakState(void);
void conker_SetDebugPage(int page);
int conker_DebugPage(void);
void overlay_binds_changed(void);

/* ---- the lines -------------------------------------------------------------- */

#define MAX_LINES 6000
#define DROP_LINES 1000         /* dropped at once when full */
#define MAX_TEXT 400            /* longer log lines are cut (a console, not a viewer) */

typedef struct {
    char *text;
    int kind;
} Line;

static Line s_lines[MAX_LINES];
static int s_nlines;
static int s_shown[MAX_LINES];  /* which lines con_out shows */
static int s_nshown;
static int s_shown_stale;       /* s_shown wants making again */

/* con_out: 0 simple, 1 detailed, 2 full; -1 before settings.ini is read. */
static const char *const s_out_names[] = { "simple", "detailed", "full" };
static int s_out = -1;

static int out_level(void)
{
    if (s_out < 0) {
        const char *v = settings_get("con_out");
        s_out = !strcmp(v, "detailed") ? 1 : !strcmp(v, "full") ? 2 : 0;
    }
    return s_out;
}

static int kind_shown(int kind)
{
    return kind <= CONSOLE_ERROR
        || (kind == CONSOLE_TAGGED && out_level() >= 1)
        || out_level() >= 2;
}

/* Which lines go first when the buffer is full (to-do 207): log lines con_out
 * hides, then log lines it shows, then what was typed and its answers. The
 * log alone (the audio log: ~1,800 lines a second) used to push a reply out
 * within seconds. */
static int drop_rank(int kind)
{
    if (kind >= CONSOLE_TAGGED)
        return kind_shown(kind) ? 1 : 0;
    return 2;
}

/* Frees DROP_LINES lines, the oldest of the lowest rank first; the order of
 * the rest stays. */
static void drop_lines(void)
{
    int left = DROP_LINES;

    for (int rank = 0; rank <= 2 && left; rank++) {
        int count = 0, keep = 0;

        /* The oldest `left` lines of this rank go. */
        for (int i = 0; i < s_nlines; i++) {
            if (drop_rank(s_lines[i].kind) == rank && count < left) {
                free(s_lines[i].text);
                s_lines[i].text = NULL;
                count++;
            }
        }
        for (int i = 0; i < s_nlines; i++)
            if (s_lines[i].text)
                s_lines[keep++] = s_lines[i];
        s_nlines = keep;
        left -= count;
    }
    s_shown_stale = 1;
}

static void add_line(int kind, const char *text, size_t len)
{
    Line *l;

    if (s_nlines == MAX_LINES)
        drop_lines();
    if (len > MAX_TEXT)
        len = MAX_TEXT;
    l = &s_lines[s_nlines];
    l->text = (char *)malloc(len + 1);
    if (!l->text)
        return;
    memcpy(l->text, text, len);
    l->text[len] = '\0';
    l->kind = kind;
    if (!s_shown_stale && kind_shown(kind))
        s_shown[s_nshown++] = s_nlines;
    s_nlines++;
}

static void make_shown(void)
{
    s_nshown = 0;
    for (int i = 0; i < s_nlines; i++)
        if (kind_shown(s_lines[i].kind))
            s_shown[s_nshown++] = i;
    s_shown_stale = 0;
}

int console_shown(void)
{
    if (s_shown_stale)
        make_shown();
    return s_nshown;
}

const char *console_shown_line(int i, int *kind)
{
    const Line *l;

    if (s_shown_stale)
        make_shown();
    if (i < 0 || i >= s_nshown)
        return "";
    l = &s_lines[s_shown[i]];
    *kind = l->kind;
    return l->text;
}

/* While the command pipe's line runs (to-do 174), everything the console
 * prints is also kept here: the pipe's reply. */
#define CAPTURE_MAX (256 * 1024)

static char *s_cap;
static size_t s_cap_n;
static int s_capturing, s_cap_errors;
static int s_errors;            /* every error so far: a script stops at a new one */

static void cap_add(const char *text)
{
    size_t len = strlen(text);

    if (!s_capturing)
        return;
    if (!s_cap)
        s_cap = (char *)malloc(CAPTURE_MAX);
    if (!s_cap || s_cap_n + len + 2 > CAPTURE_MAX)
        return;
    memcpy(s_cap + s_cap_n, text, len);
    s_cap_n += len;
    s_cap[s_cap_n++] = '\n';
    s_cap[s_cap_n] = '\0';
}

/* A reply or an error: on the console and in the log. */
static void say(int kind, const char *fmt, va_list ap)
{
    char text[MAX_TEXT + 1];
    int n = vsnprintf(text, sizeof(text), fmt, ap);

    if (n < 0)
        return;
    add_line(kind, text, strlen(text));
    fprintf(stderr, "[CONSOLE] %s\n", text);
    cap_add(text);
    if (kind == CONSOLE_ERROR) {
        s_errors++;
        if (s_capturing)
            s_cap_errors++;
    }
}

static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    say(CONSOLE_REPLY, fmt, ap);
    va_end(ap);
}

static void complain(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    say(CONSOLE_ERROR, fmt, ap);
    va_end(ap);
}

/* ---- the log ---------------------------------------------------------------- */

#define LOG_BACKLOG (256 * 1024)

static HANDLE s_log = INVALID_HANDLE_VALUE;
static int s_log_tried;
static LONGLONG s_log_pos;
static char s_log_part[MAX_TEXT + 1];   /* a line not yet ended */
static size_t s_log_part_n;
static int s_log_skip;                  /* in a line cut by the backlog limit */

static void open_log(void)
{
    wchar_t path[MAX_PATH * 2];
    HANDLE out;

    s_log_tried = 1;
    if (_fileno(stderr) < 0)
        return;
    out = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (out == INVALID_HANDLE_VALUE || GetFileType(out) != FILE_TYPE_DISK)
        return;
    if (!GetFinalPathNameByHandleW(out, path, MAX_PATH * 2, 0))
        return;
    s_log = CreateFileW(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
}

static void log_line(const char *text, size_t len)
{
    while (len && text[len - 1] == '\r')
        len--;
    if (!len || !strncmp(text, "[CONSOLE]", 9))
        return;
    add_line(text[0] == '[' ? CONSOLE_TAGGED : CONSOLE_PLAIN, text, len);
}

void console_poll_log(void)
{
    static char buf[64 * 1024];
    LARGE_INTEGER size, at;
    DWORD got = 0;
    const char *p, *end;

    if (!s_log_tried)
        open_log();
    if (s_log == INVALID_HANDLE_VALUE)
        return;
    if (!GetFileSizeEx(s_log, &size) || size.QuadPart == s_log_pos)
        return;
    if (size.QuadPart - s_log_pos > LOG_BACKLOG) {
        s_log_pos = size.QuadPart - LOG_BACKLOG;
        s_log_part_n = 0;
        s_log_skip = 1;
    }
    at.QuadPart = s_log_pos;
    if (!SetFilePointerEx(s_log, at, NULL, FILE_BEGIN)
        || !ReadFile(s_log, buf, sizeof(buf), &got, NULL) || !got)
        return;
    s_log_pos += got;
    p = buf;
    end = buf + got;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t len = (size_t)((nl ? nl : end) - p);

        if (s_log_skip) {
            if (nl)
                s_log_skip = 0;
        } else if (!nl) {
            /* The rest arrives later; what does not fit is cut. */
            size_t room = sizeof(s_log_part) - s_log_part_n;
            if (len > room)
                len = room;
            memcpy(s_log_part + s_log_part_n, p, len);
            s_log_part_n += len;
        } else if (s_log_part_n) {
            size_t room = sizeof(s_log_part) - s_log_part_n;
            if (len > room)
                len = room;
            memcpy(s_log_part + s_log_part_n, p, len);
            log_line(s_log_part, s_log_part_n + len);
            s_log_part_n = 0;
        } else {
            log_line(p, len);
        }
        if (!nl)
            break;
        p = nl + 1;
    }
}

/* ---- Rare's debug pages ----------------------------------------------------- */

const char *console_page_name(int page)
{
    /* What each page showed in the field (2026-11-08). No name: the game has
     * no such page (2, 14, 18, 20, 21: its table goes straight to the end) or
     * it prints nothing (6). The per-module times on 3, 4 and 5 stay 0: the
     * code that measured them is not in this build (nothing but a reset writes
     * [0x007FA008]). */
    static const char *const names[CONSOLE_DEBUG_PAGES] = {
        "Off",
        "Memory used",
        NULL,
        "Performance (fps, effect culling; module times are 0)",
        "Frame timers, asset loads, DVD reads",
        "Shadow timers",
        NULL,
        "Positions (player, camera, floor, water)",
        "Camera",
        "Camera and character attributes",
        "Camera flags",
        "Player stats (lives, energy, cash)",
        "Character",
        "Character animation",
        NULL,
        "Malloc stats",
        "Character draw passes",
        "Malloc stack, memory totals",
        NULL,
        "Memory total",
        NULL,
        NULL,
        "Xbox Live / System Link connections",
        "Physics allocations",
        "Radio owners",
        "Effects (active / total)",
    };
    return page >= 0 && page < CONSOLE_DEBUG_PAGES ? names[page] : NULL;
}

int console_page_step(int page, int dir)
{
    do
        page = (page + dir + CONSOLE_DEBUG_PAGES) % CONSOLE_DEBUG_PAGES;
    while (!console_page_name(page));
    return page;
}

/* ---- the commands ----------------------------------------------------------- */

#define MAX_ARGS 8

typedef struct {
    const char *name;
    const char *args;
    const char *help;
    void (*run)(int argc, char **argv);
} Command;

static void cmd_help(int argc, char **argv);

static void cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < s_nlines; i++)
        free(s_lines[i].text);
    s_nlines = 0;
    s_nshown = 0;
    s_shown_stale = 0;
}

static void cmd_con_out(int argc, char **argv)
{
    if (argc < 2) {
        reply("con_out is %s", s_out_names[out_level()]);
        return;
    }
    for (int i = 0; i < 3; i++)
        if (!strcmp(argv[1], s_out_names[i])) {
            s_out = i;
            s_shown_stale = 1;
            settings_set("con_out", s_out_names[i]);
            if (!s_log_tried)
                open_log();
            if (i && s_log == INVALID_HANDLE_VALUE)
                complain("This run's log is not a file (a pipe or a console), "
                         "so only command replies can be shown");
            reply("con_out = %s", s_out_names[i]);
            return;
        }
    complain("con_out wants simple, detailed or full");
}

static void cmd_metrics(int argc, char **argv)
{
    static const char *const names[] = {
        "off", "fps", "fps, frame times and memory",
        "fps, frame times, memory and what limits"
    };

    if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] <= '3' && !argv[1][1])
        overlay_set_metrics(argv[1][0] - '0');
    else if (argc >= 2) {
        complain("metrics wants 0, 1, 2 or 3");
        return;
    }
    reply("metrics = %d (%s)", overlay_metrics(), names[overlay_metrics()]);
}

static void cmd_debugpage(int argc, char **argv)
{
    int page = conker_DebugPage();

    if (argc >= 2) {
        char *end;
        long n;
        if (!strcmp(argv[1], "next"))
            n = console_page_step(page, 1);
        else if (!strcmp(argv[1], "prev"))
            n = console_page_step(page, -1);
        else if (!strcmp(argv[1], "off"))
            n = 0;
        else {
            n = strtol(argv[1], &end, 0);
            if (*end || !console_page_name((int)n)) {
                complain("debugpage wants a page number (no value: the list), "
                         "next, prev or off");
                return;
            }
        }
        page = (int)n;
        conker_SetDebugPage(page);
    } else {
        for (int i = 1; i < CONSOLE_DEBUG_PAGES; i++)
            if (console_page_name(i))
                reply("  %2d  %s", i, console_page_name(i));
    }
    reply("debugpage = %d (%s)", page, console_page_name(page));
}

static void cmd_soak(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    switch (conker_SoakState()) {
    case 2:
        complain("The soak test is running (close the game to stop it)");
        break;
    case 1:
        conker_StartSoak();
        reply("Soak test started: every map in turn, then the chapters, over "
              "and over");
        break;
    default:
        complain("The soak test starts only from the main menu");
        break;
    }
}

/* The cheats (SPEC 10.3): no value swaps it. */
static void cmd_cheat(int argc, char **argv)
{
    int id = cheat_of_command(argv[0]);

    if (argc >= 2) {
        if (!strcmp(argv[1], "on") || !strcmp(argv[1], "1"))
            cheat_set(id, 1);
        else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "0"))
            cheat_set(id, 0);
        else {
            complain("%s wants on or off (no value: swaps it)", argv[0]);
            return;
        }
    } else {
        cheat_set(id, !cheat_is_set(id));
    }
    reply("%s (%s) is %s", cheat_name(id), argv[0],
          cheat_is_set(id) ? "on" : "off");
}

static void cmd_cue(int argc, char **argv)
{
    char *end;
    long cue;

    if (argc < 2) {
        complain("cue wants a sound's number, 0x7A1 for example");
        return;
    }
    cue = strtol(argv[1], &end, 0);
    if (*end || cue <= 0 || cue >= 0xDDA) {
        complain("cue wants a number from 1 to 0xDD9");
        return;
    }
    conker_PlayCue((int)cue);
    reply("Playing sound 0x%lX", cue);
}

/* Case-insensitive: does `s` hold `part`? */
static int strstr_ci(const char *s, const char *part)
{
    size_t n = strlen(part);
    for (; *s; s++)
        if (!_strnicmp(s, part, n))
            return 1;
    return 0;
}

/* tune list [part of a name] | tune <number|name> | tune stop (to-do 159): the
 * jukebox's tunes, the music manager's sequences (AD_SEQ_*). */
static void cmd_tune(int argc, char **argv)
{
    const int n = conker_TuneCount();
    char *end;
    long t;

    if (argc >= 2 && !_stricmp(argv[1], "list")) {
        int shown = 0;
        for (int i = 1; i < n; i++) {
            const char *name = conker_TuneName(i);
            if (!name || (argc >= 3 && !strstr_ci(name, argv[2])))
                continue;
            reply("%4d  %s%s", i, name, conker_TuneWhere(i) ? "  (can't play here)" : "");
            shown++;
        }
        if (!shown)
            complain("No tune has \"%s\" in its name", argc >= 3 ? argv[2] : "");
        return;
    }
    if (argc == 2 && !_stricmp(argv[1], "stop")) {
        if (!conker_TunePlaying() && !conker_TuneLoading()) {
            complain("The jukebox isn't playing a tune");
            return;
        }
        conker_TuneStop();
        reply("Stopping tune %d; the music it replaced comes back",
              conker_TunePlaying() ? conker_TunePlaying() : conker_TuneLoading());
        return;
    }
    if (argc != 2) {
        complain("tune list [name] | tune <number|name> | tune stop");
        return;
    }
    t = strtol(argv[1], &end, 0);
    if (*end) {                         /* a name: whole, else the only one with it */
        int found = 0, hits = 0;
        for (int i = 1; i < n; i++) {
            const char *name = conker_TuneName(i);
            if (!name)
                continue;
            if (!_stricmp(name, argv[1]) || (!_strnicmp(name, "AD_SEQ_", 7)
                                             && !_stricmp(name + 7, argv[1]))) {
                found = i;
                hits = 1;
                break;
            }
            if (strstr_ci(name, argv[1])) {
                found = i;
                hits++;
            }
        }
        if (!hits) {
            complain("No tune has \"%s\" in its name", argv[1]);
            return;
        }
        if (hits > 1) {
            complain("%d tunes have \"%s\" in their name: tune list %s", hits, argv[1], argv[1]);
            return;
        }
        t = found;
    }
    if (t < 1 || t >= n || !conker_TuneName((int)t)) {
        complain("tune wants a number from tune list (1 to %d)", n - 1);
        return;
    }
    if (conker_TuneWhere((int)t)) {     /* to-do 193 */
        complain("Tune %ld %s can't play here. %s", t, conker_TuneName((int)t),
                 conker_TuneWhere((int)t));
        return;
    }
    conker_TunePlay((int)t);
    reply("Playing tune %ld %s", t, conker_TuneName((int)t));
}

static void cmd_imp(int argc, char **argv)
{
    if (argc < 2) {
        complain("imp wants an animation's number");
        return;
    }
    conker_ImpAnim((int)strtol(argv[1], NULL, 0));
    reply("Asked the fire imp for animation %s", argv[1]);
}

/* warp list | warp <level> | warp cp list [level] | warp cp <checkpoint>. A level
 * by its number or the start of its name. */
static int warp_level_arg(const char *arg)
{
    char *end;
    long n = strtol(arg, &end, 0);

    if (!*end)
        return conker_WarpLevelName((int)n) ? (int)n : -1;
    for (int i = 0; i < WARP_LEVELS; i++) {
        const char *name = conker_WarpLevelName(i);
        if (name && !_strnicmp(name, arg, strlen(arg)))
            return i;
    }
    return -1;
}

static void warp_go(int cp, int level)
{
    const char *no = conker_WarpRefused();

    if (no) {
        complain("%s", no);
        return;
    }
    /* A level by itself: its first checkpoint, or as the soak test loads it if none. */
    if (cp < 0)
        cp = conker_WarpFirstCheckpoint(level);
    if (cp >= 0)
        conker_WarpToCheckpoint(cp);
    else
        conker_WarpSoak(level);
    reply("Warping. Nothing is saved until the main menu");
}

/* A level with scenes, by its number or the start of its name. */
static int cutscene_level_arg(const char *arg)
{
    char *end;
    long n = strtol(arg, &end, 0);

    if (!*end)
        return conker_CutsceneLevelName((int)n) ? (int)n : -1;
    for (int i = 0; i < WARP_LEVELS; i++) {
        const char *name = conker_CutsceneLevelName(i);
        if (name && !_strnicmp(name, arg, strlen(arg)))
            return i;
    }
    return -1;
}

/* Level and scene from two words; complains and returns 0 if they aren't. */
static int cutscene_args(const char *lv, const char *sc, int *level, int *scene)
{
    char *end;
    long n;

    *level = cutscene_level_arg(lv);
    if (*level < 0) {
        complain("No scenes in a level called %s (cutscene list shows them)", lv);
        return 0;
    }
    n = strtol(sc, &end, 0);
    if (*end || n < 0 || n > 63 || !(conker_CutsceneScenes(*level) >> n & 1u)) {
        complain("%s has no scene %s (cutscene list %d shows them)",
                 conker_CutsceneLevelName(*level), sc, *level);
        return 0;
    }
    *scene = (int)n;
    return 1;
}

/* cutscene list [level] | cutscene <level> <scene> */
static void cmd_cutscene(int argc, char **argv)
{
    int level, scene;
    const char *no;

    if (argc >= 2 && !strcmp(argv[1], "list")) {
        char line[200];
        int only = argc >= 3 ? cutscene_level_arg(argv[2]) : -1;

        if (argc >= 3 && only < 0) {
            complain("No scenes in a level called %s (cutscene list shows them)", argv[2]);
            return;
        }
        for (int p = 0; p < WARP_LEVELS; p++) {
            const int i = conker_LevelSorted(p);
            unsigned long long s = conker_CutsceneScenes(i);
            size_t at = 0;
            int n, from = -1;

            if (!conker_CutsceneLevelName(i) || (only >= 0 && i != only))
                continue;
            line[0] = '\0';
            for (n = 0; n <= 64; n++) {
                int on = n < 64 && (s >> n & 1u);
                if (on && from < 0)
                    from = n;
                if (!on && from >= 0) {
                    at += (size_t)snprintf(line + at, sizeof(line) - at,
                                           from == n - 1 ? "%s%d" : "%s%d-%d",
                                           at ? ", " : "", from, n - 1);
                    from = -1;
                }
            }
            reply("  %2d  %-20s %s", i, conker_CutsceneLevelName(i), line);
        }
    } else if (argc >= 3) {
        if (!cutscene_args(argv[1], argv[2], &level, &scene))
            return;
        no = conker_CutsceneRefused(level, 0);
        if (no) {
            complain("%s", no);
            return;
        }
        conker_CutsceneWatch(level, scene);
        reply("Playing it, then back here. Hold to skip");
    } else {
        complain("cutscene list [level], cutscene <level> <scene>");
    }
}

static void cmd_noclip(int argc, char **argv)
{
    int on = !conker_Noclip();
    const char *no;

    if (argc >= 2) {
        if (!strcmp(argv[1], "on") || !strcmp(argv[1], "1"))
            on = 1;
        else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "0"))
            on = 0;
        else {
            complain("noclip wants on or off (no value: swaps it)");
            return;
        }
    }
    no = conker_NoclipRefused();
    if (on && no) {
        complain("%s", no);
        return;
    }
    conker_SetNoclip(on);
    reply("Noclip is %s", on ? "on" : "off");
}

/* editSave <1|2|3> <word><number> (to-do 147): the save starts at that part. */
static void cmd_editsave(int argc, char **argv)
{
    char msg[256];
    int slot, chapter, part;

    if (argc == 2 && !_stricmp(argv[1], "list")) {
        for (int c = 0; c < ES_CHAPTERS; c++) {
            char line[512];
            int at = snprintf(line, sizeof line, "%-7s %s:", edit_save_word(c),
                              edit_save_chapter_name(c));
            for (int p = 1; p <= edit_save_parts(c) && at < (int)sizeof line; p++)
                at += snprintf(line + at, sizeof line - (size_t)at, "%s %d %s",
                               p > 1 ? "," : "", p, edit_save_part_name(c, p));
            reply("%s", line);
        }
        return;
    }
    if (argc != 3 || argv[1][1] || argv[1][0] < '1' || argv[1][0] > '3') {
        complain("editSave <1|2|3> <word><number> (editSave 1 slop2; editSave list)");
        return;
    }
    slot = argv[1][0] - '0';
    if (!edit_save_parse(argv[2], &chapter, &part)) {
        complain("No part called %s (editSave list shows them)", argv[2]);
        return;
    }
    if (conker_EditSave(slot, chapter, part, msg, sizeof msg))
        complain("%s", msg);
    else
        reply("%s. The old save is in edit_save_backups", msg);
}

static void cmd_wireframe(int argc, char **argv)
{
    static const char *const names[3] = { "off", "models", "all" };
    int to = conker_Wireframe() ? 0 : 1;

    if (argc >= 2) {
        for (to = 0; to < 3 && strcmp(argv[1], names[to]); to++)
            ;
        if (to == 3) {
            complain("wireframe wants models, all or off (no value: swaps models and off)");
            return;
        }
    }
    conker_SetWireframe(to);
    reply("Wireframe %s", names[to]);
}

static void cmd_timescale(int argc, char **argv)
{
    int to = conker_TimescaleLast();

    if (argc >= 2) {
        char *end;
        long n = strtol(argv[1], &end, 0);
        if (*end || n < 0 || n > 300) {
            complain("timescale wants 0 to 300 (100 = normal; no value: swaps with the last)");
            return;
        }
        to = (int)n;
    }
    conker_SetTimescale(to);
    reply("Timescale %d%%", to);
}

static void cmd_freecam(int argc, char **argv)
{
    int on = !photo_freecam();

    if (argc >= 2) {
        if (!strcmp(argv[1], "on") || !strcmp(argv[1], "1"))
            on = 1;
        else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "0"))
            on = 0;
        else {
            complain("freecam wants on or off (no value: swaps it)");
            return;
        }
    }
    photo_freecam_set(on);
    reply("Freecam %s", on ? "on (not in multiplayer, under pause or the pause menu)" : "off");
}

static void cmd_warp(int argc, char **argv)
{
    char name[80];
    int level;

    if (argc < 2) {
        complain("warp list, warp <level>, warp soak <level>, warp cp list [level], "
                 "warp cp <number>, warp scene <level> <scene>");
    } else if (!strcmp(argv[1], "soak") && argc >= 3) {
        const char *no = conker_WarpRefused();

        level = warp_level_arg(argv[2]);
        if (level < 0) {
            complain("No level called %s (warp list shows them)", argv[2]);
            return;
        }
        if (no) {
            complain("%s", no);
            return;
        }
        conker_WarpSoak(level);
        reply("Warping, as the soak test loads it. Nothing is saved until the main menu");
    } else if (!strcmp(argv[1], "scene") && argc >= 4) {
        const char *no;
        int scene;

        if (!cutscene_args(argv[2], argv[3], &level, &scene))
            return;
        no = conker_CutsceneRefused(level, 1);
        if (no) {
            complain("%s", no);
            return;
        }
        conker_WarpToCutscene(level, scene);
        reply("Warping. Nothing is saved until the main menu");
    } else if (!strcmp(argv[1], "list")) {
        for (int p = 0; p < WARP_LEVELS; p++) {
            const int i = conker_LevelSorted(p);
            if (conker_WarpLevelName(i))
                reply("  %2d  %s", i, conker_WarpLevelName(i));
        }
    } else if (!strcmp(argv[1], "cp") && argc >= 3 && !strcmp(argv[2], "list")) {
        int only = argc >= 4 ? warp_level_arg(argv[3]) : -1;
        if (argc >= 4 && only < 0) {
            complain("No level called %s (warp list shows them)", argv[3]);
            return;
        }
        for (int i = 0; i < WARP_CHECKPOINTS; i++) {
            level = conker_WarpCheckpoint(i, name, sizeof(name));
            if (level >= 0 && (only < 0 || level == only))
                reply("  %3d  level %2d  %s", i, level, name);
        }
    } else if (!strcmp(argv[1], "cp") && argc >= 3) {
        char *end;
        long cp = strtol(argv[2], &end, 0);
        if (*end || conker_WarpCheckpoint((int)cp, NULL, 0) < 0) {
            complain("No checkpoint %s (warp cp list shows them)", argv[2]);
            return;
        }
        warp_go((int)cp, -1);
    } else {
        level = warp_level_arg(argv[1]);
        if (level < 0) {
            complain("No level called %s (warp list shows them)", argv[1]);
            return;
        }
        warp_go(-1, level);
    }
}

/* ---- position, spots, values, characters (to-dos 155, 156, 158) -------------- */

static const char *level_name(int level)
{
    /* Multiplayer's maps as the game's map list names them (user
     * 2026-11-11; Beach Dead conker-notes 2026-10-27). Not warp targets. */
    static const char *const mp[] = {
        "Beach Dead", "Fortress Deux", "Castle Von Tedistein", "A Bridge Too Narrow",
        "Three Towers", "Doon", "T.M.S Spamono", "The Ditch",
    };
    static char b[24];
    const char *n = conker_WarpLevelName(level);
    if (n)
        return n;
    if (level >= 0x4B && level <= 0x52 && mp[level - 0x4B])
        return mp[level - 0x4B];
    snprintf(b, sizeof b, "0x%02X", level);
    return b;
}

/* "x y z" as the console prints a place. */
static void say_place(const char *what, float x, float y, float z)
{
    reply("%s %.0f %.0f %.0f (%s)", what, x, y, z, level_name(conker_Level()));
}

static void cmd_pos(int argc, char **argv)
{
    float x, y, z;
    (void)argc; (void)argv;
    if (!conker_PlayerPos(&x, &y, &z)) {
        complain("Not in a level (pos works where Conker is)");
        return;
    }
    say_place("Conker is at", x, y, z);
}

static int parse_float(const char *s, float *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end)
        return 0;
    *out = (float)v;
    return 1;
}

/* tp <x> <y> <z> | tp <character number> | tp cam */
static void cmd_tp(int argc, char **argv)
{
    float x, y, z;

    if (!conker_PlayerPos(&x, &y, &z)) {
        complain("Not in a level (tp works where Conker is)");
        return;
    }
    if (argc == 4) {
        if (!parse_float(argv[1], &x) || !parse_float(argv[2], &y)
            || !parse_float(argv[3], &z)) {
            complain("tp wants three numbers: tp <x> <y> <z>");
            return;
        }
    } else if (argc == 2 && !_stricmp(argv[1], "cam")) {
        if (!photo_freecam_pos(&x, &y, &z)) {
            complain("tp cam needs freecam on (freecam), then fly where he goes");
            return;
        }
    } else if (argc == 2 && isdigit((unsigned char)argv[1][0])) {
        int n = atoi(argv[1]);
        if (!conker_CharPos(n, &x, &y, &z)) {
            complain("No character %d (chars lists the ones near)", n);
            return;
        }
        x += 40.0f;                     /* beside it, not inside */
    } else {
        complain("tp <x> <y> <z> | tp <number from chars> | tp cam (freecam's camera)");
        return;
    }
    conker_Teleport(x, y, z);
    say_place("Conker goes to", x, y, z);
}

/* Spots: <save dir>\spots.txt, one "<level> <name> <x> <y> <z>" a line. */
#define MAX_SPOTS 256
typedef struct { int level; char name[40]; float x, y, z; } Spot;

static int spots_load(Spot *sp)
{
    char path[MAX_PATH], line[256];
    FILE *f;
    int n = 0;

    snprintf(path, sizeof path, "%s\\spots.txt", settings_save_dir());
    if (fopen_s(&f, path, "r") || !f)
        return 0;
    while (n < MAX_SPOTS && fgets(line, sizeof line, f)) {
        Spot *s = &sp[n];
        if (line[0] == ';' || line[0] == '#')
            continue;
        if (sscanf_s(line, "%x %39s %f %f %f", &s->level, s->name,
                     (unsigned)sizeof s->name, &s->x, &s->y, &s->z) == 5)
            n++;
    }
    fclose(f);
    return n;
}

static int spots_save(const Spot *sp, int n)
{
    char path[MAX_PATH];
    FILE *f;

    snprintf(path, sizeof path, "%s\\spots.txt", settings_save_dir());
    if (fopen_s(&f, path, "w") || !f)
        return 0;
    fputs("; Saved spots (console: spot save|go|list|delete <name>): level (hex), name, x y z\n", f);
    for (int i = 0; i < n; i++)
        fprintf(f, "%02X %s %.1f %.1f %.1f\n", sp[i].level, sp[i].name, sp[i].x, sp[i].y,
                sp[i].z);
    return fclose(f) == 0;
}

static void cmd_spot(int argc, char **argv)
{
    static Spot sp[MAX_SPOTS];
    int n = spots_load(sp), level = conker_Level(), i, found = -1;
    float x, y, z;

    if (argc == 2 && !_stricmp(argv[1], "list")) {
        int shown = 0;
        for (i = 0; i < n; i++)
            if (sp[i].level == level) {
                reply("%s: %.0f %.0f %.0f", sp[i].name, sp[i].x, sp[i].y, sp[i].z);
                shown++;
            }
        reply("%d spot%s in %s (%d in all, spots.txt in the save folder)", shown,
              shown == 1 ? "" : "s", level_name(level), n);
        return;
    }
    if (argc != 3 || (_stricmp(argv[1], "save") && _stricmp(argv[1], "go")
                      && _stricmp(argv[1], "delete"))) {
        complain("spot save|go|delete <name> | spot list (spots are kept per level)");
        return;
    }
    for (i = 0; i < n; i++)
        if (sp[i].level == level && !_stricmp(sp[i].name, argv[2]))
            found = i;
    if (!_stricmp(argv[1], "save")) {
        if (!conker_PlayerPos(&x, &y, &z)) {
            complain("Not in a level");
            return;
        }
        if (found < 0) {
            if (n == MAX_SPOTS) {
                complain("Too many spots (%d): delete some first", MAX_SPOTS);
                return;
            }
            found = n++;
            sp[found].level = level;
            strncpy_s(sp[found].name, sizeof sp[found].name, argv[2], _TRUNCATE);
        }
        sp[found].x = x;
        sp[found].y = y;
        sp[found].z = z;
        if (!spots_save(sp, n))
            complain("Could not write spots.txt");
        else
            say_place("Spot saved:", x, y, z);
        return;
    }
    if (found < 0) {
        complain("No spot %s in %s (spot list)", argv[2], level_name(level));
        return;
    }
    if (!_stricmp(argv[1], "delete")) {
        sp[found] = sp[--n];
        if (!spots_save(sp, n))
            complain("Could not write spots.txt");
        else
            reply("Spot %s deleted", argv[2]);
        return;
    }
    if (!conker_PlayerPos(&x, &y, &z)) {
        complain("Not in a level");
        return;
    }
    conker_Teleport(sp[found].x, sp[found].y, sp[found].z);
    say_place("Conker goes to", sp[found].x, sp[found].y, sp[found].z);
}

/* set energy|lives|cash|oxygen <n> */
static void cmd_set(int argc, char **argv)
{
    static const char *const names[] = { "energy", "lives", "cash", "oxygen" };
    static const int lo[] = { 1, 0, 0, 0 }, hi[] = { 6, 100, 1000000, 72 };
    char *end;
    long v;
    int i;

    for (i = 0; argc == 3 && i < 4 && _stricmp(argv[1], names[i]); i++)
        ;
    if (argc != 3 || i == 4) {
        complain("set energy <1-6> | lives <0-100> | cash <0-1000000> | oxygen <seconds of air, 0-72>");
        return;
    }
    v = strtol(argv[2], &end, 10);
    if (*end || v < lo[i] || v > hi[i]) {
        complain("set %s wants %d to %d", names[i], lo[i], hi[i]);
        return;
    }
    if (!conker_SetValue(i, (int)v)) {
        complain("Not in the story (set works on Conker in a story level)");
        return;
    }
    reply("%s set to %ld%s", names[i], v, i == 3 ? " s" : "");
}

static void cmd_kill(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!conker_Kill()) {
        complain("Not in the story (kill works on Conker in a story level)");
        return;
    }
    reply("Conker dies (a life lost)");
}

static void cmd_chars(int argc, char **argv)
{
    ConkerChar list[40];
    int n = conker_CharsNear(list, 40), max = argc >= 2 ? atoi(argv[1]) : 20;

    (void)argv;
    if (n < 0) {
        complain("Not in a level");
        return;
    }
    if (max <= 0 || max > 40)
        max = 20;
    for (int i = 0; i < n && i < max; i++)
        reply("%3d  %-24s %6.0f away  energy %d", list[i].index, list[i].name,
              list[i].dist, list[i].energy);
    reply("%d character%s (nearest first; tp <number> goes there)", n, n == 1 ? "" : "s");
}

/* ---- key binds (to-do 157): settings.ini's [binds]; the keys reach us through
 * overlay.cpp's window hook (--debug, not while typing). ---- */
static int bind_key_ok(const char *name)
{
    int vk = controls_parse_key(name);
    return vk > 0 && vk < 0x100 && vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_MBUTTON
           && vk != VK_XBUTTON1 && vk != VK_XBUTTON2;
}

static void cmd_bind(int argc, char **argv)
{
    char cmd[200] = "";
    int n = settings_bind_count();

    if (argc == 1 || (argc == 2 && !_stricmp(argv[1], "list"))) {
        for (int i = 0; i < n; i++)
            reply("%s: %s", settings_bind_key(i), settings_bind_command(i));
        reply("%d key%s bound (bind <key> <command>, unbind <key>, bind reset)", n,
              n == 1 ? "" : "s");
        return;
    }
    if (argc == 2 && !_stricmp(argv[1], "reset")) {
        settings_binds_reset();
        overlay_binds_changed();
        reply("Key binds back to the defaults (none)");
        return;
    }
    if (argc < 3) {
        complain("bind <key> <command> | bind list | bind reset (bind f5 warp Windy)");
        return;
    }
    if (!bind_key_ok(argv[1])) {
        complain("No key called %s (F3's names: f5, k, numpad1, home, ...; no mouse buttons)",
                 argv[1]);
        return;
    }
    for (int i = 2; i < argc; i++) {
        if (i > 2)
            strcat_s(cmd, sizeof cmd, " ");
        strcat_s(cmd, sizeof cmd, argv[i]);
    }
    if (!settings_bind_set(argv[1], cmd)) {
        complain("Could not save it (48 keys at most)");
        return;
    }
    overlay_binds_changed();
    reply("%s runs: %s", argv[1], cmd);
}

static void cmd_unbind(int argc, char **argv)
{
    if (argc == 2 && !_stricmp(argv[1], "all")) {
        settings_binds_reset();
        overlay_binds_changed();
        reply("No key bound");
        return;
    }
    if (argc != 2) {
        complain("unbind <key> | unbind all");
        return;
    }
    for (int i = 0; i < settings_bind_count(); i++)
        if (!_stricmp(settings_bind_key(i), argv[1])) {
            settings_bind_set(argv[1], NULL);
            overlay_binds_changed();
            reply("%s runs nothing now", argv[1]);
            return;
        }
    complain("%s is not bound (bind list)", argv[1]);
}

/* ---- dev tools: scripts, waits, pad, shot, mark (to-dos 166-168, 174) -------- */

/* What a script or a pipe line waits for before its next line or its reply.
 * A command that takes time sets the hold of the runner whose line it is
 * (s_hold_for); typed lines and binds have none and don't wait. */
enum { HOLD_NONE, HOLD_WAIT, HOLD_LOAD, HOLD_SHOT, HOLD_SCRIPT, HOLD_SCENE };

typedef struct {
    int kind;
    ULONGLONG start, until, quiet_since;
    int started, level0;
    long seq;
} Hold;

static Hold s_pipe_hold, s_script_hold;
static Hold *s_hold_for;        /* the runner of the line running now */
static int s_skip_echo;         /* the pipe's own line: no "> line" in its reply */
static const char *s_cur_line;  /* that line as written (echo, mark: the rest) */

static void run_line(const char *line, int typed);

/* The text after the command's first word, spaces kept. */
static const char *line_rest(void)
{
    const char *p = s_cur_line ? s_cur_line : "";
    while (*p && isspace((unsigned char)*p))
        p++;
    while (*p && !isspace((unsigned char)*p))
        p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int need_runner(const char *cmd)
{
    if (s_hold_for)
        return 1;
    complain("%s works in a script (exec) or through the command pipe", cmd);
    return 0;
}

static void cmd_wait(int argc, char **argv)
{
    char *end;
    double s = argc >= 2 ? strtod(argv[1], &end) : -1;

    if (argc < 2 || *end || s < 0 || s > 3600) {
        complain("wait wants seconds (0-3600, 0.5 works)");
        return;
    }
    if (!need_runner("wait"))
        return;
    s_hold_for->kind = HOLD_WAIT;
    s_hold_for->until = GetTickCount64() + (ULONGLONG)(s * 1000.0);
}

static void cmd_waitload(int argc, char **argv)
{
    char *end;
    long s = argc >= 2 ? strtol(argv[1], &end, 0) : 120;

    if (argc >= 2 && (*end || s < 1 || s > 3600)) {
        complain("waitload wants the most seconds to wait (1-3600; default 120)");
        return;
    }
    if (!need_runner("waitload"))
        return;
    memset(s_hold_for, 0, sizeof(*s_hold_for));
    s_hold_for->kind = HOLD_LOAD;
    s_hold_for->start = GetTickCount64();
    s_hold_for->until = s_hold_for->start + (ULONGLONG)s * 1000;
    s_hold_for->level0 = conker_Level();
    s_hold_for->started = conker_LevelLoading();
}

/* Until the cutscene playing ends (the parts it goes on into too); with none
 * playing, one starting within 10 s, as waitload. */
static void cmd_waitscene(int argc, char **argv)
{
    char *end;
    long s = argc >= 2 ? strtol(argv[1], &end, 0) : 600;

    if (argc >= 2 && (*end || s < 1 || s > 3600)) {
        complain("waitscene wants the most seconds to wait (1-3600; default 600)");
        return;
    }
    if (!need_runner("waitscene"))
        return;
    memset(s_hold_for, 0, sizeof(*s_hold_for));
    s_hold_for->kind = HOLD_SCENE;
    s_hold_for->start = GetTickCount64();
    s_hold_for->until = s_hold_for->start + (ULONGLONG)s * 1000;
    s_hold_for->started = conker_SceneBusy();
}

static void cmd_pad(int argc, char **argv)
{
    if (argc < 2) {
        complain("pad wants presses as RECOMP_PAD_LIVE lines: a:100 lup:2000 &rt:300 "
                 "p2-start mouse:40,0");
        return;
    }
    for (int i = 1; i < argc; i++)
        if (!xbox_PadLiveLine(argv[i])) {
            complain("pad: too many presses waiting, %s left out", argv[i]);
            return;
        }
    reply("pad %s", line_rest());
}

/* A name of letters, digits, - _ . only (the file name in shots\). */
static int shot_name_ok(const char *n)
{
    if (!*n || strlen(n) > 80 || n[0] == '.')
        return 0;
    for (; *n; n++)
        if (!isalnum((unsigned char)*n) && *n != '-' && *n != '_' && *n != '.')
            return 0;
    return 1;
}

/* A shot into <save dir>\shots\<name>.png (taken by the overlay at this
 * frame's present, before any menu of ours is drawn); a runner's line waits
 * for the file. 0 when it can't. */
static long shot_take(const char *name)
{
    char path[MAX_PATH], why[160];
    long seq;

    snprintf(path, sizeof(path), "%s\\shots", settings_save_dir());
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s\\shots\\%s%s", settings_save_dir(), name,
             _stricmp(name + (strlen(name) > 4 ? strlen(name) - 4 : 0), ".png") ? ".png"
                                                                               : "");
    seq = overlay_dev_shot(path, why, sizeof(why));
    if (!seq) {
        complain("No shot: %s", why);
        return 0;
    }
    if (s_hold_for) {
        memset(s_hold_for, 0, sizeof(*s_hold_for));
        s_hold_for->kind = HOLD_SHOT;
        s_hold_for->seq = seq;
        s_hold_for->until = GetTickCount64() + 10000;
    }
    return seq;
}

static void stamp_now(char *out, size_t size)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, size, "%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
}

static void cmd_shot(int argc, char **argv)
{
    char name[96];

    if (argc >= 2) {
        if (!shot_name_ok(argv[1])) {
            complain("shot wants a name of letters, digits, - _ . (no name: the date and time)");
            return;
        }
        snprintf(name, sizeof(name), "%s", argv[1]);
    } else {
        char stamp[32];
        stamp_now(stamp, sizeof(stamp));
        snprintf(name, sizeof(name), "shot_%s", stamp);
    }
    shot_take(name);
}

static const char *level_name(int level);

/* Where the game is, one line (to-do 169): level, checkpoint, the story's
 * part, place, cutscene, what Conker is doing, weapon, what B does (to-do
 * 201), vehicle, camera, frame rate, then what we changed. */
static void state_line(char *out, size_t size)
{
    const int level = conker_Level();
    float x, y, z;
    size_t n;
    uint32_t on = cheats_active();
    ConkerState st;
    double fps = overlay_fps_now();

    conker_State(&st);
    n = (size_t)snprintf(out, size, "level %s (0x%02X)", level_name(level), level & 0xFF);
    if (n < size && st.checkpoint >= 0)
        n += (size_t)snprintf(out + n, size - n, " | cp %d %s (spawn %d)", st.checkpoint,
                              st.cp_name, st.spawn);
    if (n < size && st.part)
        n += (size_t)snprintf(out + n, size - n, " | story %s%d %s",
                              edit_save_word(st.chapter), st.part,
                              edit_save_part_name(st.chapter, st.part));
    if (n < size && conker_PlayerPos(&x, &y, &z))
        n += (size_t)snprintf(out + n, size - n, " | pos %.0f %.0f %.0f", x, y, z);
    if (n < size && st.scene >= 0)
        n += (size_t)snprintf(out + n, size - n, " | cutscene %d (%.1f of %.1f s)", st.scene,
                              st.scene_s, st.scene_len);
    if (n < size && st.doing)
        n += (size_t)snprintf(out + n, size - n, " | %s", st.doing);
    if (n < size && st.weapon)
        n += st.weapon_name
             ? (size_t)snprintf(out + n, size - n, " | weapon %s", st.weapon_name)
             : (size_t)snprintf(out + n, size - n, " | weapon 0x%02X", st.weapon);
    if (n < size && st.b_doing)
        n += st.b_doing_name
             ? (size_t)snprintf(out + n, size - n, " | B doing %s", st.b_doing_name)
             : (size_t)snprintf(out + n, size - n, " | B doing 0x%02X", st.b_doing);
    if (n < size && st.b_here)
        n += st.b_here_name
             ? (size_t)snprintf(out + n, size - n, " | B %s", st.b_here_name)
             : (size_t)snprintf(out + n, size - n, " | B 0x%02X", st.b_here);
    if (n < size && st.b_after)
        n += st.b_after_name
             ? (size_t)snprintf(out + n, size - n, ", then %s", st.b_after_name)
             : (size_t)snprintf(out + n, size - n, ", then 0x%02X", st.b_after);
    if (n < size && st.vehicle[0])
        n += (size_t)snprintf(out + n, size - n, " | vehicle %s", st.vehicle);
    if (n < size && st.camera)
        n += (size_t)snprintf(out + n, size - n, " | camera 0x%X", st.camera);
    if (n < size && fps > 0.0)
        n += (size_t)snprintf(out + n, size - n, " | %.0f fps", fps);
    if (n < size && conker_LevelLoading())
        n += (size_t)snprintf(out + n, size - n, " | loading");
    if (n < size && conker_Timescale() != 100)
        n += (size_t)snprintf(out + n, size - n, " | timescale %d%%", conker_Timescale());
    if (n < size && conker_Noclip())
        n += (size_t)snprintf(out + n, size - n, " | noclip");
    if (n < size && photo_freecam())
        n += (size_t)snprintf(out + n, size - n, " | freecam");
    else if (photo_active() && st.doing && !strcmp(st.doing, "photo mode"))
        ;   /* the keys' set said it already: once (user 2026-11-11) */
    else if (n < size && photo_active())
        n += (size_t)snprintf(out + n, size - n, " | photo mode");
    else if (n < size && photo_frozen())
        n += (size_t)snprintf(out + n, size - n, " | paused");
    for (int id = 0; id < CHEAT_COUNT && n < size; id++)
        if (on & CHEAT_BIT(id))
            n += (size_t)snprintf(out + n, size - n, "%s%s",
                                  n && strstr(out, " | cheats ") ? ", " : " | cheats ",
                                  cheat_name(id));
}

static void cmd_state(int argc, char **argv)
{
    char line[MAX_TEXT];
    (void)argc; (void)argv;
    state_line(line, sizeof(line));
    reply("%s", line);
}

static int s_marks;

static void cmd_mark(int argc, char **argv)
{
    char line[MAX_TEXT - 40], name[96], stamp[32];
    const char *note = line_rest();
    (void)argc; (void)argv;

    s_marks++;
    state_line(line, sizeof(line));
    stamp_now(stamp, sizeof(stamp));
    fprintf(stderr, "[MARK] %d%s%s | %s | %s\n", s_marks, note[0] ? " " : "", note,
            stamp, line);
    reply("Mark %d%s%s: %s", s_marks, note[0] ? " " : "", note, line);
    snprintf(name, sizeof(name), "mark_%s_%d", stamp, s_marks);
    shot_take(name);
}

static void cmd_echo(int argc, char **argv)
{
    (void)argc; (void)argv;
    reply("%s", line_rest());
}

/* Scripts: a stack of files (exec inside a script runs the other file, then
 * goes on), one line a frame, holding on the waits. # or ; starts a comment
 * line. The first error stops them all. */
#define SCRIPT_DEPTH 4
#define SCRIPT_LINES 2000

typedef struct {
    char *text;                 /* the file, lines ended by '\0' */
    char *line[SCRIPT_LINES];
    int lineno[SCRIPT_LINES];
    int n, at;
    char name[MAX_PATH];
} Script;

static Script s_scripts[SCRIPT_DEPTH];
static int s_nscripts;
static ULONGLONG s_script_t0;

static void script_pop(void)
{
    Script *sc = &s_scripts[--s_nscripts];
    free(sc->text);
    memset(sc, 0, sizeof(*sc));
}

static void script_stop_all(void)
{
    while (s_nscripts)
        script_pop();
    memset(&s_script_hold, 0, sizeof(s_script_hold));
}

/* The file as given, else in the save folder; .txt may be left out. */
static FILE *script_open(const char *arg, char *found, size_t size)
{
    char try_[MAX_PATH];
    const int has_ext = strchr(arg, '.') != NULL && strrchr(arg, '.') > strrchr(arg, '\\');
    const int absolute = arg[0] == '\\' || (arg[0] && arg[1] == ':');
    FILE *f = NULL;

    for (int pass = 0; pass < 4 && !f; pass++) {
        const int in_save = pass >= 2, ext = pass & 1;
        if ((in_save && absolute) || (ext && has_ext))
            continue;
        snprintf(try_, sizeof(try_), "%s%s%s%s", in_save ? settings_save_dir() : "",
                 in_save ? "\\" : "", arg, ext ? ".txt" : "");
        if (!fopen_s(&f, try_, "rb") && f)
            snprintf(found, size, "%s", try_);
        else
            f = NULL;
    }
    return f;
}

static int script_push(const char *arg)
{
    Script *sc;
    FILE *f;
    long len;
    char *p;
    int lineno = 0;

    if (s_nscripts == SCRIPT_DEPTH) {
        complain("exec: scripts nested more than %d deep", SCRIPT_DEPTH);
        return 0;
    }
    sc = &s_scripts[s_nscripts];
    f = script_open(arg, sc->name, sizeof(sc->name));
    if (!f) {
        complain("exec: no file %s (a path, or a file in the save folder)", arg);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    sc->text = (char *)malloc((size_t)len + 1);
    if (!sc->text || fread(sc->text, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(sc->text);
        memset(sc, 0, sizeof(*sc));
        complain("exec: %s could not be read", arg);
        return 0;
    }
    fclose(f);
    sc->text[len] = '\0';
    for (p = sc->text; *p && sc->n < SCRIPT_LINES;) {
        char *nl = strchr(p, '\n'), *e;
        if (nl)
            *nl = '\0';
        lineno++;
        e = p + strlen(p);
        while (e > p && (e[-1] == '\r' || isspace((unsigned char)e[-1])))
            *--e = '\0';
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p && *p != '#' && *p != ';') {
            sc->line[sc->n] = p;
            sc->lineno[sc->n++] = lineno;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!s_nscripts)
        s_script_t0 = GetTickCount64();
    s_nscripts++;
    reply("exec: %s (%d lines)", sc->name, sc->n);
    return 1;
}

static void cmd_exec(int argc, char **argv)
{
    if (argc < 2) {
        if (!s_nscripts) {
            reply("No script running (exec <file> starts one)");
            return;
        }
        for (int i = 0; i < s_nscripts; i++)
            reply("Running %s, line %d of %d", s_scripts[i].name,
                  s_scripts[i].at ? s_scripts[i].lineno[s_scripts[i].at - 1] : 0,
                  s_scripts[i].n);
        return;
    }
    if (!strcmp(argv[1], "stop")) {
        if (!s_nscripts) {
            reply("No script running");
            return;
        }
        script_stop_all();
        reply("Scripts stopped");
        return;
    }
    if (s_hold_for == &s_script_hold) {
        script_push(line_rest());           /* a script's exec: that file, then on */
        return;
    }
    if (s_nscripts) {
        complain("A script is running (exec stop ends it)");
        return;
    }
    if (script_push(line_rest()) && s_hold_for) {
        memset(s_hold_for, 0, sizeof(*s_hold_for));
        s_hold_for->kind = HOLD_SCRIPT;     /* the pipe answers when it ends */
    }
}

static int s_shots_told;        /* shots whose outcome was printed, by number */

/* 1 once what h waits for is over (its outcome printed), 0 while it lasts. */
static int hold_done(Hold *h)
{
    const ULONGLONG now = GetTickCount64();

    switch (h->kind) {
    case HOLD_WAIT:
        if (now < h->until)
            return 0;
        break;
    case HOLD_LOAD: {
        const int loading = conker_LevelLoading();
        if (!h->started) {
            if (loading || conker_Level() != h->level0) {
                h->started = 1;
            } else if (now - h->start >= 10000) {
                reply("waitload: no load started in 10 s");
                break;
            } else {
                return 0;
            }
        }
        if (loading) {
            h->quiet_since = 0;
        } else if (!h->quiet_since) {
            h->quiet_since = now;
        } else if (now - h->quiet_since >= 1000) {
            reply("waitload: %s loaded after %.1f s", level_name(conker_Level()),
                  (now - h->start) / 1000.0);
            break;
        }
        if (now >= h->until) {
            complain("waitload: still loading after %.0f s", (now - h->start) / 1000.0);
            break;
        }
        return 0;
    }
    case HOLD_SCENE: {
        /* Over once no scene plays or waits, and no load runs, for 0.5 s: the
         * gap between two parts is shorter. */
        const int busy = conker_SceneBusy() || (h->started && conker_LevelLoading());
        if (!h->started) {
            if (busy) {
                h->started = 1;
            } else if (now - h->start >= 10000 || now >= h->until) {
                reply("waitscene: no cutscene started in %.0f s", (now - h->start) / 1000.0);
                break;
            } else {
                return 0;
            }
        }
        if (busy) {
            h->quiet_since = 0;
        } else if (!h->quiet_since) {
            h->quiet_since = now;
        } else if (now - h->quiet_since >= 500) {
            reply("waitscene: the cutscene ended after %.1f s",
                  (h->quiet_since - h->start) / 1000.0);
            break;
        }
        if (now >= h->until) {
            complain("waitscene: still in a cutscene after %.0f s", (now - h->start) / 1000.0);
            break;
        }
        return 0;
    }
    case HOLD_SHOT:
        if (s_shots_told >= h->seq)
            break;
        if (now >= h->until) {
            complain("shot: no picture came in 10 s");
            break;
        }
        return 0;
    case HOLD_SCRIPT:
        if (s_nscripts)
            return 0;
        break;
    default:
        break;
    }
    h->kind = HOLD_NONE;
    return 1;
}

static void shots_frame(void)
{
    char msg[MAX_TEXT];
    int r;

    while ((r = overlay_dev_shot_result(s_shots_told + 1, msg, sizeof(msg))) != 0) {
        s_shots_told++;
        if (r > 0)
            reply("%s", msg);
        else
            complain("%s", msg);
    }
}

static void script_frame(void)
{
    Script *sc;
    int errors = s_errors;

    if (!s_nscripts)
        return;
    sc = &s_scripts[s_nscripts - 1];
    if (s_script_hold.kind) {
        if (!hold_done(&s_script_hold))
            return;
    } else if (sc->at < sc->n) {
        s_hold_for = &s_script_hold;
        run_line(sc->line[sc->at++], 0);
        s_hold_for = NULL;
    }
    if (s_errors != errors && s_nscripts) {
        sc = &s_scripts[s_nscripts - 1];
        complain("exec: stopped at %s line %d", sc->name,
                 sc->at ? sc->lineno[sc->at - 1] : 0);
        script_stop_all();
        return;
    }
    /* Finished files go; the last one says how long it all took. */
    while (s_nscripts && !s_script_hold.kind
           && s_scripts[s_nscripts - 1].at >= s_scripts[s_nscripts - 1].n) {
        char name[MAX_PATH];
        snprintf(name, sizeof(name), "%s", s_scripts[s_nscripts - 1].name);
        script_pop();
        if (!s_nscripts)
            reply("exec: %s done after %.1f s", name,
                  (GetTickCount64() - s_script_t0) / 1000.0);
    }
}

static int s_pipe_busy;

static void pipe_finish(void)
{
    cmd_pipe_done(s_cap ? s_cap : "", s_cap_errors);
    s_capturing = 0;
    s_cap_n = 0;
    if (s_cap)
        s_cap[0] = '\0';
    s_pipe_busy = 0;
}

static void pipe_frame(void)
{
    char line[1024];

    if (s_pipe_busy) {
        if (hold_done(&s_pipe_hold))
            pipe_finish();
        return;
    }
    if (!cmd_pipe_take(line, sizeof(line)))
        return;
    s_pipe_busy = 1;
    s_capturing = 1;
    s_cap_errors = 0;
    s_cap_n = 0;
    if (s_cap)
        s_cap[0] = '\0';
    memset(&s_pipe_hold, 0, sizeof(s_pipe_hold));
    s_hold_for = &s_pipe_hold;
    s_skip_echo = 1;            /* the client knows what it sent */
    run_line(line, 0);
    s_skip_echo = 0;
    s_hold_for = NULL;
    if (!s_pipe_hold.kind)
        pipe_finish();
}

void console_frame(void)
{
    static int started;

    if (!started) {
        started = 1;
        cmd_pipe_start();
    }
    shots_frame();
    script_frame();
    pipe_frame();
}

/* ---- logs switched live (to-do 171) ---------------------------------------- */

static void cmd_log(int argc, char **argv)
{
    int id, level;
    char *end;

    if (argc < 2 || !_stricmp(argv[1], "list")) {
        for (int i = 0; i < DLOG_COUNT; i++)
            reply("%-9s %-4s %s", dev_log_name(i),
                  dev_log(i) == 0 ? "off" : dev_log(i) == 1 ? "on" : dev_log(i) == 2 ? "2" : "3+",
                  dev_log_what(i));
        reply("log <name> on|off|2: the lines go to the log (con_out detailed shows them)");
        return;
    }
    id = dev_log_find(argv[1]);
    if (id < 0) {
        complain("No log called %s (log list)", argv[1]);
        return;
    }
    if (argc < 3) {
        reply("%s is %s (%s)", dev_log_name(id), dev_log(id) ? "on" : "off", dev_log_env(id));
        return;
    }
    level = !_stricmp(argv[2], "on") ? 1 : !_stricmp(argv[2], "off") ? 0
          : (int)strtol(argv[2], &end, 10);
    if ((_stricmp(argv[2], "on") && _stricmp(argv[2], "off") && *end)
        || level < 0 || level > dev_log_max(id)) {
        complain("log %s on|off%s", dev_log_name(id), dev_log_max(id) > 1 ? "|2" : "");
        return;
    }
    dev_log_set(id, level);
    fprintf(stderr, "[LOGSWITCH] %s = %d\n", dev_log_env(id), level);
    reply("%s %s", dev_log_name(id), level == 0 ? "off" : level == 1 ? "on" : "on, level 2");
}

/* ---- story flags by name (to-do 170) --------------------------------------- */

/* The flag's name holds the text (any case). */
static int flag_holds(int i, const char *text)
{
    const char *name = conker_FlagName(i);
    char a[96], b[96];

    if (!name)
        return 0;
    snprintf(a, sizeof a, "%s", name);
    snprintf(b, sizeof b, "%s", text);
    _strlwr_s(a, sizeof a);
    _strlwr_s(b, sizeof b);
    return strstr(a, b) != NULL;
}

/* A task by number or name: the exact name (any case), else the one name
 * holding the text. -1 = none or several, said. */
static int flag_find(const char *text)
{
    char *end;
    long n = strtol(text, &end, 10);
    int found = -1, count = 0;

    if (!*end && n >= 0 && n < conker_FlagCount())
        return (int)n;
    for (int i = 0; i < conker_FlagCount(); i++) {
        const char *name = conker_FlagName(i);
        if (name && !_stricmp(name, text))
            return i;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < conker_FlagCount(); i++) {
            if (!flag_holds(i, text))
                continue;
            if (!pass) {
                count++;
                found = i;
            } else if (count++ < 8) {
                reply("  %d %s", i, conker_FlagName(i));
            }
        }
        if (!pass && count < 2)
            break;
        if (!pass)
            count = 0;
    }
    if (count == 1)
        return found;
    if (!count)
        complain("No story flag called %s (flag list)", text);
    else
        complain("%d flags hold \"%s\"%s: say which", count, text, count > 8 ? " (8 shown)" : "");
    return -1;
}

static void flag_line(int i)
{
    int state = 0, on = conker_Flag(i, &state);
    /* The state byte is the game's own bookkeeping, not saved: 1 = the task
     * runs in this level, 3 = done this session. */
    reply("%3d %-34s %s%s", i, conker_FlagName(i) ? conker_FlagName(i) : "?",
          on ? "on" : "off", state == 1 ? " (running here)" : "");
}

static void cmd_flag(int argc, char **argv)
{
    int id, shown = 0;

    if (argc < 2) {
        complain("flag list [text|on|off] | flag <name|number> [on|off]");
        return;
    }
    if (conker_Flag(0, NULL) < 0) {
        complain("No story is being played");
        return;
    }
    if (!_stricmp(argv[1], "list")) {
        for (int i = 0; i < conker_FlagCount(); i++) {
            if (argc >= 3 && !_stricmp(argv[2], "on")) {
                if (conker_Flag(i, NULL) != 1)
                    continue;
            } else if (argc >= 3 && !_stricmp(argv[2], "off")) {
                if (conker_Flag(i, NULL) != 0)
                    continue;
            } else if (argc >= 3 && !flag_holds(i, argv[2])) {
                continue;
            }
            flag_line(i);
            shown++;
        }
        reply("%d flag%s", shown, shown == 1 ? "" : "s");
        return;
    }
    id = flag_find(argv[1]);
    if (id < 0)
        return;
    if (argc < 3) {
        flag_line(id);
        return;
    }
    if (_stricmp(argv[2], "on") && _stricmp(argv[2], "off")) {
        complain("flag <name|number> on|off");
        return;
    }
    if (!conker_FlagSet(id, !_stricmp(argv[2], "on"))) {
        complain("No story is being played");
        return;
    }
    reply("%d %s %s: the level sees it from its next load (warp, a death's reload)", id,
          conker_FlagName(id) ? conker_FlagName(id) : "?", argv[2]);
}

/* ---- memory: peek / poke / watch (to-do 172) ------------------------------- */

int xbox_WatchSet(const char *spec);
void xbox_WatchStop(void);
uint32_t xbox_WatchAddress(void);

/* A guest address: 0x0084B290, or a chain as RECOMP_WATCH takes it,
 * [[0x008470DC]+0x3E0]+0x568 (each [ ] reads the dword there). 0 = not one,
 * with the reason said. */
static int parse_addr(const char *text, uint32_t *out)
{
    const char *p = text;
    int depth = 0;
    uint32_t a = 0;
    char *end;

    while (*p == '[') {
        if (depth == 8) {
            complain("%s: more than 8 [ ]", text);
            return 0;
        }
        depth++;
        p++;
    }
    a = strtoul(p, &end, 16);
    if (end == p) {
        complain("%s is no address (hex, 0x0084B290 or [0x008470DC]+0x3E0)", text);
        return 0;
    }
    p = end;
    for (;;) {
        if (*p == '+' || *p == '-') {
            const int minus = *p == '-';
            uint32_t off = strtoul(p + 1, &end, 16);
            if (end == p + 1) {
                complain("%s: an offset wants hex", text);
                return 0;
            }
            a = minus ? a - off : a + off;
            p = end;
        } else if (*p == ']') {
            uint32_t v;
            if (!depth) {
                complain("%s: a ] without its [", text);
                return 0;
            }
            if (!conker_Peek(a, &v, 4)) {
                complain("%s: 0x%08X can't be read", text, a);
                return 0;
            }
            if (!v) {
                complain("%s: the pointer at 0x%08X is 0", text, a);
                return 0;
            }
            a = v;
            depth--;
            p++;
        } else {
            break;
        }
    }
    if (*p || depth) {
        complain("%s is no address (hex, 0x0084B290 or [0x008470DC]+0x3E0)", text);
        return 0;
    }
    *out = a;
    return 1;
}

static int type_size(const char *t)
{
    return !_stricmp(t, "u8") || !_stricmp(t, "s8") ? 1
         : !_stricmp(t, "u16") || !_stricmp(t, "s16") ? 2
         : !_stricmp(t, "u32") || !_stricmp(t, "s32") || !_stricmp(t, "f32") ? 4 : 0;
}

static void value_text(const char *t, const uint8_t *b, char *out, size_t size)
{
    uint32_t u = 0;
    float f;

    memcpy(&u, b, (size_t)type_size(t));
    if (!_stricmp(t, "f32")) {
        memcpy(&f, b, 4);
        snprintf(out, size, "%g", f);
    } else if (t[0] == 's' || t[0] == 'S') {
        const int v = type_size(t) == 1 ? (int8_t)u : type_size(t) == 2 ? (int16_t)u : (int32_t)u;
        snprintf(out, size, "%d", v);
    } else {
        snprintf(out, size, "0x%0*X (%u)", type_size(t) * 2, u, u);
    }
}

static void cmd_peek(int argc, char **argv)
{
    uint32_t a;
    uint8_t b[256];

    if (argc < 2) {
        complain("peek <address> [bytes | u8|u16|u32|s8|s16|s32|f32 [count]]");
        return;
    }
    if (!parse_addr(argv[1], &a))
        return;
    if (argc >= 3 && type_size(argv[2])) {
        const int sz = type_size(argv[2]);
        long count = argc >= 4 ? strtol(argv[3], NULL, 0) : 1;
        if (count < 1 || count > 64) {
            complain("peek: a count of 1-64");
            return;
        }
        if (!conker_Peek(a, b, (unsigned)(sz * count))) {
            complain("0x%08X can't be read", a);
            return;
        }
        for (long i = 0; i < count; i++) {
            char v[48];
            value_text(argv[2], b + i * sz, v, sizeof v);
            reply("0x%08X %s %s", a + (uint32_t)(i * sz), argv[2], v);
        }
        return;
    }
    if (argc >= 3) {
        char *end;
        long n = strtol(argv[2], &end, 0);
        if (*end || n < 1 || n > 256) {
            complain("peek: bytes 1-256, or a type (u8 u16 u32 s8 s16 s32 f32)");
            return;
        }
        if (!conker_Peek(a, b, (unsigned)n)) {
            complain("0x%08X can't be read", a);
            return;
        }
        for (long i = 0; i < n; i += 16) {
            char line[16 * 3 + 1];
            size_t k = 0;
            for (long j = i; j < n && j < i + 16; j++)
                k += (size_t)snprintf(line + k, sizeof line - k, " %02X", b[j]);
            reply("0x%08X%s", a + (uint32_t)i, line);
        }
        return;
    }
    if (!conker_Peek(a, b, 4)) {
        complain("0x%08X can't be read", a);
        return;
    }
    {
        uint32_t u;
        float f;
        memcpy(&u, b, 4);
        memcpy(&f, b, 4);
        reply("0x%08X: 0x%08X (%u, f32 %g) bytes %02X %02X %02X %02X", a, u, u, f,
              b[0], b[1], b[2], b[3]);
    }
}

static void cmd_poke(int argc, char **argv)
{
    uint32_t a;
    const char *t;
    uint8_t was[4], now[4];
    char *end;
    char before[48], after[48];

    if (argc < 3) {
        complain("poke <address> <value> [u8|u16|u32|s8|s16|s32|f32] (a value with . is f32)");
        return;
    }
    if (!parse_addr(argv[1], &a))
        return;
    t = argc >= 4 ? argv[3] : strchr(argv[2], '.') ? "f32" : "u32";
    if (!type_size(t)) {
        complain("poke: the type is u8, u16, u32, s8, s16, s32 or f32");
        return;
    }
    if (!_stricmp(t, "f32")) {
        float f = strtof(argv[2], &end);
        memcpy(now, &f, 4);
    } else {
        long long v = strtoll(argv[2], &end, 0);
        uint32_t u = (uint32_t)v;
        memcpy(now, &u, 4);
    }
    if (*end) {
        complain("poke: %s is no number", argv[2]);
        return;
    }
    if (!conker_Peek(a, was, (unsigned)type_size(t))
        || !conker_Poke(a, now, (unsigned)type_size(t))) {
        complain("0x%08X can't be written", a);
        return;
    }
    value_text(t, was, before, sizeof before);
    value_text(t, now, after, sizeof after);
    reply("0x%08X %s: %s -> %s", a, t, before, after);
}

static void cmd_watch(int argc, char **argv)
{
    uint32_t a;
    int r;

    if (argc < 2) {
        a = xbox_WatchAddress();
        if (a)
            reply("Watching 0x%08X ([WATCH] lines in the log: con_out detailed shows them)", a);
        else
            reply("Nothing watched (watch <address>)");
        return;
    }
    if (!_stricmp(argv[1], "off")) {
        xbox_WatchStop();
        reply("Watch off");
        return;
    }
    /* A chain is followed now: the dword it leads to is the one watched. */
    if (!parse_addr(argv[1], &a))
        return;
    {
        char spec[16];
        snprintf(spec, sizeof spec, "0x%08X", a);
        r = xbox_WatchSet(spec);
    }
    if (r == 1)
        reply("Watching 0x%08X: each change is a [WATCH] line with the writer (con_out "
              "detailed shows them); a page fault per write to its page, so slower", a);
    else if (r == 2)
        reply("0x%08X isn't mapped yet: watched once it is", a);
    else
        complain("0x%08X can't be watched", a);
}

static const Command s_commands[] = {
    { "help", "[command]", "what the commands do", cmd_help },
    { "warp", "list | <level> | soak <level> | cp list [level] | cp <number> | scene <level> <scene>",
      "goes to a level, checkpoint or cutscene; nothing is saved until the main menu", cmd_warp },
    { "cutscene", "list [level] | <level> <scene>",
      "plays a cutscene, then back to where you were", cmd_cutscene },
    { "noclip", "[on|off]", "Conker flies through everything", cmd_noclip },
    { "pos", "", "where Conker is", cmd_pos },
    { "tp", "<x> <y> <z> | <number> | cam",
      "puts Conker there (a number from chars; cam = freecam's camera)", cmd_tp },
    { "spot", "save|go|delete <name> | list",
      "named places, kept per level in the save folder (spots.txt)", cmd_spot },
    { "set", "energy|lives|cash|oxygen <n>", "sets one of Conker's values (oxygen: seconds of air)",
      cmd_set },
    { "kill", "", "Conker dies now: a life lost and the reload, as the game's instant kills",
      cmd_kill },
    { "chars", "[count]", "the characters in the level, nearest first (tp <number> goes there)",
      cmd_chars },
    { "editSave", "<1|2|3> <word><number> | list",
      "moves a save to the start of a chapter's part, as the Chapters list starts it",
      cmd_editsave },
    { "freecam", "[on|off]", "photo mode's fly camera with the world running", cmd_freecam },
    { "timescale", "[0-300]", "the game's speed in percent; no value swaps with the last",
      cmd_timescale },
    { "godmode", "[on|off]", "God mode", cmd_cheat },
    { "reloading", "[on|off]", "No reload", cmd_cheat },
    { "rosebud", "[on|off]", "$1,000,000", cmd_cheat },
    { "inftail", "[on|off]", "Infinite tail spin", cmd_cheat },
    { "allchapters", "[on|off]", "Unlock all chapters", cmd_cheat },
    { "infoxygen", "[on|off]", "Infinite oxygen", cmd_cheat },
    { "inflives", "[on|off]", "Infinite lives", cmd_cheat },
    { "mpunlocks", "[on|off]", "Multiplayer unlocks", cmd_cheat },
    { "glass", "[on|off]", "Glass Squirrel", cmd_cheat },
    { "bullettime", "[on|off]", "Bullet time (then B or both sticks clicked)", cmd_cheat },
    { "moonjump", "[on|off]", "Moon Jump", cmd_cheat },
    { "superspeed", "[on|off]", "Super Speed", cmd_cheat },
    { "bighead", "[on|off]", "Big Head", cmd_cheat },
    { "ironboots", "[on|off]", "Iron Boots (walks on the bottom under water)", cmd_cheat },
    { "holysquirrel", "[on|off]", "Holy Squirrel (walks on water)", cmd_cheat },
    { "diveanywhere", "[on|off]", "Dive Anywhere (dives in any water)", cmd_cheat },
    { "mouse", "[on|off]", "Conker the Mouse (half size, higher voice)", cmd_cheat },
    { "bear", "[on|off]", "Conker the Bear (twice the size, lower voice)", cmd_cheat },
    { "wireframe", "[models|all|off]", "draws characters and objects, or everything, as lines",
      cmd_wireframe },
    { "cue", "<number>", "plays one of the game's sounds", cmd_cue },
    { "tune", "list [name] | <number|name> | stop", "plays one of the game's music tunes",
      cmd_tune },
    { "imp", "<number>", "the fire imp's animation, on the main menu", cmd_imp },
    { "bind", "<key> <command> | list | reset",
      "a key runs a console command (kept in settings.ini; reset = none); autoexec.txt in the "
      "save folder runs at the main menu", cmd_bind },
    { "unbind", "<key> | all", "a bound key runs nothing again", cmd_unbind },
    { "clear", "", "empties this console", cmd_clear },
    { "con_out", "[simple|detailed|full]",
      "what this console shows: command replies; + the tagged log lines; the "
      "whole log", cmd_con_out },
    { "metrics", "[0|1|2]",
      "the F1 counter: off; fps; fps, frame times and memory", cmd_metrics },
    { "debugpage", "[number|next|prev|off]",
      "Rare's debug pages (no value: the list)", cmd_debugpage },
    { "soak", "", "the game's soak test (from the main menu)", cmd_soak },
    { "exec", "<file> | stop", "runs a script: a text file of console commands, one a frame, "
      "waiting on wait / waitload / shot; stops at the first error (no value: what runs)",
      cmd_exec },
    { "wait", "<seconds>", "a script or the command pipe waits that long (0.5 works)", cmd_wait },
    { "waitload", "[most seconds]", "a script or the command pipe waits until a level has "
      "loaded (a load starting within 10 s, then 1 s with none; default at most 120 s)",
      cmd_waitload },
    { "waitscene", "[most seconds]", "a script or the command pipe waits until the cutscene "
      "playing has ended, with the parts it goes on into (none playing: one starting within "
      "10 s; default at most 600 s)", cmd_waitscene },
    { "pad", "<press> ...", "presses on the game's controller: a:100, lup:2000, &rt:300 "
      "(at once, on top of a held one), p2-start, mouse:40,0", cmd_pad },
    { "shot", "[name]", "the picture as shown, without menus, into shots\\ in the save "
      "folder (no name: shot_<date>_<time>)", cmd_shot },
    { "mark", "[note]", "a numbered [MARK] line in the log with where the game is, and a "
      "shot (also the mark key, F7)", cmd_mark },
    { "state", "", "where the game is: level, checkpoint, the story's part, Conker's place, "
      "cutscene, what he is doing, weapon, vehicle, camera mode, frame rate, loading, "
      "timescale, noclip, freecam / photo mode, cheats on", cmd_state },
    { "echo", "<text>", "prints the text (notes in a script's output)", cmd_echo },
    { "log", "list | <name> [on|off|2]", "switches a log on or off while the game runs "
      "(as its RECOMP_<NAME>_LOG at start; the lines go to the log)", cmd_log },
    { "flag", "list [text|on|off] | <name|number> [on|off]",
      "the story's flags (the game's 238 tasks, as Edit Save names them): which are done, "
      "and setting one; the level sees a change from its next load", cmd_flag },
    { "peek", "<address> [bytes | u8|u16|u32|s8|s16|s32|f32 [count]]",
      "reads the game's memory (hex address; [0x008470DC]+0x3E0 reads the pointer there)",
      cmd_peek },
    { "poke", "<address> <value> [u8|u16|u32|s8|s16|s32|f32]",
      "writes the game's memory (default u32, f32 when the value has a .)", cmd_poke },
    { "watch", "[<address> | off]", "names the game code that changes a dword: [WATCH] "
      "lines in the log, one address at a time (no value: what is watched)", cmd_watch },
};
#define COMMANDS ((int)(sizeof(s_commands) / sizeof(s_commands[0])))

static void cmd_help(int argc, char **argv)
{
    for (int i = 0; i < COMMANDS; i++) {
        const Command *c = &s_commands[i];
        if (argc >= 2 && strcmp(argv[1], c->name))
            continue;
        reply("%s%s%s: %s", c->name, c->args[0] ? " " : "", c->args, c->help);
        if (argc >= 2)
            return;
    }
    if (argc >= 2)
        complain("No command called %s", argv[1]);
}

/* ---- typing ------------------------------------------------------------------ */

#define HISTORY 32
#define HISTORY_TEXT 256

static char s_history[HISTORY][HISTORY_TEXT];
static int s_history_n;         /* lines kept, oldest first */
static int s_history_at;        /* the one Up / Down is on; s_history_n = none */

static void remember(const char *line)
{
    if (s_history_n && !strcmp(s_history[s_history_n - 1], line)) {
        s_history_at = s_history_n;
        return;
    }
    if (s_history_n == HISTORY) {
        memmove(s_history[0], s_history[1], (HISTORY - 1) * HISTORY_TEXT);
        s_history_n--;
    }
    strncpy_s(s_history[s_history_n], HISTORY_TEXT, line, _TRUNCATE);
    s_history_n++;
    s_history_at = s_history_n;
}

const char *console_history(int step)
{
    int at = s_history_at + step;

    if (at < 0 || at > s_history_n)
        return NULL;
    s_history_at = at;
    return at == s_history_n ? "" : s_history[at];
}

/* A line from a script or the pipe (typed 0) stays out of Up / Down. */
static void run_line(const char *line, int typed)
{
    char copy[HISTORY_TEXT], *argv[MAX_ARGS], *ctx = NULL, *tok;
    char echo[HISTORY_TEXT + 2];
    const char *was = s_cur_line;
    int argc = 0;

    while (*line && isspace((unsigned char)*line))
        line++;
    if (!*line)
        return;
    if (typed)
        remember(line);
    snprintf(echo, sizeof(echo), "> %s", line);
    add_line(CONSOLE_COMMAND, echo, strlen(echo));
    fprintf(stderr, "[CONSOLE] %s\n", echo);
    if (!s_skip_echo)
        cap_add(echo);
    s_skip_echo = 0;

    strncpy_s(copy, sizeof(copy), line, _TRUNCATE);
    for (tok = strtok_s(copy, " \t", &ctx); tok && argc < MAX_ARGS;
         tok = strtok_s(NULL, " \t", &ctx))
        argv[argc++] = tok;
    s_cur_line = line;
    for (int i = 0; i < COMMANDS; i++)
        if (!_stricmp(argv[0], s_commands[i].name)) {
            s_commands[i].run(argc, argv);
            s_cur_line = was;
            return;
        }
    s_cur_line = was;
    complain("No command called %s (help lists them)", argv[0]);
}

void console_run(const char *line)
{
    run_line(line, 1);
}

int console_complete(char *buf, int size)
{
    size_t len = strlen(buf), common = 0;
    const char *first = NULL;
    int n = 0;

    if (strchr(buf, ' ')) {
        char name[32] = "";
        sscanf_s(buf, "%31s", name, (unsigned)sizeof(name));
        for (int i = 0; i < COMMANDS; i++)
            if (!_stricmp(name, s_commands[i].name) && s_commands[i].args[0])
                reply("%s %s", s_commands[i].name, s_commands[i].args);
        return 0;
    }
    for (int i = 0; i < COMMANDS; i++) {
        const char *c = s_commands[i].name;
        if (_strnicmp(c, buf, len))
            continue;
        if (!first) {
            first = c;
            common = strlen(c);
        } else {
            size_t k = len;
            while (k < common && first[k] == c[k])
                k++;
            common = k;
        }
        n++;
    }
    if (!n)
        return 0;
    if (n > 1 && common == len) {
        for (int i = 0; i < COMMANDS; i++)
            if (!strncmp(s_commands[i].name, buf, len))
                reply("  %s", s_commands[i].name);
        return 0;
    }
    if ((int)common + 2 > size)
        return 0;
    memcpy(buf, first, common);
    buf[common] = '\0';
    if (n == 1)
        strcat_s(buf, (size_t)size, " ");
    return 1;
}
