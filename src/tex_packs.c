/*
 * Texture packs (to-do 288, SPEC 9.3 "Texture packs"): folders in
 * <textures>\packs\<name>\ (the textures folder next to the .exe,
 * RECOMP_TEX_REPLACE), each read with all its subfolders by the toolkit's
 * tex_replace.c, the top pack first: it wins on a texture two packs change.
 * The PNGs right in <textures> (our title logo) stay on underneath.
 *
 * A PNG in a pack goes by the texture pack kit's name: its path inside the
 * pack as the export lays it out (zpackage\Level\BarnInside\
 * BarnInside_texturefile34.png), which the name table
 * <save>\assets\texnames.tsv (texkit.h; made by the launcher's game-assets
 * step from the player's own files) turns into the texture's hash; or, as in
 * <textures> itself, by a hash at the start of its name.
 *
 * Packs are named, never given as paths: a name with a folder separator,
 * "." / "..", or a character Windows refuses in a folder name is left out,
 * so nothing outside packs\ is read (above all textures\dump, the kit's
 * export: read as a pack it would load all 9,624 of its PNGs).
 *
 * Which packs: RECOMP_TEX_PACKS at start (a developer switch, to-do 288 part
 * 2a), then tex_packs_set while the game runs (the console's texpacks, part
 * 2b): the toolkit lists the folders again between two frames and draws
 * every texture again. The setting, the F2 list and the launcher's come
 * next.
 *
 * Under every pack: the sharper text and buttons (to-do 53, sharp_gui.c),
 * <save>\assets\sharp\, made by the carve, when the stamp says they were made
 * and F2 Display "Sharper Text and Buttons" is on (sharp_gui,
 * RECOMP_SHARP_GUI). A pack the player turns on still wins over them.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "caff_cache.h"
#include "settings.h"
#include "sharp_gui.h"
#include "tex_packs.h"

/* The toolkit's tex_replace.h (src/kernel, not on our include path). */
typedef uint64_t (*TexReplaceNamer)(const char *path_in_pack, void *ctx);
extern void tex_replace_set_namer(TexReplaceNamer namer, void *ctx);
extern int  tex_replace_set_packs(const char *const *dirs, int n);

#define NAME_SLOTS (1u << 16)           /* over twice the disc's 20,073 pictures */
#define MAX_PACKS  64                   /* the toolkit's limit too */
#define MAX_CANT   5                    /* "can't be replaced" lines said */

typedef struct {
    const char *path;                   /* lower case, '\' separators; into s_text */
    uint64_t    hash;                   /* 0: the kit says nothing can replace it */
} NameSlot;

static char     *s_text;                /* the table's text, kept for the slots */
static NameSlot *s_slot;
static int       s_names, s_cant;
static int       s_names_tried;         /* the table read (or tried) once */

static char      s_root[MAX_PATH * 3];  /* the textures folder, '\' separators; "" = off */
static char      s_save[MAX_PATH * 3];  /* the save folder (the name table's) */
static char      s_on[4096];            /* the packs on now, "A|B" */
static char      s_sharp_dir[MAX_PATH * 3]; /* <save>\assets\sharp when made; "" = none */
static int       s_sharp = -1;          /* F2 "Sharper Text and Buttons"; -1 = not read yet */

int sharp_gui_on(void)
{
    if (s_sharp < 0) {
        const char *e = getenv("RECOMP_SHARP_GUI");
        s_sharp = !(e && (!strcmp(e, "0") || !_stricmp(e, "off")));
    }
    return s_sharp;
}

void conker_SetSharpGui(int on)
{
    s_sharp = on != 0;
    fprintf(stderr, "[TEXPACK] sharper text and buttons %s\n", s_sharp ? "on" : "off");
    if (s_root[0])
        tex_packs_set(s_on);
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* Windows file names don't mind case; '/' as '\'. (Every name on the disc
 * is ASCII.) */
static void fold(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s + 32);
        else if (*s == '/')
            *s = '\\';
    }
}

