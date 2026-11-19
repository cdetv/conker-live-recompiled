/*
 * Carved assets for the game (to-do 43): the game's open of a disc file under
 * dvddata\aid\ goes to <save dir>\assets\aid\<the same path> when the cache
 * holds that file. The game itself then loads it through its own doors
 * (docs/research/r3-asset-loading.md): a loose resource at its normal path,
 * a character package at spawn, a level package list entry.
 *
 * The cache adds files and never replaces one the disc has: a cached file
 * whose path also exists on the disc is left alone (a [CAFF] line says so).
 * Developer switch RECOMP_ASSETS_REPLACE=1 lets those replace the disc's too,
 * for tests such as serving an unpacked level from the cache.
 *
 * Only files the stamp vouches for (listed, at its size) are served; see
 * vouch() below for the test exceptions.
 *
 * The cache's file list is read once here (nothing is carved while the game
 * runs), so an open costs a lookup in memory and no disc access. The first
 * 20 redirected opens are logged; RECOMP_CAFF_LOG=1 logs every one. Chained in
 * front of the save-name and decensor redirects (src/save_names.c,
 * src/decensor.c) through g_xbox_path_redirect; every other path goes on to
 * them unchanged.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "caff_cache.h"

extern int (*g_xbox_path_redirect)(const char *xbox_path, WCHAR *host_path, DWORD buf_size);
static int (*s_prev)(const char *, WCHAR *, DWORD);

typedef struct {
    char  *rel;         /* below aid\, as found in the cache */
    WCHAR *host;        /* <save dir>\assets\aid\<rel> */
    int    on_disc;     /* the disc has this path too */
    unsigned long long size;
} Asset;

static Asset *s_assets;
static int    s_count, s_cap, s_replace, s_log_all;
static LONG   s_logged;

static int cmp_asset(const void *a, const void *b)
{
    return _stricmp(((const Asset *)a)->rel, ((const Asset *)b)->rel);
}

static void add(const WCHAR *root, const WCHAR *rel, unsigned long long size)
{
    char u[MAX_PATH * 3];
    WCHAR host[MAX_PATH * 2];
    size_t hl;

    if (!WideCharToMultiByte(CP_UTF8, 0, rel, -1, u, sizeof u, NULL, NULL))
        return;
    if (s_count == s_cap) {
        int cap = s_cap ? s_cap * 2 : 64;
        Asset *na = realloc(s_assets, (size_t)cap * sizeof *na);
        if (!na)
            return;
        s_assets = na;
        s_cap = cap;
    }
    swprintf_s(host, MAX_PATH * 2, L"%s\\%s", root, rel);
    hl = wcslen(host) + 1;
    s_assets[s_count].rel = _strdup(u);
    s_assets[s_count].host = malloc(hl * sizeof(WCHAR));
    if (!s_assets[s_count].rel || !s_assets[s_count].host) {
        free(s_assets[s_count].rel);
        free(s_assets[s_count].host);
        return;
    }
    memcpy(s_assets[s_count].host, host, hl * sizeof(WCHAR));
    s_assets[s_count].on_disc = 0;
    s_assets[s_count].size = size;
    s_count++;
}

/* Every file below root\rel, rel growing as it goes down. A half-written
 * "<name>.tmp" (a carve stopped midway) is not an asset. */
