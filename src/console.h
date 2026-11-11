/**
 * The debug console (--debug only; to-do 14, SPEC 10.3): its commands, what
 * it shows and the log it follows. overlay.cpp draws it (build_console) and
 * calls all of this on its own thread, the executor.
 */
#ifndef CONKER_CONSOLE_H
#define CONKER_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* What a line of the console is. con_out decides which are shown: simple the
 * first three, detailed also the tagged log lines, full the whole log. */
enum {
    CONSOLE_COMMAND,    /* what was typed */
    CONSOLE_REPLY,      /* a command's answer */
    CONSOLE_ERROR,      /* a command that could not do it */
    CONSOLE_TAGGED,     /* a log line that starts with a [TAG] */
    CONSOLE_PLAIN       /* any other log line */
};

/* New lines of the log, once a frame while the console is open. */
void console_poll_log(void);

/* Runs one typed line. */
void console_run(const char *line);

/* The lines con_out shows: how many, and one of them with its kind. */
int console_shown(void);
const char *console_shown_line(int i, int *kind);

/* Tab: the first word of buf completed as far as it can be, in place; lists
 * the commands it could still be, as a reply. Returns 1 if buf changed. */
int console_complete(char *buf, int size);

/* Up (step -1) and Down (+1) through what was typed before: that line, ""
 * past the newest, NULL with nothing further that way. */
const char *console_history(int step);

/* The name of Rare's debug page n (0 = off), for this console and the Debug
 * menu; NULL for a number with no page. Step: the next page with a name that
 * way (dir 1 or -1), round and round through 0. */
#define CONSOLE_DEBUG_PAGES 26
const char *console_page_name(int page);
int console_page_step(int page, int dir);

#ifdef __cplusplus
}
#endif

#endif
