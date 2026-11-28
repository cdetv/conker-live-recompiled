/*
 * Conker's packages (CAFF28): checking, opening and walking them. See caff.h.
 *
 * Everything here works on plain memory and trusts nothing it reads: a
 * package in the player's game folder may be damaged, so every count, size
 * and offset is checked (in 64 bits) before it is used, and a package that
 * does not add up is refused with a CAFF_E_* code instead of read past.
 *
 * The format (all 266 packages of the PAL disc checked, 2026-11-16 / -18):
 *   header 0x180 bytes, magic "CAFF28.01.05.0031"; +0x14 checksum; +0x18
 *   files, +0x1C parts, +0x20 / +0x24 two tables (empty in Conker), +0x28
 *   relocation records, +0x2C relocation offsets, +0x34 the TOC's offset in
 *   .data, +0x38 the first section's file offset, +0x3D section count,
 *   +0x3E packed flag; per section 0x28 bytes from +0x40: name, +0x10
 *   unpacked size, +0x24 stored size. (+0x0C is not "compressed": the .gpu
 *   sections are packed with 0 there.)
 *   A packed section is blocks {u32 unpacked, u32 stored, bytes}.
 *   TOC: u32 name block size k; if k, a u32 offset per file and k bytes of
 *   names; the parts (14 bytes: u32 file, u32 offset in its section, u32
 *   size, u8 section 1 / 2, u8 log2 alignment); the two empty tables (12 and
 *   4 bytes an entry); relocation records {u32 source part, u32 target part,
 *   u32 count}; then all their offsets (u32, in the source part, not always
 *   4-aligned).
 * The layout was first read by the Berri research (scripts/berri_pkg.py) and
 * Txukie's conker_caff.c (MIT); this code is ours.
 *
 * caff_closure and caff_write_loose follow conker_caff_extract in Txukie's
 * xboxrecomp-conker (MIT, Copyright (c) 2026 sp00nz, commit d0edebc): they
 * worked out how one resource of a package can be written as a loose file
 * the game loads (its file first, parts re-packed at their alignment, files,
 * parts and relocations numbered anew). Thank you. Written anew here on the
 * checked reader. On the disc every directory entry points at the first
 * .data part of its file, +0x20 (all 22,532 entries, 2026-11-18), so the
 * resource is part 1 of its loose file; the writer refuses anything else.
 */
#include "caff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#endif

#define CAFF_MAGIC      "CAFF28.01.05.0031"
#define CAFF_MAGIC_LEN  17u
#define MAX_ALIGN_LOG2  12u                 /* the disc's largest is 7 */
#define MAX_PACKAGE     (1024ull << 20)     /* a Conker package is < 30 MB */

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

typedef struct { uint32_t src, off, dst; } Reloc;

struct CaffPkg {
    uint8_t       *buf;
    size_t         n;
    CaffInfo       info;
    const uint8_t *sec[CAFF_MAX_SECTIONS];
    const uint8_t *index;                   /* the parts, 14 bytes each */
    const uint8_t *rel;                     /* relocation records */
    const uint8_t *roff;                    /* relocation offsets */
    uint32_t      *rel_first;               /* record i's first offset */
    const uint8_t *name_offs;               /* NULL: no name block */
    const uint8_t *names;
    Reloc         *map;                     /* every relocated dword, sorted */
    CaffDirEntry  *dir;
    int            ndir;
};

const char *caff_strerror(int err)
{
    switch (err) {
    case CAFF_OK:                return "ok";
    case CAFF_E_OPEN:            return "cannot open or read the file";
    case CAFF_E_MAGIC:           return "not a Conker package (CAFF28)";
    case CAFF_E_HEADER:          return "damaged package header";
    case CAFF_E_NEED_GUEST:      return "packed, needs the game's unpacker";
    case CAFF_E_SECTION_TOO_BIG: return "a section is too big to unpack";
    case CAFF_E_UNPACK:          return "unpacking failed";
    case CAFF_E_NOT_FOUND:       return "not in the package";
    case CAFF_E_NOMEM:           return "out of memory";
    case CAFF_E_TOC:             return "damaged table of contents";
    default:                     return "unknown error";
    }
}

uint32_t caff_checksum(const uint8_t *hdr)
{
    uint32_t c = 0, i, top;

    for (i = 0; i < CAFF_HEADER_SIZE; i++) {
        int32_t b = (i >= 0x14 && i < 0x18) ? 0 : (int8_t)hdr[i];
        c = (c << 4) + (uint32_t)b;
        if ((top = c & 0xF0000000u))
            c ^= top | (top >> 24);
    }
    return c;
}

static int fail(int err, char *why, size_t cap, const char *text)
{
    if (why && cap)
        snprintf(why, cap, "%s", text);
    return err;
}

/* ── The stored header ─────────────────────────────────────────────────── */

