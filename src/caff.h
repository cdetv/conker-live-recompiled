/*
 * Conker's packages: Rare's CAFF28 archives (the .rbm / .rba files under
 * dvddata\aid), read from the player's own game files (to-do 43; plan in
 * docs/research/r4-0.3.4-package-reader.md). Plain memory in, plain memory
 * out: no guest, no window,
 * no global state, so it links into a stand-alone test (tests/caff) and moves
 * to Linux by swapping caff_read_file / caff_write_file only.
 *
 * A package is a 0x180-byte header, then its sections (.data, then .gpu)
 * back to back. A packed package's sections are runs of blocks that only the
 * game's own decompressor undoes (src/caff_guest.c): caff_open takes that as
 * a function, and without one a packed package fails with CAFF_E_NEED_GUEST.
 *
 * Packed means: the header's flag (+0x3E) is set AND a section's stored size
 * differs from its unpacked size. Two packages on the disc have the flag set
 * over a section stored as it is (Characters\MultiObjects\CombatSquirrelFlag
 * and CombatTedizFlag): they open without the decompressor.
 *
 * Inside the unpacked .data is the table of contents: the files' names (only
 * some packages have them), the parts (each a run of bytes in .data or .gpu
 * belonging to one file), and the relocations: dwords of one part that hold
 * an offset into another part (the game adds that part's address on load).
 * A zpackage's part 1 is its directory (tag "package"): resource names and a
 * relocated pointer to each resource.
 */
#ifndef CAFF_H
#define CAFF_H

#include <stddef.h>
#include <stdint.h>

#define CAFF_HEADER_SIZE   0x180u
#define CAFF_MAX_SECTIONS  4

enum {
    CAFF_OK                = 0,
    CAFF_E_OPEN            = -1,  /* the file cannot be opened or read */
    CAFF_E_MAGIC           = -2,  /* not a CAFF28.01.05.0031 package */
    CAFF_E_HEADER          = -3,  /* checksum, sizes or sections out of range */
    CAFF_E_NEED_GUEST      = -4,  /* packed, and no decompressor given */
    CAFF_E_SECTION_TOO_BIG = -5,  /* a section does not fit the decompressor */
    CAFF_E_UNPACK          = -6,  /* the decompressor failed */
    CAFF_E_NOT_FOUND       = -7,  /* no such resource in the package */
    CAFF_E_NOMEM           = -8,  /* out of memory */
    CAFF_E_TOC             = -9   /* table of contents damaged */
};

/* One line, for logs and messages. */
const char *caff_strerror(int err);

/* The header checksum (+0x14): an ELF hash of the 0x180 header bytes taken
 * as signed, with the checksum's own four bytes counted as zero. */
uint32_t caff_checksum(const uint8_t *header);

/* ── The header of a package as stored (packed or not) ─────────────────── */

typedef struct {
    int      sections;                      /* 1 or 2 on the disc */
    char     name[CAFF_MAX_SECTIONS][9];    /* ".data", ".gpu" */
    uint32_t unpacked[CAFF_MAX_SECTIONS];
    uint32_t stored[CAFF_MAX_SECTIONS];
    int      packed[CAFF_MAX_SECTIONS];     /* goes through the decompressor */
    uint32_t blocks[CAFF_MAX_SECTIONS];     /* a packed section's block count */
    uint32_t data_offset;                   /* file offset of the first section */
    int      flag_set;                      /* +0x3E as stored */
    int      any_packed;
    uint64_t file_size;                     /* data_offset + stored sizes */
} CaffHeader;

/* Check a package's header (magic, checksum, sections, the file's size, and
 * a packed section's block chain). CAFF_OK or CAFF_E_*; `why` (optional)
 * gets a short reason. */
int caff_header_read(const uint8_t *file, size_t n, CaffHeader *out,
                     char *why, size_t why_cap);

/* ── An opened (unpacked) package ──────────────────────────────────────── */

typedef struct CaffPkg CaffPkg;

/* Undo one packed section: n_src stored bytes (its blocks) into exactly
 * n_dst bytes. CAFF_OK or CAFF_E_*. */
