/*
 * The texture pack kit's list (to-do 238; texkit.h): every package of the
 * player's game read through src/texkit.c and written as one TSV, a row per
 * picture, so a pack maker finds a texture's replacement name (its hash)
 * without playing to it:
 *
 *   hash     16 hex digits: a PNG named <hash>_<anything>.png in textures\
 *            replaces it (the toolkit's tex_replace.h); "-" when the game
 *            can't replace it
 *   replace  yes / maybe (why) / no (why)
 *   package  "Level_Canyon" (the extractor's package name)
 *   name     the extractor's picture name ("New_Conker_Eye.1@3",
 *            "Canyon_texturefile22", ".001" for a repeat in one package)
 *   format, width, height, levels, kind, frame
 *   png      where --texture-export puts its PNG: one per hash, in the
 *            first package's own folder below aid (package order), under
 *            the extractor's own file name, as it lays them out below
 *            dvddata\aid (user 2026-11-26): zpackage\Level\Bank\<name>.png.
 *            Each folder holds one package, whose file names are unique
 *            ignoring case, so two pictures never share a path; a pack
 *            uses these names, which the game turns into hashes through
 *            the name table (to-do 288)
 *   file, at, own, game_at, decoded  for checking against the extractor:
 *            file number, level 0's offset in its .gpu part (the frame
 *            table's) and its own bytes, where the game reads it, and the
 *            size it is decoded at when that differs from the header
 *
 * The export (--texture-export) walks the same way and writes each hash's
 * PNG once, at its `png` path below the folder, then the list beside them as
 * textures.tsv.
 *
 * Plain memory and caff.h only; the decompressor is passed in. Made from the
 * player's game files: stays on their PC (SPEC 3.3).
 */
#include "texkit.h"
#include "png_write.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *b;
    size_t n, cap;
    int    nomem;
} Buf;

static void put(Buf *o, const char *s, size_t n)
{
    if (o->nomem)
        return;
    if (o->n + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap : 1 << 20;
        char *nb;

        while (o->n + n + 1 > c)
            c *= 2;
        if (!(nb = realloc(o->b, c))) {
            o->nomem = 1;
            return;
        }
        o->b = nb;
        o->cap = c;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = 0;
}

static void putf(Buf *o, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int k;

    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0)
        put(o, tmp, (size_t)k < sizeof tmp ? (size_t)k : sizeof tmp - 1);
}

/* A name from inside a package (latin-1 bytes) as UTF-8; tabs and line
 * breaks (never in a name on the disc) as spaces, so a row stays a row. */
static void put_name(Buf *o, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20) {
            put(o, " ", 1);
        } else if (c < 0x80) {
            put(o, (const char *)&c, 1);
        } else {
            char u[2] = { (char)(0xC0 | c >> 6), (char)(0x80 | (c & 0x3F)) };

            put(o, u, 2);
        }
    }
}

/* "zpackage\Level\Bank\default.rbm" -> "zpackage\Level\Bank": the package's
 * folder below aid, where the export puts its PNGs. */
static void package_folder(const char *rel, char *out, size_t cap)
{
    const char *slash = strrchr(rel, '\\');
    size_t n = slash ? (size_t)(slash - rel) : 0;

    if (n >= cap)
        n = cap - 1;
    memcpy(out, rel, n);
    out[n] = 0;
}

/* "zpackage\Level\Windmill\default.rbm" -> "Level\Windmill" for progress. */
static void short_name(const char *rel, char *out, size_t cap)
{
    size_t n;

    if (!strncmp(rel, "zpackage\\", 9))
        rel += 9;
    snprintf(out, cap, "%s", rel);
    n = strlen(out);
    if (n > 12 && (!strcmp(out + n - 12, "\\default.rbm") || !strcmp(out + n - 12, "\\default.rba")))
        out[n - 12] = 0;
}

/* The PNG of each hash: the first picture that has it. */
#define SEEN_SLOTS (1u << 17)

typedef struct {
    uint64_t *hash;             /* 0 = empty */
    char    **png;
    int       n;
} Seen;

static const char *seen_png(Seen *s, uint64_t h, const char *png, int *is_new)
{
    uint32_t i = (uint32_t)(h * 0x9E3779B97F4A7C15ull >> 47) & (SEEN_SLOTS - 1);

    *is_new = 0;
    for (; s->hash[i]; i = (i + 1) & (SEEN_SLOTS - 1))
        if (s->hash[i] == h)
            return s->png[i];
    if (s->n >= (int)(SEEN_SLOTS / 2) || !(s->png[i] = malloc(strlen(png) + 1)))
        return png;
    strcpy(s->png[i], png);
    s->hash[i] = h;
    s->n++;
    *is_new = 1;
    return s->png[i];
}

