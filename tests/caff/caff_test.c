/*
 * Package reader test (to-do 43): src/caff.c on its own, no game.
 *
 *   caff_test raw  <dvddata\aid dir> <out.json>   every package as stored:
 *       header check (packed or not); the ones not packed fully parsed
 *   caff_test dump <unpacked dir> <out.json>      every package fully parsed
 *       (a folder of unpacked packages, e.g. conker.exe --unpack-all's)
 *   caff_test fuzz <dir> [rounds]                 damaged copies (cut short,
 *       bytes changed with the checksum mended, counts made huge) must be
 *       refused, never read past: build with -DCAFF_ASAN=ON to catch that;
 *       the ones that open also get a loose package written per resource
 *   caff_test loose <unpacked dir> <out dir | -> <package rel path>...
 *       every resource of those packages as a loose package (caff_write_loose),
 *       opened again and checked against the source part by part, pointer by
 *       pointer; written below <out dir> for scripts/caff.py checkloose
 *   caff_test inventory <unpacked dir> <out.json>  src/caff_inventory.c on a
 *       folder of unpacked packages (no guest); compare with caff.py
 *       inventory's file: caff.py invcompare <a.json> <b.json>
 *
 * The JSON is the same shape scripts/caff.py writes (caff.py raw / dump),
 * so the two readers are compared with: caff.py compare <a.json> <b.json>.
 * Output is made from game data: keep it in _work (SPEC 3.3).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "caff.h"

static const char *err_name(int rc)
{
    switch (rc) {
    case CAFF_E_OPEN: return "OPEN";
    case CAFF_E_MAGIC: return "MAGIC";
    case CAFF_E_HEADER: return "HEADER";
    case CAFF_E_NEED_GUEST: return "NEED_GUEST";
    case CAFF_E_SECTION_TOO_BIG: return "SECTION_TOO_BIG";
    case CAFF_E_UNPACK: return "UNPACK";
    case CAFF_E_NOT_FOUND: return "NOT_FOUND";
    case CAFF_E_NOMEM: return "NOMEM";
    case CAFF_E_TOC: return "TOC";
    default: return "?";
    }
}

static void jstr(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(o, "\\%c", c);
        else if (c < 0x20 || c >= 0x7F)
            fprintf(o, "\\u%04x", c);
        else
            fputc(c, o);
    }
    fputc('"', o);
}

/* The parsed table of contents, as caff.py's Package.dump() writes it. */
static void dump_pkg(FILE *o, const CaffPkg *k)
{
    CaffInfo in;
    CaffPart p;
    CaffDirEntry d;
    uint32_t i, j, h = 0;
    int s;

    caff_info(k, &in);
    fprintf(o, "\"files\": %u, \"parts\": %u, \"relocs\": %u, \"roffs\": %u, \"tag\": ",
            in.files, in.parts, in.relocs, in.reloc_offsets);
    jstr(o, in.tag);
    fprintf(o, ", \"sections\": [");
    for (s = 0; s < in.sections; s++) {
        fprintf(o, "%s[", s ? ", " : "");
        jstr(o, in.name[s]);
        fprintf(o, ", %u]", in.size[s]);
    }
    fprintf(o, "], \"names\": [");
    for (i = 1; in.has_names && i <= in.files; i++) {
        fprintf(o, "%s", i > 1 ? ", " : "");
        jstr(o, caff_file_name(k, i));
    }
    fprintf(o, "], \"part_list\": [");
    for (i = 1; i <= in.parts; i++) {
        caff_part(k, i, &p);
        fprintf(o, "%s[%u, %u, %u, %u, %u]", i > 1 ? ", " : "", p.file, p.offset,
                p.size, p.section, p.align_log2);
    }
    fprintf(o, "], \"rel_list\": [");
    for (i = 0; i < in.relocs; i++) {
        uint32_t src, dst, cnt;
        const uint8_t *offs;

        caff_reloc(k, i, &src, &dst, &cnt, &offs);
        fprintf(o, "%s[%u, %u, %u]", i ? ", " : "", src, dst, cnt);
        for (j = 0; j < cnt; j++) {
            uint32_t v;
            memcpy(&v, offs + 4 * j, 4);
            h = h * 31u + v;
        }
    }
    fprintf(o, "], \"roff_hash\": %u, \"dir\": [", h);
    for (s = 0; s < caff_dir_count(k); s++) {
        caff_dir_entry(k, s, &d);
        fprintf(o, "%s[", s ? ", " : "");
        jstr(o, d.name);
        fprintf(o, ", %u, %u]", d.part, d.offset);
    }
    fprintf(o, "]");
}

typedef struct {
    FILE *out;
    int   raw;
    int   n, bad, packed, flag_only, first;
    uint32_t max_blocks;
} Walk;