typedef int (*CaffUnpackFn)(const uint8_t *src, size_t n_src,
                            uint8_t *dst, size_t n_dst);

/* Read and open a package file (UTF-8 path). `unpack` may be NULL: then a
 * packed package gives CAFF_E_NEED_GUEST. Logs one [CAFF] line on failure. */
int  caff_open(const char *path, CaffUnpackFn unpack, CaffPkg **out);

/* Open a package already in memory and not packed (the bytes are copied).
 * `name` (may be NULL) is only for the [CAFF] line on failure. */
int  caff_open_mem(const uint8_t *data, size_t n, const char *name,
                   CaffPkg **out);

void caff_close(CaffPkg *pkg);

/* The same package stored unpacked: each packed section through `unpack`,
 * the flag clear, stored sizes = unpacked sizes, checksum recomputed.
 * *out is malloc'd. A package with nothing packed comes back as a copy with
 * the same changes (so the two flag-only packages lose their flag). */
int  caff_unpack_with(const uint8_t *file, size_t n, CaffUnpackFn unpack,
                      uint8_t **out, size_t *n_out);

typedef struct {
    uint32_t files, parts, relocs, reloc_offsets;
    uint32_t toc;                           /* offset of the TOC in .data */
    int      sections;
    char     name[CAFF_MAX_SECTIONS][9];
    uint32_t size[CAFF_MAX_SECTIONS];       /* unpacked */
    int      flag_set;                      /* the file as read had +0x3E set */
    int      was_packed;                    /* a section went through unpack */
    int      has_names;                     /* the TOC has a file name block */
    char     tag[16];                       /* part 1: "package", "texture" */
} CaffInfo;

void caff_info(const CaffPkg *pkg, CaffInfo *out);

/* The whole unpacked package (header + sections), valid until caff_close. */
const uint8_t *caff_bytes(const CaffPkg *pkg, size_t *n);

typedef struct {
    uint32_t       file;        /* from 1 */
    uint32_t       offset;      /* in its section */
    uint32_t       size;
    uint8_t        section;     /* 1 .data, 2 .gpu */
    uint8_t        align_log2;
    const uint8_t *bytes;       /* inside caff_bytes */
} CaffPart;

/* Part n (from 1). CAFF_OK or CAFF_E_NOT_FOUND. */
int caff_part(const CaffPkg *pkg, uint32_t n, CaffPart *out);

/* File f's name (from 1), or NULL when the package has no name block. */
const char *caff_file_name(const CaffPkg *pkg, uint32_t f);

/* The part a relocated dword points into: the dword at `offset` in part
 * `src`. 0 when that dword is not relocated. */
uint32_t caff_reloc_target(const CaffPkg *pkg, uint32_t src, uint32_t offset);

/* Relocation record i (from 0): source part, target part, how many dwords,
 * and their offsets in the source part. */
int caff_reloc(const CaffPkg *pkg, uint32_t i, uint32_t *src, uint32_t *dst,
               uint32_t *count, const uint8_t **offsets);

/* ── A zpackage's directory ────────────────────────────────────────────── */

typedef struct {
    const char *name;       /* e.g. "aid\bfdmodel\attach\FryingPanBound\default.rbm" */
    uint32_t    part;       /* the part the resource starts in (0 = none) */
    uint32_t    offset;     /* where in that part */
} CaffDirEntry;

int caff_dir_count(const CaffPkg *pkg);
int caff_dir_entry(const CaffPkg *pkg, int i, CaffDirEntry *out);
/* Directory index of `name` (exact, case-insensitive), or -1. */
int caff_find(const CaffPkg *pkg, const char *name);

/* ── One resource and everything it needs ──────────────────────────────── */

typedef struct {
    uint32_t  start;          /* the part the directory points at */
    uint32_t  start_offset;   /* and where in it */
    uint32_t *files;          /* source file numbers, the resource's own first */
    uint32_t  nfiles;
    uint32_t *parts;          /* source part numbers in the loose file's order:
                                 file by file, its .data parts, then its .gpu */
    uint32_t  nparts;
    uint64_t  data_bytes;     /* the parts' sizes, no padding */
    uint64_t  gpu_bytes;
} CaffClosure;