static int hex16(const char *s, uint64_t *out)
{
    uint64_t h = 0;
    int i;

    for (i = 0; i < 16; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return 0;
        h = h << 4 | (uint64_t)v;
    }
    *out = h;
    return !s[16];
}

static void insert(const char *path, uint64_t hash)
{
    uint32_t i = fnv(path) & (NAME_SLOTS - 1);

    if (s_names >= (int)(NAME_SLOTS / 2))
        return;
    for (; s_slot[i].path; i = (i + 1) & (NAME_SLOTS - 1))
        if (!strcmp(s_slot[i].path, path))
            return;                     /* the first line wins (none twice on the disc) */
    s_slot[i].path = path;
    s_slot[i].hash = hash;
    s_names++;
}

/* The table's lines "<hash or ->\t<path>" into the slots, in place. */
static int load_names(const char *save)
{
    size_t n;
    char *t = caff_cache_names(save, &n), *line, *next;

    if (!t)
        return 0;
    if (!(s_slot = calloc(NAME_SLOTS, sizeof *s_slot))) {
        free(t);
        return 0;
    }
    line = strchr(t, '\n');             /* past "texnames\t1" */
    for (line = line ? line + 1 : t + n; *line; line = next) {
        char *end = strchr(line, '\n'), *tab;
        uint64_t h = 0;

        next = end ? end + 1 : line + strlen(line);
        if (end) {
            *end = 0;
            if (end > line && end[-1] == '\r')
                end[-1] = 0;
        }
        if (!(tab = strchr(line, '\t')))
            continue;
        *tab = 0;
        if (strcmp(line, "-") && !hex16(line, &h))
            continue;
        fold(tab + 1);
        insert(tab + 1, h);
    }
    s_text = t;
    return 1;
}

/* The toolkit's namer: a PNG's path inside a pack -> the hash it replaces. */
static uint64_t namer(const char *path_in_pack, void *ctx)
{
    char k[1024];
    uint32_t i;

    (void)ctx;
    if (!s_slot || strlen(path_in_pack) >= sizeof k)
        return 0;
    strcpy(k, path_in_pack);
    fold(k);
    for (i = fnv(k) & (NAME_SLOTS - 1); s_slot[i].path; i = (i + 1) & (NAME_SLOTS - 1))
        if (!strcmp(s_slot[i].path, k)) {
            if (!s_slot[i].hash && s_cant++ < MAX_CANT)
                fprintf(stderr, "[TEXPACK] %s: the game can't replace that texture (a cube map, a "
                        "volume texture or a frame it reads elsewhere: the replace column of "
                        "textures.tsv)\n", path_in_pack);
            return s_slot[i].hash;
        }
    return 0;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

/* A folder right in packs\: no separator, no "." / "..", nothing Windows
 * refuses in a folder name (which also can't end in a dot). */
static int good_name(const char *s)
{
    size_t n = strlen(s);

    if (!n || n > 120 || s[n - 1] == '.')
        return 0;
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || strchr("\\/:*?\"<>|", *s))
            return 0;
    return 1;
}

