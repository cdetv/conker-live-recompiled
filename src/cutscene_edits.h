/**
 * "Update Cutscene" (Extras, to-do 47; SPEC 10.4): the training's weapon
 * scenes edited to match the weapon (cutscene_edits.c). The calls come from
 * the cutscene hooks in recomp_manual.c, all on the game thread.
 */
#ifndef CONKER_CUTSCENE_EDITS_H
#define CONKER_CUTSCENE_EDITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A scene starts (0x0004EC10's entry): picks the edit, if any. next_part =
 * started as the next part of the scene before (called from 0x00050A39). */
void cutedits_scene_start(uint32_t level, uint32_t scene, uint32_t slot, int next_part);
/* A scene ends (0x000507A0's entry), with the part that follows (-1: none). */
void cutedits_scene_end(uint32_t slot, uint32_t next_level, int next_scene);
/* skip_now (0x0004FDC0's entry): every edit ends. */
void cutedits_skip(void);
/* cutscene_event_run 0x0005E240, before: 1 = drop this event (not run). */
int  cutedits_drop(uint32_t slot, uint32_t rec, int time);
/* The same, after the event ran. */
void cutedits_ran(uint32_t slot, uint32_t rec, int time);
/* The dispatcher's entry (0x00056050; mode 0 = this frame's events). */
void cutedits_dispatch(uint32_t mode, uint32_t slot);
/* The scene clock step's entry (0x000501F0; esi = slot). */
void cutedits_clock(uint32_t slot);
/* Each frame (cheats_frame). */
void cutedits_frame(void);

/* recomp_manual.c: stops the speech when it is this sound (ad_mpeg_Stop
 * 0x0001B180, which also lets go of its lip timeline). 1 = stopped. */
int conker_voice_stop(unsigned sound);

#ifdef __cplusplus
}
#endif

#endif