static const char *kind_word(uint32_t kind)
{
    switch (kind) {
    case TEXKIT_KIND_IMAGE:    return "image";
    case TEXKIT_KIND_CUBE:     return "cube map";
    case TEXKIT_KIND_VOLUME:   return "volume";
    case TEXKIT_KIND_VARIANTS: return "frames";
    default:                   return "other";
    }
}

/* Picture i's PNG at <dir>\<png>, unless it is there already. */
static void export_png(const char *dir, const char *png, const TexkitPkg *t, int i,
                       TexkitResult *r)
{
    char path[1024], why[256];
    uint8_t *rgba = NULL, *file = NULL;
    uint32_t w, h;
    size_t n, k;
    int ch, rc;

    snprintf(path, sizeof path, "%s\\%s", dir, png);
    if (caff_file_exists(path)) {
        r->skipped++;
        return;
    }
    if ((rc = texkit_decode(t, i, &rgba, &w, &h, why, sizeof why)) != CAFF_OK) {
        fprintf(stderr, "[TEX] %s: %s\n", t->package, why[0] ? why : caff_strerror(rc));
        r->bad++;
        return;
    }
    /* the channels the format has, from the front of each RGBA texel */
    ch = texkit_png_channels(t->img[i].layout.format);
    for (k = 0; ch < 4 && k < (size_t)w * h; k++) {
        uint8_t *s = rgba + k * 4, *d = rgba + k * ch;

        if (ch == 2) {
            d[0] = s[0];
            d[1] = s[3];
        } else {
            memmove(d, s, (size_t)ch);
        }
    }
    if (png_encode(w, h, ch, rgba, &file, &n)) {
        fprintf(stderr, "[TEX] %s: out of memory for %s\n", t->package, png);
        r->bad++;
    } else if (caff_make_parent_dirs(path) || caff_write_file(path, file, n)) {
        fprintf(stderr, "[TEX] can't write %s\n", path);
        r->bad++;
    } else {
        r->written++;
        r->bytes += n;
    }
    free(file);
    free(rgba);
}

static void one_package(Buf *o, Seen *seen, const char *path, const char *rel,
                        CaffUnpackFn unpack, const char *export_dir, TexkitResult *r)
{
    CaffPkg *k = NULL;
    TexkitPkg t;
    char why[256], folder[512];
    int rc, i;

    package_folder(rel, folder, sizeof folder);
    if ((rc = caff_open(path, unpack, &k)) != CAFF_OK) {
        r->failed++;                    /* caff_open said why in a [CAFF] line */
        return;
    }
    if ((rc = texkit_read(k, rel, &t, why, sizeof why)) != CAFF_OK) {
        fprintf(stderr, "[TEX] %s: %s\n", rel, why[0] ? why : caff_strerror(rc));
        r->failed++;
        caff_close(k);
        return;
    }
    r->textures += t.ntex;
    r->images += t.nimg;
    for (i = 0; i < t.nimg; i++) {
        const TexkitImage *m = &t.img[i];
        const TexkitTexture *x = &t.tex[m->texture];
        char png[TEXKIT_NAME_CAP * 3 + 32], hex[17] = "-";
        const char *where = "-";
        int is_new = 0;

        if (m->hash) {
            Buf p = { 0 };

            snprintf(hex, sizeof hex, "%016llX", (unsigned long long)m->hash);
            putf(&p, "%s%s", folder, folder[0] ? "\\" : "");
            put_name(&p, m->file_name);
            snprintf(png, sizeof png, "%s", p.b ? p.b : "");
            free(p.b);
            where = seen_png(seen, m->hash, png, &is_new);
            r->distinct += is_new;
            if (export_dir && is_new)
                export_png(export_dir, png, &t, i, r);
        }
        putf(o, "%s\t%s\t%s\t", hex, texkit_replace_word(m->replace), t.package);
        put_name(o, m->name);
        putf(o, "\t%s\t%u\t%u\t%u\t%s\t%u\t%s\t%u\t%u\t%u\t%u\t", texkit_format_name(x->format),
             x->width, x->height, x->mips, kind_word(x->kind), m->variant, where, x->file,
             m->at - x->gpu_offset, m->own, m->game_at - x->gpu_offset);
        if (!m->layout_ok)
            put(o, "?", 1);
        else if (m->layout.format != x->format || m->layout.width != x->width
                 || m->layout.height != x->height)
            putf(o, "%s %ux%u", texkit_format_name(m->layout.format), m->layout.width,
                 m->layout.height);
        put(o, "\n", 1);
    }
    texkit_free(&t);
    caff_close(k);
}

