/*
 * The texture pack kit's reader (to-do 238; texkit.h): finds, names and
 * hashes every texture of one opened package. A C port of a texture
 * extractor's texture walk (the walk, the names, the frame sizes), checked
 * against that code's own listing of the disc; the
 * hash is the toolkit's texture_stage name for a replacement.
 *
 * Coordinates: the extractor works on "mapped" copies where every pointer is
 * a file offset. Here a pointer is an offset inside the part its relocation
 * names, so every pixel pointer is turned into an offset in the .gpu
 * section (that part's offset + the pointer), and every .data part start
 * into an offset in the .data section. Comparisons the extractor makes
 * between file offsets are the same comparisons in those terms.
 */
#include "texkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC_DATA 1
#define SEC_GPU  2
#define HEADER_AT 0x20u         /* H = the texture's .data part + 0x20 */
#define TABLE_AT  0x44u         /* H+0x24: kind 4's frame table pointer */
#define LINK      0x20u         /* a directory entry points at its file's .data + 0x20 */
#define PAD       0x80u         /* a mip chain is sometimes padded to this */
#define OTHER_FORMAT 0x3Au      /* textures.md 3.3: tried last for a frame */
#define MAX_FRAMES 4096u        /* 128 on the disc */
#define MAX_MIPS   16u          /* 11 on the disc */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static int fail(int rc, char *why, size_t cap, const char *fmt, const char *what)
{
    if (why && cap)
        snprintf(why, cap, fmt, what);
    return rc;
}

/* ── Hash and sizes ─────────────────────────────────────────────────────── */

