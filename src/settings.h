/**
 * settings.ini in the save folder (settings.c), for main.c and the F2
 * overlay (overlay.cpp). Options are named by their key alone ("widescreen",
 * "mix"); keys are unique across sections.
 */
#ifndef CONKER_SETTINGS_H
#define CONKER_SETTINGS_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read <save_dir>\settings.ini, creating it if missing, and set the runtime
 * switches it holds. Must run before anything reads them. */
void settings_load(const char *save_dir);

/* paths.game_dir, or NULL when the file leaves it empty. */
const char *settings_game_dir(void);

/* The save folder settings_load was given (settings.ini, Photos\). */
const char *settings_save_dir(void);

/* An option's value in the file now (its default when absent), and the
 * value the game started with: when the two differ, the change applies at
 * the next start. NULL for an unknown key. */
const char *settings_get(const char *key);
const char *settings_started(const char *key);

/* The value a new file gets: what "back to the default" sets. */
const char *settings_default(const char *key);

/* The environment variable that overrides the option, when it was set
 * before the file was read (run.ps1 -Env): the file's value is then not
 * used. NULL when the file's value is the one in effect. */
const char *settings_env_override(const char *key);

/* Check a value, store it and write the whole file back. 1 on success;
 * 0 for a value the option does not take or a file that could not be
 * written (logged either way). */
int settings_set(const char *key, const char *value);

/* render_resolution's value as a size in pixels, by the same rules as at
 * start-up (auto, auto-4:3, native, WxH). 0 x 0 for 640x480, the title's
 * own size (RECOMP_RENDER_RES unset). 1 on success; 0 when there is no
 * monitor size or the value is not a size. For the F2 menu, which applies
 * it at once with xbox_SetRenderRes. */
int settings_render_size(const char *value, unsigned *w, unsigned *h);

/* --dump-settings (SPEC 9.2): every option at its default, as if a new
 * file had been written, without reading or writing one or setting any
 * runtime switch. */
void settings_use_defaults(const char *save_dir);

/* The "options" member of the dump: every option's section, key, kind,
 * default, choices or range and its settings.ini comment, as JSON. */
void settings_dump_options(FILE *f);

/* s as a JSON string, quotes included. */
void settings_json_string(FILE *f, const char *s);

/* --debug on the command line: debug-only choices are shown. */
void settings_set_debug(int on);
int settings_debug(void);

/* Key binds (the console's bind, to-do 157): settings.ini's [binds] section,
 * "<key> = <command>". Executor thread for the writes. */
int settings_bind_count(void);
const char *settings_bind_key(int i);
const char *settings_bind_command(int i);
int settings_bind_set(const char *key, const char *command);   /* NULL = unbind */
int settings_binds_reset(void);

#ifdef __cplusplus
}
#endif

#endif
