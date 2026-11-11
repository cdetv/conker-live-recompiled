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

static void add_line(int kind, const char *text, size_t len)
{
    Line *l;

    if (s_nlines == MAX_LINES) {
        for (int i = 0; i < DROP_LINES; i++)
            free(s_lines[i].text);
        memmove(s_lines, s_lines + DROP_LINES,
                (MAX_LINES - DROP_LINES) * sizeof(Line));
        s_nlines -= DROP_LINES;
        s_shown_stale = 1;
    }
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

/* A reply or an error: on the console and in the log. */
static void say(int kind, const char *fmt, va_list ap)
{
    char text[MAX_TEXT + 1];
    int n = vsnprintf(text, sizeof(text), fmt, ap);

    if (n < 0)
        return;
    add_line(kind, text, strlen(text));
    fprintf(stderr, "[CONSOLE] %s\n", text);
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
        "off", "fps", "fps, frame times and memory"
    };

    if (argc >= 2 && (!strcmp(argv[1], "0") || !strcmp(argv[1], "1")
                      || !strcmp(argv[1], "2")))
        overlay_set_metrics(argv[1][0] - '0');
    else if (argc >= 2) {
        complain("metrics wants 0, 1 or 2");
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
            reply("%4d  %s", i, name);
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
    static char b[24];
    const char *n = conker_WarpLevelName(level);
    if (n)
        return n;
    snprintf(b, sizeof b, "level 0x%02X", level);
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

void console_run(const char *line)
{
    char copy[HISTORY_TEXT], *argv[MAX_ARGS], *ctx = NULL, *tok;
    char echo[HISTORY_TEXT + 2];
    int argc = 0;

    while (*line && isspace((unsigned char)*line))
        line++;
    if (!*line)
        return;
    remember(line);
    snprintf(echo, sizeof(echo), "> %s", line);
    add_line(CONSOLE_COMMAND, echo, strlen(echo));
    fprintf(stderr, "[CONSOLE] %s\n", echo);

    strncpy_s(copy, sizeof(copy), line, _TRUNCATE);
    for (tok = strtok_s(copy, " \t", &ctx); tok && argc < MAX_ARGS;
         tok = strtok_s(NULL, " \t", &ctx))
        argv[argc++] = tok;
    for (int i = 0; i < COMMANDS; i++)
        if (!_stricmp(argv[0], s_commands[i].name)) {
            s_commands[i].run(argc, argv);
            return;
        }
    complain("No command called %s (help lists them)", argv[0]);
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
