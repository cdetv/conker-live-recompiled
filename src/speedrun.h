/**
 * The speedrun timer (speedrun.c; to-do 154): game time without loads from a
 * new game, the total and the current chapter's part.
 */
#ifndef CONKER_SPEEDRUN_H
#define CONKER_SPEEDRUN_H

#ifdef __cplusplus
extern "C" {
#endif

void speedrun_game_start(int level);    /* game thread: a game starts (0x002A95B0) */
void speedrun_skip(void);               /* a load just ended: its time is not counted */
void speedrun_frame(void);              /* game thread, once a frame */
void speedrun_set_cutscenes(int on);    /* F2 "Time cutscenes" */
/* 0 before any game this session; chapter / part from 0, -1 = none yet. */
int conker_Speedrun(double *total_ms, double *part_ms, int *chapter, int *part);

#ifdef __cplusplus
}
#endif

#endif
