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
/* Level 0x1D with the front end running: the real main menu, not a warp there. */
int conker_OnMainMenu(void);
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
/* NULL = it can play here; else it can't (greyed, refused) and this says where
 * it does play (to-do 193). */
const char *conker_TuneWhere(int i);

/* A wave bank a cheat sound needs (to-do 199): want = 1 keeps it loaded (adds
 * it if the level hasn't it), 0 lets ours go. Returns 1 when its cues play now.
 * Game thread, every frame. */
enum { SOUND_BANK_X, SOUND_BANK_ITSWAR, SOUND_BANK_SFX, SOUND_BANK_N };
int conker_SoundBank(int which, int want);

void debug_tools_frame(void);   /* game thread, end of a frame */

/* Story flags (to-do 170): the game's 238 tasks by name; done = its progress
 * bit. conker_Flag: 1 done, 0 not, -1 no story running (*state = the task's
 * state byte, 3 = done). conker_FlagSet asks for it on the game thread; the
 * level's tasks see it from its next load. */
int conker_FlagCount(void);
const char *conker_FlagName(int id);
int conker_Flag(int id, int *state);
int conker_FlagSet(int id, int on);

/* Guest memory for the console's peek / poke (to-do 172): 0 = not mapped. A
 * poke is at most 8 bytes, written from the calling thread. */
int conker_Peek(unsigned va, void *out, unsigned n);
int conker_Poke(unsigned va, const void *in, unsigned n);

/* The state line's game parts (console state, to-do 169; recomp_manual.c).
 * Reads only, any thread. */
typedef struct {
    int spawn;              /* the spawn the level was entered at */
    int checkpoint;         /* the task table's row for level + spawn, -1 none */
    char cp_name[48];
    int chapter, part;      /* the story's furthest part (part 0 = none) */
    int scene;              /* the cutscene playing, -1 none */
    float scene_s, scene_len;   /* seconds into it, its end */
    const char *doing;      /* the controls' set: "single player", "cutscene" ... */
    int weapon;             /* the weapon out (a B kind), 0 none */
    const char *weapon_name;    /* NULL when not known */
    int b_doing;            /* a B action under way that is no weapon, 0 none */
    const char *b_doing_name;
    int b_here;             /* what B does where Conker stands (pad or level weapon) */
    const char *b_here_name;
    int b_after;            /* the manual's pad: what B does once it is read */
    const char *b_after_name;
    char vehicle[32];       /* what Conker drives, "" none */
    unsigned camera;        /* camera 0's mode, 0 none */
} ConkerState;
int conker_State(ConkerState *s);
/* A B kind's name (0x002FD7D0's switch; story and multiplayer weapons,
 * context pads), NULL when not known (to-do 201). */
const char *conker_BKindName(int kind);
/* A cutscene plays, ends, or waits to start at the end of a load. */
int conker_SceneBusy(void);

#ifdef __cplusplus
}
#endif

#endif
