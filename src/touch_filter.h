/*
 * Touch-made mouse messages and Proton's second copy of a tap (to-do 270;
 * architecture "Mouse in menus: touchscreens"). Each process's window thread
 * calls touch_filter for every message it gets, before anything else sees
 * it: the game's window hook (overlay.cpp) and Report Bug's small window
 * (bug_report_win.c, a process of its own).
 */
#ifndef CONKER_TOUCH_FILTER_H
#define CONKER_TOUCH_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero = drop the message (a tap's second click under Proton); *touch =
 * a touchscreen made this mouse message. Reads GetMessageExtraInfo, so call
 * it for the message this thread got last. */
int touch_filter(unsigned msg, int *touch);

#ifdef __cplusplus
}
#endif

#endif /* CONKER_TOUCH_FILTER_H */
