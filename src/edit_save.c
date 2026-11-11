/**
 * Edit Save (to-do 147, SPEC 10.3): see edit_save.h.
 *
 * conker.bin (5632 bytes; conker-notes "Saves"): +0 total size, +4 data size,
 * +0xC HMAC-SHA1 of bytes 0-11, at +0x20 the data: a count (+0x24) and that
 * many sorted entries {u32 key, u32 type, u32 value}, the key an ELF hash of
 * a name (0x00353490 writes them, 0x00353860 reads them); the last 20 bytes
 * of the total are an HMAC-SHA1 of everything before them. Both signatures
 * use the XboxSignatureKey, all zeros in our runtime. Names: Level,
 * StartPoint, Money, Lives, StartingEnergy, Time, TaskFlags0-25, one per
 * progress bit (names at 0x0074D00C), and 170 others left as they are.
 *
 * What the Chapters list does for a part (0x001FB760): the progress wiped
 * (0x0035DF40, the task flags too), the part's checkpoint's prerequisites
 * done, each with its own first (0x0035DD30), then everything a done task
 * also completes, until nothing changes (0x0035DDD0); level and spawn from
 * the checkpoint's row in the task table (0x0059E9A0, 20 bytes: +8 level,
 * +0xA spawn, +0xC prerequisites, +0x10 also done, u16 lists ending 0xFFFF),
 * the part's cash as the money. Lives and play time are not touched. His
 * energy is the full 6 (3 while the hangover isn't cured, as the game's own
 * first saves have it). Checked against the Chapters list's own start
 * (Sloprano 2, conker-notes 2026-11-09).
 */
#include <windows.h>
#include <bcrypt.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit_save.h"

#define TASKS       0x0059E9A0u      /* 20 bytes a checkpoint / task */
#define BIT_NAMES   0x0074D00Cu      /* 8 bytes a bit: its name */
#define PART_LISTS  0x0074CD44u      /* a chapter's {u16 cash, u16 checkpoint} */
#define PART_COUNTS 0x0074CD68u      /* parts per chapter */
#define CURED_BIT   1                /* TrainingCureHangover */
#define SAVE_SIZE   0x1600u

static const char *const s_words[ES_CHAPTERS] = {
    "hung", "windy", "bat", "barn", "slop", "uga", "spooky", "war", "heist",
};
static const char *const s_chapters[ES_CHAPTERS] = {
    "Hungover", "Windy", "Bat's Tower", "Barn Boys", "Sloprano", "Uga Buga",
    "Spooky", "It's War", "Heist",
};
/* The user's list (SPEC 10.3); the counts are the game's. */
static const char *const s_parts[ES_CHAPTERS][13] = {
    { "Scaredy Birdy", "Panhandled", "Gargoyle" },
    { "Mrs. Bee", "Poo Cabin", "Pruned", "Yeehaa!", "Sewage Sucks",
      "Great Balls of Poo", "Wasp's Revenge", "Mr. Barrel" },
    { "Mrs. Catfish", "Barry's Mates", "Cog's Revenge", "The Combination",
      "Blast Doors", "Clang's Lair", "Pisstastic", "Brass Monkeys",
      "Bullfish's Revenge" },
    { "Marvin", "Mad Pitchfork", "Sunny Days", "Barry + Co.", "Buff You",
      "Haybot Wars", "Frying Tonight", "Slam Dunk" },
    { "Corn off the Cob", "Sweet Melody", "U-Bend Blues", "The Bluff" },
    { "Drunken Nights", "Sacrifice", "Phlegm", "Worship", "Rock Solid",
      "Bomb Run", "Mugged", "Raptor Food", "Buga the Knut" },
    { "Mr. Death", "Count Batula", "Zombies", "Mr. Barrel" },
    { "It's War", "Power's Off", "TNT", "The Assault", "Sole Survivor",
      "Casualty Dept.", "Saving Private Rodent", "Chemical Warfare",
      "The Tower", "Little Girl", "The Experiment", "Countdown",
      "Peace at Last" },
    { "The Windmill's Dead", "Enter The Vertex", "The Vault", "End Cutscene" },
};
static const int s_counts[ES_CHAPTERS] = { 3, 8, 9, 8, 4, 9, 4, 13, 4 };

const char *edit_save_word(int chapter)
{
    return chapter >= 0 && chapter < ES_CHAPTERS ? s_words[chapter] : NULL;
}

