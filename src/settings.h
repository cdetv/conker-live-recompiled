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

/* Quality presets (to-do 202): Low, Medium, High (= the defaults), Ultra,
 * each a value for the same few costly options. current: the preset every
 * one of them matches now, -1 = none ("Custom"; settings_preset_name(-1)).
 * apply: sets them all and writes the file once (1 on success); the caller
 * then applies each key to the running game as F2 does. */
#define SETTINGS_PRESETS 4
const char *settings_preset_name(int p);
int settings_preset_key_count(void);
const char *settings_preset_key(int k);
const char *settings_preset_value(int p, int k);
int settings_preset_current(void);
int settings_preset_apply(int p);
void settings_dump_presets(FILE *f);

/* To-do 203: the preset a first start (no settings.ini yet) takes on this
 * PC, -1 = the defaults; why: the threads and graphics card it went by. */
int settings_first_start_preset(char *why, size_t n);

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