void tex_packs_start(const char *save_dir)
{
    const char *v = getenv("RECOMP_TEX_PACKS"), *base = getenv("RECOMP_TEX_REPLACE");
    char *p;
    size_t r;

    if (save_dir && strlen(save_dir) < sizeof s_save)
        strcpy(s_save, save_dir);
    if (!base || !*base || strlen(base) >= sizeof s_root) {
        if (v && *v)
            fprintf(stderr, "[TEXPACK] the textures folder is off (RECOMP_TEX_REPLACE) or its "
                    "name too long: no packs\n");
        return;
    }
    strcpy(s_root, base);
    for (p = s_root; *p; p++)           /* as the toolkit writes it in its lines */
        if (*p == '/')
            *p = '\\';
    r = strlen(s_root);
    while (r > 1 && s_root[r - 1] == '\\')
        s_root[--r] = 0;
    /* The sharper text and buttons, when the carve made them (to-do 53). */
    if (s_save[0] && caff_cache_sharp(s_save)
        && snprintf(s_sharp_dir, sizeof s_sharp_dir, "%s\\assets\\" SHARP_DIR, s_save)
           >= (int)sizeof s_sharp_dir)
        s_sharp_dir[0] = 0;
    if (!s_sharp_dir[0])
        fprintf(stderr, "[TEXPACK] no sharper text and buttons in %s\\assets (the launcher's "
                "\"Preparing game assets\" makes them)\n", s_save);
    if (v && *v) {
        if (settings_pack_count())
            fprintf(stderr, "[TEXPACK] RECOMP_TEX_PACKS is set: settings.ini's [texture_packs] "
                    "not used at start\n");
        tex_packs_set(v);
        return;
    }
    {
        /* settings.ini's on packs, in its order (the menu writes them). */
        char list[sizeof s_on];
        int i, n = settings_pack_count();
        size_t at = 0;

        list[0] = 0;
        for (i = 0; i < n; i++) {
            const char *name = settings_pack_name(i);
            size_t len = strlen(name);
            if (!settings_pack_on(i))
                continue;
            if (at + len + 2 > sizeof list)
                break;                  /* 64 names of 127 bytes fit; never reached */
            if (at)
                list[at++] = '|';
            memcpy(list + at, name, len + 1);
            at += len;
        }
        if (at || (s_sharp_dir[0] && sharp_gui_on()))
            tex_packs_set(list);
    }
}

int tex_packs_set(const char *v)
{
    char list[sizeof s_on], on[sizeof s_on], *p, *dirs[MAX_PACKS], *names[MAX_PACKS];
    int n = 0, i, j, ndirs, ok;

    if (!s_root[0]) {
        fprintf(stderr, "[TEXPACK] the textures folder is off (RECOMP_TEX_REPLACE): no packs\n");
        return -1;
    }
    if (strlen(v) >= sizeof list) {
        fprintf(stderr, "[TEXPACK] the pack list is too long (%u bytes): the packs stay as "
                "they were\n", (unsigned)strlen(v));
        return -1;
    }
    strcpy(list, v);
    on[0] = 0;
    for (p = list; p && n < MAX_PACKS - 1;) {   /* one place left for the sharper set */
        char *bar = strchr(p, '|'), *name = p;

        if (bar)
            *bar = 0;
        p = bar ? bar + 1 : NULL;
        name = trim(name);
        if (!*name)
            continue;
        if (!good_name(name)) {
            fprintf(stderr, "[TEXPACK] \"%s\" left out: a pack is named by its folder in "
                    "%s\\packs, nothing else\n", name, s_root);
            continue;
        }
        for (j = 0; j < n && _stricmp(names[j], name); j++)
            ;
        if (j < n)
            continue;                   /* named twice: the higher place counts */
        if (!(dirs[n] = malloc(strlen(s_root) + strlen(name) + 8))) {
            for (i = 0; i < n; i++)
                free(dirs[i]);
            fprintf(stderr, "[TEXPACK] out of memory: the packs stay as they were\n");
            return -1;
        }
        sprintf(dirs[n], "%s\\packs\\%s", s_root, name);
        names[n++] = name;
        if (on[0])
            strcat(on, "|");
        strcat(on, name);               /* never longer than the list it came from */
    }
    /* The table once, the first time a pack is on (none at start: nothing
     * read). The namer is in place before the toolkit lists the folders. */
    if (n && !s_names_tried) {
        s_names_tried = 1;
        if (load_names(s_save)) {
            tex_replace_set_namer(namer, NULL);
            fprintf(stderr, "[TEXPACK] texture name table: %d names\n", s_names);
        } else {
            fprintf(stderr, "[TEXPACK] no texture name table in %s\\assets (made with the game "
                    "assets the launcher prepares): only PNGs named by hash work in packs\n",
                    s_save);
        }
    }
    /* The sharper text and buttons under every pack (to-do 53). */
    ndirs = n;
    if (s_sharp_dir[0] && sharp_gui_on() && (dirs[ndirs] = _strdup(s_sharp_dir)) != NULL)
        ndirs++;
    ok = tex_replace_set_packs((const char *const *)dirs, ndirs);
    if (!ok) {
        fprintf(stderr, "[TEXPACK] out of memory: the packs stay as they were\n");
    } else {
        strcpy(s_on, on);
        if (!n)
            fprintf(stderr, "[TEXPACK] no packs on\n");
        for (i = 0; i < n; i++)
            fprintf(stderr, "[TEXPACK] %d. %s\n", i + 1, dirs[i]);
        fprintf(stderr, "[TEXPACK] sharper text and buttons: %s\n",
                ndirs > n ? dirs[n] : s_sharp_dir[0] ? "off" : "not made");
    }
    for (i = 0; i < ndirs; i++)
        free(dirs[i]);
    return ok ? n : -1;
}

