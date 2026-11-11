/**
 * Edit Save (to-do 147, SPEC 10.3; edit_save.c): moves a story save's place in
 * the story to the start of a chapter's part, as the Chapters list starts it.
 * Used by the Debug menu and the console (in the game, recomp_manual.c
 * conker_EditSave also updates the game's copy of the slot) and by
 * `conker.exe --edit-save` (the launcher; main.c reads the game's tables
 * from default.xbe).
 */
#ifndef CONKER_EDIT_SAVE_H
#define CONKER_EDIT_SAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ES_CHAPTERS 9
#define ES_BITS     238

/* Reads n bytes of the game's data at a guest address; 0 = not there. */
typedef int (*EsRead)(uint32_t va, void *out, size_t n, void *ctx);

const char *edit_save_word(int chapter);            /* "hung" ... "heist" */
const char *edit_save_chapter_name(int chapter);    /* "Hungover" ... */
int edit_save_parts(int chapter);                   /* 0 past the last */
const char *edit_save_part_name(int chapter, int part);   /* part from 1 */

/* "slop2" (any case) -> chapter 4, part 2. 0 = not a part. */
int edit_save_parse(const char *text, int *chapter, int *part);

typedef struct {
    int checkpoint, level, spawn, money, energy;
    uint8_t bits[(ES_BITS + 7) / 8 + 1];   /* the progress, as the record holds it */
    int count, last;        /* the record's +0x2CC, +0x2D0: bits set, the last one */
    int shown;              /* +0x1C: the part the slot screen shows (from 1) */
    char backup[260];       /* where the old save went */
} EsResult;

/* <save_dir>\saves\Game<slot>\conker.bin to the part's start; a copy of the
 * slot's folder first, in <save_dir>\edit_save_backups. 0 = done; otherwise err
 * says why and nothing changed. */
int edit_save_file(const char *save_dir, int slot, int chapter, int part,
                   EsRead rd, void *ctx, EsResult *res, char *err, size_t errn);

/* A reader over a loaded default.xbe (its sections at their addresses). */
typedef struct {
    const uint8_t *data;
    size_t size;
} EsXbe;
int edit_save_xbe_read(uint32_t va, void *out, size_t n, void *xbe);

#ifdef __cplusplus
}
#endif

#endif