const char *edit_save_chapter_name(int chapter)
{
    return chapter >= 0 && chapter < ES_CHAPTERS ? s_chapters[chapter] : NULL;
}

int edit_save_parts(int chapter)
{
    return chapter >= 0 && chapter < ES_CHAPTERS ? s_counts[chapter] : 0;
}

const char *edit_save_part_name(int chapter, int part)
{
    if (part < 1 || part > edit_save_parts(chapter))
        return NULL;
    return s_parts[chapter][part - 1];
}

int edit_save_parse(const char *text, int *chapter, int *part)
{
    char w[16];
    size_t n = 0;
    int num = 0;

    while (text[n] && isalpha((unsigned char)text[n]) && n < sizeof(w) - 1) {
        w[n] = (char)tolower((unsigned char)text[n]);
        n++;
    }
    w[n] = '\0';
    if (!isdigit((unsigned char)text[n]))
        return 0;
    for (const char *p = text + n; *p; p++) {
        if (!isdigit((unsigned char)*p) || num > 99)
            return 0;
        num = num * 10 + (*p - '0');
    }
    for (int c = 0; c < ES_CHAPTERS; c++)
        if (!strcmp(w, s_words[c]) && num >= 1 && num <= s_counts[c]) {
            *chapter = c;
            *part = num;
            return 1;
        }
    return 0;
}

/* ---- the game's data ------------------------------------------------------- */

int edit_save_xbe_read(uint32_t va, void *out, size_t n, void *ctx)
{
    const EsXbe *x = (const EsXbe *)ctx;
    uint32_t base, count, hdrs;

    if (!x || x->size < 0x124 || memcmp(x->data, "XBEH", 4))
        return 0;
    base = *(const uint32_t *)(x->data + 0x104);
    count = *(const uint32_t *)(x->data + 0x11C);
    hdrs = *(const uint32_t *)(x->data + 0x120) - base;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *s = x->data + hdrs + i * 0x38u;
        uint32_t sva, vsize, raw, rsize;
        if (hdrs + (i + 1) * 0x38u > x->size)
            return 0;
        sva = *(const uint32_t *)(s + 4);
        vsize = *(const uint32_t *)(s + 8);
        raw = *(const uint32_t *)(s + 0xC);
        rsize = *(const uint32_t *)(s + 0x10);
        if (va >= sva && va + n <= sva + vsize) {
            uint32_t off = va - sva;
            memset(out, 0, n);
            if (off < rsize) {
                size_t take = rsize - off < n ? rsize - off : n;
                if ((size_t)raw + off + take > x->size)
                    return 0;
                memcpy(out, x->data + raw + off, take);
            }
            return 1;
        }
    }
    return 0;
}

typedef struct {
    EsRead rd;
    void *ctx;
    int ok;
} Reader;

static uint32_t r32(Reader *r, uint32_t va)
{
    uint32_t v = 0;
    if (!r->rd(va, &v, 4, r->ctx))
        r->ok = 0;
    return v;
}

static uint16_t r16(Reader *r, uint32_t va)
{
    uint16_t v = 0;
    if (!r->rd(va, &v, 2, r->ctx))
        r->ok = 0;
    return v;
}

static uint8_t r8(Reader *r, uint32_t va)
{
    uint8_t v = 0;
    if (!r->rd(va, &v, 1, r->ctx))
        r->ok = 0;
    return v;
}

static uint32_t elf_hash(const char *s)
{
    uint32_t h = 0;
    for (; *s; s++) {
        uint32_t g;
        h = (h << 4) + (uint32_t)(int)(signed char)*s;
        g = h & 0xF0000000u;
        if (g) {
            h ^= g >> 24;
            h &= ~g;
        }
    }
    return h;
}

/* A u16 list (0xFFFF ends it), at most cap entries. */
static int task_list(Reader *r, uint32_t va, uint16_t *out, int cap)
{
    int n = 0;
    if (!va)
        return 0;
    while (n < cap) {
        uint16_t v = r16(r, va + (uint32_t)n * 2u);
        if (!r->ok || v == 0xFFFF)
            break;
        out[n++] = v;
    }
    return n;
}

typedef struct {
    Reader *r;
    uint8_t state[ES_BITS];
    uint8_t *bits;
    int depth;
} Closure;

