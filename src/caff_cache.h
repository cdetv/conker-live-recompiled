/* The carved-asset cache, <save dir>\assets\ (to-do 43): see caff_cache.c. */
#ifndef CAFF_CACHE_H
#define CAFF_CACHE_H

#include <stddef.h>

enum {
    CAFF_CACHE_OK    = 0,   /* up to date / carved */
    CAFF_CACHE_STALE = 1,   /* needs a carve */
    CAFF_CACHE_ERROR = -1   /* no game files, or the cache can't be written */
};

/* done of total packages handled; `what` names the last one. */
typedef void (*CaffCarveProgress)(int done, int total, const char *what, void *ctx);

/* Is the cache made from these game files, with every recipe's file in it?
 * No guest needed. Paths are UTF-8. */
int  caff_cache_check(const char *game_dir, const char *save_dir);

/* Carve every recipe into the cache, then (inventory = 1, the launcher's
 * carve) assets\inventory.json and the texture name table assets\texnames.tsv
 * (texkit.h, to-do 288) from every package, sweep what the new stamp doesn't
 * list, write the stamp last. inventory = 0 (the game at start) keeps an
 * inventory and name table still made from the same packages. sharp = 1
 * makes the sharper text and buttons (assets\sharp\, sharp_gui.h; to-do 53),
 * 0 keeps the last set when made from the same packages. Needs
 * caff_guest_begin() first. CAFF_CACHE_ERROR when the stamp, the inventory or
 * the name table can't be written. */
int  caff_cache_carve(const char *game_dir, const char *save_dir, int inventory, int sharp,
                      CaffCarveProgress progress, void *ctx);

/* 1 when the stamp says the sharper text and buttons were made (to-do 53):
 * the game then loads assets\sharp\ as the lowest texture pack. */
int  caff_cache_sharp(const char *save_dir);

/* At game start, before the game runs: carve only when a feature that is on
 * needs a file the cache lacks. `feature_on` (may be NULL) says which of the
 * built-in recipes' features are on ("voice": to-do 216), and "sharp" (the
 * sharper text and buttons, to-do 53). */
void caff_cache_start(const char *game_dir, const char *save_dir,
                      int (*feature_on)(const char *feature));

/* The lip-sync table, assets\lipsync.bin (to-do 216): every
 * aid\mpegspeech\<track>\default.cut.bin of the recipes' packages.
 *   "LIPS", u32 format (1), u32 tracks, then per track, sorted by name:
 *   char name[32] (lower case, NUL padded), u32 entries, entries x 12 bytes
 *   as stored (u16 marker, u16 shape, f32 time s, f32 length s).
 * Its bytes (malloc'd) when the stamp vouches for it, else NULL. */
#define CAFF_LIPS_FILE   "lipsync.bin"
#define CAFF_LIPS_FORMAT 1
unsigned char *caff_cache_lips(const char *save_dir, size_t *n);

/* The texture name table, assets\texnames.tsv (to-do 288; its lines in
 * texkit.h): its text (malloc'd, NUL-terminated) when the stamp vouches for
 * it and its first line is "texnames\t1", else NULL (never prepared, a cache
 * from before it, or changed since). */
char *caff_cache_names(const char *save_dir, size_t *n);

/* The files the stamp vouches for: their paths below assets\aid\ and sizes
 * (failed recipes and the inventory left out). -1 when there is no stamp
 * (never carved, or a carve stopped midway); *out malloc'd. */
typedef struct {
    char               path[256];
    unsigned long long size;
} CaffCacheFile;

int  caff_cache_files(const char *save_dir, CaffCacheFile **out);

/* A package by name ("Level\Windmill", "texture\Avatars\ace") or file. */
int  caff_resolve_package(const char *game_dir, const char *name, char *out, size_t cap);

#endif
