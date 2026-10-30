/**
 * The decensor patch as two switches (decensor.c): for main.c and the F2
 * overlay (overlay.cpp). `text` picks the switch: 0 = audio, 1 = text.
 */
#ifndef CONKER_DECENSOR_H
#define CONKER_DECENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Find the patch (paths.decensor_dir, else "decensor" next to the .exe) and
 * install the path redirect. After settings_load, before the game opens a
 * file. */
void decensor_init(const char *save_dir);

/* 1 when the patch holds files for that switch. */
int decensor_available(int text);

/* Turn a switch on or off; the game's next open of a file follows it. */
void decensor_set(int text, int on);

#ifdef __cplusplus
}
#endif

#endif
