/**
 * The decensor patch (nastys/conker-live-and-reloaded-decensor), as two
 * switches: uncensored audio and uncensored text (SPEC 10.1).
 *
 * The patch is replacement files only, laid out like the disc: the cutscene
 * voice banks and the Sloprano song bank in dvddata\Audio\, and the per-map
 * text in dvddata\aid\text\<language>\<map>\default.bin. The player (or the
 * launcher) unpacks it as is into a folder, "decensor" next to the .exe by
 * default (paths.decensor_dir). Nothing is copied over the game files: the
 * toolkit's path redirect (g_xbox_path_redirect) sends the game's opens of
 * those files to the patch's copies while a switch is on, so turning it off
 * gives the original back. A switch is read at every open, so a change
 * applies to whatever the game opens next: text at the next level load,
 * but the cutscene voice banks are opened once at start-up and stay open,
 * so the F2 menu changes audio at the next start only.
 *
 * The cache partition. The game copies some banks to Z: once (the Sloprano
 * song among them) and plays them from there after that, so redirecting the
 * disc file alone changes nothing once the copy exists. A Z: path whose file
 * the patch replaces goes to CacheUncensored\ in the save folder instead of
 * Cache\, and the patch's copy is put there at start-up whenever the
 * original copy is in Cache\, so the game finds the cache in the same state
 * either way. The patch folder itself is never written.
 */

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "decensor.h"
#include "settings.h"

/* kernel.h; the host path is WCHAR on Windows. */
extern int (*g_xbox_path_redirect)(const char *xbox_path,
                                   WCHAR *host_path, DWORD buf_size);

#define MAX_FILES 512

typedef struct {
    char  key[MAX_PATH];     /* "dvddata\audio\cut_general.xwb", lower case */
    WCHAR rel[MAX_PATH];     /* the same as found on disk */
    int   text;              /* 1 = text switch, 0 = audio switch */
} PatchFile;

static PatchFile s_files[MAX_FILES];
static int   s_count;
static WCHAR s_dir[MAX_PATH];        /* the patch folder (holds dvddata\) */
static WCHAR s_cache[MAX_PATH];      /* <save>\CacheUncensored */
static WCHAR s_orig_cache[MAX_PATH]; /* <save>\Cache, the Z: contents */
static volatile LONG s_on[2] = { 1, 1 };   /* [0] audio, [1] text */

static int cmp_file(const void *a, const void *b)
{
    return strcmp(((const PatchFile *)a)->key, ((const PatchFile *)b)->key);
}