static void one(Walk *w, const char *path, const char *rel)
{
    uint8_t *data;
    size_t n;
    CaffHeader h;
    CaffPkg *k = NULL;
    char why[96];
    int rc, s;

    fprintf(w->out, "%s\n ", w->first ? "" : ",");
    w->first = 0;
    jstr(w->out, rel);
    fprintf(w->out, ": {");
    w->n++;
    if ((rc = caff_read_file(path, &data, &n))) {
        fprintf(w->out, "\"error\": \"%s\"}", err_name(rc));
        w->bad++;
        return;
    }
    rc = caff_header_read(data, n, &h, why, sizeof why);
    if (rc) {
        fprintf(w->out, "\"error\": \"%s\"}", err_name(rc));
        printf("  %s: %s (%s)\n", rel, caff_strerror(rc), why);
        w->bad++;
        free(data);
        return;
    }
    if (w->raw) {
        fprintf(w->out, "\"flag\": %d, \"packed\": %d, \"stored\": [", h.flag_set, h.any_packed);
        for (s = 0; s < h.sections; s++) {
            fprintf(w->out, "%s[%u, %u, %d, %u]", s ? ", " : "", h.unpacked[s],
                    h.stored[s], h.packed[s], h.blocks[s]);
            if (h.blocks[s] > w->max_blocks)
                w->max_blocks = h.blocks[s];
        }
        fprintf(w->out, "]");
        if (h.any_packed)
            w->packed++;
        if (h.flag_set && !h.any_packed) {
            w->flag_only++;
            printf("  flag set, not packed: %s\n", rel);
        }
    }
    if (!h.any_packed) {
        rc = caff_open_mem(data, n, rel, &k);
        if (rc) {
            fprintf(w->out, "%s\"error\": \"%s\"", w->raw ? ", " : "", err_name(rc));
            w->bad++;
        } else {
            fprintf(w->out, "%s", w->raw ? ", " : "");
            dump_pkg(w->out, k);
            caff_close(k);
        }
    } else if (!w->raw) {
        fprintf(w->out, "\"error\": \"NEED_GUEST\"");
        w->bad++;
    }
    fprintf(w->out, "}");
    free(data);
}