int caff_header_read(const uint8_t *file, size_t n, CaffHeader *h,
                     char *why, size_t cap)
{
    uint64_t at;
    int i;

    memset(h, 0, sizeof *h);
    if (why && cap)
        why[0] = 0;
    if (n < CAFF_MAGIC_LEN || memcmp(file, CAFF_MAGIC, CAFF_MAGIC_LEN))
        return fail(CAFF_E_MAGIC, why, cap, "no CAFF28.01.05.0031 at the start");
    if (n < CAFF_HEADER_SIZE)
        return fail(CAFF_E_HEADER, why, cap, "file shorter than its header");
    if (caff_checksum(file) != get32(file + 0x14))
        return fail(CAFF_E_HEADER, why, cap, "header checksum does not match");
    h->sections = file[0x3D];
    h->flag_set = file[0x3E] != 0;
    h->data_offset = get32(file + 0x38);
    if (h->sections < 1 || h->sections > CAFF_MAX_SECTIONS)
        return fail(CAFF_E_HEADER, why, cap, "section count out of range");
    if (h->data_offset < CAFF_HEADER_SIZE || h->data_offset > n)
        return fail(CAFF_E_HEADER, why, cap, "first section outside the file");

    at = h->data_offset;
    for (i = 0; i < h->sections; i++) {
        const uint8_t *s = file + 0x40 + 0x28 * i;

        memcpy(h->name[i], s, 8);
        h->name[i][8] = 0;
        h->unpacked[i] = get32(s + 0x10);
        h->stored[i] = get32(s + 0x24);
        if (h->unpacked[i] > MAX_PACKAGE || h->stored[i] > MAX_PACKAGE)
            return fail(CAFF_E_HEADER, why, cap, "section size out of range");
        h->packed[i] = h->flag_set && h->stored[i] != h->unpacked[i];
        if (!h->packed[i] && h->stored[i] != h->unpacked[i])
            return fail(CAFF_E_HEADER, why, cap,
                        "sizes differ in a section that is not packed");
        if (at + h->stored[i] > n)
            return fail(CAFF_E_HEADER, why, cap, "file shorter than its sections");
        if (h->packed[i]) {
            /* The block chain must end exactly on the stored size and add
             * up to the unpacked size, or the unpacker would run off. */
            const uint8_t *p = file + at, *end = p + h->stored[i];
            uint64_t sum = 0;

            while (end - p >= 8) {
                uint32_t bu = get32(p), bs = get32(p + 4);

                p += 8;
                if (bs > (uint64_t)(end - p))
                    break;
                p += bs;
                sum += bu;
                h->blocks[i]++;
            }
            if (p != end || sum != h->unpacked[i])
                return fail(CAFF_E_HEADER, why, cap,
                            "a packed section's blocks do not add up");
            h->any_packed = 1;
        }
        at += h->stored[i];
    }
    if (strcmp(h->name[0], ".data"))
        return fail(CAFF_E_HEADER, why, cap, "first section is not .data");
    h->file_size = at;
    return CAFF_OK;
}

/* ── Unpacking into a new buffer ───────────────────────────────────────── */

int caff_unpack_with(const uint8_t *file, size_t n, CaffUnpackFn unpack,
                     uint8_t **out, size_t *n_out)
{
    CaffHeader h;
    uint64_t total;
    uint8_t *buf, *dst;
    const uint8_t *src;
    int i, rc;

    *out = NULL;
    *n_out = 0;
    if ((rc = caff_header_read(file, n, &h, NULL, 0)))
        return rc;
    if (h.any_packed && !unpack)
        return CAFF_E_NEED_GUEST;
    total = h.data_offset;
    for (i = 0; i < h.sections; i++)
        total += h.unpacked[i];
    if (total > MAX_PACKAGE)
        return CAFF_E_HEADER;
    if (!(buf = malloc((size_t)total)))
        return CAFF_E_NOMEM;
    memcpy(buf, file, h.data_offset);
    src = file + h.data_offset;
    dst = buf + h.data_offset;
    for (i = 0; i < h.sections; i++) {
        if (h.packed[i]) {
            if ((rc = unpack(src, h.stored[i], dst, h.unpacked[i]))) {
                free(buf);
                return rc;
            }
        } else {
            memcpy(dst, src, h.stored[i]);
        }
        put32(buf + 0x40 + 0x28 * i + 0x24, h.unpacked[i]);
        src += h.stored[i];
        dst += h.unpacked[i];
    }
    buf[0x3E] = 0;
    put32(buf + 0x14, caff_checksum(buf));
    *out = buf;
    *n_out = (size_t)total;
    return CAFF_OK;
}

/* ── Opening an unpacked package ───────────────────────────────────────── */

static int reloc_cmp(const void *a, const void *b)
{
    const Reloc *x = a, *y = b;

    if (x->src != y->src)
        return x->src < y->src ? -1 : 1;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return 0;
}

static int part_get(const CaffPkg *k, uint32_t n, CaffPart *p)
{
    const uint8_t *e;

    if (n < 1 || n > k->info.parts)
        return CAFF_E_NOT_FOUND;
    e = k->index + 14u * (n - 1);
    p->file = get32(e);
    p->offset = get32(e + 4);
    p->size = get32(e + 8);
    p->section = e[12];
    p->align_log2 = e[13];
    p->bytes = (p->section >= 1 && p->section <= k->info.sections)
             ? k->sec[p->section - 1] + p->offset : NULL;
    return CAFF_OK;
}

