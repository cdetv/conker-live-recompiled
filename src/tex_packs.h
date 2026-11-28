/* Texture packs (to-do 288; tex_packs.c). */
#ifndef TEX_PACKS_H
#define TEX_PACKS_H

#ifdef __cplusplus
extern "C" {
#endif

#define TEX_PACK_NAME 128               /* a pack's name, UTF-8, with its 0 */

/* At start, after settings_load and before the first texture: remembers the
 * textures folder (RECOMP_TEX_REPLACE) and the save folder, then turns on
 * the packs settings.ini's [texture_packs] has on, in its order, or the ones
 * RECOMP_TEX_PACKS names when that is set (tex_packs_set). */
void tex_packs_start(const char *save_dir);

/* A pack in the F2 list (tex_packs_list). */
typedef struct {
    char name[TEX_PACK_NAME];
    int  on;
} TexPack;

/* The F2 list: the folders in <textures>\packs, in settings.ini's order with
 * its on / off, then the folders it doesn't name yet (new: off) by name; a
 * line whose folder is gone is left out. Their number (at most max), -1
 * when the textures folder is off. Reads the disk. */
int tex_packs_list(TexPack *list, int max);

/* The list saved as settings.ini's [texture_packs] (the whole list, its
 * order) and its on packs switched on at once (tex_packs_set) when they
 * changed. 1 when both worked. */
int tex_packs_save(const TexPack *list, int n);

/* The packs on: "Pack A|Pack B", the top one first (it wins on a texture
 * two packs change), as the folders <textures>\packs\<name>\ for the
 * toolkit's tex_replace; "" for none. Switches from the next frame, while
 * the game runs. The texture name table (<save_dir>\assets\texnames.tsv) is
 * read the first time a pack is on, so their PNGs go by the texture pack
 * kit's names. A name that isn't a plain folder name is left out (a
 * [TEXPACK] line), one given twice counts at its higher place. The number
 * of packs on now, -1 when nothing changed (the textures folder is off, the
 * list is too long, no memory). */
int tex_packs_set(const char *list);

/* The packs on now, as tex_packs_set takes them ("" for none). */
const char *tex_packs_on(void);

/* The folders in <textures>\packs (each a pack, on or not), sorted by name
 * (upper / lower case ignored), at most max: their number, -1 when the
 * textures folder is off. Read from the disk at each call. */
int tex_packs_found(char (*names)[TEX_PACK_NAME], int max);

#ifdef __cplusplus
}
#endif

#endif