static void walk(Walk *w, const char *root, const char *rel)
{
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2], full[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE hf;

    sprintf_s(pat, sizeof pat, "%s\\%s*", root, rel);
    if ((hf = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        size_t len = strlen(fd.cFileName);

        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        sprintf_s(sub, sizeof sub, "%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            strcat_s(sub, sizeof sub, "\\");
            walk(w, root, sub);
        } else if (len > 4 && (!_stricmp(fd.cFileName + len - 4, ".rbm")
                               || !_stricmp(fd.cFileName + len - 4, ".rba"))) {
            sprintf_s(full, sizeof full, "%s\\%s", root, sub);
            one(w, full, sub);
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

/* ── fuzz ──────────────────────────────────────────────────────────────── */

static uint32_t s_rng = 0x2F6B1D35u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int s_fuzz_n, s_fuzz_ok, s_fuzz_refused, s_fuzz_loose_bad;

static void try_open(const uint8_t *d, size_t n)
{
    CaffPkg *k = NULL;
    int rc = caff_open_mem(d, n, NULL, &k), i;

    s_fuzz_n++;
    if (rc) {
        s_fuzz_refused++;
        return;
    }
    /* Opened: every accessor must stay inside the package. */
    s_fuzz_ok++;
    {
        CaffInfo in;
        CaffPart p;
        CaffDirEntry e;
        volatile uint32_t sum = 0;
        uint32_t j;

        caff_info(k, &in);
        for (j = 1; j <= in.parts; j++)
            if (!caff_part(k, j, &p) && p.size)
                sum += p.bytes[0] + p.bytes[p.size - 1];
        for (i = 0; i < caff_dir_count(k); i++)
            if (!caff_dir_entry(k, i, &e))
                sum += (uint32_t)strlen(e.name);
        for (j = 1; in.has_names && j <= in.files; j++)
            sum += (uint32_t)strlen(caff_file_name(k, j));
        /* A few resources written loose and opened again. */
        for (i = 0; i < caff_dir_count(k) && i < 4; i++) {
            uint8_t *b;
            size_t nb;
            CaffPkg *l;

            if (!caff_write_loose(k, (int)(rnd() % (uint32_t)caff_dir_count(k)), &b, &nb)) {
                if (!caff_open_mem(b, nb, NULL, &l))
                    caff_close(l);
                else
                    s_fuzz_loose_bad++;
                free(b);
            }
        }
        (void)sum;
    }
    caff_close(k);
}

static void fuzz_one(const char *path, int rounds)
{
    uint8_t *data, *c;
    size_t n;
    CaffHeader h;
    int r;

    if (caff_read_file(path, &data, &n))
        return;
    if (caff_header_read(data, n, &h, NULL, 0) || h.any_packed || !(c = malloc(n))) {
        free(data);
        return;
    }
    for (r = 0; r < rounds; r++) {
        size_t m = n;
        uint32_t toc_at = h.data_offset + *(uint32_t *)(data + 0x34);

        memcpy(c, data, n);
        switch (rnd() % 5) {
        case 0:                         /* cut short */
            m = rnd() % n;
            break;
        case 1:                         /* a header byte, checksum mended */
            c[rnd() % CAFF_HEADER_SIZE] ^= (uint8_t)(1u << (rnd() % 8));
            *(uint32_t *)(c + 0x14) = caff_checksum(c);
            break;
        case 2:                         /* a count made huge */
            *(uint32_t *)(c + 0x18 + 4 * (rnd() % 6)) = rnd() | 0x80000000u;
            *(uint32_t *)(c + 0x14) = caff_checksum(c);
            break;
        case 3: {                       /* bytes in the table of contents */
            int b;
            for (b = 0; b < 4 && toc_at < n; b++)
                c[toc_at + rnd() % (uint32_t)(n - toc_at)] = (uint8_t)rnd();
            break;
        }
        default: {                      /* bytes anywhere */
            int b;
            for (b = 0; b < 8; b++)
                c[rnd() % n] = (uint8_t)rnd();
            *(uint32_t *)(c + 0x14) = caff_checksum(c);
            break;
        }
        }
        try_open(c, m);
    }
    free(c);
    free(data);
}

static void fuzz_walk(const char *root, const char *rel, int rounds)
{
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2], full[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE hf;

    sprintf_s(pat, sizeof pat, "%s\\%s*", root, rel);
    if ((hf = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        size_t len = strlen(fd.cFileName);

        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        sprintf_s(sub, sizeof sub, "%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            strcat_s(sub, sizeof sub, "\\");
            fuzz_walk(root, sub, rounds);
        } else if (len > 4 && (!_stricmp(fd.cFileName + len - 4, ".rbm")
                               || !_stricmp(fd.cFileName + len - 4, ".rba"))) {
            sprintf_s(full, sizeof full, "%s\\%s", root, sub);
            fuzz_one(full, rounds);
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

/* ── loose ─────────────────────────────────────────────────────────────── */

static int s_loose_n, s_loose_bad;
static unsigned long long s_loose_bytes;

#define LOOSE_FAIL(...) do { printf("  FAIL %s: ", d.name); printf(__VA_ARGS__); \
                             printf("\n"); ok = 0; goto next; } while (0)

/* Every directory entry of one unpacked package as a loose package, opened
 * again and checked against the source part by part, pointer by pointer. */
static void loose_pkg(const char *dir, const char *rel, const char *out_root)
{
    char path[MAX_PATH * 2], out[MAX_PATH * 2], stem[MAX_PATH];
    uint8_t *data, *buf;
    size_t n, nb;
    CaffPkg *k, *l;
    int i;

    sprintf_s(path, sizeof path, "%s\\%s", dir, rel);
    if (caff_read_file(path, &data, &n) || caff_open_mem(data, n, rel, &k)) {
        printf("cannot open %s\n", rel);
        s_loose_bad++;
        return;
    }
    free(data);
    strcpy_s(stem, sizeof stem, rel);
    if (strrchr(stem, '\\'))
        *strrchr(stem, '\\') = 0;
    for (i = 0; i < caff_dir_count(k); i++) {
        CaffDirEntry d;
        CaffClosure c;
        CaffInfo in, li;
        CaffPart sp, lp;
        uint32_t *newpart = NULL, j, r, want_rel = 0, want_off = 0;
        int ok = 1, rc;

        caff_dir_entry(k, i, &d);
        caff_info(k, &in);
        l = NULL;
        buf = NULL;
        s_loose_n++;
        if ((rc = caff_closure(k, i, &c))) {
            printf("  FAIL %s: closure %s\n", d.name, caff_strerror(rc));
            s_loose_bad++;
            continue;
        }
        if ((rc = caff_write_loose(k, i, &buf, &nb)))
            LOOSE_FAIL("write %s", caff_strerror(rc));
        if ((rc = caff_open_mem(buf, nb, d.name, &l)))
            LOOSE_FAIL("reopen %s", caff_strerror(rc));
        caff_info(l, &li);
        if (li.files != c.nfiles || li.parts != c.nparts)
            LOOSE_FAIL("counts %u/%u vs %u/%u", li.files, li.parts, c.nfiles, c.nparts);
        if (!caff_file_name(l, 1) || strcmp(caff_file_name(l, 1), d.name))
            LOOSE_FAIL("file 1 is not named after the resource");
        if (d.offset != 0x20)
            LOOSE_FAIL("starts at +0x%X, not +0x20", d.offset);
        newpart = calloc(in.parts + 1, sizeof *newpart);
        for (j = 0; j < c.nparts; j++)
            newpart[c.parts[j]] = j + 1;
        if (newpart[c.start] != 1)
            LOOSE_FAIL("the resource is part %u", newpart[c.start]);
        for (j = 1; j <= c.nparts; j++) {
            uint32_t f;

            caff_part(k, c.parts[j - 1], &sp);
            caff_part(l, j, &lp);
            for (f = 0; f < c.nfiles && c.files[f] != sp.file; f++) ;
            if (lp.size != sp.size || lp.section != sp.section
                || lp.align_log2 != sp.align_log2 || lp.file != f + 1
                || memcmp(lp.bytes, sp.bytes, sp.size)
                || lp.offset % (1u << lp.align_log2))
                LOOSE_FAIL("part %u (source %u) differs", j, c.parts[j - 1]);
        }
        /* Every pointer of a part taken points at the same part, renumbered;
         * and the loose package has no other pointers. */
        for (r = 0; r < in.relocs; r++) {
            uint32_t src, dst, cnt, q;
            const uint8_t *offs;

            caff_reloc(k, r, &src, &dst, &cnt, &offs);
            if (!newpart[src])
                continue;
            want_rel++;
            want_off += cnt;
            for (q = 0; q < cnt; q++) {
                uint32_t o;
                memcpy(&o, offs + 4 * q, 4);
                if (caff_reloc_target(l, newpart[src], o) != newpart[dst])
                    LOOSE_FAIL("pointer +0x%X of part %u lost", o, src);
            }
        }
        if (li.relocs != want_rel || li.reloc_offsets != want_off)
            LOOSE_FAIL("%u relocations / %u pointers, expected %u / %u", li.relocs,
                       li.reloc_offsets, want_rel, want_off);
        if (out_root) {
            sprintf_s(out, sizeof out, "%s\\%s\\%s", out_root, stem, d.name);
            if (caff_make_parent_dirs(out) || caff_write_file(out, buf, nb))
                LOOSE_FAIL("cannot write %s", out);
        }
        s_loose_bytes += nb;
next:
        if (!ok)
            s_loose_bad++;
        free(newpart);
        caff_close(l);
        free(buf);
        caff_closure_free(&c);
    }
    caff_close(k);
}

int main(int argc, char **argv)
{
    Walk w;

    if (argc >= 5 && !strcmp(argv[1], "loose")) {
        int a;
        for (a = 4; a < argc; a++)
            loose_pkg(argv[2], argv[a], strcmp(argv[3], "-") ? argv[3] : NULL);
        printf("loose: %d resources, %d failed, %llu bytes written\n", s_loose_n,
               s_loose_bad, s_loose_bytes);
        return s_loose_bad ? 1 : 0;
    }

    if (argc >= 4 && !strcmp(argv[1], "inventory")) {
        CaffInventoryResult r;
        int rc = caff_inventory_write(argv[2], NULL, argv[3], NULL, NULL, &r);

        if (rc) {
            fprintf(stderr, "inventory: %s\n", caff_strerror(rc));
            return 1;
        }
        printf("inventory: %d packages, %d resources, %d error(s), %zu bytes\n",
               r.packages, r.resources, r.errors, r.bytes);
        return r.errors ? 1 : 0;
    }

    if (argc >= 3 && !strcmp(argv[1], "fuzz")) {
        int rounds = argc > 3 ? atoi(argv[3]) : 200;
        fuzz_walk(argv[2], "", rounds);
        printf("fuzz: %d damaged copies, %d refused, %d opened (and walked) without "
               "a fault; %d loose packages written from them would not open again\n",
               s_fuzz_n, s_fuzz_refused, s_fuzz_ok, s_fuzz_loose_bad);
        return 0;
    }
    if (argc < 4 || (strcmp(argv[1], "raw") && strcmp(argv[1], "dump"))) {
        fprintf(stderr, "usage: caff_test raw|dump|inventory <dir> <out.json> | fuzz <dir> [rounds]\n"
                        "       caff_test loose <unpacked dir> <out dir | -> <package>...\n");
        return 2;
    }
    memset(&w, 0, sizeof w);
    w.raw = !strcmp(argv[1], "raw");
    w.first = 1;
    if (fopen_s(&w.out, argv[3], "w")) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 2;
    }
    fprintf(w.out, "{");
    walk(&w, argv[2], "");
    fprintf(w.out, "\n}\n");
    fclose(w.out);
    printf("%s: %d packages, %d refused", argv[1], w.n, w.bad);
    if (w.raw)
        printf(", %d packed (most blocks in a section: %u), %d flag set but not packed",
               w.packed, w.max_blocks, w.flag_only);
    printf("\n");
    return w.bad ? 1 : 0;
}