/* Is there a NUL in p[0 .. n)? */
static int terminated(const uint8_t *p, uint64_t n)
{
    return n && memchr(p, 0, (size_t)n) != NULL;
}

static int parse_toc(CaffPkg *k, char *why, size_t cap)
{
    CaffInfo *in = &k->info;
    const uint8_t *data = k->sec[0];
    uint64_t dsize = in->size[0], pos = in->toc, need, sum;
    uint32_t i, j, nmap = 0, k_names, ntab1, ntab2;
    CaffPart p, t;

    ntab1 = get32(k->buf + 0x20);
    ntab2 = get32(k->buf + 0x24);
    if (pos + 4 > dsize)
        return fail(CAFF_E_TOC, why, cap, "table of contents outside .data");
    if (in->files > in->parts)
        return fail(CAFF_E_TOC, why, cap, "more files than parts");
    k_names = get32(data + pos);
    pos += 4;
    if (k_names) {
        need = 4ull * in->files + k_names;
        if (need > dsize - pos)
            return fail(CAFF_E_TOC, why, cap, "name block runs past .data");
        k->name_offs = data + pos;
        k->names = data + pos + 4ull * in->files;
        for (i = 0; i < in->files; i++) {
            uint32_t o = get32(k->name_offs + 4ull * i);

            if (o >= k_names || !terminated(k->names + o, k_names - o))
                return fail(CAFF_E_TOC, why, cap, "a file name runs past its block");
        }
        pos += need;
        in->has_names = 1;
    }
    need = 14ull * in->parts + 12ull * ntab1 + 4ull * ntab2
         + 12ull * in->relocs + 4ull * in->reloc_offsets;
    if (need > dsize - pos)
        return fail(CAFF_E_TOC, why, cap, "tables run past .data");
    k->index = data + pos;
    pos += 14ull * in->parts + 12ull * ntab1 + 4ull * ntab2;
    k->rel = data + pos;
    k->roff = k->rel + 12ull * in->relocs;

    for (i = 1; i <= in->parts; i++) {
        part_get(k, i, &p);
        if (p.file < 1 || p.file > in->files)
            return fail(CAFF_E_TOC, why, cap, "a part's file number is out of range");
        if (p.section < 1 || p.section > in->sections)
            return fail(CAFF_E_TOC, why, cap, "a part's section is out of range");
        if (p.align_log2 > MAX_ALIGN_LOG2)
            return fail(CAFF_E_TOC, why, cap, "a part's alignment is out of range");
        if ((uint64_t)p.offset + p.size > in->size[p.section - 1])
            return fail(CAFF_E_TOC, why, cap, "a part runs past its section");
    }

    if (!(k->rel_first = malloc(((size_t)in->relocs + 1) * sizeof *k->rel_first)))
        return CAFF_E_NOMEM;
    for (i = 0, sum = 0; i < in->relocs; i++) {
        k->rel_first[i] = (uint32_t)sum;
        sum += get32(k->rel + 12ull * i + 8);
        if (sum > in->reloc_offsets)
            break;
    }
    if (sum != in->reloc_offsets)
        return fail(CAFF_E_TOC, why, cap, "relocation counts do not add up");
    k->rel_first[in->relocs] = (uint32_t)sum;
    if (in->reloc_offsets
        && !(k->map = malloc((size_t)in->reloc_offsets * sizeof *k->map)))
        return CAFF_E_NOMEM;
    for (i = 0; i < in->relocs; i++) {
        uint32_t src = get32(k->rel + 12ull * i), dst = get32(k->rel + 12ull * i + 4);
        uint32_t cnt = k->rel_first[i + 1] - k->rel_first[i];

        if (part_get(k, src, &p) || part_get(k, dst, &t))
            return fail(CAFF_E_TOC, why, cap, "a relocation names a missing part");
        for (j = 0; j < cnt; j++) {
            uint32_t o = get32(k->roff + 4ull * (k->rel_first[i] + j));

            if ((uint64_t)o + 4 > p.size)
                return fail(CAFF_E_TOC, why, cap, "a relocation runs past its part");
            if (get32(p.bytes + o) > t.size)
                return fail(CAFF_E_TOC, why, cap, "a pointer runs past the part it points into");
            k->map[nmap].src = src;
            k->map[nmap].off = o;
            k->map[nmap].dst = dst;
            nmap++;
        }
    }
    if (nmap > 1)
        qsort(k->map, nmap, sizeof *k->map, reloc_cmp);
    for (i = 1; i < nmap; i++)
        if (k->map[i].src == k->map[i - 1].src && k->map[i].off == k->map[i - 1].off)
            return fail(CAFF_E_TOC, why, cap, "a dword is relocated twice");

    /* Part 1's tag, and a zpackage's directory. */
    if (in->parts && !part_get(k, 1, &p)) {
        size_t len = p.size < sizeof in->tag - 1 ? p.size : sizeof in->tag - 1;
        const uint8_t *z = memchr(p.bytes, 0, len);

        memcpy(in->tag, p.bytes, z ? (size_t)(z - p.bytes) : len);
        if (!strcmp(in->tag, "package")) {
            uint32_t count;

            if (p.size < 0x24)
                return fail(CAFF_E_TOC, why, cap, "directory part too small");
            count = get32(p.bytes + 0x20);
            if (0x24ull + 8ull * count > p.size)
                return fail(CAFF_E_TOC, why, cap, "directory runs past its part");
            if (count && !(k->dir = calloc(count, sizeof *k->dir)))
                return CAFF_E_NOMEM;
            for (i = 0; i < count; i++) {
                uint32_t slot = 0x24 + 8 * i, np, no = get32(p.bytes + slot);
                CaffPart q;

                np = caff_reloc_target(k, 1, slot);
                part_get(k, np ? np : 1, &q);
                if (no >= q.size || !terminated(q.bytes + no, q.size - no))
                    return fail(CAFF_E_TOC, why, cap, "a directory name runs past its part");
                k->dir[i].name = (const char *)q.bytes + no;
                k->dir[i].part = caff_reloc_target(k, 1, slot + 4);
                k->dir[i].offset = get32(p.bytes + slot + 4);
            }
            k->ndir = (int)count;
        }
    }
    return CAFF_OK;
}

