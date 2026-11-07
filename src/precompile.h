/*
 * Shader pre-compile pass (to-do 25, SPEC 10.1 item 1): conker.exe
 * --precompile-shaders, started from the launcher's Files tab. See
 * precompile.c.
 */
#ifndef CONKER_PRECOMPILE_H
#define CONKER_PRECOMPILE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PRECOMPILE_BOOT,        /* starting up, waiting for the front end */
    PRECOMPILE_LEVELS,      /* the soak test's map pass */
    PRECOMPILE_CHAPTERS,    /* its chapter pass (every task) */
    PRECOMPILE_FINISH,      /* waiting for the last compiles */
    PRECOMPILE_DONE
};

typedef struct {
    int stage;              /* PRECOMPILE_* */
    int levels, levels_total;
    int chapter, chapters;  /* chapter counts from 1 once begun */
    int task, tasks;        /* in the current chapter */
    float progress;         /* 0-1, the whole run */
    unsigned seconds;       /* since the start */
    unsigned shaders;       /* shaders compiled this run */
    int resumed;            /* continues a stopped run */
    int skipping;           /* racing through tasks the stopped run did */
} PrecompileStatus;

/* From the command line, before settings.ini is read: the switches this run
 * needs (they win over the file's options), and the mode on. */
void precompile_enable(void);
int  precompile_on(void);
/* Once the shader cache folder is known (NULL or empty: no cache, so the
 * pass would keep nothing): the result file goes there. */
void precompile_set_cache_dir(const char *dir);
/* Game thread, once a frame (frame_timer_update's hook). */
void precompile_poll(void);
/* For the progress screen (overlay.cpp), any thread. */
void precompile_status(PrecompileStatus *st);
/* Esc or the window closing: the result file says "stopped" (what was
 * compiled stays in the cache). The caller closes the window. */
void precompile_stop(void);

#ifdef __cplusplus
}
#endif

#endif