static void scan(const WCHAR *root, const WCHAR *rel, int depth)
{
    WCHAR pat[MAX_PATH * 2], sub[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > 16)
        return;
    swprintf_s(pat, MAX_PATH * 2, rel[0] ? L"%s\\%s\\*" : L"%s\\*", root, rel);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        size_t n = wcslen(fd.cFileName);

        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(sub, MAX_PATH * 2, rel[0] ? L"%s\\%s" : L"%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan(root, sub, depth + 1);
        else if (!(n > 4 && !_wcsicmp(fd.cFileName + n - 4, L".tmp")))
            add(root, sub, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* "D:\dvddata\aid\x", "\Device\CdRom0\dvddata\aid\x", "\??\D:\dvddata\aid\x"
 * -> "x"; anything else -> NULL. */
static const char *aid_rel(const char *p)
{
    if (!_strnicmp(p, "\\Device\\CdRom0\\", 15))
        p += 15;
    else if (!_strnicmp(p, "D:\\", 3))
        p += 3;
    else if (!_strnicmp(p, "\\??\\D:\\", 7))
        p += 7;
    else
        return NULL;
    return _strnicmp(p, "dvddata\\aid\\", 12) ? NULL : p + 12;
}

static int redirect(const char *xbox_path, WCHAR *host, DWORD n)
{
    const char *rel = aid_rel(xbox_path);

    if (rel && s_count) {
        Asset key, *a;

        key.rel = (char *)rel;
        a = bsearch(&key, s_assets, (size_t)s_count, sizeof *s_assets, cmp_asset);
        if (a && (!a->on_disc || s_replace) && wcslen(a->host) < n) {
            wcscpy_s(host, n, a->host);
            if (InterlockedIncrement(&s_logged) <= 20 || s_log_all)
                fprintf(stderr, "[CAFF] %s -> assets\\aid\\%s\n", xbox_path, a->rel);
            return 1;
        }
    }
    return s_prev ? s_prev(xbox_path, host, n) : 0;
}

/* Is aid\<rel> served from the cache? (for the guards, to-do 43 step 7) */
int caff_assets_has(const char *rel)
{
    Asset key, *a;

    if (!s_count)
        return 0;
    key.rel = (char *)rel;
    a = bsearch(&key, s_assets, (size_t)s_count, sizeof *s_assets, cmp_asset);
    return a && (!a->on_disc || s_replace);
}

static int flag_on(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0");
}

static void drop(Asset *a)
{
    free(a->rel);
    free(a->host);
}

/* Only what the stamp vouches for is served (to-do 43, step 7): listed there,
 * at the stamp's size. A file it doesn't list (a stray, a test's leftover) or
 * of another size (damaged) could make the game halt on a model it can't
 * read; without a stamp (never carved, or a carve stopped midway) nothing is
 * served. A linked assets\aid (tests: run.ps1 -AssetsLink) and
 * RECOMP_ASSETS_ANY=1 (tests: files put in by hand, run.ps1 -Copy) are served
 * as they are. */
static void vouch(const char *save_dir, int linked)
{
    CaffCacheFile *f;
    int nf, i, j, kept = 0, said = 0;

    if (linked || flag_on("RECOMP_ASSETS_ANY")) {
        fprintf(stderr, "[CAFF] assets: %s, so every file there is served without the stamp's check\n",
                linked ? "assets\\aid is a link (a test)" : "RECOMP_ASSETS_ANY=1");
        return;
    }
    if ((nf = caff_cache_files(save_dir, &f)) < 0) {
        fprintf(stderr, "[CAFF] assets: no stamp.json (never carved, or a carve stopped midway): "
                "none of the %d file(s) in assets\\aid served\n", s_count);
        for (i = 0; i < s_count; i++)
            drop(&s_assets[i]);
        s_count = 0;
        return;
    }
    for (i = 0; i < s_count; i++) {
        const CaffCacheFile *m = NULL;

        for (j = 0; j < nf && !m; j++)
            if (!_stricmp(f[j].path, s_assets[i].rel))
                m = &f[j];
        if (m && m->size == s_assets[i].size) {
            s_assets[kept++] = s_assets[i];
            continue;
        }
        if (said++ < 20) {
            if (m)
                fprintf(stderr, "[CAFF] assets\\aid\\%s not served: %llu bytes, the stamp says %llu "
                        "(damaged; the launcher carves it again)\n", s_assets[i].rel,
                        s_assets[i].size, m->size);
            else
                fprintf(stderr, "[CAFF] assets\\aid\\%s not served: the stamp doesn't list it\n",
                        s_assets[i].rel);
        }
        drop(&s_assets[i]);
    }
    s_count = kept;
    free(f);
}

/* Call after the other redirects are in place, before the game runs. */
void caff_redirect_init(const char *game_dir, const char *save_dir)
{
    WCHAR root[MAX_PATH], game[MAX_PATH], disc[MAX_PATH * 2];
    DWORD at;
    int i, replaced = 0;

    s_replace = flag_on("RECOMP_ASSETS_REPLACE");
    s_log_all = flag_on("RECOMP_CAFF_LOG");
    if (!MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, game, MAX_PATH))
        return;
    swprintf_s(root, MAX_PATH, L"%s\\assets\\aid", game);
    if (!MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, game, MAX_PATH))
        return;
    scan(root, L"", 0);
    if (!s_count)
        return;
    at = GetFileAttributesW(root);
    vouch(save_dir, at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT));
    if (!s_count)
        return;
    qsort(s_assets, (size_t)s_count, sizeof *s_assets, cmp_asset);
    for (i = 0; i < s_count; i++) {
        WCHAR rel[MAX_PATH * 2];

        MultiByteToWideChar(CP_UTF8, 0, s_assets[i].rel, -1, rel, MAX_PATH * 2);
        swprintf_s(disc, MAX_PATH * 2, L"%s\\dvddata\\aid\\%s", game, rel);
        if (GetFileAttributesW(disc) != INVALID_FILE_ATTRIBUTES) {
            s_assets[i].on_disc = 1;
            replaced++;
        }
    }
    s_prev = g_xbox_path_redirect;
    g_xbox_path_redirect = redirect;
    fprintf(stderr, "[CAFF] assets: %d files in %S", s_count, root);
    if (replaced)
        fprintf(stderr, "; %d of them the disc has too: %s", replaced,
                s_replace ? "served from the cache (RECOMP_ASSETS_REPLACE)"
                          : "the disc's are kept");
    fprintf(stderr, "\n");
}
