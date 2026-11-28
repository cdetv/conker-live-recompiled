/*
 * The texture pack kit (to-do 238): every texture in the player's own
 * packages, named and hashed so a pack maker can find it without playing to
 * it. Ported from a texture extractor (its texture walk and the facts in
 * its format notes): the same walk, the same names, so each picture lines
 * up with the extractor's PNG of it.
 *
 * Plain memory and caff.h only (no guest, no window), so it links into the
 * stand-alone test (tests/texkit). Everything it reads is the player's game
 * data: what it writes stays on their PC (SPEC 3.3).
 *
 * Words (the extractor's):
 *   texture  a file of type "texture" in a package: a header in its .data
 *            part, its pixels in its .gpu part
 *   image    one picture of a texture: the texture itself, or one frame
 *            ("variant") of a kind-4 texture (New_Conker_Eye.1@3)
 *   hash     the name the game's replacement textures go by: hash_bytes of
 *            the level-0 bytes the game uploads (toolkit nv2a_draw_d3d11.c
 *            texture_stage), 16 hex digits at the start of the PNG's name
 */
#ifndef TEXKIT_H
#define TEXKIT_H

#include <stddef.h>
#include <stdint.h>

#include "caff.h"

#define TEXKIT_NAME_CAP 128

/* textures.md section 2: H+0x04 */
enum { TEXKIT_KIND_IMAGE = 0, TEXKIT_KIND_CUBE = 2, TEXKIT_KIND_VOLUME = 3,
       TEXKIT_KIND_VARIANTS = 4 };

/* Can a replacement PNG stand in for it (texture_stage: 2D, not a cube map,
 * not read signed)? */
enum {
    TEXKIT_REPLACE_YES = 0,
    TEXKIT_REPLACE_CUBE,        /* no: cube maps are never replaced */
    TEXKIT_REPLACE_VOLUME,      /* no: drawn as a 3D texture (PROJECT3D stage;
                                   the barn floor's light maps, Conker's grime),
                                   which texture_stage never replaces */
    TEXKIT_REPLACE_BUMP,        /* maybe: V8U8, only when not read signed */
    TEXKIT_REPLACE_OUTSIDE,     /* no: the game's bytes run past the package */
    TEXKIT_REPLACE_FORMAT,      /* no: a format we don't know */
    TEXKIT_REPLACE_ELSEWHERE    /* no: the game spaces the frames by the header's
                                   size, so it reads this one elsewhere (3
                                   textures: textures.md 3.3) */
};

typedef struct {
    uint32_t file;              /* file number in the package (from 1) */
    uint32_t data_part;         /* the part its header is in */
    char     source;            /* 's' standalone .rba, 'e' a directory entry,
                                   'o' <owner>_texturefile<N> */
    uint32_t format, kind, width, height, mips;
    uint32_t count;             /* kind 3: depth; kind 4: frames; else 0 */
    uint32_t gpu_offset;        /* its .gpu part, in the .gpu section */
    uint32_t gpu_size;
} TexkitTexture;

/* What an image's level 0 really is (textures.md 3.3): the header's format
 * and size, except 15 frames on the disc whose own bytes fit another size. */
typedef struct {
    uint32_t format, width, height, depth;
} TexkitLayout;

typedef struct {
    int      texture;           /* index into TexkitPkg.tex */
    uint32_t variant;           /* frame (0 unless kind 4) */
    char     name[TEXKIT_NAME_CAP];      /* unique in the package (".001") */
    char     file_name[TEXKIT_NAME_CAP]; /* cleaned, "_001" on a case clash, ".png" */
    uint32_t at;                /* level 0, in the .gpu section (the frame table) */
    uint32_t own;               /* its own bytes: up to the next frame or the part's end */
    int      layout_ok;         /* 0: its bytes fit no size (layout = header's) */
    TexkitLayout layout;        /* what it is decoded as */
    /* what the game uploads and hashes (texture_stage) */
    uint32_t game_at;           /* frame v at the first + v x the chain rounded to 0x80 */
    uint32_t game_bytes;        /* level 0 at the header's format and size, every slice */
    uint64_t hash;              /* 0 when not hashed (cube map, outside, format) */
    int      replace;           /* TEXKIT_REPLACE_* */
} TexkitImage;