/* Open `buf` (malloc'd, taken over: freed on failure too). */
static int open_owned(uint8_t *buf, size_t n, const char *name, int flag_set,
                      int was_packed, CaffPkg **out)
{
    CaffHeader h;
    CaffPkg *k;
    char why[96];
    uint64_t at;
    int i, rc;

    *out = NULL;
    rc = caff_header_read(buf, n, &h, why, sizeof why);
    if (!rc && h.any_packed) {
        rc = CAFF_E_NEED_GUEST;
        why[0] = 0;
    }
    if (rc) {
        free(buf);
        if (name)
            fprintf(stderr, "[CAFF] %s: %s%s%s\n", name, caff_strerror(rc),
                    why[0] ? ": " : "", why);
        return rc;
    }
    if (!(k = calloc(1, sizeof *k))) {
        free(buf);
        return CAFF_E_NOMEM;
    }
    k->buf = buf;
    k->n = n;
    k->info.files = get32(buf + 0x18);
    k->info.parts = get32(buf + 0x1C);
    k->info.relocs = get32(buf + 0x28);
    k->info.reloc_offsets = get32(buf + 0x2C);
    k->info.toc = get32(buf + 0x34);
    k->info.sections = h.sections;
    k->info.flag_set = flag_set;
    k->info.was_packed = was_packed;
    for (i = 0, at = h.data_offset; i < h.sections; i++) {
        memcpy(k->info.name[i], h.name[i], sizeof h.name[i]);
        k->info.size[i] = h.unpacked[i];
        k->sec[i] = buf + at;
        at += h.unpacked[i];
    }
    why[0] = 0;
    if ((rc = parse_toc(k, why, sizeof why))) {
        if (name)
            fprintf(stderr, "[CAFF] %s: %s%s%s\n", name, caff_strerror(rc),
                    why[0] ? ": " : "", why);
        caff_close(k);
        return rc;
    }
    *out = k;
    return CAFF_OK;
}

int caff_open_mem(const uint8_t *data, size_t n, const char *name, CaffPkg **out)
{
    uint8_t *copy;

    *out = NULL;
    if (!(copy = malloc(n ? n : 1)))
        return CAFF_E_NOMEM;
    memcpy(copy, data, n);
    return open_owned(copy, n, name, n > 0x3E && data[0x3E], 0, out);
}

int caff_open(const char *path, CaffUnpackFn unpack, CaffPkg **out)
{
    uint8_t *file, *flat;
    size_t n, n_flat;
    CaffHeader h;
    char why[96];
    int rc;

    *out = NULL;
    if ((rc = caff_read_file(path, &file, &n))) {
        fprintf(stderr, "[CAFF] %s: %s\n", path, caff_strerror(rc));
        return rc;
    }
    rc = caff_header_read(file, n, &h, why, sizeof why);
    if (rc || !h.any_packed)        /* open_owned logs a bad header itself */
        return open_owned(file, n, path, n > 0x3E && file[0x3E], 0, out);
    if (!unpack) {
        free(file);
        fprintf(stderr, "[CAFF] %s: %s\n", path, caff_strerror(CAFF_E_NEED_GUEST));
        return CAFF_E_NEED_GUEST;
    }
    rc = caff_unpack_with(file, n, unpack, &flat, &n_flat);
    free(file);
    if (rc) {
        fprintf(stderr, "[CAFF] %s: %s\n", path, caff_strerror(rc));
        return rc;
    }
    return open_owned(flat, n_flat, path, 1, 1, out);
}

void caff_close(CaffPkg *k)
{
    if (!k)
        return;
    free(k->dir);
    free(k->map);
    free(k->rel_first);
    free(k->buf);
    free(k);
}

void caff_info(const CaffPkg *k, CaffInfo *out)
{
    *out = k->info;
}

const uint8_t *caff_bytes(const CaffPkg *k, size_t *n)
{
    *n = k->n;
    return k->buf;
}