/* Every file under s_dir\<rel>, recursively. */
static void scan(const WCHAR *rel)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf_s(pattern, MAX_PATH, L"%s\\%s\\*", s_dir, rel);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        WCHAR child[MAX_PATH];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(child, MAX_PATH, L"%s\\%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan(child);
        } else if (s_count < MAX_FILES) {
            PatchFile *f = &s_files[s_count];
            char *p;
            if (!WideCharToMultiByte(CP_UTF8, 0, child, -1, f->key, MAX_PATH,
                                     NULL, NULL))
                continue;
            for (p = f->key; *p; p++)
                *p = (char)tolower((unsigned char)*p);
            if (!strncmp(f->key, "dvddata\\aid\\text\\", 17))
                f->text = 1;
            else if (!strncmp(f->key, "dvddata\\audio\\", 14))
                f->text = 0;
            else {
                fprintf(stderr, "[DECENSOR] not used (neither audio nor "
                        "text): %s\n", f->key);
                continue;
            }
            wcscpy_s(f->rel, MAX_PATH, child);
            s_count++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static const PatchFile *lookup(const char *key)
{
    PatchFile k;
    char *p;
    if (strlen(key) >= MAX_PATH)
        return NULL;
    strcpy_s(k.key, MAX_PATH, key);
    for (p = k.key; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
        if (*p == '/')
            *p = '\\';
    }
    return bsearch(&k, s_files, (size_t)s_count, sizeof(PatchFile), cmp_file);
}

/* Length of a disc prefix ("\Device\CdRom0\", "D:\", "\??\D:\") at the
 * start of `path`, or 0. */
static size_t disc_prefix(const char *path)
{
    if (!_strnicmp(path, "\\Device\\CdRom0\\", 15)) return 15;
    if (!_strnicmp(path, "D:\\", 3))                return 3;
    if (!_strnicmp(path, "\\??\\D:\\", 7))          return 7;
    return 0;
}

/* The same for the cache partition (Z:, Partition3-5 behind it). */
static size_t cache_prefix(const char *path)
{
    if (!_strnicmp(path, "Z:\\", 3))                return 3;
    if (!_strnicmp(path, "\\??\\Z:\\", 7))          return 7;
    if (!_strnicmp(path, "\\Device\\Harddisk0\\Partition", 27)
        && path[27] >= '3' && path[27] <= '5' && path[28] == '\\')
        return 29;
    return 0;
}

/* Make every folder above `file`. */
static void make_parents(const WCHAR *file)
{
    WCHAR dir[MAX_PATH];
    WCHAR *p;
    wcscpy_s(dir, MAX_PATH, file);
    for (p = dir + 3; *p; p++) {
        if (*p == L'\\') {
            *p = L'\0';
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
    }
}

static int redirect(const char *xbox_path, WCHAR *host, DWORD n)
{
    char key[MAX_PATH];
    const PatchFile *f;
    size_t skip;

    if ((skip = disc_prefix(xbox_path)) != 0) {
        f = lookup(xbox_path + skip);
        if (!f || !s_on[f->text])
            return 0;
        swprintf_s(host, n, L"%s\\%s", s_dir, f->rel);
        return 1;
    }
    if ((skip = cache_prefix(xbox_path)) != 0) {
        /* Z:\Audio\x.xwb is the copy of dvddata\Audio\x.xwb. */
        sprintf_s(key, MAX_PATH, "dvddata\\%s", xbox_path + skip);
        f = lookup(key);
        if (!f || !s_on[f->text])
            return 0;
        swprintf_s(host, n, L"%s\\%s", s_cache, f->rel + 8);
        make_parents(host);
        return 1;
    }
    return 0;
}

/* The patch's copy in CacheUncensored\ for every bank the game has already
 * copied to Cache\ (see the top of the file). */
static void fill_cache(void)
{
    int i, copied = 0;
    for (i = 0; i < s_count; i++) {
        WCHAR orig[MAX_PATH], mine[MAX_PATH], src[MAX_PATH];
        const WCHAR *rest = s_files[i].rel + 8;   /* after "dvddata\" */
        swprintf_s(orig, MAX_PATH, L"%s\\%s", s_orig_cache, rest);
        swprintf_s(mine, MAX_PATH, L"%s\\%s", s_cache, rest);
        if (GetFileAttributesW(orig) == INVALID_FILE_ATTRIBUTES
            || GetFileAttributesW(mine) != INVALID_FILE_ATTRIBUTES)
            continue;
        swprintf_s(src, MAX_PATH, L"%s\\%s", s_dir, s_files[i].rel);
        make_parents(mine);
        if (CopyFileW(src, mine, TRUE))
            copied++;
        else
            fprintf(stderr, "[DECENSOR] could not copy %S to the cache "
                    "(error %lu)\n", src, GetLastError());
    }
    if (copied)
        fprintf(stderr, "[DECENSOR] %d bank(s) put in CacheUncensored\n",
                copied);
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return !(e && *e == '0');
}

void decensor_init(const char *save_dir)
{
    /* RECOMP_DECENSOR_DIR (test runs) wins over settings.ini. */
    const char *dir = getenv("RECOMP_DECENSOR_DIR");
    WCHAR save[MAX_PATH], probe[MAX_PATH];
    int audio = 0, text = 0, i;

    if (!dir || !*dir)
        dir = settings_started("decensor_dir");

    s_on[0] = env_on("RECOMP_UNCENSORED_AUDIO");
    s_on[1] = env_on("RECOMP_UNCENSORED_TEXT");

    if (dir && *dir) {
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, s_dir, MAX_PATH);
    } else {
        WCHAR *slash;
        GetModuleFileNameW(NULL, s_dir, MAX_PATH);
        slash = wcsrchr(s_dir, L'\\');
        if (slash)
            *slash = L'\0';
        wcscat_s(s_dir, MAX_PATH, L"\\decensor");
    }
    {
        size_t len = wcslen(s_dir);
        WCHAR *q;
        for (q = s_dir; *q; q++)
            if (*q == L'/')
                *q = L'\\';
        while (len > 1 && (s_dir[len - 1] == L'\\' || s_dir[len - 1] == L'/'))
            s_dir[--len] = L'\0';
    }

    swprintf_s(probe, MAX_PATH, L"%s\\dvddata", s_dir);
    if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[DECENSOR] no patch in %S (switches unavailable)\n",
                s_dir);
        return;
    }
    scan(L"dvddata");
    if (!s_count) {
        fprintf(stderr, "[DECENSOR] %S\\dvddata holds no audio or text\n",
                s_dir);
        return;
    }
    qsort(s_files, (size_t)s_count, sizeof(PatchFile), cmp_file);
    for (i = 0; i < s_count; i++)
        s_files[i].text ? text++ : audio++;

    /* The same conversion xbox_path_init uses, so Cache\ matches. */
    MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, save, MAX_PATH);
    swprintf_s(s_cache, MAX_PATH, L"%s\\CacheUncensored", save);
    swprintf_s(s_orig_cache, MAX_PATH, L"%s\\Cache", save);
    fill_cache();

    g_xbox_path_redirect = redirect;
    fprintf(stderr, "[DECENSOR] patch in %S: %d audio, %d text files; "
            "audio %s, text %s\n", s_dir, audio, text,
            s_on[0] ? "on" : "off", s_on[1] ? "on" : "off");
}

int decensor_available(int text)
{
    for (int i = 0; i < s_count; i++)
        if (s_files[i].text == (text != 0))
            return 1;
    return 0;
}

void decensor_set(int text, int on)
{
    InterlockedExchange(&s_on[text != 0], on != 0);
    fprintf(stderr, "[DECENSOR] %s %s\n", text ? "text" : "audio",
            on ? "on" : "off");
}
