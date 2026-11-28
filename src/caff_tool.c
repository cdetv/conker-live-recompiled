/*
 * The package reader from the command line, without the game (to-do 43):
 *
 *   conker.exe --list-package <package> [--json] [--game-dir <folder>]
 *       one package: header, sections (unpacked / stored), counts, its
 *       directory. <package> is a file (.rbm / .rba) or a name under the
 *       game's dvddata\aid: "Level\Windmill" (zpackage\Level\Windmill),
 *       "texture\Avatars\ace", "zpackage\Characters\Conker".
 *   conker.exe --extract <package> <resource> <out file> [--game-dir <folder>]
 *       developer switch: one resource of a package and everything it needs
 *       as a loose package (caff_write_loose). <resource> is its directory
 *       name or a short one ("FryingPanBound") when only one entry has it.
 *   conker.exe --unpack-all <out folder> [--game-dir <folder>]
 *       developer switch: every package of the game unpacked by the game's
 *       own code (src/caff_guest.c) into <out folder> at its path below aid\.
 *   Both write game data: keep it in _work (SPEC 3.3).
 *   conker.exe --carve [--check] [--game-dir <folder>] [--save-dir <folder>]
 *       the launcher's step after an import (src/caff_cache.c): fills
 *       <save>\assets\ from the game files (the recipes' files, then
 *       inventory.json from all 266 packages, then the sharper text and
 *       buttons, assets\sharp\, to-do 53); "PROGRESS <done> <total>
 *       <name>" lines, then DONE or FAILED. --check only says whether it is needed:
 *       exit 0 up to date, 3 stale, 1 no game files (no guest, ~no time).
 *   conker.exe --texture-list <out.tsv> [--game-dir <folder>]
 *       the texture pack kit (to-do 238, src/texkit_tool.c): every picture
 *       of every package with its replacement name (hash) as a TSV; the
 *       player's game data, so it stays on their PC. "PROGRESS <done>
 *       <total> <name>" lines, then DONE (with the counts) or FAILED.
 *   conker.exe --texture-export <out folder> [--game-dir <folder>]
 *       the same, and each picture a pack can replace as a PNG, once per
 *       hash: <out>\<package's folder below aid>\<name>.png (the
 *       extractor's tree and file names: zpackage\Level\Bank\...), the list as
 *       <out>\textures.tsv. PNGs already there are kept (a stopped export
 *       goes on); DONE with the counts and bytes, or FAILED.
 *
 * Only the XBE goes into guest memory (main.c conker_tool_guest_up): no
 * window, kernel or game. Paths come from the wide command line as UTF-8,
 * so folders with any characters work. Text on stdout, [CAFF] reasons on
 * stderr; exit 0 done, 1 failed, 2 bad arguments.
 */
#include <windows.h>
#include <shellapi.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "caff.h"
#include "caff_cache.h"
#include "texkit.h"

int  conker_tool_guest_up(const void *xbe, size_t size);
void conker_tool_guest_down(void);
void conker_default_save_dir_utf8(char *out, size_t cap);

#define PATH_CAP 1024

static void w2u(const wchar_t *w, char *u, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, u, cap, NULL, NULL))
        u[0] = 0;
}

static void slashes(char *s)
{
    for (; *s; s++)
        if (*s == '/')
            *s = '\\';
}

static int has_ext(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && (!_stricmp(s + n - 4, ".rbm") || !_stricmp(s + n - 4, ".rba"));
}

static void jstr(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20 || c >= 0x7F)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