int caff_part(const CaffPkg *k, uint32_t n, CaffPart *out)
{
    return part_get(k, n, out);
}

const char *caff_file_name(const CaffPkg *k, uint32_t f)
{
    if (!k->name_offs || f < 1 || f > k->info.files)
        return NULL;
    return (const char *)k->names + get32(k->name_offs + 4ull * (f - 1));
}

uint32_t caff_reloc_target(const CaffPkg *k, uint32_t src, uint32_t offset)
{
    size_t lo = 0, hi = k->map ? k->info.reloc_offsets : 0;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Reloc *r = &k->map[mid];

        if (r->src == src && r->off == offset)
            return r->dst;
        if (r->src < src || (r->src == src && r->off < offset))
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

int caff_reloc(const CaffPkg *k, uint32_t i, uint32_t *src, uint32_t *dst,
               uint32_t *count, const uint8_t **offsets)
{
    if (i >= k->info.relocs)
        return CAFF_E_NOT_FOUND;
    *src = get32(k->rel + 12ull * i);
    *dst = get32(k->rel + 12ull * i + 4);
    *count = k->rel_first[i + 1] - k->rel_first[i];
    *offsets = k->roff + 4ull * k->rel_first[i];
    return CAFF_OK;
}

int caff_dir_count(const CaffPkg *k)
{
    return k->ndir;
}

int caff_dir_entry(const CaffPkg *k, int i, CaffDirEntry *out)
{
    if (i < 0 || i >= k->ndir)
        return CAFF_E_NOT_FOUND;
    *out = k->dir[i];
    return CAFF_OK;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int caff_find(const CaffPkg *k, const char *name)
{
    int i;

    for (i = 0; i < k->ndir; i++) {
        const char *a = k->dir[i].name, *b = name;

        while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return i;
    }
    return -1;
}

/* ── One resource's closure ────────────────────────────────────────────── */

/* First index in the sorted relocation map with source part >= src. */
static size_t map_first(const CaffPkg *k, uint32_t src)
{
    size_t lo = 0, hi = k->map ? k->info.reloc_offsets : 0;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k->map[mid].src < src)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void caff_closure_free(CaffClosure *c)
{
    if (!c)
        return;
    free(c->files);
    free(c->parts);
    memset(c, 0, sizeof *c);
}

int caff_closure(const CaffPkg *k, int dir_index, CaffClosure *out)
{
    uint32_t np = k->info.parts, nf = k->info.files, i, *stack = NULL, sp = 0;
    uint32_t *first = NULL, *by_file = NULL;    /* parts grouped by file */
    uint8_t *sel = NULL, *fsel = NULL;
    CaffDirEntry e;
    CaffPart p;
    int rc = CAFF_E_NOMEM, sec;

    memset(out, 0, sizeof *out);
    if (caff_dir_entry(k, dir_index, &e))
        return CAFF_E_NOT_FOUND;
    if (!e.part)
        return CAFF_E_TOC;
    sel = calloc((size_t)np + 1, 1);
    fsel = calloc((size_t)nf + 1, 1);
    first = calloc((size_t)nf + 2, sizeof *first);
    by_file = malloc(((size_t)np + 1) * sizeof *by_file);
    /* Each part is pushed at most once through sel / fsel bookkeeping below,
     * plus once per relocation target: bound the stack by both. */
    stack = malloc(((size_t)np + k->info.reloc_offsets + 1) * sizeof *stack);
    out->files = malloc(((size_t)nf + 1) * sizeof *out->files);
    out->parts = malloc(((size_t)np + 1) * sizeof *out->parts);
    if (!sel || !fsel || !first || !by_file || !stack || !out->files || !out->parts)
        goto done;

    /* Parts by file (counting sort, keeps part order inside a file). */
    for (i = 1; i <= np; i++) {
        part_get(k, i, &p);
        first[p.file + 1]++;
    }
    for (i = 1; i <= nf + 1; i++)
        first[i] += first[i - 1];
    {
        uint32_t *fill = calloc((size_t)nf + 1, sizeof *fill);
        if (!fill)
            goto done;
        for (i = 1; i <= np; i++) {
            part_get(k, i, &p);
            by_file[first[p.file] + fill[p.file]++] = i;
        }
        free(fill);
    }

    /* Walk: a part brings its file's other parts and its pointers' targets. */
    stack[sp++] = e.part;
    while (sp) {
        uint32_t q = stack[--sp], f;
        size_t m;

        if (sel[q])
            continue;
        sel[q] = 1;
        part_get(k, q, &p);
        f = p.file;
        if (!fsel[f]) {
            fsel[f] = 1;
            for (i = first[f]; i < first[f + 1]; i++)
                if (!sel[by_file[i]])
                    stack[sp++] = by_file[i];
        }
        for (m = map_first(k, q); m < k->info.reloc_offsets && k->map[m].src == q; m++)
            if (!sel[k->map[m].dst])
                stack[sp++] = k->map[m].dst;
    }

    /* Files: the resource's own first, then by their lowest part. */
    part_get(k, e.part, &p);
    out->files[out->nfiles++] = p.file;
    fsel[p.file] = 2;
    for (i = 1; i <= np; i++) {
        part_get(k, i, &p);
        if (sel[i] && fsel[p.file] == 1) {
            fsel[p.file] = 2;
            out->files[out->nfiles++] = p.file;
        }
    }
    /* Parts: file by file, .data then .gpu, each in part order. */
    for (i = 0; i < out->nfiles; i++) {
        uint32_t f = out->files[i], j;

        for (sec = 1; sec <= 2; sec++)
            for (j = first[f]; j < first[f + 1]; j++) {
                part_get(k, by_file[j], &p);
                if (p.section != sec)
                    continue;
                out->parts[out->nparts++] = by_file[j];
                if (sec == 1)
                    out->data_bytes += p.size;
                else
                    out->gpu_bytes += p.size;
            }
    }
    out->start = e.part;
    out->start_offset = e.offset;
    rc = CAFF_OK;
done:
    free(sel);
    free(fsel);
    free(first);
    free(by_file);
    free(stack);
    if (rc)
        caff_closure_free(out);
    return rc;
}

/* ── Writing one resource as a loose package ───────────────────────────── */

typedef struct { uint8_t *b; size_t len, cap; int bad; } Buf;

static void buf_add(Buf *b, const void *src, size_t n)
{
    if (b->bad)
        return;
    if (b->len + n > b->cap) {
        size_t cap = (b->len + n) * 2 + 4096;
        uint8_t *nb = realloc(b->b, cap);

        if (!nb) {
            b->bad = 1;
            return;
        }
        b->b = nb;
        b->cap = cap;
    }
    if (src)
        memcpy(b->b + b->len, src, n);
    else
        memset(b->b + b->len, 0, n);
    b->len += n;
}

static void buf_u32(Buf *b, uint32_t v)
{
    uint8_t t[4];

    put32(t, v);
    buf_add(b, t, 4);
}

static void buf_pad(Buf *b, size_t align)
{
    size_t want = (b->len + align - 1) & ~(align - 1);
    buf_add(b, NULL, want - b->len);
}

int caff_write_loose(const CaffPkg *k, int dir_index, uint8_t **out, size_t *n_out)
{
    CaffClosure c;
    Buf d = {0}, g = {0}, names = {0};
    uint32_t *newpart = NULL, *newfile = NULL, i, nr = 0, no = 0, toc;
    uint8_t hdr[CAFF_HEADER_SIZE];
    CaffPart p;
    CaffDirEntry e;
    int rc, s, nsec;

    *out = NULL;
    *n_out = 0;
    if ((rc = caff_closure(k, dir_index, &c)))
        return rc;
    caff_dir_entry(k, dir_index, &e);
    rc = CAFF_E_NOMEM;
    newpart = calloc((size_t)k->info.parts + 1, sizeof *newpart);
    newfile = calloc((size_t)k->info.files + 1, sizeof *newfile);
    if (!newpart || !newfile)
        goto done;
    for (i = 0; i < c.nfiles; i++)
        newfile[c.files[i]] = i + 1;
    for (i = 0; i < c.nparts; i++)
        newpart[c.parts[i]] = i + 1;
    rc = CAFF_E_TOC;
    if (newpart[c.start] != 1)          /* the game takes part 1 as the resource */
        goto done;

    /* The parts' bytes at their alignment; the index entries alongside. */
    {
        Buf idx = {0};

        for (i = 0; i < c.nparts; i++) {
            Buf *b;
            uint8_t ent[14];

            part_get(k, c.parts[i], &p);
            b = p.section == 1 ? &d : &g;
            buf_pad(b, (size_t)1 << p.align_log2);
            put32(ent, newfile[p.file]);
            put32(ent + 4, (uint32_t)b->len);
            put32(ent + 8, p.size);
            ent[12] = p.section;
            ent[13] = p.align_log2;
            buf_add(&idx, ent, 14);
            buf_add(b, p.bytes, p.size);
        }
        buf_pad(&d, 4);
        toc = (uint32_t)d.len;

        /* Names, one per file, as loose packages carry them: the resource's
         * own, then (as Txukie's) a placeholder per other file. */
        for (i = 0; i < c.nfiles; i++) {
            char nm[48];
            const char *s2 = nm;

            if (i == 0)
                s2 = e.name;
            else
                snprintf(nm, sizeof nm, "C:\\Temp\\part%u.bin", c.files[i]);
            buf_add(&names, s2, strlen(s2) + 1);
        }
        buf_u32(&d, (uint32_t)names.len);
        {
            uint32_t at = 0;
            for (i = 0; i < c.nfiles; i++) {
                const char *s2 = (const char *)names.b + at;
                buf_u32(&d, at);
                at += (uint32_t)strlen(s2) + 1;
            }
        }
        buf_add(&d, names.b, names.len);
        buf_add(&d, idx.b, idx.len);
        free(idx.b);
        if (idx.bad)
            d.bad = 1;
    }

    /* The relocations among the parts taken, renumbered; their offsets. */
    {
        Buf offs = {0};

        for (i = 0; i < k->info.relocs; i++) {
            uint32_t src, dst, cnt;
            const uint8_t *o;

            caff_reloc(k, i, &src, &dst, &cnt, &o);
            if (!newpart[src])
                continue;
            if (!newpart[dst]) {        /* cannot happen: the closure follows them */
                free(offs.b);
                goto done;
            }
            buf_u32(&d, newpart[src]);
            buf_u32(&d, newpart[dst]);
            buf_u32(&d, cnt);
            buf_add(&offs, o, 4ull * cnt);
            nr++;
            no += cnt;
        }
        buf_add(&d, offs.b, offs.len);
        free(offs.b);
        if (offs.bad)
            d.bad = 1;
    }
    buf_pad(&d, 4);
    buf_pad(&g, 128);
    rc = CAFF_E_NOMEM;
    if (d.bad || g.bad || names.bad)
        goto done;

    /* Header: the source's, with this package's counts and sizes. */
    memcpy(hdr, k->buf, CAFF_HEADER_SIZE);
    nsec = g.len ? 2 : 1;
    put32(hdr + 0x18, c.nfiles);
    put32(hdr + 0x1C, c.nparts);
    put32(hdr + 0x20, 0);
    put32(hdr + 0x24, 0);
    put32(hdr + 0x28, nr);
    put32(hdr + 0x2C, no);
    put32(hdr + 0x30, 0);
    put32(hdr + 0x34, toc);
    put32(hdr + 0x38, CAFF_HEADER_SIZE);
    hdr[0x3D] = (uint8_t)nsec;
    hdr[0x3E] = 0;
    for (s = 0; s < CAFF_MAX_SECTIONS; s++) {
        uint8_t *se = hdr + 0x40 + 0x28 * s;
        uint32_t len = (uint32_t)(s ? g.len : d.len);

        if (s >= nsec) {
            memset(se, 0, 0x28);
            continue;
        }
        put32(se + 0x0C, 0);
        put32(se + 0x10, len);
        put32(se + 0x24, len);
    }
    put32(hdr + 0x14, caff_checksum(hdr));

    if (!(*out = malloc(CAFF_HEADER_SIZE + d.len + g.len)))
        goto done;
    memcpy(*out, hdr, CAFF_HEADER_SIZE);
    memcpy(*out + CAFF_HEADER_SIZE, d.b, d.len);
    if (g.len)
        memcpy(*out + CAFF_HEADER_SIZE + d.len, g.b, g.len);
    *n_out = CAFF_HEADER_SIZE + d.len + g.len;
    rc = CAFF_OK;
done:
    free(d.b);
    free(g.b);
    free(names.b);
    free(newpart);
    free(newfile);
    caff_closure_free(&c);
    return rc;
}

/* ── Files ─────────────────────────────────────────────────────────────── */

#ifdef _WIN32
static FILE *open_utf8(const char *path, const wchar_t *mode)
{
    wchar_t wide[1024];
    FILE *f = NULL;

    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                             (int)(sizeof wide / sizeof *wide)))
        return NULL;
    if (_wfopen_s(&f, wide, mode))
        return NULL;
    return f;
}
#define TELL64(f)  _ftelli64(f)
#define SEEK64     _fseeki64
#else
static FILE *open_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}
#define TELL64(f)  ftello(f)
#define SEEK64     fseeko
#endif