uint64_t texkit_hash(const uint8_t *p, size_t n, uint64_t h)
{
    size_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t dxt_block(uint32_t f)
{
    return f == 0x0C ? 8 : f == 0x0E || f == 0x0F ? 16 : 0;
}

static uint32_t texel_bytes(uint32_t f)
{
    switch (f) {
    case 0x00: return 1;                /* L8 */
    case 0x04: case 0x05: case 0x1A: case 0x28: return 2;
    case 0x07: case 0x3A: return 4;
    default:   return 0;
    }
}

/* 64-bit: textures.md 3.3's search tries sizes up to W << 16. */
static uint64_t level64(uint32_t f, uint64_t w, uint64_t h, uint64_t d)
{
    if (dxt_block(f))
        return ((w + 3) / 4) * ((h + 3) / 4) * dxt_block(f) * d;
    return w * h * d * texel_bytes(f);
}

uint32_t texkit_level_size(uint32_t format, uint32_t w, uint32_t h, uint32_t depth)
{
    uint64_t n = level64(format, w, h, depth);

    return n > 0xFFFFFFFFull ? 0 : (uint32_t)n;
}

static uint64_t chain64(uint32_t f, uint64_t w, uint64_t h, int64_t levels)
{
    uint64_t sum = 0;
    int64_t i;

    for (i = 0; i < levels; i++) {
        uint64_t a = i < 63 ? w >> i : 0, b = i < 63 ? h >> i : 0;

        sum += level64(f, a ? a : 1, b ? b : 1, 1);
    }
    return sum;
}

uint64_t texkit_chain_size(uint32_t format, uint32_t w, uint32_t h, uint32_t levels)
{
    return chain64(format, w, h, levels);
}

static uint64_t pad80(uint64_t n)
{
    return (n + PAD - 1) / PAD * PAD;
}

static int fits(uint64_t size, uint64_t own)
{
    return own == size || own == pad80(size);
}

static int bit_length(uint32_t x)
{
    int n = 0;

    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

/* textures.md 3.3 rule step 2: the largest W*2^k x H*2^k (k != 0, both sides
 * >= 1) with max(1, mips + k) levels in format f whose chain fits `own`. */
static int other_size(uint32_t f, uint32_t w, uint32_t h, uint32_t mips, uint64_t own,
                      TexkitLayout *out)
{
    int bw = bit_length(w), bh = bit_length(h), k, found = 0;

    for (k = -(bw < bh ? bw : bh) + 1; k <= 16; k++) {
        uint64_t w2, h2, size;
        int64_t levels;

        if (!k)
            continue;
        w2 = k > 0 ? (uint64_t)w << k : (uint64_t)(w >> -k);
        h2 = k > 0 ? (uint64_t)h << k : (uint64_t)(h >> -k);
        levels = (int64_t)mips + k;
        size = chain64(f, w2, h2, levels > 1 ? levels : 1);
        if (size > own)                 /* chains only grow with k */
            break;
        if (fits(size, own)) {
            out->format = f;
            out->width = (uint32_t)w2;
            out->height = (uint32_t)h2;
            out->depth = 1;
            found = 1;
        }
    }
    return found;
}

int texkit_layout(uint32_t format, uint32_t kind, uint32_t w, uint32_t h, uint32_t mips,
                  uint32_t count, uint32_t own, TexkitLayout *out)
{
    out->format = format;
    out->width = w;
    out->height = h;
    out->depth = kind == TEXKIT_KIND_VOLUME && count > 1 ? count : 1;
    if (kind != TEXKIT_KIND_VARIANTS || !w || !h)
        return 1;
    if (!dxt_block(format) && !texel_bytes(format))
        return 0;                       /* the extractor can't size it either */
    if (fits(chain64(format, w, h, mips > 1 ? mips : 1), own))
        return 1;
    if (other_size(format, w, h, mips, own, out)
        || other_size(OTHER_FORMAT, w, h, mips, own, out))
        return 1;
    out->format = format;
    out->width = w;
    out->height = h;
    return 0;
}

/* ── Names (texture-names.md) ───────────────────────────────────────────── */

/* The parts of a path split on '\' and '/', empty ones dropped: up to 64,
 * as [start, end) pairs into the path. */
static int split(const char *p, const char *s[64], const char *e[64])
{
    int n = 0;

    while (*p && n < 64) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        s[n] = p;
        while (*p && *p != '\\' && *p != '/')
            p++;
        e[n++] = p;
    }
    return n;
}

static void put_part(char *out, size_t cap, size_t *at, const char *s, const char *e)
{
    while (s < e && *at + 1 < cap)
        out[(*at)++] = *s++;
    out[*at] = 0;
}

void texkit_package_name(const char *path, char *out, size_t cap)
{
    const char *s[64], *e[64];
    int n = split(path, s, e), i, first;
    size_t at = 0;

    out[0] = 0;
    first = n >= 3 ? n - 3 : 0;
    for (i = first; i < n - 1; i++) {
        if (i > first)
            put_part(out, cap, &at, "_", "_" + 1);
        put_part(out, cap, &at, s[i], e[i]);
    }
}

static int lower1(int c)
{
    /* Python's str.lower() on the latin-1 characters the names are read as */
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 32;
    return c;
}

void texkit_leaf(const char *path, char *out, size_t cap)
{
    const char *s[64], *e[64];
    int n = split(path, s, e), i = n - 1;
    size_t at = 0;

    out[0] = 0;
    if (n <= 0)
        return;
    if (n >= 2 && e[n - 1] - s[n - 1] >= 7) {
        static const char def[] = "default";
        int j, same = 1;

        for (j = 0; j < 7; j++)
            if (lower1((unsigned char)s[n - 1][j]) != def[j])
                same = 0;
        if (same)
            i = n - 2;
    }
    put_part(out, cap, &at, s[i], e[i]);
}

void texkit_standalone_name(const char *package, char *out, size_t cap)
{
    static const char *const prefix[] = { "Avatars_", "Frontend_", "MPlevelpictures_", "tips_" };
    size_t i;

    for (i = 0; i < sizeof prefix / sizeof *prefix; i++)
        if (!strncmp(package, prefix[i], strlen(prefix[i]))) {
            snprintf(out, cap, "%s", package + strlen(prefix[i]));
            return;
        }
    snprintf(out, cap, "%s", package);
}

static int bad_char(int c)
{
    return c && strchr("\\/:*?\"<>|", c) != NULL;
}

/* Python's \s on the latin-1 characters */
static int space(int c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0;
}

void texkit_clean_name(const char *name, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)name, *end;
    size_t at = 0;

    if (!cap)
        return;
    while (*p == ' ')                   /* strip(" ") */
        p++;
    end = p + strlen((const char *)p);
    while (end > p && end[-1] == ' ')
        end--;
    while (p < end && at + 1 < cap) {
        if (bad_char(*p)) {
            while (p < end && bad_char(*p))
                p++;
            out[at++] = '_';
        } else if (space(*p)) {
            while (p < end && space(*p))
                p++;
            out[at++] = ' ';
        } else {
            out[at++] = (char)*p++;
        }
    }
    while (at && (out[at - 1] == ' ' || out[at - 1] == '.'))
        at--;
    out[at] = 0;
    if (!at)
        snprintf(out, cap, "Image");
}

