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

/* To-do 47 ("Update Cutscene", cutscene_edits.c): one speech wave served cut.
 * The cue's wave (found by the cue's name in the sound banks) reads as its
 * kept byte ranges joined end to end, then silence, at the same file size;
 * the wave must be len bytes (a different game copy is refused). Its bytes
 * are read once here, at the scene start, before the speech start reads the
 * stream. With a Mouse / Bear cheat on, the pitched wave is the one cut. A
 * bank copy the game writes to its cache keeps the wave as recorded.
 * 1 = set; 0 = refused, why says why. One edit at a time (a new one replaces
 * it); _clear ends it. */
typedef struct { unsigned from, to; } VoiceKeep;
int  voice_wave_edit(const char *cue, unsigned len, const VoiceKeep *keep, int n,
                     char *why, int why_size);
void voice_wave_edit_clear(void);

#endif