int caff_read_file(const char *path, uint8_t **data, size_t *n)
{
    FILE *f;
    long long len;
    uint8_t *d;

    *data = NULL;
    *n = 0;
#ifdef _WIN32
    f = open_utf8(path, L"rb");
#else
    f = open_utf8(path, "rb");
#endif
    if (!f)
        return CAFF_E_OPEN;
    if (SEEK64(f, 0, SEEK_END) || (len = TELL64(f)) < 0 || SEEK64(f, 0, SEEK_SET)) {
        fclose(f);
        return CAFF_E_OPEN;
    }
    if ((unsigned long long)len > MAX_PACKAGE || !(d = malloc(len ? (size_t)len : 1))) {
        fclose(f);
        return CAFF_E_NOMEM;
    }
    if (fread(d, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(d);
        return CAFF_E_OPEN;
    }
    fclose(f);
    *data = d;
    *n = (size_t)len;
    return CAFF_OK;
}

int caff_file_exists(const char *path)
{
    FILE *f;

#ifdef _WIN32
    f = open_utf8(path, L"rb");
#else
    f = open_utf8(path, "rb");
#endif
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int caff_make_parent_dirs(const char *path)
{
    char buf[1024];
    size_t i, n = strlen(path), start = 0;

    if (n >= sizeof buf)
        return CAFF_E_OPEN;
    memcpy(buf, path, n + 1);
    /* Skip "C:\" and "\\server\share\": those are not folders to make. */
    if (n >= 2 && buf[1] == ':')
        start = 3;
    else if (n >= 2 && (buf[0] == '\\' || buf[0] == '/') && (buf[1] == '\\' || buf[1] == '/')) {
        int seps = 0;
        for (start = 2; start < n && seps < 2; start++)
            if (buf[start] == '\\' || buf[start] == '/')
                seps++;
    }
    for (i = start; i < n; i++) {
        char sep = buf[i];

        if (sep != '\\' && sep != '/')
            continue;
        buf[i] = 0;
        if (i > start) {
#ifdef _WIN32
            wchar_t w[1024];

            if (!MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, 1024)
                || (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS))
                return CAFF_E_OPEN;
#else
            if (mkdir(buf, 0777) && errno != EEXIST)
                return CAFF_E_OPEN;
#endif
        }
        buf[i] = sep;
    }
    return CAFF_OK;
}

int caff_write_file(const char *path, const void *data, size_t n)
{
    char tmp[1024];
    FILE *f;
    int ok;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return CAFF_E_OPEN;
#ifdef _WIN32
    f = open_utf8(tmp, L"wb");
#else
    f = open_utf8(tmp, "wb");
#endif
    if (!f)
        return CAFF_E_OPEN;
    ok = fwrite(data, 1, n, f) == n;
    if (fclose(f))
        ok = 0;
    if (ok) {
#ifdef _WIN32
        wchar_t wt[1024], wp[1024];

        ok = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wt, 1024)
          && MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024)
          && MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) {
#ifdef _WIN32
        wchar_t wt[1024];

        if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wt, 1024))
            DeleteFileW(wt);
