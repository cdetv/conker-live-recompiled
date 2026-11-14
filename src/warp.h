/**
 * Level warp (--debug; to-do 15, SPEC 10.3). In recomp_manual.c, section "level
 * warp"; the console's warp command and the Debug menu use it. The lists only
 * read the game's tables; a warp asked for here runs on the game thread at the
 * end of its frame.
 */
#ifndef CONKER_WARP_H
#define CONKER_WARP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WARP_LEVELS 75
#define WARP_CHECKPOINTS 238

/* A level's name; NULL when the list leaves it out (unused, multiplayer only, the
 * menus). */
const char *conker_WarpLevelName(int level);

/* A checkpoint's level, -1 if it isn't listed; its name as ASCII when name is
 * given. */
int conker_WarpCheckpoint(int cp, char *name, int size);

/* Why a warp can't happen now, or NULL. */
const char *conker_WarpRefused(void);

/* A level is loading, or a warp waits to start (waitload). */
int conker_LevelLoading(void);

void conker_WarpToCheckpoint(int cp);
void conker_WarpToLevel(int level);

/* A level's first listed checkpoint, -1 if it has none. */
int conker_WarpFirstCheckpoint(int level);

/* The level as the soak test loads it: nothing started, nothing done, its first spawn. */
void conker_WarpSoak(int level);

/* The level at this place in the list by name, -1 past the end; it may have no name to
 * show (conker_WarpLevelName and conker_CutsceneLevelName say). */
int conker_LevelSorted(int pos);

/* A warp happened and the main menu hasn't come since: nothing saves. */
int conker_Warped(void);

/* The cutscene viewer (to-do 99). Bit n of a level's scenes = its scene n; 0 = it
 * has none. */
unsigned long long conker_CutsceneScenes(int level);
const char *conker_CutsceneLevelName(int level);
int conker_CutsceneCurrentLevel(void);

/* Why a scene can't start now, or NULL; warp = it stays there. */
const char *conker_CutsceneRefused(int level, int warp);

/* Plays it, then back to where the player was: the same level with Conker at his
 * spot, or the main menu. */
void conker_CutsceneWatch(int level, int scene);

/* A warp to the level with the scene at the end of its load; it stays there. */
void conker_WarpToCutscene(int level, int scene);

/* Wireframe (to-do 34): 0 off, 1 characters and objects, 2 all of the 3D scene. */
int conker_Wireframe(void);
void conker_SetWireframe(int mode);

/* Timescale (to-do 21): 0-300 %; 0 = the pause key's freeze. Last: what it was before. */
int conker_Timescale(void);
int conker_TimescaleLast(void);
void conker_SetTimescale(int percent);

/* Noclip (to-do 24). */
int conker_Noclip(void);
const char *conker_NoclipRefused(void);
void conker_SetNoclip(int on);

/* Edit Save (to-do 147): Game 1-3 to a chapter's part (edit_save.h numbers:
 * chapter from 0, part from 1). Why not, NULL when it may; msg says what was
 * done or why not. Returns 0 when done. */
const char *conker_EditSaveRefused(int slot);
int conker_EditSave(int slot, int chapter, int part, char *msg, size_t n);

#ifdef __cplusplus
}
#endif

#endif