static void set_bit(uint8_t *bits, int i)
{
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* 0x0035DD30: a task's prerequisites done, each with its own first. */
static void mark_prereqs(Closure *c, int task)
{
    uint16_t list[64];
    int n;

    if (task < 0 || task >= ES_BITS || ++c->depth > 64) {
        c->depth--;
        return;
    }
    n = task_list(c->r, r32(c->r, TASKS + (uint32_t)task * 20u + 0xC), list, 64);
    for (int i = 0; i < n; i++)
        if (list[i] < ES_BITS) {
            mark_prereqs(c, list[i]);
            c->state[list[i]] = 3;
            set_bit(c->bits, list[i]);
        }
    c->depth--;
}

/* 0x0035DD50: what a done task also completes. Returns how many were new. */
static int also_done(Closure *c, int task)
{
    uint16_t list[64];
    int n, made = 0;

    n = task_list(c->r, r32(c->r, TASKS + (uint32_t)task * 20u + 0x10), list, 64);
    for (int i = 0; i < n; i++) {
        if (list[i] >= ES_BITS)
            continue;
        set_bit(c->bits, list[i]);
        if (c->state[list[i]] != 3) {
            c->state[list[i]] = 3;
            mark_prereqs(c, list[i]);
            made++;
        }
    }
    return made;
}

static void progress_for(Reader *r, int checkpoint, uint8_t *bits)
{
    Closure c;
    int made, rounds = 0;

    memset(&c, 0, sizeof c);
    c.r = r;
    c.bits = bits;
    mark_prereqs(&c, checkpoint);
    also_done(&c, checkpoint);
    do {
        made = 0;
        for (int i = 0; i < ES_BITS; i++)
            if (c.state[i] == 3)
                made += also_done(&c, i);
    } while (made && ++rounds < 1000);
}

/* ---- the file -------------------------------------------------------------- */

static int hmac_sha1(const uint8_t *data, size_t n, uint8_t out[20])
{
    static const uint8_t key[16];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, (PUCHAR)key, sizeof key, 0) >= 0
        && BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) >= 0
        && BCryptFinishHash(h, out, 20, 0) >= 0)
        ok = 1;
    if (h)
        BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static int signed_ok(const uint8_t *d, uint32_t total)
{
    uint8_t s[20];
    return hmac_sha1(d, 12, s) && !memcmp(s, d + 12, 20)
        && hmac_sha1(d, total - 20, s) && !memcmp(s, d + total - 20, 20);
}

static int sign(uint8_t *d, uint32_t total)
{
    return hmac_sha1(d, 12, d + 12) && hmac_sha1(d, total - 20, d + total - 20);
}

typedef struct {
    uint8_t *d;
    uint32_t count;
} Entries;

