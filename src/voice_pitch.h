/**
 * Conker's voice pitched for Conker the Mouse / Bear (to-dos 212, 215, 216): voice_pitch.c.
 */
#ifndef CONKER_VOICE_PITCH_H
#define CONKER_VOICE_PITCH_H

/* Sets the toolkit's file hooks; before the game opens its sound banks. Reads
 * the lip table from <save_dir>\assets (to-do 216). */
void voice_pitch_init(const char *save_dir);

/* To-do 216, the speech start (ad_mpeg_Start): is anything to do (a size
 * cheat on, or RECOMP_VOICE_MIXED=2)? Then voice_mixed_start with the sound
 * table entry's sound bank and cue numbers and cue name, the lip-sync name the
 * game's map gives (NULL: none) and the lip-sync talker's name. */
int  voice_mixed_wanted(void);
void voice_mixed_start(unsigned sound, int sdbk, int cue, const char *cue_name,
                       const char *lip_name, const char *talker);

#endif