#else
        remove(tmp);
#endif
        return CAFF_E_OPEN;
    }
    return CAFF_OK;
}

/* ── The game's packages, from the folder listing ──────────────────────── */

typedef struct {
    CaffListed *l;
    int n, cap;
} Lister;

static int has_pkg_ext(const char *name)
{
    size_t n = strlen(name);
    const char *e = name + n - 4;

    if (n < 4)
        return 0;
#ifdef _WIN32
    return !_stricmp(e, ".rbm") || !_stricmp(e, ".rba");
#else
    return !strcasecmp(e, ".rbm") || !strcasecmp(e, ".rba");
#endif
}

static void lister_add(Lister *s, const char *rel, uint64_t size, uint64_t time)
{
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 512;
        CaffListed *nl = realloc(s->l, (size_t)c * sizeof *nl);
        if (!nl)
            return;
        s->l = nl;
        s->cap = c;
    }
    if ((s->l[s->n].rel = malloc(strlen(rel) + 1)) == NULL)
        return;
    strcpy(s->l[s->n].rel, rel);
    s->l[s->n].size = size;
    s->l[s->n].time = time;
    s->n++;
}

#ifdef _WIN32
static void list_dir(Lister *s, const wchar_t *root, const wchar_t *rel, int depth)
{
    wchar_t pat[1024], sub[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > 16)
        return;
    if (rel[0])
        swprintf_s(pat, 1024, L"%s\\%s\\*", root, rel);
    else
        swprintf_s(pat, 1024, L"%s\\*", root);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        char u[1024];

        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (rel[0])
            swprintf_s(sub, 1024, L"%s\\%s", rel, fd.cFileName);
        else
            swprintf_s(sub, 1024, L"%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            list_dir(s, root, sub, depth + 1);
            continue;
        }
        if (!WideCharToMultiByte(CP_UTF8, 0, sub, -1, u, sizeof u, NULL, NULL) || !has_pkg_ext(u))
            continue;
        lister_add(s, u, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                   ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32)
                   | fd.ftLastWriteTime.dwLowDateTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static void list_dir(Lister *s, const char *root, const char *rel, int depth)
{
    char dir[1024], sub[1024], full[2048];
    struct dirent *e;
    struct stat st;
    DIR *d;

    if (depth > 16)
        return;
    snprintf(dir, sizeof dir, rel[0] ? "%s/%s" : "%s", root, rel);
    for (char *p = dir + strlen(root); *p; p++)
        if (*p == '\\')
            *p = '/';
    if (!(d = opendir(dir)))
        return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, rel[0] ? "%s\\%s" : "%s%s", rel, e->d_name);
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (stat(full, &st))
            continue;
        if (S_ISDIR(st.st_mode))
            list_dir(s, root, sub, depth + 1);
        else if (has_pkg_ext(e->d_name))
            lister_add(s, sub, (uint64_t)st.st_size, (uint64_t)st.st_mtime);
    }
    closedir(d);
}
#endif

static int listed_cmp(const void *a, const void *b)
{
#ifdef _WIN32
    return _stricmp(((const CaffListed *)a)->rel, ((const CaffListed *)b)->rel);
#else
    return strcasecmp(((const CaffListed *)a)->rel, ((const CaffListed *)b)->rel);
#endif
}

int caff_list_packages(const char *aid_root, CaffListed **out)
{
    Lister s = { 0 };
#ifdef _WIN32
    wchar_t root[1024];

    *out = NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, aid_root, -1, root, 1024))
        return 0;
    list_dir(&s, root, L"", 0);
#else
    *out = NULL;
    list_dir(&s, aid_root, "", 0);
#endif
    if (s.n > 1)
        qsort(s.l, (size_t)s.n, sizeof *s.l, listed_cmp);
    *out = s.l;
    return s.n;
}

void caff_list_free(CaffListed *list, int n)
{
    int i;

    for (i = 0; list && i < n; i++)
        free(list[i].rel);
    free(list);
}