typedef struct {
    char          package[TEXKIT_NAME_CAP];  /* "Level_Canyon", "Avatars_Bang" */
    TexkitTexture *tex;
    int           ntex;
    TexkitImage   *img;
    int           nimg;
    int           skipped;      /* textures without a .gpu part (none on the disc) */
    const uint8_t *gpu;         /* the package's .gpu section: valid while it is open */
    uint32_t      gpu_size;
} TexkitPkg;

/* The textures and images of one opened package. `rel` = its path below
 * dvddata\aid ("zpackage\Level\Canyon\default.rbm"): it gives the package
 * name. A package with no textures (animations, the two flag packages)
 * gives none. CAFF_OK, or CAFF_E_TOC (with `why`) when its texture data is
 * damaged, or CAFF_E_NOMEM. Free with texkit_free. */
int  texkit_read(const CaffPkg *pkg, const char *rel, TexkitPkg *out, char *why, size_t cap);
void texkit_free(TexkitPkg *p);

/* ── The pieces, for the test ───────────────────────────────────────────── */

/* The toolkit's hash_bytes (nv2a_draw_d3d11.c), from h = 0xCBF29CE484222325. */
uint64_t texkit_hash(const uint8_t *p, size_t n, uint64_t h);
#define TEXKIT_HASH_SEED 0xCBF29CE484222325ull

/* Bytes of level 0 (whole 4x4 blocks for DXT; else texels), x depth. 0 for a
 * format not on the disc. */
uint32_t texkit_level_size(uint32_t format, uint32_t w, uint32_t h, uint32_t depth);
/* Bytes of a 2D mip chain (level i = max(1, W >> i) x max(1, H >> i)). */
uint64_t texkit_chain_size(uint32_t format, uint32_t w, uint32_t h, uint32_t levels);
/* textures.md 3.3: the layout of a frame with `own` bytes. 1, or 0 when
 * nothing fits (then *out = the header's). */
int texkit_layout(uint32_t format, uint32_t kind, uint32_t w, uint32_t h, uint32_t mips,
                  uint32_t count, uint32_t own, TexkitLayout *out);

/* texture-names.md: "<grandparent>_<parent>" of a package path; the leaf a
 * directory entry gives; a standalone .rba's name; the file name rule. */
void texkit_package_name(const char *path, char *out, size_t cap);
void texkit_leaf(const char *path, char *out, size_t cap);
void texkit_standalone_name(const char *package, char *out, size_t cap);
void texkit_clean_name(const char *name, char *out, size_t cap);
/* Names made unique in the given order (".001", exact case), then file names
 * ("_001" for names equal ignoring case, ".png"). Over texkit_read's images. */
void texkit_name_images(TexkitImage *img, int n, char (*base)[TEXKIT_NAME_CAP]);

const char *texkit_format_name(uint32_t format);   /* "DXT1", "L8", or "0x13" */
const char *texkit_replace_word(int replace);      /* the list's column */

/* ── Pixels (src/texkit_pixels.c: the extractor's pixel decoding) ───────── */

/* Picture i of p as RGBA (malloc'd *rgba, *w x *h x 4, rows top to bottom),
 * decoded at its layout from level 0 at its own place (`at`), as the
 * extractor does: its PNG is the same picture. The package must still be
 * open. CAFF_OK; CAFF_E_TOC (with `why`) when level 0 runs past its own
 * bytes or its format, kind or size can't be decoded; CAFF_E_NOMEM. */