static int walk(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                const char *export_dir, TexkitProgress progress, void *ctx,
                TexkitResult *result)
{
    CaffListed *l;
    TexkitResult r = { 0 };
    Buf o = { 0 };
    Seen seen = { 0 };
    int n = caff_list_packages(aid_root, &l), i, rc;

    seen.hash = calloc(SEEN_SLOTS, sizeof *seen.hash);
    seen.png = calloc(SEEN_SLOTS, sizeof *seen.png);
    if (!seen.hash || !seen.png) {
        free(seen.hash);
        free(seen.png);
        caff_list_free(l, n);
        return CAFF_E_NOMEM;
    }
    putf(&o, "%s", "hash\treplace\tpackage\tname\tformat\twidth\theight\tlevels\tkind\tframe\tpng"
                   "\tfile\tat\town\tgame_at\tdecoded\n");
    for (i = 0; i < n; i++) {
        char path[1024], name[256];

        snprintf(path, sizeof path, "%s\\%s", aid_root, l[i].rel);
        one_package(&o, &seen, path, l[i].rel, unpack, export_dir, &r);
        r.packages++;
        if (progress) {
            short_name(l[i].rel, name, sizeof name);
            progress(i + 1, n, name, ctx);
        }
    }
    caff_list_free(l, n);
    for (i = 0; i < (int)SEEN_SLOTS; i++)
        free(seen.png[i]);
    free(seen.png);
    free(seen.hash);
    if (o.nomem) {
        free(o.b);
        return CAFF_E_NOMEM;
    }
    rc = caff_make_parent_dirs(out_path);
    if (!rc)
        rc = caff_write_file(out_path, o.b, o.n);
    if (rc)
        fprintf(stderr, "[TEX] can't write %s\n", out_path);
    free(o.b);
    if (result)
        *result = r;
    return rc;
}

int texkit_list(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                TexkitProgress progress, void *ctx, TexkitResult *result)
{
    return walk(aid_root, unpack, out_path, NULL, progress, ctx, result);
}

/* ── The name table (to-do 288; texkit.h) ──────────────────────────────── */

struct TexkitNames {
    Buf o;
    int pictures, hashed, failed;
};

TexkitNames *texkit_names_new(void)
{
    TexkitNames *t = calloc(1, sizeof *t);

    if (t)
        putf(&t->o, "texnames\t%d\n", TEXKIT_NAMES_FORMAT);
    return t;
}

void texkit_names_add(TexkitNames *t, const CaffPkg *k, const char *rel)
{
    TexkitPkg p;
    char why[256], folder[512];
    int rc, i;

    if ((rc = texkit_read(k, rel, &p, why, sizeof why)) != CAFF_OK) {
        fprintf(stderr, "[TEX] names: %s: %s\n", rel, why[0] ? why : caff_strerror(rc));
        t->failed++;
        return;
    }
    /* as one_package names the export's PNGs: the folder as it is (already
     * UTF-8, from the folder listing), the file name from inside the package */
    package_folder(rel, folder, sizeof folder);
    for (i = 0; i < p.nimg; i++) {
        const TexkitImage *m = &p.img[i];

        if (m->hash) {
            putf(&t->o, "%016llX\t", (unsigned long long)m->hash);
            t->hashed++;
        } else {
            put(&t->o, "-\t", 2);
        }
        putf(&t->o, "%s%s", folder, folder[0] ? "\\" : "");
        put_name(&t->o, m->file_name);
        put(&t->o, "\n", 1);
        t->pictures++;
    }
    texkit_free(&p);
}

int texkit_names_write(TexkitNames *t, const char *path, unsigned long long *bytes)
{
    int rc;

    if (bytes)
        *bytes = 0;
    if (t->o.nomem) {
        fprintf(stderr, "[TEX] names: out of memory\n");
        return CAFF_E_NOMEM;
    }
    rc = caff_make_parent_dirs(path);
    if (!rc)
        rc = caff_write_file(path, t->o.b, t->o.n);
    if (rc)
        fprintf(stderr, "[TEX] can't write %s\n", path);
    else if (bytes)
        *bytes = t->o.n;
    return rc;
}

void texkit_names_counts(const TexkitNames *t, int *pictures, int *hashed, int *failed)
{
    *pictures = t->pictures;
    *hashed = t->hashed;
    *failed = t->failed;
}

void texkit_names_free(TexkitNames *t)
{
    if (t) {
        free(t->o.b);
        free(t);
    }
}

int texkit_export(const char *aid_root, CaffUnpackFn unpack, const char *out_dir,
                  TexkitProgress progress, void *ctx, TexkitResult *result)
{
    char list[1024];

    snprintf(list, sizeof list, "%s\\textures.tsv", out_dir);
    if (caff_make_parent_dirs(list)) {
        fprintf(stderr, "[TEX] can't make %s\n", out_dir);
        return CAFF_E_OPEN;
    }
    return walk(aid_root, unpack, list, out_dir, progress, ctx, result);
}
