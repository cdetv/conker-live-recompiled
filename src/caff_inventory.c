/*
 * The inventory (to-do 43, step 6): every package of the game opened once and
 * its directory written as JSON, so a feature can find a resource by name
 * without opening 266 packages (to-do 128's missing pictures, 238's texture
 * kit, Berri's palette). conker.exe --carve writes it to
 * <save dir>\assets\inventory.json (src/caff_cache.c); tests/caff writes one
 * from a folder of unpacked packages. The shape is scripts/caff.py
 * inventory's, so the two are compared directly.
 *
 * Made from the player's game files on their PC: never shipped (SPEC 3.3).
 * Plain memory and caff.h only (no guest here: the decompressor is passed
 * in), one package open at a time.
 */
#include "caff.h"

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
    char tmp[256];
    va_list ap;
    int k;

    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0)
        put(o, tmp, (size_t)k < sizeof tmp ? (size_t)k : sizeof tmp - 1);
}

/* A JSON string. latin1: the bytes are characters 0-255 (names inside a
 * package, as caff.py reads them); else the text is UTF-8 (folder names). */
static void put_str(Buf *o, const char *s, int latin1)
{
    put(o, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            put(o, e, 2);
        } else if (c < 0x20 || (latin1 && c >= 0x7F)) {
            putf(o, "\\u%04x", c);
        } else {
            put(o, (const char *)&c, 1);
        }
    }
    put(o, "\"", 1);
}

/* "zpackage\Level\Windmill\default.rbm" -> "Level\Windmill" for the
 * progress line. */
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

static void one_package(Buf *o, const char *path, CaffUnpackFn unpack, CaffInventoryResult *r,
                        CaffPackageVisit visit, void *vctx)
{
    CaffPkg *k = NULL;
    CaffInfo in;
    int rc, i, nd;

    if ((rc = caff_open(path, unpack, &k)) != CAFF_OK) {
        put(o, "{\"error\": ", 10);
        put_str(o, caff_strerror(rc), 0);
        put(o, "}", 1);
        r->errors++;
        return;
    }
    if (visit)
        visit(k, path, vctx);
    caff_info(k, &in);
    put(o, "{\"tag\": ", 8);
    put_str(o, in.tag, 1);
    putf(o, ", \"files\": %u, \"parts\": %u, \"sections\": {", in.files, in.parts);
    for (i = 0; i < in.sections; i++) {
        if (i)
            put(o, ", ", 2);
        put_str(o, in.name[i], 1);
        putf(o, ": %u", in.size[i]);
    }
    put(o, "}, \"dir\": [", 11);
    nd = caff_dir_count(k);
    for (i = 0; i < nd; i++) {
        CaffDirEntry e;
        CaffClosure c;
        uint32_t nf = 0;
        unsigned long long d = 0, g = 0;

        if (caff_dir_entry(k, i, &e))
            continue;
        /* No part (none on the disc): no files, as caff.py has it. */
        if (e.part) {
            if ((rc = caff_closure(k, i, &c)) != CAFF_OK) {
                fprintf(stderr, "[CAFF] inventory: %s in %s: %s\n", e.name, path, caff_strerror(rc));
                r->errors++;
                continue;
            }
            nf = c.nfiles;
            d = c.data_bytes;
            g = c.gpu_bytes;
            caff_closure_free(&c);
        }
        put(o, i ? ",\n  {\"name\": " : "\n  {\"name\": ", i ? 13 : 12);
        put_str(o, e.name, 1);
        putf(o, ", \"files\": %u, \"data\": %llu, \"gpu\": %llu}", nf, d, g);
        r->resources++;
    }
    put(o, nd ? "\n ]}" : "]}", nd ? 4 : 2);
    caff_close(k);
}

int caff_inventory_write(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                         CaffInventoryProgress progress, void *ctx,
                         CaffInventoryResult *result)
{
    return caff_inventory_write_visit(aid_root, unpack, out_path, progress, ctx, NULL, NULL, result);
}

int caff_inventory_write_visit(const char *aid_root, CaffUnpackFn unpack, const char *out_path,
                               CaffInventoryProgress progress, void *ctx,
                               CaffPackageVisit visit, void *vctx,
                               CaffInventoryResult *result)
{
    CaffListed *l;
    CaffInventoryResult r = { 0 };
    Buf o = { 0 };
    int n = caff_list_packages(aid_root, &l), i, rc;

    put(&o, "{", 1);
    for (i = 0; i < n; i++) {
        char path[1024], name[256];

        snprintf(path, sizeof path, "%s\\%s", aid_root, l[i].rel);
        put(&o, i ? ",\n " : "\n ", i ? 3 : 2);
        put_str(&o, l[i].rel, 0);
        put(&o, ": ", 2);
        one_package(&o, path, unpack, &r, visit, vctx);
        r.packages++;
        if (progress) {
            short_name(l[i].rel, name, sizeof name);
            progress(i + 1, n, name, ctx);
        }
    }
    put(&o, n ? "\n}\n" : "}\n", n ? 3 : 2);
    caff_list_free(l, n);
    if (o.nomem) {
        free(o.b);
        return CAFF_E_NOMEM;
    }
    r.bytes = o.n;
    rc = caff_make_parent_dirs(out_path);
    if (!rc)
        rc = caff_write_file(out_path, o.b, o.n);
    if (rc)
        fprintf(stderr, "[CAFF] inventory: can't write %s\n", out_path);
    free(o.b);
    if (result)
        *result = r;
    return rc;
}