int texkit_decode(const TexkitPkg *p, int i, uint8_t **rgba, uint32_t *w, uint32_t *h,
                  char *why, size_t cap);
/* Channels its PNG holds (by the decoded format): 1 L8 (grey), 2 A8L8 (grey
 * + alpha), 3 R5G6B5 / X8R8G8B8 / V8U8 (RGB), else 4 (RGBA). */
int texkit_png_channels(uint32_t format);
/* Level 0 at `data` (n bytes there) of a w x h (x depth: slice 0) texture as
 * RGBA into rgba (w x h x 4). 0; -1 too few bytes, an unknown format or a
 * swizzled size not a power of two; -2 out of memory. For the test. */
int texkit_decode_raw(uint32_t format, const uint8_t *data, size_t n, uint32_t w, uint32_t h,
                      uint32_t depth, uint8_t *rgba);

/* ── The list and the export (src/texkit_tool.c, conker.exe) ────────────── */

typedef void (*TexkitProgress)(int done, int total, const char *what, void *ctx);

typedef struct {
    int packages, failed;       /* failed: packages that didn't open or read */
    int textures, images;
    int distinct;               /* replaceable pictures (one per hash) */
    /* the export */
    int written, skipped;       /* skipped: its PNG was there already */
    int bad;                    /* PNGs that couldn't be decoded or written */
    unsigned long long bytes;   /* of the PNGs written */
} TexkitResult;

/* Every package below `aid_root` (packed ones through `unpack`) as a TSV:
 * one row per image, the hash first. CAFF_OK, or CAFF_E_OPEN when the file
 * can't be written. */
int texkit_list(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                TexkitProgress progress, void *ctx, TexkitResult *result);

/* The same walk, writing each hash's PNG once into out_dir (its `png`
 * column: the package's folder below aid and the picture's file name, as the
 * extractor lays them out: zpackage\Level\Bank\<name>.png) and the list as
 * out_dir\textures.tsv.
 * A PNG already there is kept (a stopped export goes on where it was); each
 * is written as .tmp and renamed, so none is ever half there. CAFF_OK, or
 * CAFF_E_OPEN when the list or the folder can't be written (a PNG that can't
 * be decoded or written is a [TEX] line and counted in `bad`). */
int texkit_export(const char *aid_root, CaffUnpackFn unpack, const char *out_dir,
                  TexkitProgress progress, void *ctx, TexkitResult *result);

/* The name table (to-do 288): every picture's path in a pack -> its hash, so a
 * pack's PNGs go by the extractor's names (the export's `png` paths) and the
 * game still finds the texture each replaces. Made by the launcher's carve
 * (caff_cache.c's inventory pass, which opens every package anyway) into
 * <save dir>\assets\texnames.tsv, from each player's own files:
 *   line 1  "texnames\t1"
 *   then    "<hash>\t<package's folder below aid>\<file name>" per picture,
 *           all of them in package order (every copy of a picture, not only
 *           the export's first holder), UTF-8; hash "-" when nothing can
 *           replace it (cube map, volume, a frame read elsewhere). */
#define TEXKIT_NAMES_FILE   "texnames.tsv"
#define TEXKIT_NAMES_FORMAT 1

typedef struct TexkitNames TexkitNames;

TexkitNames *texkit_names_new(void);                    /* NULL: no memory */
/* One open package's pictures; rel = its path below aid
 * ("zpackage\Level\Bank\default.rbm"). A package texkit_read can't read is a
 * [TEX] line and counted, the rest of the table goes on. */
void texkit_names_add(TexkitNames *t, const CaffPkg *k, const char *rel);
/* Written in one go; CAFF_OK and its size, else a [TEX] line. */
int  texkit_names_write(TexkitNames *t, const char *path, unsigned long long *bytes);
void texkit_names_counts(const TexkitNames *t, int *pictures, int *hashed, int *failed);
void texkit_names_free(TexkitNames *t);

#endif /* TEXKIT_H */
