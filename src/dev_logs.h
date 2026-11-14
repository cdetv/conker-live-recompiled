/**
 * Log switches that can change while the game runs (to-do 171; the console's
 * log command, --debug). Each starts as its RECOMP_<NAME>_LOG says; the code
 * that prints asks dev_log every time instead of keeping what it read once.
 */
#ifndef CONKER_DEV_LOGS_H
#define CONKER_DEV_LOGS_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DLOG_AUDIO, DLOG_XACT, DLOG_VOLUME, DLOG_CHAR, DLOG_SKIP, DLOG_CUTEVENT,
    DLOG_DOF, DLOG_ATTACH, DLOG_MODERN, DLOG_NOCLIP, DLOG_MENU, DLOG_PARTICLE,
    DLOG_PHYS30, DLOG_SHOT30, DLOG_CUTSCENE_FLASH, DLOG_LIGHTMAP,
    DLOG_COUNT
};

/* The switch's level: 0 off, 1 on, 2 and up for the switches that have more. */
int dev_log(int id);
void dev_log_set(int id, int level);

int dev_log_find(const char *name);     /* "audio" -> DLOG_AUDIO, -1 = none */
const char *dev_log_name(int id);       /* "audio" */
const char *dev_log_env(int id);        /* "RECOMP_AUDIO_LOG" */
const char *dev_log_what(int id);       /* what it prints */
int dev_log_max(int id);                /* its highest level */

#ifdef __cplusplus
}
#endif

#endif
