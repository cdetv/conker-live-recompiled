/**
 * The --debug tools of the 0.3.0 second round (debug_tools.c; to-dos 155,
 * 156, 158, 159). Any thread may ask; the game thread acts in
 * debug_tools_frame.
 */
#ifndef CONKER_DEBUG_TOOLS_H
#define CONKER_DEBUG_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int index;              /* the character's number (tp <number>) */
    char name[32];          /* its type's name */
    float dist;             /* from Conker */
    int energy;
} ConkerChar;

enum { DBG_SET_ENERGY, DBG_SET_LIVES, DBG_SET_CASH, DBG_SET_OXYGEN };

int conker_Level(void);
/* 1 with Conker's place, 0 outside a level with him (the main menu too). */
int conker_PlayerPos(float *x, float *y, float *z);
void conker_Teleport(float x, float y, float z);
/* 0 outside a story level. Oxygen is in seconds of air left (0-72). */
int conker_SetValue(int what, int value);
int conker_Kill(void);
int conker_CharPos(int n, float *x, float *y, float *z);
/* The characters in the level but Conker, nearest first; -1 outside a level. */
int conker_CharsNear(ConkerChar *out, int max);

/* The jukebox (to-do 159): the game's sound table, read from the game. */
int conker_SoundCount(void);
const char *conker_SoundName(int i, int *number);   /* NULL past the end */
void conker_JukeboxPlay(int number);    /* stops the jukebox's last one first */
void conker_JukeboxStop(void);
/* Its tunes (the music manager's sequences, 1..count-1; NULL = no such tune). */
int conker_TuneCount(void);
const char *conker_TuneName(int i);
void conker_TunePlay(int number);       /* replaces the music once it plays */
void conker_TuneStop(void);             /* the replaced music back */
int conker_TunePlaying(void);           /* the jukebox's tune, 0 = none */
enum { TUNE_NONE, TUNE_STARTING, TUNE_PLAYING, TUNE_ENDED, TUNE_LOADING };
/* TUNE_*; *failed = the last tune that couldn't play in its level (0 = none).
 * TUNE_LOADING: conker_TuneLoading()'s music bank loads (the jukebox's tune
 * before it, if any, plays on meanwhile). */
int conker_TuneStatus(int *failed);
int conker_TuneLoading(void);           /* the tune whose music loads, 0 = none */
int conker_TuneBad(int i);              /* 1 = didn't play in this level */

/* A wave bank a cheat sound needs (to-do 199): want = 1 keeps it loaded (adds
 * it if the level hasn't it), 0 lets ours go. Returns 1 when its cues play now.
 * Game thread, every frame. */
enum { SOUND_BANK_X, SOUND_BANK_ITSWAR, SOUND_BANK_N };
int conker_SoundBank(int which, int want);

void debug_tools_frame(void);   /* game thread, end of a frame */

#ifdef __cplusplus
}
#endif

#endif