static int list_package(const char *path, int json)
{
    uint8_t *file;
    size_t n;
    CaffHeader h;
    CaffPkg *k = NULL;
    CaffInfo in;
    CaffDirEntry d;
    CaffPart p;
    char why[96];
    int rc, s, i;

    if ((rc = caff_read_file(path, &file, &n))) {
        fprintf(stderr, "[CAFF] %s: %s\n", path, caff_strerror(rc));
        return 1;
    }
    rc = caff_header_read(file, n, &h, why, sizeof why);
    free(file);
    if (rc) {
        fprintf(stderr, "[CAFF] %s: %s: %s\n", path, caff_strerror(rc), why);
        return 1;
    }
    if (h.any_packed && caff_guest_begin())
        return 1;
    rc = caff_open(path, h.any_packed ? caff_guest_unpack : NULL, &k);
    caff_guest_end();
    if (rc)
        return 1;
    caff_info(k, &in);

    if (json) {
        printf("{\"path\": ");
        jstr(path);
        printf(", \"flag\": %d, \"packed\": %d, \"sections\": [", h.flag_set, h.any_packed);
        for (s = 0; s < h.sections; s++) {
            printf("%s{\"name\": ", s ? ", " : "");
            jstr(h.name[s]);
            printf(", \"unpacked\": %u, \"stored\": %u, \"packed\": %d, \"blocks\": %u}",
                   h.unpacked[s], h.stored[s], h.packed[s], h.blocks[s]);
        }
        printf("], \"files\": %u, \"parts\": %u, \"relocs\": %u, \"reloc_offsets\": %u, "
               "\"tag\": ", in.files, in.parts, in.relocs, in.reloc_offsets);
        jstr(in.tag);
        printf(", \"dir\": [");
        for (i = 0; i < caff_dir_count(k); i++) {
            caff_dir_entry(k, i, &d);
            printf("%s\n  {\"name\": ", i ? "," : "");
            jstr(d.name);
            printf(", \"part\": %u, \"offset\": %u}", d.part, d.offset);
        }
        printf("]}\n");
    } else {
        printf("%s\n", path);
        printf("  %s\n", h.any_packed ? "packed (unpacked by the game's own code)"
                         : h.flag_set ? "packed flag set, but stored as it is"
                         : "not packed");
        for (s = 0; s < h.sections; s++)
            printf("  section %-6s %10u bytes (stored %u%s)\n", h.name[s], h.unpacked[s],
                   h.stored[s], h.packed[s] ? ", packed" : "");
        printf("  %u files, %u parts, %u relocations (%u pointers), tag \"%s\", "
               "checksum ok\n", in.files, in.parts, in.relocs, in.reloc_offsets, in.tag);
        if (caff_dir_count(k))
            printf("  directory, %d entries:\n", caff_dir_count(k));
        for (i = 0; i < caff_dir_count(k); i++) {
            caff_dir_entry(k, i, &d);
            caff_part(k, d.part, &p);
            printf("    %-72s part %5u +0x%-6X (part %u bytes, %s)\n", d.name, d.part,
                   d.offset, p.size, p.section == 2 ? ".gpu" : ".data");
        }
    }
    caff_close(k);
    return 0;
}

typedef struct {
    const char *out;
    int n, bad;
    unsigned long long stored, unpacked;
} Unpack;

static void unpack_one(Unpack *u, const char *path, const char *rel)
{
    uint8_t *file = NULL, *flat = NULL;
    size_t n, n_flat;
    CaffPkg *k = NULL;
    char dst[PATH_CAP];
    int rc;

    u->n++;
    if ((rc = caff_read_file(path, &file, &n))
        || (rc = caff_unpack_with(file, n, caff_guest_unpack, &flat, &n_flat))
        || (rc = caff_open_mem(flat, n_flat, rel, &k))) {
        fprintf(stderr, "[CAFF] %s: %s\n", rel, caff_strerror(rc));
        printf("FAILED %s: %s\n", rel, caff_strerror(rc));
        u->bad++;
        goto done;
    }
    snprintf(dst, sizeof dst, "%s\\%s", u->out, rel);
    if (caff_make_parent_dirs(dst) || caff_write_file(dst, flat, n_flat)) {
        printf("FAILED %s: cannot write %s\n", rel, dst);
        u->bad++;
        goto done;
    }
    printf("%s %zu -> %zu\n", rel, n, n_flat);
    u->stored += n;
    u->unpacked += n_flat;
done:
    caff_close(k);
    free(flat);
    free(file);
}