/* A set of strings (open addressing, its own copies). */
typedef struct {
    char   **slot;
    size_t   cap, n;
} StrSet;

static uint64_t str_hash(const char *s)
{
    uint64_t h = TEXKIT_HASH_SEED;

    while (*s)
        h = (h ^ (unsigned char)*s++) * 0x100000001B3ull;
    return h;
}

static int set_init(StrSet *t, size_t n)
{
    t->cap = 64;
    while (t->cap < 4 * n + 4)
        t->cap *= 2;
    t->n = 0;
    t->slot = calloc(t->cap, sizeof *t->slot);
    return t->slot ? 0 : -1;
}

static void set_free(StrSet *t)
{
    size_t i;

    for (i = 0; t->slot && i < t->cap; i++)
        free(t->slot[i]);
    free(t->slot);
}

static int set_has(const StrSet *t, const char *s)
{
    size_t i = (size_t)str_hash(s) & (t->cap - 1);

    for (; t->slot[i]; i = (i + 1) & (t->cap - 1))
        if (!strcmp(t->slot[i], s))
            return 1;
    return 0;
}

static int set_add(StrSet *t, const char *s)
{
    size_t i = (size_t)str_hash(s) & (t->cap - 1);

    if (t->n + 1 > t->cap / 2)
        return -1;                      /* sized for n by set_init */
    for (; t->slot[i]; i = (i + 1) & (t->cap - 1))
        if (!strcmp(t->slot[i], s))
            return 0;
    if (!(t->slot[i] = malloc(strlen(s) + 1)))
        return -1;
    strcpy(t->slot[i], s);
    t->n++;
    return 0;
}

static void lowered(const char *s, char *out, size_t cap)
{
    size_t i;

    for (i = 0; s[i] && i + 1 < cap; i++)
        out[i] = (char)lower1((unsigned char)s[i]);
    out[i] = 0;
}

static const TexkitImage *s_sort_img;

/* (the cleaned name ignoring case, then creation order) */
static int by_lower_name(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    const unsigned char *p = (const unsigned char *)s_sort_img[x].file_name;
    const unsigned char *q = (const unsigned char *)s_sort_img[y].file_name;

    for (;; p++, q++) {
        int c = lower1(*p), d = lower1(*q);

        if (c != d)
            return c < d ? -1 : 1;
        if (!c)
            break;
    }
    return x < y ? -1 : x > y;
}