/* The entry's value, by name; NULL when the save has none. */
static uint32_t *entry(Entries *e, const char *name)
{
    uint32_t key = elf_hash(name);
    uint32_t lo = 0, hi = e->count;

    while (lo < hi) {               /* sorted by key, as the game writes them */
        uint32_t mid = (lo + hi) / 2;
        uint32_t k = *(uint32_t *)(e->d + 0x28 + mid * 12u);
        if (k == key)
            return (uint32_t *)(e->d + 0x28 + mid * 12u + 8u);
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static void copy_folder(const char *from, const char *to)
{
    char pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    CreateDirectoryA(to, NULL);
    snprintf(pat, sizeof pat, "%s\\*", from);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(a, sizeof a, "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof b, "%s\\%s", to, fd.cFileName);
        CopyFileA(a, b, FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); return 1; } while (0)

int edit_save_file(const char *save_dir, int slot, int chapter, int part,
                   EsRead rd, void *ctx, EsResult *res, char *err, size_t errn)
{
    Reader r = { rd, ctx, 1 };
    char folder[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH];
    uint8_t d[SAVE_SIZE];
    uint32_t total, size, list, cp, cash, *v;
    Entries e;
    FILE *f;
    size_t got;
    int parts;

    memset(res, 0, sizeof *res);
    if (slot < 1 || slot > 3)
        FAIL("Game 1, 2 or 3");
    parts = edit_save_parts(chapter);
    if (part < 1 || part > parts)
        FAIL("No such part");
    snprintf(folder, sizeof folder, "%s\\saves\\Game%d", save_dir, slot);
    snprintf(path, sizeof path, "%s\\conker.bin", folder);
    if (fopen_s(&f, path, "rb") || !f)
        FAIL("Game %d has no save yet", slot);
    got = fread(d, 1, sizeof d, f);
    fclose(f);
    total = got >= 8 ? *(uint32_t *)d : 0;
    size = got >= 8 ? *(uint32_t *)(d + 4) : 0;
    if (got != sizeof d || total != SAVE_SIZE || size < 8 || 0x20u + size > total - 20)
        FAIL("Game %d's save isn't one this game wrote", slot);
    if (!signed_ok(d, total))
        FAIL("Game %d's save isn't signed as this game signs it", slot);
    e.d = d;
    e.count = *(uint32_t *)(d + 0x24);
    if (0x28u + e.count * 12u > 0x20u + size)
        FAIL("Game %d's save isn't one this game wrote", slot);

    /* The part, as the Chapters list has it. */
    if (r8(&r, PART_COUNTS + (uint32_t)chapter) != (uint8_t)parts)
        FAIL("The game's chapter list isn't the one expected");
    list = r32(&r, PART_LISTS + (uint32_t)chapter * 4u);
    cash = r16(&r, list + (uint32_t)(part - 1) * 4u);
    cp = r16(&r, list + (uint32_t)(part - 1) * 4u + 2u);
    if (!r.ok || cp >= ES_BITS)
        FAIL("Can't read the game's chapter list");
    res->checkpoint = (int)cp;
    res->money = (int)cash;
    res->level = r16(&r, TASKS + cp * 20u + 8u);
    res->spawn = r8(&r, TASKS + cp * 20u + 0xAu);
    progress_for(&r, (int)cp, res->bits);
    if (!r.ok)
        FAIL("Can't read the game's task table");
    res->energy = res->bits[CURED_BIT >> 3] & (1u << (CURED_BIT & 7)) ? 6 : 3;

    /* The entries. */
    {
        static const char *const keys[] = { "Level", "StartPoint", "Money", "StartingEnergy" };
        const int vals[] = { res->level, res->spawn, res->money, res->energy };
        for (int i = 0; i < 4; i++) {
            if (!(v = entry(&e, keys[i])))
                FAIL("Game %d's save has no %s", slot, keys[i]);
            *v = (uint32_t)vals[i];
        }
    }
    for (int i = 0; i < 26; i++) {
        char k[16];
        snprintf(k, sizeof k, "TaskFlags%d", i);
        if (!(v = entry(&e, k)))
            FAIL("Game %d's save has no %s", slot, k);
        *v = 0;
    }
    for (int i = 0; i < ES_BITS; i++) {
        char name[64];
        uint32_t p = r32(&r, BIT_NAMES + (uint32_t)i * 8u);
        int on = (res->bits[i >> 3] >> (i & 7)) & 1;
        if (!r.ok || !p || !rd(p, name, sizeof name, ctx))
            FAIL("Can't read the game's progress names");
        name[sizeof name - 1] = '\0';
        if (!(v = entry(&e, name)))
            FAIL("Game %d's save has no %s", slot, name);
        *v = (uint32_t)on;
        if (on) {
            res->count++;
            res->last = i;
        }
    }
    /* The part the slot screen shows: the last part whose bit is set. */
    {
        int shown = 0, before = 1;
        for (int c = 0; c < ES_CHAPTERS; c++) {
            uint8_t n = r8(&r, PART_COUNTS + (uint32_t)c);
            uint32_t l = r32(&r, PART_LISTS + (uint32_t)c * 4u);
            for (int p = 0; p < n; p++) {
                uint16_t b = r16(&r, l + (uint32_t)p * 4u + 2u);
                if (b < ES_BITS && (res->bits[b >> 3] >> (b & 7)) & 1)
                    shown = before + p;
            }
            before += n;
        }
        res->shown = shown ? shown : 1;
    }
    if (!sign(d, total))
        FAIL("Couldn't sign the save");

    /* A copy of the slot first, then the new file in one step. */
    {
        char stamp[32];
        time_t t = time(NULL);
        struct tm tm;
        localtime_s(&tm, &t);
        strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
        snprintf(res->backup, sizeof res->backup, "%s\\edit_save_backups", save_dir);
        CreateDirectoryA(res->backup, NULL);
        snprintf(res->backup, sizeof res->backup, "%s\\edit_save_backups\\Game%d_%s",
                 save_dir, slot, stamp);
        copy_folder(folder, res->backup);
    }
    snprintf(tmp, sizeof tmp, "%s.edit", path);
    if (fopen_s(&f, tmp, "wb") || !f)
        FAIL("Can't write in %s", folder);
    got = fwrite(d, 1, sizeof d, f);
    if (fclose(f) || got != sizeof d) {
        DeleteFileA(tmp);
        FAIL("Can't write in %s", folder);
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        FAIL("Can't replace %s", path);
    }
    return 0;
}
