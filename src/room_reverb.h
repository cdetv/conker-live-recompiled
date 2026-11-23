/**
 * Enhanced room reverb (to-do 232, 0.5.0; F2 Sound, off by default): the
 * game's own reverb presets by level and under water (room_reverb.c).
 */
#ifndef CONKER_ROOM_REVERB_H
#define CONKER_ROOM_REVERB_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROOM_REVERB_PRESETS 26
#define ROOM_REVERB_AUTO    (-1)   /* room_reverb_force: the table again */

/* main.c, after settings.ini: RECOMP_ROOM_REVERB / _STRENGTH / _PRESET or the
 * settings (with --debug). */
void room_reverb_init(void);

/* Once a game frame (recomp_manual.c, next to audio_fx_frame): picks the
 * preset for where Conker is and hands it to the APU when it changes. */
void room_reverb_frame(void);

/* F2 or the console changed room_reverb / room_reverb_strength in
 * settings.ini (any thread; taken at the next game frame). */
void room_reverb_apply_settings(void);

/* The strength while F2's slider moves, 0-100 (any thread). */
void room_reverb_set_strength(float percent);

/* The console's reverb off / auto (also settings.ini's room_reverb, saved by
 * the console): on or off from now, over RECOMP_ROOM_REVERB too; ends a
 * forced preset. */
void room_reverb_switch(int on);

/* The console: a preset 0-25 played everywhere (on even with the setting
 * off), or ROOM_REVERB_AUTO = back to the setting and the table. */
void room_reverb_force(int preset);

/* The preset a name or number means ("cave", "stone corridor", "10"), or -1. */
int room_reverb_preset_of(const char *text);
const char *room_reverb_preset_name(int preset);

/* State for the console and F2: on (setting or environment), the forced
 * preset (-1 none), the preset playing now (-1 = the game's own reverb) and
 * why, the strength in percent. */
int room_reverb_on(void);
int room_reverb_forced(void);
int room_reverb_now(const char **why);
float room_reverb_strength(void);
int room_reverb_env_decides(void);

#ifdef __cplusplus
}
#endif

#endif