void texkit_name_images(TexkitImage *img, int n, char (*base)[TEXKIT_NAME_CAP])
{
    StrSet taken;
    int i, *order;
    char name[TEXKIT_NAME_CAP + 16], low[TEXKIT_NAME_CAP + 16];

    if (n <= 0 || set_init(&taken, (size_t)n))
        return;
    for (i = 0; i < n; i++) {           /* ".001": exact case */
        int number = 0;

        snprintf(name, sizeof name, "%s", base[i]);
        while (set_has(&taken, name))
            snprintf(name, sizeof name, "%s.%03d", base[i], ++number);
        set_add(&taken, name);
        snprintf(img[i].name, sizeof img[i].name, "%s", name);
        texkit_clean_name(img[i].name, img[i].file_name, sizeof img[i].file_name);
    }
    set_free(&taken);

    /* "_001": names equal ignoring case, written in order of the name
     * ignoring case, then creation order */
    order = malloc((size_t)n * sizeof *order);
    if (!order || set_init(&taken, (size_t)n)) {
        free(order);
        return;
    }
    for (i = 0; i < n; i++)
        order[i] = i;
    s_sort_img = img;
    qsort(order, (size_t)n, sizeof *order, by_lower_name);
    for (i = 0; i < n; i++) {
        TexkitImage *m = &img[order[i]];
        char clean[TEXKIT_NAME_CAP];
        int number = 0;

        snprintf(clean, sizeof clean, "%s", m->file_name);
        snprintf(name, sizeof name, "%s.png", clean);
        lowered(name, low, sizeof low);
        while (set_has(&taken, low)) {
            snprintf(name, sizeof name, "%s_%03d.png", clean, ++number);
            lowered(name, low, sizeof low);
        }
        set_add(&taken, low);
        snprintf(m->file_name, sizeof m->file_name, "%s", name);
    }
    set_free(&taken);
    free(order);
}

const char *texkit_format_name(uint32_t f)
{
    static char other[8];

    switch (f) {
    case 0x00: return "L8";
    case 0x04: return "A4R4G4B4";
    case 0x05: return "R5G6B5";
    case 0x07: return "X8R8G8B8";
    case 0x0C: return "DXT1";
    case 0x0E: return "DXT3";
    case 0x0F: return "DXT5";
    case 0x1A: return "A8L8";
    case 0x28: return "V8U8";
    case 0x3A: return "A8B8G8R8";
    default:
        snprintf(other, sizeof other, "0x%02X", f & 0xFF);
        return other;
    }
}

const char *texkit_replace_word(int r)
{
    switch (r) {
    case TEXKIT_REPLACE_YES:     return "yes";
    case TEXKIT_REPLACE_CUBE:    return "no (cube map)";
    case TEXKIT_REPLACE_VOLUME:  return "no (volume texture, drawn in 3D)";
    case TEXKIT_REPLACE_BUMP:    return "maybe (bump map: only when not read signed)";
    case TEXKIT_REPLACE_OUTSIDE: return "no (its bytes run past the package)";
    case TEXKIT_REPLACE_ELSEWHERE: return "no (the game reads this frame from another place)";
    default:                     return "no (unknown format)";
    }
}

/* ── One package ────────────────────────────────────────────────────────── */

typedef struct {
    const CaffPkg *k;
    CaffInfo       in;
    uint32_t      *data_part;   /* per file: its .data part (the last one) */
    uint32_t      *gpu_part;    /* and its .gpu part (the last one) */
    const uint8_t *gpu;         /* the .gpu section */
    uint32_t       gpu_size;
    uint32_t      *pointers;    /* scratch: one texture's frame pointers */
    char          *why;
    size_t         cap;
} Walk;

typedef struct {
    uint32_t    at;             /* .data section offset of a file's .data part */
    int         index;          /* directory order: the first entry wins */
    const char *name;
} Link;