static void walk(Unpack *u, const char *root, const char *rel)
{
    wchar_t pat[PATH_CAP];
    char pat_u[PATH_CAP], name[PATH_CAP], sub[PATH_CAP], full[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE hf;

    snprintf(pat_u, sizeof pat_u, "%s\\%s*", root, rel);
    if (!MultiByteToWideChar(CP_UTF8, 0, pat_u, -1, pat, PATH_CAP)
        || (hf = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        w2u(fd.cFileName, name, sizeof name);
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s%s", rel, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            strcat_s(sub, sizeof sub, "\\");
            walk(u, root, sub);
        } else if (has_ext(name)) {
            snprintf(full, sizeof full, "%s\\%s", root, sub);
            unpack_one(u, full, sub);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

static int unpack_all(const char *game, const char *out)
{
    char root[PATH_CAP], marker[PATH_CAP];
    Unpack u;

    memset(&u, 0, sizeof u);
    u.out = out;
    snprintf(root, sizeof root, "%s\\dvddata\\aid", game);
    snprintf(marker, sizeof marker, "%s\\x", out);
    if (caff_make_parent_dirs(marker)) {
        fprintf(stderr, "[CAFF] cannot make %s\n", out);
        return 1;
    }
    if (caff_guest_begin())
        return 1;
    walk(&u, root, "");
    caff_guest_end();
    printf("done: %d packages, %d failed, %llu -> %llu bytes, into %s\n", u.n, u.bad,
           u.stored, u.unpacked, out);
    return u.n && !u.bad ? 0 : 1;
}

/* A directory entry by its full name, or by a short one: the only entry with
 * "\<short>\" in its name ("FryingPanBound"). -1 when none or several. */
static int find_resource(const CaffPkg *k, const char *name)
{
    char want[PATH_CAP], low[PATH_CAP];
    CaffDirEntry e;
    int i, hit = -1, hits = 0;
    size_t j;

    if ((i = caff_find(k, name)) >= 0)
        return i;
    snprintf(want, sizeof want, "\\%s\\", name);
    slashes(want);
    for (j = 0; want[j]; j++)
        want[j] = (char)tolower((unsigned char)want[j]);
    for (i = 0; i < caff_dir_count(k); i++) {
        caff_dir_entry(k, i, &e);
        snprintf(low, sizeof low, "%s", e.name);
        for (j = 0; low[j]; j++)
            low[j] = (char)tolower((unsigned char)low[j]);
        if (strstr(low, want)) {
            if (hits++ < 8)
                fprintf(stderr, "  %s\n", e.name);
            hit = i;
        }
    }
    if (hits == 1)
        return hit;
    fprintf(stderr, "[CAFF] %s: %s in the package\n", name,
            hits ? "more than one match" : "no such resource");
    return -1;
}

static int extract(const char *path, const char *name, const char *out)
{
    CaffHeader h;
    CaffPkg *k = NULL;
    CaffClosure c;
    uint8_t *file, *loose = NULL;
    size_t n;
    char why[96];
    int i, rc;

    if (caff_read_file(path, &file, &n))
        return 1;
    rc = caff_header_read(file, n, &h, why, sizeof why);
    free(file);
    if (rc) {
        fprintf(stderr, "[CAFF] %s: %s: %s\n", path, caff_strerror(rc), why);
        return 1;
    }
    if (h.any_packed && caff_guest_begin())
        return 1;
    rc = caff_open(path, h.any_packed ? caff_guest_unpack : NULL, &k);
    caff_guest_end();
    if (rc)
        return 1;
    if ((i = find_resource(k, name)) < 0
        || (rc = caff_closure(k, i, &c))) {
        caff_close(k);
        return 1;
    }
    if ((rc = caff_write_loose(k, i, &loose, &n))
        || (rc = caff_make_parent_dirs(out)) || (rc = caff_write_file(out, loose, n))) {
        fprintf(stderr, "[CAFF] %s: %s\n", name, caff_strerror(rc));
        rc = 1;
    } else {
        CaffDirEntry e;

        caff_dir_entry(k, i, &e);
        printf("%s: %u files, %u parts, %llu + %llu bytes (.data + .gpu), %zu bytes "
               "written to %s\n", e.name, c.nfiles, c.nparts,
               (unsigned long long)c.data_bytes, (unsigned long long)c.gpu_bytes, n, out);
        rc = 0;
    }
    caff_closure_free(&c);
    free(loose);
    caff_close(k);
    return rc;
}

static void carve_progress(int done, int total, const char *what, void *ctx)
{
    (void)ctx;
    printf("PROGRESS %d %d %s\n", done, total, what);
    fflush(stdout);
}

int caff_tool_cmd(void)
{
    wchar_t **wargv, exe[PATH_CAP], *slash;
    char mode[32], pos[3][PATH_CAP], game[PATH_CAP] = "", xbe[PATH_CAP], a[PATH_CAP];
    char path[PATH_CAP], save[PATH_CAP] = "", *arg = pos[0];
    uint8_t *xbe_data = NULL;
    size_t xbe_size = 0;
    int argc, i, json = 0, check = 0, rc, npos = 0, want;

    if (!(wargv = CommandLineToArgvW(GetCommandLineW(), &argc)) || argc < 2)
        return 2;
    w2u(wargv[1], mode, sizeof mode);
    want = !strcmp(mode, "--extract") ? 3 : !strcmp(mode, "--carve") ? 0 : 1;
    for (i = 2; i < argc; i++) {
        w2u(wargv[i], a, sizeof a);
        if (!strcmp(a, "--game-dir") && i + 1 < argc) {
            w2u(wargv[++i], game, sizeof game);
        } else if (!strcmp(a, "--save-dir") && i + 1 < argc && !strcmp(mode, "--carve")) {
            w2u(wargv[++i], save, sizeof save);
        } else if (!strcmp(a, "--json") && !strcmp(mode, "--list-package")) {
            json = 1;
        } else if (!strcmp(a, "--check") && !strcmp(mode, "--carve")) {
            check = 1;
        } else if (npos < want && a[0] && a[0] != '-') {
            snprintf(pos[npos++], PATH_CAP, "%s", a);
        } else {
            npos = -1;
            break;
        }
    }
    if (npos != want) {
        fprintf(stderr, "Usage: conker --list-package <package> [--json] [--game-dir <folder>]\n"
                        "       conker --extract <package> <resource> <out file> [--game-dir <folder>]\n"
                        "       conker --unpack-all <out folder> [--game-dir <folder>]\n"
                        "       conker --carve [--check] [--game-dir <folder>] [--save-dir <folder>]\n"
                        "       conker --texture-list <out.tsv> [--game-dir <folder>]\n"
                        "       conker --texture-export <out folder> [--game-dir <folder>]\n");
        LocalFree(wargv);
        return 2;
    }
    if (!game[0]) {
        GetModuleFileNameW(NULL, exe, PATH_CAP);
        if ((slash = wcsrchr(exe, L'\\')))
            *slash = 0;
        w2u(exe, game, sizeof game);
        strcat_s(game, sizeof game, "\\game");
    }
    LocalFree(wargv);
    slashes(game);
    for (i = (int)strlen(game); i > 1 && game[i - 1] == '\\'; i--)
        game[i - 1] = 0;
    if (!strcmp(mode, "--carve")) {
        if (!save[0])
            conker_default_save_dir_utf8(save, sizeof save);
        slashes(save);
        for (i = (int)strlen(save); i > 1 && save[i - 1] == '\\'; i--)
            save[i - 1] = 0;
    }

    /* --carve --check: from the folder listing and the stamp, no guest. */
    if (check) {
        rc = caff_cache_check(game, save);
        printf("%s\n", rc == CAFF_CACHE_OK ? "UP TO DATE" : rc == CAFF_CACHE_STALE
                       ? "STALE" : "ERROR no game files");
        return rc == CAFF_CACHE_OK ? 0 : rc == CAFF_CACHE_STALE ? 3 : 1;
    }

    snprintf(xbe, sizeof xbe, "%s\\default.xbe", game);
    if (caff_read_file(xbe, &xbe_data, &xbe_size)) {
        fprintf(stderr, "No default.xbe in %s\n", game);
        return 1;
    }
    if (strcmp(mode, "--unpack-all") && strcmp(mode, "--carve") && strcmp(mode, "--texture-list")
        && strcmp(mode, "--texture-export")
        && !caff_resolve_package(game, arg, path, sizeof path)) {
        fprintf(stderr, "[CAFF] no package %s (as a file or under %s\\dvddata\\aid)\n",
                arg, game);
        free(xbe_data);
        return 1;
    }
    if (!conker_tool_guest_up(xbe_data, xbe_size)) {
        fprintf(stderr, "default.xbe in %s is not a supported version, or guest "
                "memory could not be set up\n", game);
        free(xbe_data);
        return 1;
    }
    if (!strcmp(mode, "--carve")) {
        rc = caff_guest_begin();
        if (!rc) {
            rc = caff_cache_carve(game, save, 1, 1, carve_progress, NULL);
            caff_guest_end();
        }
        printf("%s\n", rc ? "FAILED" : "DONE");
        rc = rc ? 1 : 0;
    } else if (!strcmp(mode, "--texture-export")) {
        char aid[PATH_CAP];
        TexkitResult tr;

        slashes(arg);
        for (i = (int)strlen(arg); i > 1 && arg[i - 1] == '\\'; i--)
            arg[i - 1] = 0;
        snprintf(aid, sizeof aid, "%s\\dvddata\\aid", game);
        rc = caff_guest_begin();
        if (!rc) {
            rc = texkit_export(aid, caff_guest_unpack, arg, carve_progress, NULL, &tr);
            caff_guest_end();
        }
        if (rc)
            printf("FAILED\n");
        else
            printf("DONE %d packages (%d failed), %d pictures a pack can replace: %d PNGs "
                   "written (%.1f MB), %d there already, %d failed, in %s\n", tr.packages,
                   tr.failed, tr.distinct, tr.written, tr.bytes / 1048576.0, tr.skipped, tr.bad,
                   arg);
        rc = rc || tr.bad ? 1 : 0;
    } else if (!strcmp(mode, "--texture-list")) {
        char aid[PATH_CAP];
        TexkitResult tr;

        slashes(arg);
        snprintf(aid, sizeof aid, "%s\\dvddata\\aid", game);
        rc = caff_guest_begin();
        if (!rc) {
            rc = texkit_list(aid, caff_guest_unpack, arg, carve_progress, NULL, &tr);
            caff_guest_end();
        }
        if (rc)
            printf("FAILED\n");
        else
            printf("DONE %d packages (%d failed), %d textures, %d pictures, %d a pack can "
                   "replace (one PNG each), in %s\n", tr.packages, tr.failed, tr.textures,
                   tr.images, tr.distinct, arg);
        rc = rc ? 1 : 0;
    } else if (!strcmp(mode, "--unpack-all")) {
        slashes(arg);
        rc = unpack_all(game, arg);
    } else if (!strcmp(mode, "--extract")) {
        slashes(pos[2]);
        rc = extract(path, pos[1], pos[2]);
    } else {
        rc = list_package(path, json);
    }
    conker_tool_guest_down();
    free(xbe_data);
    return rc;
}