const char *tex_packs_on(void)
{
    return s_on;
}

static int cmp_name(const void *a, const void *b)
{
    return _stricmp((const char *)a, (const char *)b);
}

int tex_packs_found(char (*names)[TEX_PACK_NAME], int max)
{
    char pat8[MAX_PATH * 3];
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0;

    if (!s_root[0])
        return -1;
    if (snprintf(pat8, sizeof pat8, "%s\\packs\\*", s_root) >= (int)sizeof pat8
        || !MultiByteToWideChar(CP_UTF8, 0, pat8, -1, pat, MAX_PATH))
        return 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;                       /* no packs folder yet */
    do {
        char u[TEX_PACK_NAME];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || !WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u, (int)sizeof u, NULL, NULL)
            || !good_name(u))           /* also "." and "..": they end in a dot */
            continue;
        if (n < max)
            strcpy(names[n++], u);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    qsort(names, (size_t)n, TEX_PACK_NAME, cmp_name);
    return n;
}

int tex_packs_list(TexPack *list, int max)
{
    static char found[MAX_PACKS][TEX_PACK_NAME];
    char used[MAX_PACKS] = { 0 };
    int nf = tex_packs_found(found, MAX_PACKS), n = 0, i, j;

    if (nf < 0)
        return -1;
    /* settings.ini's lines first, for the folders that are there. */
    for (i = 0; i < settings_pack_count() && n < max; i++) {
        for (j = 0; j < nf && _stricmp(found[j], settings_pack_name(i)); j++)
            ;
        if (j == nf || used[j])
            continue;
        used[j] = 1;
        strcpy(list[n].name, found[j]); /* the folder's own spelling */
        list[n++].on = settings_pack_on(i);
    }
    /* Then the new folders, off, by name (found is sorted). */
    for (j = 0; j < nf && n < max; j++)
        if (!used[j]) {
            strcpy(list[n].name, found[j]);
            list[n++].on = 0;
        }
    return n;
}

int tex_packs_save(const TexPack *list, int n)
{
    const char *names[MAX_PACKS];
    int on[MAX_PACKS], i, ok;
    char want[sizeof s_on];
    size_t at = 0;

    if (n > MAX_PACKS)
        n = MAX_PACKS;
    want[0] = 0;
    for (i = 0; i < n; i++) {
        size_t len = strlen(list[i].name);
        names[i] = list[i].name;
        on[i] = list[i].on;
        if (!on[i] || at + len + 2 > sizeof want)
            continue;
        if (at)
            want[at++] = '|';
        memcpy(want + at, list[i].name, len + 1);
        at += len;
    }
    ok = settings_packs_set(n, names, on);
    if (!ok)
        fprintf(stderr, "[TEXPACK] settings.ini could not be written: the packs are not saved\n");
    if (strcmp(want, s_on))             /* an off pack moved changes nothing drawn */
        ok = tex_packs_set(want) >= 0 && ok;
    return ok;
}