/* Directory entry `dir_index`'s closure: every part its start part reaches
 * through relocations, and every part of each file reached. */
int  caff_closure(const CaffPkg *pkg, int dir_index, CaffClosure *out);
void caff_closure_free(CaffClosure *c);

/* That closure as a loose package of its own (malloc'd), as the game loads
 * one at aid\<kind>\<name>\default.<ext>: the resource's file first, parts
 * at their alignment, files / parts / relocations numbered anew, not packed,
 * checksum recomputed. After Txukie's conker_caff_extract (MIT). */
int  caff_write_loose(const CaffPkg *pkg, int dir_index, uint8_t **buf, size_t *n);

/* ── The game's own unpacker (src/caff_guest.c; the game build only) ─────
 * Before the game runs, on the thread that will run it, with the XBE in
 * guest memory: caff_guest_begin(), then caff_open(path, caff_guest_unpack,
 * ...) as often as needed, then caff_guest_end(). begin gives
 * CAFF_E_NEED_GUEST when guest memory isn't set up or the scratch is in use. */

int  caff_guest_begin(void);
void caff_guest_end(void);
int  caff_guest_unpack(const uint8_t *src, size_t n_src, uint8_t *dst, size_t n_dst);

/* ── Files (UTF-8 paths; the one place that differs per platform) ──────── */

/* Whole file into a malloc'd buffer. CAFF_OK, CAFF_E_OPEN or CAFF_E_NOMEM. */
int caff_read_file(const char *path, uint8_t **data, size_t *n);
/* Write to "<path>.tmp", then rename over <path>. CAFF_OK or CAFF_E_OPEN. */
int caff_write_file(const char *path, const void *data, size_t n);
/* Make every folder above `path` (not the last part). CAFF_OK or CAFF_E_OPEN. */
int caff_make_parent_dirs(const char *path);

/* Every package (.rbm / .rba) below `aid_root` (a game's dvddata\aid), from
 * the folder listing alone (no file opened): its path below the root with
 * '\' between folders, size and last-write time; sorted by path, case
 * ignored. Returns how many (0: none, or no such folder); *out is malloc'd,
 * freed with caff_list_free. */
typedef struct {
    char    *rel;           /* "zpackage\Level\Windmill\default.rbm" */
    uint64_t size;
    uint64_t time;          /* Windows FILETIME; seconds elsewhere */
} CaffListed;

int  caff_list_packages(const char *aid_root, CaffListed **out);
void caff_list_free(CaffListed *list, int n);

/* ── The inventory (src/caff_inventory.c) ──────────────────────────────── */

/* Every package below `aid_root` opened (packed ones through `unpack`) and
 * written to `out_path` as JSON, the shape of scripts/caff.py inventory:
 *   { "<rel path>": {"tag", "files", "parts", "sections": {name: size},
 *                    "dir": [{"name", "files", "data", "gpu"}, ...]}, ... }
 * one entry per package; "dir" lists a zpackage's resources with the files
 * each needs and their .data / .gpu bytes (caff_closure). A package that
 * doesn't open is { "error": "<why>" } (and a [CAFF] line); the rest go on.
 * `progress` (may be NULL) gets done / total packages after each. CAFF_OK, or
 * CAFF_E_OPEN when the file can't be written, CAFF_E_NOMEM. */
typedef void (*CaffInventoryProgress)(int done, int total, const char *what, void *ctx);

typedef struct {
    int packages, resources, errors;
    size_t bytes;           /* the JSON's size */
} CaffInventoryResult;

int caff_inventory_write(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                         CaffInventoryProgress progress, void *ctx,
                         CaffInventoryResult *result);

/* The same, with `visit` (may be NULL) shown each package that opened, before
 * it is closed (its full path): what else a carve takes from the packages it
 * opens anyway (the lip timelines, to-do 216). */
typedef void (*CaffPackageVisit)(const CaffPkg *pkg, const char *path, void *vctx);

int caff_inventory_write_visit(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                               CaffInventoryProgress progress, void *ctx,
                               CaffPackageVisit visit, void *vctx,
                               CaffInventoryResult *result);

#endif /* CAFF_H */
