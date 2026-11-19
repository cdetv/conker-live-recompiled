/*
 * The carved-asset cache (to-do 43): <save dir>\assets\, made on the player's
 * PC from their own game files and never shipped (SPEC 3.3).
 *
 *   assets\aid\...      carved files at the path the game asks for; served by
 *                       src/caff_redirect.c
 *   assets\inventory.json  every package's directory with each resource's
 *                       .data / .gpu bytes (src/caff_inventory.c); made by
 *                       the launcher's carve, which opens all 266 packages
 *   assets\lipsync.bin  the lip-sync timelines (to-do 216; caff_cache.h),
 *                       read by src/voice_pitch.c, never by the game
 *   assets\stamp.json   what the cache was made from and what it holds
 *
 * Recipes say what to carve, one per line:
 *   "<feature> loose <package> <resource>", e.g. "pan loose
 *     Level\TrainingLevel aid\bfdmodel\attach\FryingPanBound\default.rbm":
 *     that resource with all it needs (caff_write_loose) goes to
 *     assets\<resource>;
 *   "<feature> lips <package> <prefix>": every <prefix><track>\default.cut.bin
 *     of the package goes into assets\lipsync.bin (one file for all such
 *     recipes; outside aid\, so the game is never served it).
 * Built in: the lip timelines of the 43 packages that hold them (feature
 * "voice", to-do 216). Developer switch RECOMP_CARVE_RECIPES=<file> adds
 * lines (feature "dev" there or not, they always count as wanted).
 *
 * The stamp holds the cache format, a fingerprint of the game's packages
 * (path, size and date of every .rbm / .rba under dvddata\aid, from the folder
 * listing: no file is opened), a fingerprint of the recipes, "inventory": 1
 * when inventory.json was made, and every carved file with its size (the
 * inventory too; and the recipes that failed). The launcher's check also
 * wants the inventory, so a cache from before it is carved once more; the
 * game at start needs only its features' files. A carve deletes the stamp
 * first and writes it last, so a carve stopped midway always reads as stale;
 * a carved file whose size isn't the stamp's is carved again.
 *
 * Who carves: the launcher right after an import, and once when it finds the
 * cache stale (conker.exe --carve, src/caff_tool.c); the game at start only
 * when a feature that is on needs a file the cache lacks (caff_cache_start;
 * "voice" = the Mouse or Bear cheat unlocked or on, main.c). Unpacking needs
 * the game's own code, so carving runs only before the game runs.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "caff.h"
#include "caff_cache.h"

#define CACHE_FORMAT      1
#define INVENTORY_FORMAT  1                 /* the stamp's "inventory" */
#define INVENTORY_FILE    "inventory.json"  /* in assets\, listed in the stamp */
#define CAP               1024

typedef struct {
    char feature[32];
    char package[256];
    char resource[256];         /* lips: the prefix before <track>\ */
    int  dev;
    int  lips;                  /* kind "lips" (else "loose") */
} Recipe;

/* Built into the exe; see the top of the file. The packages that hold lip
 * timelines (aid\mpegspeech\<track>\default.cut.bin; 239 tracks, 37 of them
 * in two packages): every level with a speaking scene, and General\Singleplayer. */
#define LIPS(pkg) "voice lips " pkg " aid\\mpegspeech\\\n"
static const char s_builtin[] =
    "# <feature> loose <package> <resource>  |  <feature> lips <package> <prefix>\n"
    LIPS("General\\Singleplayer")
    LIPS("Level\\BankEnd") LIPS("Level\\BankEntranceAtNight") LIPS("Level\\BankEntrance")
    LIPS("Level\\BarnArena") LIPS("Level\\BarnInside") LIPS("Level\\BarnOutside")
    LIPS("Level\\BeachRunup") LIPS("Level\\BladeBoardroom") LIPS("Level\\BoilerRoom")
    LIPS("Level\\BrontoCavern") LIPS("Level\\BullPaddock") LIPS("Level\\Canyon")
    LIPS("Level\\CogLand") LIPS("Level\\DinoArena") LIPS("Level\\DogFish")
    LIPS("Level\\EndArena") LIPS("Level\\Frontend") LIPS("Level\\GenericCutscenes")
    LIPS("Level\\Harbour") LIPS("Level\\HauntedHouse") LIPS("Level\\Hell")
    LIPS("Level\\HorrorIntro") LIPS("Level\\InsideSafe") LIPS("Level\\Lab")
    LIPS("Level\\LiftTunnel") LIPS("Level\\PooBoss") LIPS("Level\\PooCabin")
    LIPS("Level\\RaptorNestFlooded") LIPS("Level\\RaptorNest") LIPS("Level\\RockSolid")
    LIPS("Level\\TankFactory") LIPS("Level\\TankTunnel") LIPS("Level\\ThroneRoom")
    LIPS("Level\\TrainingCave") LIPS("Level\\TrainingLevel") LIPS("Level\\UBend")
    LIPS("Level\\VolcanoInside") LIPS("Level\\VolcanoRace") LIPS("Level\\WarBoss")
    LIPS("Level\\WindmillAtNight") LIPS("Level\\WindmillExploded") LIPS("Level\\Windmill");