static int link_cmp(const void *a, const void *b)
{
    const Link *x = a, *y = b;

    if (x->at != y->at)
        return x->at < y->at ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

/* The first directory entry linked to the .data part at `at`, or NULL. */
static const char *linked(const Link *l, int n, uint32_t at)
{
    int lo = 0, hi = n;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;

        if (l[mid].at < at)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && l[lo].at == at ? l[lo].name : NULL;
}

/* A relocated pointer in part `src` at `off`, as an offset in its target's
 * section. 0 and *ok = 0 when not relocated or not into `section`. */
static uint32_t pointer(const Walk *w, uint32_t src, uint32_t off, int section, int *ok)
{
    CaffPart s, t;
    uint32_t dst;

    *ok = 0;
    if (caff_part(w->k, src, &s) || (uint64_t)off + 4 > s.size)
        return 0;
    if (!(dst = caff_reloc_target(w->k, src, off)) || caff_part(w->k, dst, &t)
        || t.section != section)
        return 0;
    *ok = 1;
    return t.offset + get32(s.bytes + off);
}

static int grow(void **p, int *cap, int need, size_t size)
{
    void *n;
    int c = *cap ? *cap : 64;

    if (need <= *cap)
        return 0;
    while (c < need)
        c *= 2;
    if (!(n = realloc(*p, (size_t)c * size)))
        return -1;
    *p = n;
    *cap = c;
    return 0;
}

/* textures.md 2: the texture of file f, appended to out->tex with its images
 * (base names into *base). 1 added, 0 no .gpu part (skipped), <0 error. */
static int add_texture(Walk *w, TexkitPkg *out, int *tcap, int *icap,
                       char (**base)[TEXKIT_NAME_CAP], uint32_t f, char source,
                       const char *name)
{
    CaffPart p, g;
    TexkitTexture *t;
    const uint8_t *h;
    uint32_t v, frames, npointers = 1, s80 = 0;
    int ok;

    if (!w->gpu_part[f])
        return 0;
    caff_part(w->k, w->data_part[f], &p);
    caff_part(w->k, w->gpu_part[f], &g);
    if (p.size < TABLE_AT)
        return fail(CAFF_E_TOC, w->why, w->cap, "%s: a texture header is cut short", name);
    if (grow((void **)&out->tex, tcap, out->ntex + 1, sizeof *out->tex))
        return CAFF_E_NOMEM;
    t = &out->tex[out->ntex];
    memset(t, 0, sizeof *t);
    h = p.bytes + HEADER_AT;
    t->file = f;
    t->data_part = w->data_part[f];
    t->source = source;
    t->format = get32(h);
    t->kind = get32(h + 0x04);
    t->width = get16(h + 0x0C);
    t->height = get16(h + 0x0E);
    t->mips = get32(h + 0x18);
    t->count = get32(h + 0x20);
    t->gpu_offset = g.offset;
    t->gpu_size = g.size;
    if (t->mips > MAX_MIPS)             /* 11 at most on the disc; sizes loop over them */
        return fail(CAFF_E_TOC, w->why, w->cap, "%s: its mip count is out of range", name);
    w->pointers[0] = pointer(w, t->data_part, HEADER_AT + 0x14, SEC_GPU, &ok);
    if (!ok)
        return fail(CAFF_E_TOC, w->why, w->cap, "%s: its pixel pointer isn't into .gpu", name);
    frames = t->kind == TEXKIT_KIND_VARIANTS ? (t->count > 1 ? t->count : 1) : 1;
    if (frames > MAX_FRAMES)
        return fail(CAFF_E_TOC, w->why, w->cap, "%s: too many frames", name);
    if (t->kind == TEXKIT_KIND_VARIANTS && t->count > 1) {
        CaffPart tp;
        uint32_t table, tpart;

        if (p.size < TABLE_AT + 4)
            return fail(CAFF_E_TOC, w->why, w->cap, "%s: a texture header is cut short", name);
        /* H+0x24 points at the frame table: an unknown u32, then the
         * pointers to frames 1 .. N-1 */
        if (!(tpart = caff_reloc_target(w->k, t->data_part, TABLE_AT))
            || caff_part(w->k, tpart, &tp))
            return fail(CAFF_E_TOC, w->why, w->cap, "%s: its frame table pointer isn't relocated", name);
        table = get32(p.bytes + TABLE_AT);
        if ((uint64_t)table + 4ull * t->count > tp.size)
            return fail(CAFF_E_TOC, w->why, w->cap, "%s: its frame table runs past its part", name);
        for (v = 1; v < t->count; v++) {
            w->pointers[v] = pointer(w, tpart, table + 4 * v, SEC_GPU, &ok);
            if (!ok)
                return fail(CAFF_E_TOC, w->why, w->cap, "%s: a frame pointer isn't into .gpu", name);
        }
        npointers = t->count;
    }
    if (t->kind == TEXKIT_KIND_VARIANTS)
        s80 = (uint32_t)pad80(chain64(t->format, t->width, t->height, t->mips > 1 ? t->mips : 1));

    if (grow((void **)&out->img, icap, out->nimg + (int)frames, sizeof *out->img))
        return CAFF_E_NOMEM;
    {
        int bcap = *icap;
        void *nb = realloc(*base, (size_t)bcap * sizeof **base);

        if (!nb)
            return CAFF_E_NOMEM;
        *base = nb;
    }
    for (v = 0; v < frames; v++) {
        TexkitImage *m = &out->img[out->nimg];
        uint64_t end = t->kind == TEXKIT_KIND_VARIANTS && v + 1 < npointers
                     ? w->pointers[v + 1] : (uint64_t)t->gpu_offset + t->gpu_size;
        uint32_t depth = t->kind == TEXKIT_KIND_VOLUME && t->count > 1 ? t->count : 1;
        uint64_t game_at = (uint64_t)w->pointers[0] + (uint64_t)v * s80;

        memset(m, 0, sizeof *m);
        m->texture = out->ntex;
        m->variant = v;
        m->at = w->pointers[v];
        m->own = end > m->at ? (uint32_t)(end - m->at) : 0;
        m->layout_ok = texkit_layout(t->format, t->kind, t->width, t->height, t->mips,
                                     t->count, m->own, &m->layout);
        /* What the game uploads: its own set-up routine (0x003FA320) places
         * frame v at the first + v x the header's chain rounded up to 0x80,
         * never reading the frame table (textures.md 3.3). */
        m->game_at = game_at > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)game_at;
        m->game_bytes = texkit_level_size(t->format, t->width, t->height, depth);
        if (t->kind == TEXKIT_KIND_CUBE)
            m->replace = TEXKIT_REPLACE_CUBE;
        else if (t->kind == TEXKIT_KIND_VOLUME)
            m->replace = TEXKIT_REPLACE_VOLUME;
        else if (!m->game_bytes)
            m->replace = TEXKIT_REPLACE_FORMAT;
        else if (game_at + m->game_bytes > w->gpu_size)
            m->replace = TEXKIT_REPLACE_OUTSIDE;
        else if (game_at != m->at)      /* 22 frames of 3 textures on the disc */
            m->replace = TEXKIT_REPLACE_ELSEWHERE;
        else {
            m->hash = texkit_hash(w->gpu + m->game_at, m->game_bytes, TEXKIT_HASH_SEED);
            m->replace = t->format == 0x28 ? TEXKIT_REPLACE_BUMP : TEXKIT_REPLACE_YES;
        }
        if (v)
            snprintf((*base)[out->nimg], TEXKIT_NAME_CAP, "%s@%u", name, v);
        else
            snprintf((*base)[out->nimg], TEXKIT_NAME_CAP, "%s", name);
        out->nimg++;
    }
    out->ntex++;
    return 1;
}

