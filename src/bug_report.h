/**
 * Report Bug (to-do 12, SPEC 9.2): one .zip a player attaches to a GitHub
 * issue, written by bug_report.c. Used by the F10 window (overlay.cpp), the
 * crash and freeze paths and `conker.exe --bug-report` (main.c; the
 * launcher's Report Bug button runs that).
 */
#ifndef CONKER_BUG_REPORT_H
#define CONKER_BUG_REPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BugReport {
    const char *save_dir;      /* the user files folder (logs\, saves\ ...) */
    const char *description;   /* the player's words, UTF-8; NULL = none */
    const char *shot_png;      /* a PNG of the moment, copied in; NULL = none */
    int include_saves;         /* saves\ goes in */
    const char *origin;        /* where it was made: "F10 in the game", ... */
    const char *extra;         /* more report.txt lines (game state), or NULL */
} BugReport;

/* Write <save_dir>\reports\report_<date>_<time>.zip. Returns 0 and the
 * zip's full path in zip_out, or -1 with a reason in zip_out. */
int bug_report_write(const BugReport *r, char *zip_out, size_t n);

/* The prefilled GitHub new-issue page for a written report (SPEC 9.2 "Open
 * GitHub Issue"): title = the description's first line ("Bug report"
 * without one); body = the description, game version and release, Windows,
 * graphics card and the zip's name to attach; the Windows user name hidden.
 * *url: the page, the description cut short if the address would be too
 * long; *text: the whole body (CRLF lines, UTF-8) for the clipboard. Both
 * malloc'd (free them). Returns 0, or -1 (out of memory, both NULL). */
int bug_report_issue(const char *description, const char *zip_path,
                     char **url, char **text);

/* Open Folder: Explorer on the reports folder with the zip selected. On a
 * thread of its own, as shell calls can take seconds. */
void bug_report_open_folder(const char *zip_path);

/* Open GitHub Issue: text (UTF-8) onto the clipboard, owned by the window
 * `owner` (with none, SetClipboardData fails), then url in the browser. On a
 * thread of its own. Returns 0, or -1 when it could not be started. */
int bug_report_open_issue(const char *url, const char *text, void *owner);

/* The small report window of `conker.exe --bug-report --crash | --freeze`
 * (bug_report_win.c): a description, the saves tick box, Create Report, then
 * Open Folder / Open GitHub Issue. BUG_WINDOW_CRASH asks first whether to make
 * a report at all. Returns when it closes. */
enum { BUG_WINDOW_CRASH = 1, BUG_WINDOW_FREEZE = 2 };
int bug_report_window(const char *save_dir, int kind);

/* The game's release once known (main.c, after the XBE check): "PAL" or
 * "USA" and the default.xbe SHA-1. Without it the report reads the newest
 * log's "default.xbe SHA-1" line. */
void bug_report_set_release(const char *release, const char *sha1);

#ifdef __cplusplus
}
#endif

#endif