#undef LIPS

/* Where a recipe's output goes, below assets\. */
static const char *recipe_out(const Recipe *r)
{
    return r->lips ? CAFF_LIPS_FILE : r->resource;
}

/* ── Small helpers ─────────────────────────────────────────────────────── */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}

#define FNV_START 0xCBF29CE484222325ull

static int to_wide(const char *u, WCHAR *w, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, u, -1, w, cap) > 0;
}

static void to_utf8(const WCHAR *w, char *u, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, u, cap, NULL, NULL))
        u[0] = 0;
}

static int file_size_u(const char *path, uint64_t *size)
{
    WCHAR w[CAP];
    WIN32_FILE_ATTRIBUTE_DATA a;

    if (!to_wide(path, w, CAP) || !GetFileAttributesExW(w, GetFileExInfoStandard, &a)
        || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return 1;
}

static int exists_u(const char *path)
{
    uint64_t s;
    return file_size_u(path, &s);
}

static void delete_u(const char *path)
{
    WCHAR w[CAP];
    if (to_wide(path, w, CAP))
        DeleteFileW(w);
}

int caff_resolve_package(const char *game, const char *name, char *out, size_t cap)
{
    static const char *const forms[] = {
        "%s\\dvddata\\aid\\zpackage\\%s\\default.rbm",
        "%s\\dvddata\\aid\\%s\\default.rbm",
        "%s\\dvddata\\aid\\%s\\default.rba",
        "%s\\dvddata\\aid\\texture\\%s\\default.rba",
    };
    char n[CAP];
    size_t i, len = strlen(name);

    if (exists_u(name) || (len > 4 && (!_stricmp(name + len - 4, ".rbm")
                                       || !_stricmp(name + len - 4, ".rba")))) {
        snprintf(out, cap, "%s", name);
        return exists_u(out);
    }
    snprintf(n, sizeof n, "%s", name);
    for (i = 0; n[i]; i++)
        if (n[i] == '/')
            n[i] = '\\';
    for (i = 0; i < sizeof forms / sizeof *forms; i++) {
        snprintf(out, cap, forms[i], game, n);
        if (exists_u(out))
            return 1;
    }
    return 0;
}

/* ── Recipes ───────────────────────────────────────────────────────────── */

static int parse_recipes(const char *text, int dev, Recipe **list, int *n, int *cap)
{
    const char *p = text;

    while (*p) {
        char line[CAP], kind[16];
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        Recipe r;

        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = e ? e + 1 : p + strlen(p);
        if (line[0] == '#' || strspn(line, " \t\r") == strlen(line))
            continue;
        memset(&r, 0, sizeof r);
        if (sscanf(line, "%31s %15s %255s %255s", r.feature, kind, r.package, r.resource) != 4
            || (strcmp(kind, "loose") && strcmp(kind, "lips")) || _strnicmp(r.resource, "aid\\", 4)) {
            fprintf(stderr, "[CAFF] recipe not understood, skipped: %s\n", line);
            continue;
        }
        r.dev = dev;
        r.lips = !strcmp(kind, "lips");
        if (*n == *cap) {
            int c = *cap ? *cap * 2 : 16;
            Recipe *nl = realloc(*list, (size_t)c * sizeof *nl);
            if (!nl)
                return -1;
            *list = nl;
            *cap = c;
        }
        (*list)[(*n)++] = r;
    }
    return 0;
}

/* Is this recipe's feature on (the game at start)? Developer recipes always. */
static int feature_on(const Recipe *r, int (*on)(const char *))
{
    return r->dev || (on && on(r->feature));
}

static int load_recipes(Recipe **list)
{
    int n = 0, cap = 0;
    const char *dev = getenv("RECOMP_CARVE_RECIPES");

    *list = NULL;
    parse_recipes(s_builtin, 0, list, &n, &cap);
    if (dev && *dev) {
        uint8_t *text;
        size_t len;

        if (!caff_read_file(dev, &text, &len)) {
            char *z = malloc(len + 1);
            if (z) {
                memcpy(z, text, len);
                z[len] = 0;
                parse_recipes(z, 1, list, &n, &cap);
                free(z);
            }
            free(text);
        } else {
            fprintf(stderr, "[CAFF] RECOMP_CARVE_RECIPES: can't read %s\n", dev);
        }
    }
    return n;
}

static uint64_t recipes_hash(const Recipe *r, int n)
{
    uint64_t h = FNV_START;
    int i;

    for (i = 0; i < n; i++) {
        h = fnv(h, r[i].feature, strlen(r[i].feature) + 1);
        h = fnv(h, r[i].package, strlen(r[i].package) + 1);
        h = fnv(h, r[i].resource, strlen(r[i].resource) + 1);
        if (r[i].lips)
            h = fnv(h, "lips", 5);
    }
    return h;
}

/* ── The game's packages, from the folder listing ──────────────────────── */

/* The packages' fingerprint (path, size, date of each; caff_list_packages
 * opens none); 0 packages = no game files. */
static int sources(const char *game, uint64_t *hash)
{
    char root[CAP];
    CaffListed *s;
    int n, i;
    uint64_t h = FNV_START;

    snprintf(root, sizeof root, "%s\\dvddata\\aid", game);
    n = caff_list_packages(root, &s);
    for (i = 0; i < n; i++) {
        char low[CAP];
        size_t j;

        for (j = 0; s[i].rel[j] && j + 1 < sizeof low; j++)
            low[j] = (char)(s[i].rel[j] >= 'A' && s[i].rel[j] <= 'Z' ? s[i].rel[j] + 32 : s[i].rel[j]);
        low[j] = 0;
        h = fnv(h, low, j + 1);
        h = fnv(h, &s[i].size, sizeof s[i].size);
        h = fnv(h, &s[i].time, sizeof s[i].time);
    }
    caff_list_free(s, n);
    *hash = h;
    return n;
}

/* ── The stamp ─────────────────────────────────────────────────────────── */

typedef struct { char path[256]; uint64_t size; int failed; } Out;

typedef struct {
    int      format, packages, inventory;
    uint64_t sources, recipes;
    Out     *out;
    int      nout, cap;
} Stamp;

static void stamp_free(Stamp *s)
{
    free(s->out);
    memset(s, 0, sizeof *s);
}

static void stamp_add(Stamp *s, const char *path, uint64_t size, int failed)
{
    if (s->nout == s->cap) {
        int c = s->cap ? s->cap * 2 : 16;
        Out *no = realloc(s->out, (size_t)c * sizeof *no);
        if (!no)
            return;
        s->out = no;
        s->cap = c;
    }
    snprintf(s->out[s->nout].path, sizeof s->out[s->nout].path, "%s", path);
    s->out[s->nout].size = size;
    s->out[s->nout].failed = failed;
    s->nout++;
}

static const Out *stamp_find(const Stamp *s, const char *path)
{
    int i;
    for (i = 0; i < s->nout; i++)
        if (!_stricmp(s->out[i].path, path))
            return &s->out[i];
    return NULL;
}

/* A JSON string value starting at q (just after its opening quote). */
static void unescape(const char *q, char *out, size_t cap)
{
    size_t n = 0;

    while (*q && *q != '"' && n + 1 < cap) {
        if (*q == '\\' && q[1])
            q++;
        out[n++] = *q++;
    }
    out[n] = 0;
}

/* Reads only what stamp_write writes (one value or entry a line). */
static int stamp_read(const char *save, Stamp *s)
{
    char path[CAP], *text, *line, *next;
    uint8_t *data;
    size_t len;
    int failed = 0;

    memset(s, 0, sizeof *s);
    snprintf(path, sizeof path, "%s\\assets\\stamp.json", save);
    if (caff_read_file(path, &data, &len))
        return 0;
    if (!(text = malloc(len + 1))) {
        free(data);
        return 0;
    }
    memcpy(text, data, len);
    text[len] = 0;
    free(data);
    for (line = text; line && *line; line = next) {
        char *q, val[256];
        unsigned long long v;

        if ((next = strchr(line, '\n')))
            *next++ = 0;
        if (sscanf(line, " \"format\": %d", &s->format) == 1
            || sscanf(line, " \"packages\": %d", &s->packages) == 1
            || sscanf(line, " \"inventory\": %d", &s->inventory) == 1)
            continue;
        if (sscanf(line, " \"sources\": \"%llx\"", &v) == 1) {
            s->sources = v;
            continue;
        }
        if (sscanf(line, " \"recipes\": \"%llx\"", &v) == 1) {
            s->recipes = v;
            continue;
        }
        if (strstr(line, "\"failed\": ["))
            failed = 1;
        else if ((q = strstr(line, "{\"path\": \"")) != NULL) {
            char *z = strstr(line, "\"size\": ");
            unescape(q + 10, val, sizeof val);
            stamp_add(s, val, z ? strtoull(z + 8, NULL, 10) : 0, 0);
        } else if (failed && (q = strchr(line, '"')) != NULL) {
            unescape(q + 1, val, sizeof val);
            stamp_add(s, val, 0, 1);
        }
    }
    free(text);
    return s->format != 0;
}

static void json_str(char **p, size_t *left, const char *s)
{
    int k;

    k = snprintf(*p, *left, "\"");
    *p += k; *left -= (size_t)k;
    for (; *s && *left > 3; s++) {
        if (*s == '\\' || *s == '"')
            *(*p)++ = '\\', (*left)--;
        *(*p)++ = *s;
        (*left)--;
    }
    k = snprintf(*p, *left, "\"");
    *p += k; *left -= (size_t)k;
}

static int stamp_write(const char *save, const Stamp *s)
{
    char path[CAP], *buf, *p;
    size_t cap = 4096 + (size_t)s->nout * 600, left = cap;
    int i, k, nf = 0, first = 1, rc;

    if (!(buf = malloc(cap)))
        return CAFF_E_NOMEM;
    p = buf;
#define PUT(...) do { k = snprintf(p, left, __VA_ARGS__); p += k; left -= (size_t)k; } while (0)
    PUT("{\n \"format\": %d,\n \"packages\": %d,\n \"sources\": \"%016llX\",\n"
        " \"recipes\": \"%016llX\",\n \"inventory\": %d,\n \"outputs\": [\n",
        s->format, s->packages, (unsigned long long)s->sources,
        (unsigned long long)s->recipes, s->inventory);
    for (i = 0; i < s->nout; i++) {
        if (s->out[i].failed) {
            nf++;
            continue;
        }
        PUT("%s  {\"path\": ", first ? "" : ",\n");
        json_str(&p, &left, s->out[i].path);
        PUT(", \"size\": %llu}", (unsigned long long)s->out[i].size);
        first = 0;
    }
    PUT("%s ],\n \"failed\": [\n", first ? "" : "\n");
    for (i = 0, first = 1; i < s->nout; i++) {
        if (!s->out[i].failed)
            continue;
        PUT("%s  ", first ? "" : ",\n");
        json_str(&p, &left, s->out[i].path);
        first = 0;
    }
    PUT("%s ]\n}\n", first ? "" : "\n");
#undef PUT
    (void)nf;
    snprintf(path, sizeof path, "%s\\assets\\stamp.json", save);
    rc = caff_write_file(path, buf, (size_t)(p - buf));
    free(buf);
    return rc;
}

/* ── Check ─────────────────────────────────────────────────────────────── */

/* Up to date for these recipes? Every listed file must still have its size. */
static int fresh(const char *game, const char *save, const Recipe *r, int n,
                 int whole_set, int *packages)
{
    Stamp s;
    uint64_t h;
    int i, ok = 1;

    *packages = sources(game, &h);
    if (!*packages)
        return CAFF_CACHE_ERROR;
    if (!stamp_read(save, &s))
        return CAFF_CACHE_STALE;
    /* The whole set (the launcher's check) includes the inventory; the game
     * at start needs only its features' files. */
    if (s.format != CACHE_FORMAT || s.sources != h || s.packages != *packages
        || (whole_set && (s.recipes != recipes_hash(r, n) || s.inventory != INVENTORY_FORMAT
                          || !stamp_find(&s, INVENTORY_FILE))))
        ok = 0;
    for (i = 0; ok && i < n; i++)
        if (!stamp_find(&s, recipe_out(&r[i])))
            ok = 0;
    for (i = 0; ok && i < s.nout; i++) {
        char path[CAP];
        uint64_t size;

        if (s.out[i].failed)
            continue;
        snprintf(path, sizeof path, "%s\\assets\\%s", save, s.out[i].path);
        if (!file_size_u(path, &size) || size != s.out[i].size)
            ok = 0;
    }
    stamp_free(&s);
    return ok ? CAFF_CACHE_OK : CAFF_CACHE_STALE;
}

int caff_cache_files(const char *save, CaffCacheFile **out)
{
    Stamp s;
    int i, n = 0;

    *out = NULL;
    if (!stamp_read(save, &s))
        return -1;
    if (s.nout && !(*out = malloc((size_t)s.nout * sizeof **out))) {
        stamp_free(&s);
        return -1;
    }
    for (i = 0; i < s.nout; i++)
        if (!s.out[i].failed && !_strnicmp(s.out[i].path, "aid\\", 4)) {
            snprintf((*out)[n].path, sizeof (*out)[n].path, "%s", s.out[i].path + 4);
            (*out)[n++].size = s.out[i].size;
        }
    stamp_free(&s);
    return n;
}

int caff_cache_check(const char *game, const char *save)
{
    Recipe *r;
    int n = load_recipes(&r), packages, rc;

    rc = fresh(game, save, r, n, 1, &packages);
    free(r);
    return rc;
}

/* ── The lip-sync table (to-do 216) ────────────────────────────────────── */

typedef struct {
    char     name[32];          /* lower case */
    uint32_t n;                 /* entries */
    uint8_t *e;                 /* n x 12 bytes */
} LipTrack;

typedef struct {
    LipTrack *t;
    int       n, cap;
    int       differ;           /* a second copy that isn't the first's */
} Lips;

static void lips_free(Lips *L)
{
    int i;
    for (i = 0; i < L->n; i++)
        free(L->t[i].e);
    free(L->t);
    memset(L, 0, sizeof *L);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Every <prefix><track>\default.cut.bin of package k into L; how many were
 * read (new or not). A track met again is kept from its first package (a
 * [CAFF] line when the two differ). */
static int lips_take(Lips *L, const CaffPkg *k, const char *prefix, const char *pkgname)
{
    static const char tail[] = "\\default.cut.bin";
    const size_t plen = strlen(prefix), tlen = sizeof tail - 1;
    int i, nd = caff_dir_count(k), found = 0;

    for (i = 0; i < nd; i++) {
        CaffDirEntry d;
        CaffPart p;
        char track[32];
        size_t len, j;
        uint32_t cnt;
        const uint8_t *at;
        int x;

        if (caff_dir_entry(k, i, &d) || _strnicmp(d.name, prefix, plen))
            continue;
        len = strlen(d.name + plen);
        if (len <= tlen || _stricmp(d.name + plen + len - tlen, tail) || len - tlen >= sizeof track)
            continue;
        for (j = 0; j < len - tlen; j++) {
            char c = d.name[plen + j];
            track[j] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        track[j] = 0;
        if (strchr(track, '\\'))
            continue;
        if (caff_part(k, d.part, &p) || d.offset > p.size || p.size - d.offset < 4) {
            fprintf(stderr, "[CAFF] voice: %s in %s unreadable\n", track, pkgname);
            continue;
        }
        at = p.bytes + d.offset;
        cnt = rd32(at);
        if (cnt > 100000 || (uint64_t)cnt * 12u > p.size - d.offset - 4) {
            fprintf(stderr, "[CAFF] voice: %s in %s: %u entries don't fit\n", track, pkgname, cnt);
            continue;
        }
        found++;
        for (x = 0; x < L->n && strcmp(L->t[x].name, track); x++)
            ;
        if (x < L->n) {
            if (L->t[x].n != cnt || memcmp(L->t[x].e, at + 4, (size_t)cnt * 12u)) {
                L->differ++;
                fprintf(stderr, "[CAFF] voice: %s in %s differs from its first copy (kept the first)\n",
                        track, pkgname);
            }
            continue;
        }
        if (L->n == L->cap) {
            int c = L->cap ? L->cap * 2 : 128;
            LipTrack *nt = realloc(L->t, (size_t)c * sizeof *nt);
            if (!nt)
                return 0;
            L->t = nt;
            L->cap = c;
        }
        memset(&L->t[L->n], 0, sizeof L->t[L->n]);
        memcpy(L->t[L->n].name, track, strlen(track) + 1);
        L->t[L->n].n = cnt;
        if (cnt && !(L->t[L->n].e = malloc((size_t)cnt * 12u)))
            return 0;
        if (cnt)
            memcpy(L->t[L->n].e, at + 4, (size_t)cnt * 12u);
        L->n++;
    }
    return found;
}

/* The inventory pass (the launcher's carve) shows each package it opens:
 * the lip recipes naming it take their tracks there, so no package is opened
 * twice. */
typedef struct {
    Lips         *lips;
    const Recipe *r;
    int           n;
    char        (*path)[CAP];   /* each lip recipe's package resolved ("" none) */
    int          *got;          /* tracks read for each */
} LipVisit;

static void lips_visit(const CaffPkg *k, const char *path, void *vctx)
{
    LipVisit *v = vctx;
    int j;

    for (j = 0; j < v->n; j++)
        if (v->r[j].lips && v->path[j][0] && !_stricmp(v->path[j], path))
            v->got[j] += lips_take(v->lips, k, v->r[j].resource, v->r[j].package);
}

static int lip_cmp(const void *a, const void *b)
{
    return strcmp(((const LipTrack *)a)->name, ((const LipTrack *)b)->name);
}

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The table to <save>\assets\lipsync.bin; its size, 0 when not written. */
static uint64_t lips_write(const char *save, Lips *L)
{
    char path[CAP];
    size_t size = 12, at;
    uint8_t *buf;
    int i, rc;

    if (!L->n)
        return 0;
    qsort(L->t, (size_t)L->n, sizeof *L->t, lip_cmp);
    for (i = 0; i < L->n; i++)
        size += 36u + (size_t)L->t[i].n * 12u;
    if (!(buf = calloc(1, size)))
        return 0;
    memcpy(buf, "LIPS", 4);
    put32le(buf + 4, CAFF_LIPS_FORMAT);
    put32le(buf + 8, (uint32_t)L->n);
    for (i = 0, at = 12; i < L->n; i++) {
        memcpy(buf + at, L->t[i].name, sizeof L->t[i].name);
        put32le(buf + at + 32, L->t[i].n);
        if (L->t[i].n)
            memcpy(buf + at + 36, L->t[i].e, (size_t)L->t[i].n * 12u);
        at += 36u + (size_t)L->t[i].n * 12u;
    }
    snprintf(path, sizeof path, "%s\\assets\\%s", save, CAFF_LIPS_FILE);
    rc = caff_make_parent_dirs(path) || caff_write_file(path, buf, size);
    free(buf);
    if (rc) {
        fprintf(stderr, "[CAFF] can't write %s\n", path);
        return 0;
    }
    return size;
}

unsigned char *caff_cache_lips(const char *save, size_t *n)
{
    Stamp s;
    const Out *o;
    char path[CAP];
    uint8_t *data = NULL;
    size_t len = 0;

    *n = 0;
    if (!stamp_read(save, &s))
        return NULL;
    o = stamp_find(&s, CAFF_LIPS_FILE);
    snprintf(path, sizeof path, "%s\\assets\\%s", save, CAFF_LIPS_FILE);
    if (o && !o->failed && !caff_read_file(path, &data, &len) && len != o->size) {
        free(data);
        data = NULL;
    }
    stamp_free(&s);
    if (data && (len < 12 || memcmp(data, "LIPS", 4) || rd32(data + 4) != CAFF_LIPS_FORMAT)) {
        free(data);
        data = NULL;
    }
    if (data)
        *n = len;
    return data;
}

/* ── Carve ─────────────────────────────────────────────────────────────── */

/* Every file under assets\aid that the new stamp doesn't list goes (an old
 * recipe's output, a test's leftover, a .tmp of a stopped carve), so the
 * redirect never serves anything the stamp doesn't vouch for. */
static void sweep(const char *save, const WCHAR *root, const WCHAR *rel, const Stamp *s, int depth)
{
    WCHAR pat[CAP], sub[CAP], full[CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > 16)
        return;
    swprintf_s(pat, CAP, rel[0] ? L"%s\\%s\\*" : L"%s\\*", root, rel);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        char u[CAP];

        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(sub, CAP, rel[0] ? L"%s\\%s" : L"%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;               /* a link (tests): never walked into */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            sweep(save, root, sub, s, depth + 1);
            continue;
        }
        to_utf8(sub, u, sizeof u);
        {
            char key[CAP];
            const Out *o;

            snprintf(key, sizeof key, "aid\\%s", u);
            o = stamp_find(s, key);
            if (!o || o->failed) {
                swprintf_s(full, CAP, L"%s\\%s", root, sub);
                DeleteFileW(full);
                fprintf(stderr, "[CAFF] removed %s (not in the new stamp)\n", key);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct {
    CaffCarveProgress progress;
    void *ctx;
    int base, total;
} InvProgress;

/* The inventory's packages counted after the recipes' on one bar. */
static void inv_progress(int done, int total, const char *what, void *ctx)
{
    InvProgress *p = ctx;
    int at = p->base + done;

    (void)total;
    p->progress(at < p->total ? at : p->total, p->total, what, p->ctx);
}

/* The last carve's inventory, when it was made from these same packages and
 * is still whole: its size, else 0. */
static uint64_t inventory_kept(const char *save, uint64_t sources_hash, int packages)
{
    Stamp old;
    const Out *o;
    char path[CAP];
    uint64_t size = 0, got;

    if (!stamp_read(save, &old))
        return 0;
    snprintf(path, sizeof path, "%s\\assets\\%s", save, INVENTORY_FILE);
    o = stamp_find(&old, INVENTORY_FILE);
    if (old.inventory == INVENTORY_FORMAT && old.sources == sources_hash && old.packages == packages
        && o && !o->failed && file_size_u(path, &got) && got == o->size)
        size = got;
    stamp_free(&old);
    return size;
}

int caff_cache_carve(const char *game, const char *save, int inventory,
                     CaffCarveProgress progress, void *ctx)
{
    Recipe *r;
    Stamp s;
    char path[CAP], pkg[CAP], inv[CAP];
    WCHAR wroot[CAP];
    int n = load_recipes(&r), i, j, done = 0, total = 0, bad = 0, rc, inv_failed = 0;
    int nlips = 0, lips_bad = 0;
    uint8_t *seen = n ? calloc((size_t)n, 1) : NULL;
    uint64_t inv_keep = 0;
    DWORD t0 = GetTickCount();
    Lips lips;

    memset(&lips, 0, sizeof lips);

    memset(&s, 0, sizeof s);
    s.format = CACHE_FORMAT;
    s.recipes = recipes_hash(r, n);
    if (!(s.packages = sources(game, &s.sources))) {
        fprintf(stderr, "[CAFF] no packages under %s\\dvddata\\aid\n", game);
        free(r);
        free(seen);
        return CAFF_CACHE_ERROR;
    }
    snprintf(path, sizeof path, "%s\\assets\\stamp.json", save);
    snprintf(inv, sizeof inv, "%s\\assets\\%s", save, INVENTORY_FILE);
    /* A folder that can't be written fails here at once, not after the
     * inventory's ~10 s. The launcher shows the last [CAFF] line. */
    {
        char probe[CAP];

        snprintf(probe, sizeof probe, "%s\\assets\\write_test", save);
        if (caff_make_parent_dirs(path) || caff_write_file(probe, "", 0)) {
            fprintf(stderr, "[CAFF] can't write in %s\\assets (no write rights there, or the disk is full)\n",
                    save);
            free(r);
            free(seen);
            return CAFF_CACHE_ERROR;
        }
        delete_u(probe);
    }
    /* A carve without the inventory (the game's own at start) keeps one the
     * last carve made from the same packages. */
    if (!inventory && !(inv_keep = inventory_kept(save, s.sources, s.packages)))
        delete_u(inv);
    delete_u(path);

    /* Lip recipes wait for the inventory pass when there is one: it opens
     * every package anyway (a pass of their own cost 7 s more). */
#define DEFERRED(x) (inventory && r[x].lips)
    /* Packages in the order the recipes first name them. */
    for (i = 0; i < n; i++)
        if (!seen[i] && !DEFERRED(i)) {
            total++;
            for (j = i; j < n; j++)
                if (!_stricmp(r[j].package, r[i].package))
                    seen[j] = 1;
        }
    if (seen)
        memset(seen, 0, (size_t)n);
    if (progress)
        progress(0, total + (inventory ? s.packages : 0), "", ctx);
    for (i = 0; i < n; i++) {
        CaffPkg *k = NULL;

        if (seen[i] || DEFERRED(i))
            continue;
        if (!caff_resolve_package(game, r[i].package, pkg, sizeof pkg)) {
            fprintf(stderr, "[CAFF] recipe package %s not found\n", r[i].package);
            rc = CAFF_E_OPEN;
        } else {
            rc = caff_open(pkg, caff_guest_unpack, &k);
        }
        for (j = i; j < n; j++) {
            uint8_t *buf;
            size_t len = 0;
            int d = -1, ok;
            Out *prev;

            if (seen[j] || DEFERRED(j) || _stricmp(r[j].package, r[i].package))
                continue;
            seen[j] = 1;
            if (r[j].lips) {            /* into the one table, written below */
                nlips++;
                if (rc || !lips_take(&lips, k, r[j].resource, r[j].package)) {
                    fprintf(stderr, "[CAFF] %s: no lip timelines from %s\n", r[j].feature, r[j].package);
                    lips_bad++;
                }
                continue;
            }
            /* Two recipes, one file: the first that works (a damaged package
             * leaves it to another one holding the same resource). */
            prev = (Out *)stamp_find(&s, r[j].resource);
            if (prev && !prev->failed)
                continue;
            snprintf(path, sizeof path, "%s\\assets\\%s", save, r[j].resource);
            if (rc || (d = caff_find(k, r[j].resource)) < 0
                || caff_write_loose(k, d, &buf, &len)) {
                fprintf(stderr, "[CAFF] %s: %s from %s failed\n", r[j].feature,
                        r[j].resource, r[j].package);
                ok = 0;
            } else {
                ok = !caff_make_parent_dirs(path) && !caff_write_file(path, buf, len);
                if (!ok)
                    fprintf(stderr, "[CAFF] can't write %s\n", path);
                free(buf);
            }
            if (prev && ok) {
                fprintf(stderr, "[CAFF] %s: %s taken from %s instead\n", r[j].feature,
                        r[j].resource, r[j].package);
                prev->size = len;
                prev->failed = 0;
                bad--;
            } else if (!prev) {
                stamp_add(&s, r[j].resource, ok ? len : 0, !ok);
                bad += !ok;
            }
        }
        caff_close(k);
        if (progress)
            progress(++done, total + (inventory ? s.packages : 0), r[i].package, ctx);
    }

    /* Every package's directory, after the recipes (it opens all of them),
     * the deferred lip recipes taking their tracks on the way. */
    if (inventory) {
        InvProgress ip = { progress, ctx, done, total + s.packages };
        CaffInventoryResult ir;
        char aid[CAP];
        DWORD t1 = GetTickCount();
        LipVisit lv;

        lv.lips = &lips;
        lv.r = r;
        lv.n = n;
        lv.path = n ? calloc((size_t)n, sizeof *lv.path) : NULL;
        lv.got = n ? calloc((size_t)n, sizeof *lv.got) : NULL;
        if (n && (!lv.path || !lv.got))
            lv.n = 0;
        for (j = 0; j < lv.n; j++)
            if (r[j].lips && !caff_resolve_package(game, r[j].package, lv.path[j], CAP))
                lv.path[j][0] = 0;
        snprintf(aid, sizeof aid, "%s\\dvddata\\aid", game);
        if (caff_inventory_write_visit(aid, caff_guest_unpack, inv, progress ? inv_progress : NULL,
                                       &ip, lv.n ? lips_visit : NULL, &lv, &ir) == CAFF_OK) {
            s.inventory = INVENTORY_FORMAT;
            stamp_add(&s, INVENTORY_FILE, ir.bytes, 0);
            fprintf(stderr, "[CAFF] inventory: %d packages, %d resources, %d error(s), %zu bytes, "
                    "in %lu ms\n", ir.packages, ir.resources, ir.errors, ir.bytes,
                    GetTickCount() - t1);
        } else {
            inv_failed = 1;
        }
        for (j = 0; j < n; j++)
            if (r[j].lips) {
                nlips++;
                if (j >= lv.n || !lv.got[j]) {
                    fprintf(stderr, "[CAFF] %s: no lip timelines from %s\n", r[j].feature, r[j].package);
                    lips_bad++;
                }
            }
        free(lv.path);
        free(lv.got);
    } else if (inv_keep) {
        s.inventory = INVENTORY_FORMAT;
        stamp_add(&s, INVENTORY_FILE, inv_keep, 0);
    }
#undef DEFERRED

    /* The lip recipes' one table: written when any track was read (a package
     * that failed leaves its tracks out, named above). */
    if (nlips) {
        uint64_t size = lips_write(save, &lips);

        stamp_add(&s, CAFF_LIPS_FILE, size, !size);
        bad += !size;
        fprintf(stderr, "[CAFF] voice: %d lip timelines from %d package(s) (%d failed, %d second copies "
                "differ), %llu bytes\n", lips.n, nlips, lips_bad, lips.differ, (unsigned long long)size);
    }
    lips_free(&lips);

    if (to_wide(save, wroot, CAP)) {
        WCHAR aid[CAP];
        DWORD a;

        /* assets\aid itself a link (a test pointing it at a folder of
         * unpacked packages): that folder is never swept. */
        swprintf_s(aid, CAP, L"%s\\assets\\aid", wroot);
        a = GetFileAttributesW(aid);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
            sweep(save, aid, L"", &s, 0);
    }
    /* Written even when the inventory failed: the carved files are good, and
     * the missing inventory leaves the stamp stale for the launcher. */
    rc = stamp_write(save, &s);
    fprintf(stderr, "[CAFF] carved %d file(s) from %d package(s), %d failed, in %lu ms%s\n",
            s.nout - bad - (s.inventory ? 1 : 0), total, bad, GetTickCount() - t0,
            rc ? "; the stamp can't be written" : "");
    if (rc || inv_failed)               /* the launcher shows the last line */
        fprintf(stderr, "[CAFF] can't write %s in %s\\assets (no write rights there, or the disk is full)\n",
                rc ? "stamp.json" : INVENTORY_FILE, save);
    stamp_free(&s);
    free(r);
    free(seen);
    return rc || inv_failed ? CAFF_CACHE_ERROR : CAFF_CACHE_OK;
}

/* ── At game start ─────────────────────────────────────────────────────── */

void caff_cache_start(const char *game, const char *save, int (*on)(const char *))
{
    Recipe *r;
    int n = load_recipes(&r), i, want = 0, packages;

    for (i = 0; i < n; i++)
        want += feature_on(&r[i], on);
    if (!want) {                    /* nothing on needs the cache: no wait */
        free(r);
        return;
    }
    /* Only the recipes of features that are on must be there. */
    for (i = 0; i < n; i++)
        if (!feature_on(&r[i], on))
            r[i--] = r[--n];
    if (fresh(game, save, r, n, 0, &packages) == CAFF_CACHE_OK) {
        free(r);
        return;
    }
    free(r);
    fprintf(stderr, "[CAFF] the asset cache lacks what a feature needs: carving now\n");
    if (caff_guest_begin() == CAFF_OK) {
        caff_cache_carve(game, save, 0, NULL, NULL);
        caff_guest_end();
    }
}