static int is_tag(const CaffPart *p, const char *tag)
{
    return p->size >= 8 && !memcmp(p->bytes, tag, 8);
}

int texkit_read(const CaffPkg *k, const char *rel, TexkitPkg *out, char *why, size_t cap)
{
    Walk w;
    Link *links = NULL;
    char (*base)[TEXKIT_NAME_CAP] = NULL;
    CaffPart p;
    int nlinks = 0, tcap = 0, icap = 0, rc = CAFF_OK, i, nd;
    uint32_t f;

    memset(out, 0, sizeof *out);
    memset(&w, 0, sizeof w);
    if (why && cap)
        why[0] = 0;
    w.k = k;
    w.why = why;
    w.cap = cap;
    caff_info(k, &w.in);
    texkit_package_name(rel, out->package, sizeof out->package);
    w.data_part = calloc((size_t)w.in.files + 1, sizeof *w.data_part);
    w.gpu_part = calloc((size_t)w.in.files + 1, sizeof *w.gpu_part);
    w.pointers = malloc(MAX_FRAMES * sizeof *w.pointers);
    if (!w.data_part || !w.gpu_part || !w.pointers) {
        rc = CAFF_E_NOMEM;
        goto done;
    }
    for (f = 1; f <= w.in.parts; f++) {
        if (caff_part(k, f, &p))
            continue;
        if (p.section == SEC_DATA)
            w.data_part[p.file] = f;
        else if (p.section == SEC_GPU) {
            w.gpu_part[p.file] = f;
            w.gpu = p.bytes - p.offset;
        }
    }
    w.gpu_size = w.in.sections >= 2 ? w.in.size[1] : 0;

    if (!w.in.files || !w.data_part[1]) {
        rc = fail(CAFF_E_TOC, why, cap, "%s: has no file 1", rel);
        goto done;
    }
    caff_part(k, w.data_part[1], &p);
    if (is_tag(&p, "texture\0")) {      /* standalone .rba: file 1 is it */
        char name[TEXKIT_NAME_CAP];

        texkit_standalone_name(out->package, name, sizeof name);
        rc = add_texture(&w, out, &tcap, &icap, &base, 1, 's', name);
        if (rc == 0)
            out->skipped++;
    } else if (is_tag(&p, "package\0")) {
        char owner[TEXKIT_NAME_CAP], name[TEXKIT_NAME_CAP + 24];
        int counter = 0;

        /* each directory entry links to the file whose .data part starts
         * 0x20 before it points (the first entry wins) */
        nd = caff_dir_count(k);
        if (nd && !(links = malloc((size_t)nd * sizeof *links))) {
            rc = CAFF_E_NOMEM;
            goto done;
        }
        for (i = 0; i < nd; i++) {
            CaffDirEntry e;
            CaffPart q;

            if (caff_dir_entry(k, i, &e) || !e.part || caff_part(k, e.part, &q)
                || q.section != SEC_DATA || (uint64_t)q.offset + e.offset < LINK)
                continue;
            links[nlinks].at = q.offset + e.offset - LINK;
            links[nlinks].index = i;
            links[nlinks].name = e.name;
            nlinks++;
        }
        if (nlinks > 1)
            qsort(links, (size_t)nlinks, sizeof *links, link_cmp);
        snprintf(owner, sizeof owner, "%s", out->package);
        for (f = 1; f <= w.in.files && rc >= 0; f++) {
            const char *entry;

            if (!w.data_part[f])
                continue;
            caff_part(k, w.data_part[f], &p);
            entry = linked(links, nlinks, p.offset);
            if (!is_tag(&p, "texture\0")) {
                if (entry) {            /* a model, level, animation ...: the new owner */
                    texkit_leaf(entry, owner, sizeof owner);
                    counter = 0;
                }
                continue;
            }
            if (entry) {
                texkit_leaf(entry, name, sizeof name);
                rc = add_texture(&w, out, &tcap, &icap, &base, f, 'e', name);
            } else {
                snprintf(name, sizeof name, "%s_texturefile%d", owner, counter);
                rc = add_texture(&w, out, &tcap, &icap, &base, f, 'o', name);
                if (rc == 1)
                    counter++;
            }
            if (rc == 0)
                out->skipped++;
        }
    } else {
        char tag[9] = "";

        if (p.size)
            memcpy(tag, p.bytes, p.size < 8 ? p.size : 8);
        rc = fail(CAFF_E_TOC, why, cap, "neither a texture nor a package container (tag '%s')", tag);
    }
    if (rc >= 0) {
        rc = CAFF_OK;
        texkit_name_images(out->img, out->nimg, base);
        out->gpu = w.gpu;
        out->gpu_size = w.gpu_size;
    }
done:
    free(base);
    free(links);
    free(w.data_part);
    free(w.gpu_part);
    free(w.pointers);
    if (rc != CAFF_OK)
        texkit_free(out);
    return rc;
}

void texkit_free(TexkitPkg *p)
{
    free(p->tex);
    free(p->img);
    p->tex = NULL;
    p->img = NULL;
    p->ntex = p->nimg = 0;
}
