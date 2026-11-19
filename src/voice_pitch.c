/**
 * Conker's voice for Conker the Mouse / Conker the Bear (to-do 212, SPEC 10.3):
 * higher or lower, each line as long as before (the user's choice 2026-11-13:
 * cutscenes stay in sync), so no timing anywhere changes.
 *
 * Every speech bank is a streamed XACT wave bank (WBND v3, flags 1; plain
 * 16-bit PCM, mono 22,050 or 44,100 Hz), read from the disc while a line
 * plays. The toolkit's file hooks (g_xbox_file_opened / _read / _closed,
 * kernel_file.c) tell this file of each open and read: where a read covers one
 * of Conker's waves, those bytes are replaced by the same wave pitched (made
 * once per wave and factor, kept). The game, XACT and the sound chip see the
 * same file sizes and offsets as ever.
 *
 * Conker's waves come from the sound banks (Audio\conker_sb.bin, 58 SDBK v11
 * banks; conker-notes 2026-11-11, 2026-11-13 "Voice, every line"): per bank,
 * the wave bank list (16-byte names at header +8, count +0x22), the cues
 * (count +0x1A, 20 bytes each from +0x38: u16 at +2 = the sound the cue plays,
 * name offset at +4; a cue's number is not its sound's in most banks:
 * cut_speech, sfx, sfx_new) and after them the sounds' 20-byte records (count
 * +0x1C). A cue is his when its name starts with "con" (conkbits, condino,
 * conk_jump ...) or holds "conker" (war_conker_*), or is one of the lines the
 * user picked out by ear (s_also; 2026-11-13), and isn't one of s_not: his own
 * lines in play and the cutscene tracks that are only his (conkmocash,
 * conkarena ...). Record byte 11 says how it finds its waves (to-do 215: most
 * of his in-play grunts and every solo cutscene line are not simple):
 *   bit 3  simple: u16 wave, u16 index into the wave bank list;
 *   bit 4  random takes: u32 = offset of a table (u16 n, u16, n x 8 bytes:
 *          u16 wave, u16 list index, u16 / u16 weight range);
 *   else   events: u32 = offset of an entry (u8 count, u24 offset) at the
 *          events {u8 type, u24 time, u8 size, u8 flags, u16, size bytes}:
 *          types 0 and 1 play a wave, the payload's first 4 bytes saying
 *          which: flags bit 2 = u32 offset of a random-takes table, else u16
 *          wave, u16 list index; the others set mix, volume ...
 * Offsets are from the SDBK's start. Waves in the shared effects banks (sfx,
 * sfx_X, sfx_looped*) are left out: the names there that match are knives,
 * gunshots, a zombie doll, pills.
 * A wave bank is known by the name in its header (hd_sfx.xwb is "sfx"); a
 * cache copy being written has none yet, so it takes the disc file's name.
 *
 * The pitch: the wave stretched in time by the factor (WSOLA: 40 ms Hann
 * frames, half overlap, each frame placed where it best matches the last
 * one's continuation), then read back at the factor's speed (linear), which
 * leaves the length and moves the pitch. Mouse +7 semitones, Bear -7.
 *
 * RECOMP_VOICE_PITCH=0 = off (his voice as recorded with either cheat);
 * [VOICE] lines: the waves found, each wave pitched the first time.
 *
 * Conversations (to-do 216; docs/research/r4-216-mouse-bear-voice.md): most
 * cutscene speech tracks, Birdy's talk among them, are one wave with every
 * speaker. Their lip-sync timelines (assets\lipsync.bin, carved from the
 * packages: src/caff_cache.c) move only the talker's mouth, and the talker
 * is Conker, so the stretches with a mouth shape are his lines. At the speech
 * start (ad_mpeg_Start 0x0001AF70, recomp_manual.c) voice_mixed_start gets the
 * cue, the lip name the game's own map gives and the talker; the cue's wave
 * is marked with those stretches (or our own times, s_table: instead of them,
 * or added for lines heard while his mouth is off screen), before the
 * stream's first read. At its first read the stretches' edges move to the
 * quietest point near them, a stretch that ends inside sound reaches on to
 * the next pause (his laugh or grunt after a line), and only the stretches
 * are pitched, blended in with 15-ms fades; the rest of the wave is as
 * recorded and the length never changes. On with either cheat, like his own
 * lines. RECOMP_VOICE_MIXED=0 = off, =2 = [VOICEMIX] lines only;
 * RECOMP_VOICE_MIXED_EXTEND=0 = no reaching on.
 */
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "voice_pitch.h"
#include "caff_cache.h"

extern void (*g_xbox_file_opened)(const wchar_t *host_path, HANDLE h);
extern void (*g_xbox_file_read)(HANDLE h, unsigned long long offset, void *buf,
                                unsigned long bytes);
extern void (*g_xbox_file_closed)(HANDLE h);
extern void (*g_xbox_file_write)(HANDLE h, unsigned long long offset, void *buf,
                                 unsigned long bytes);

float conker_SizeFactor(void);          /* recomp_manual.c: 0.5, 2 or 1 */

#define MAX_BANKS   128
#define MAX_OPEN    64
#define MAX_SDBK    128
#define MOUSE_PITCH 1.4983f             /* +7 semitones */
#define BEAR_PITCH  0.6674f             /* -7 semitones */

typedef struct {
    uint32_t off, len;                  /* file offset and bytes of the wave */
    uint32_t rate;
    int16_t *raw;                       /* as recorded, once read */
    int16_t *pitched[2];                /* [0] Mouse, [1] Bear; made on first use */
} Wave;

typedef struct {
    char name[17];                      /* wave bank name, e.g. sfx_speech_eng */
    wchar_t path[MAX_PATH];             /* its file, once the game opened it */
    int nwant;                          /* his waves (numbers) before the file is read */
    uint16_t *want;
    int nwaves;
    Wave *waves;                        /* sorted by off */
    int ready;                          /* 1 = header read, -1 = failed */
    uint8_t *ent;                       /* every wave's header entry (mixed tracks) */
    uint32_t count, esz, data;
} Bank;

static Bank s_bank[MAX_BANKS];
static int s_nbank;
/* .xwb file name -> the bank's own name (from its header), for cache copies
 * opened before their header is written. */
#define MAX_ALIAS 128
static struct { char file[40]; char bank[17]; } s_alias[MAX_ALIAS];
static int s_nalias;
static struct { HANDLE h; int bank; } s_open[MAX_OPEN];
static CRITICAL_SECTION s_lock;
static int s_parsed;                    /* conker_sb.bin read (1) or failed (-1) */
static int s_on = -1;

/* size bytes at off of the game's own open file h (it may hold it unshared and
 * unbuffered, so: sector-aligned reads into an aligned buffer), its file
 * pointer put back after. 1 when all of it was read. */
static int read_at(HANDLE h, unsigned long long off, void *out, DWORD size)
{
    const unsigned long long a = off & ~4095ull;
    const DWORD n = (DWORD)(((off + size + 4095ull) & ~4095ull) - a);
    LARGE_INTEGER zero, pos;
    OVERLAPPED ov;
    uint8_t *buf;
    DWORD got = 0;
    BOOL ok;

    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT))
        return 0;
    buf = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf)
        return 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)a;
    ov.OffsetHigh = (DWORD)(a >> 32);
    ok = ReadFile(h, buf, n, &got, &ov);
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    ok = ok && got >= (DWORD)(off - a) + size;
    if (ok)
        memcpy(out, buf + (off - a), size);
    VirtualFree(buf, 0, MEM_RELEASE);
    return ok;
}

static int on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_VOICE_PITCH");
        s_on = !(e && *e == '0');
    }
    return s_on;
}

/* RECOMP_VOICE_LOG=1: [VOICE] open / close lines for his banks' files. */
static int voice_log(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_VOICE_LOG");
        v = e && *e == '1';
    }
    return v;
}

static Bank *bank_named(const char *name, int make)
{
    for (int i = 0; i < s_nbank; i++)
        if (!_stricmp(s_bank[i].name, name))
            return &s_bank[i];
    if (!make || s_nbank == MAX_BANKS)
        return NULL;
    snprintf(s_bank[s_nbank].name, sizeof s_bank[s_nbank].name, "%s", name);
    return &s_bank[s_nbank++];
}

/* Lines of his that aren't named for him: the user's picks by ear
 * (2026-11-13, to-do 215's list); a name matches as it is or followed by a
 * take number (strain1 .. strain10). */
static const char *const s_also[] = {
    "strain", "stopthat", "holdstill", "submerge",      /* in play */
    "mocash", "eatled", "bitchhelmet", "getheadpiece",  /* cutscene tracks */
    "tenoclock", "zombrise", "barrelnight", "whichway", "finalkey",
    "cantgetworse",
    /* His thoughts in play (no lip data; the user's picks 2026-11-18, to-do
     * 264): training (cut_hungover) and Windy (cut_windy). Whole names: the
     * neighbours (mancata2..9 and the rest) aren't his picks. */
    "trainth1", "trainth5", "hung_new03", "mancata1", "mancata6", "swimman1",
};

/* Named for him but not his voice: conka (an effect, user 2026-11-13),
 * conk_poo_splat (Sloprano's poo-ball splat; its waves are that sound's).
 * matrix_conker_gunout (the gun pulled out, user 2026-11-13) sits in the
 * effects bank sfx, left out there. */
static const char *const s_not[] = { "conka", "conk_poo_splat" };

static int name_is(const char *low, const char *want)
{
    const size_t n = strlen(want);
    if (strncmp(low, want, n))
        return 0;
    for (low += n; *low; low++)
        if (!isdigit((unsigned char)*low))
            return 0;
    return 1;
}

static int his(const char *name)
{
    char low[64];
    size_t i;

    for (i = 0; i + 1 < sizeof low && name[i]; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    for (i = 0; i < sizeof s_not / sizeof *s_not; i++)
        if (!strcmp(low, s_not[i]))
            return 0;
    for (i = 0; i < sizeof s_also / sizeof *s_also; i++)
        if (name_is(low, s_also[i]))
            return 1;
    return !strncmp(low, "con", 3) || strstr(low, "conker") != NULL;
}

static void want_add(Bank *b, uint16_t wave)
{
    for (int i = 0; i < b->nwant; i++)
        if (b->want[i] == wave)
            return;
    b->want = realloc(b->want, (size_t)(b->nwant + 1) * sizeof *b->want);
    if (b->want)
        b->want[b->nwant++] = wave;
}

/* The shared sound-effects wave banks: nothing of his voice is pitched there. */
static int effects_bank(const char *name)
{
    return !_stricmp(name, "sfx") || !_stricmp(name, "sfx_X")
        || !_strnicmp(name, "sfx_looped", 10);
}

/* One SDBK being read: its bytes [0, end) and its wave bank list. With obank
 * set, the waves found go there (a mixed track's, to-do 216) instead of into
 * his wanted lists. */
typedef struct {
    const uint8_t *s;
    uint32_t end, list;
    uint16_t nbank;
    int waves;                          /* his waves added from it */
    char (*obank)[17];
    uint16_t *owave;
    int omax, on;
} Sdbk;

static int sd_ok(const Sdbk *k, uint32_t off, uint32_t n)
{
    return off <= k->end && n <= k->end - off;
}

static void sd_wave(Sdbk *k, uint16_t wave, uint16_t bi)
{
    char bname[17];
    Bank *b;

    if (bi >= k->nbank)
        return;
    memcpy(bname, k->s + k->list + bi * 16u, 16);
    bname[16] = 0;
    if (k->obank) {
        if (k->on < k->omax && !effects_bank(bname)) {
            memcpy(k->obank[k->on], bname, sizeof bname);
            k->owave[k->on++] = wave;
        }
        return;
    }
    if (effects_bank(bname) || (b = bank_named(bname, 1)) == NULL)
        return;
    want_add(b, wave);
    k->waves++;
}

/* A random-takes table: u16 n, u16, n x {u16 wave, u16 list index, u16, u16}. */
static void sd_takes(Sdbk *k, uint32_t off)
{
    uint16_t n;

    if (!sd_ok(k, off, 4))
        return;
    n = *(const uint16_t *)(k->s + off);
    if (!sd_ok(k, off + 4, n * 8u))
        return;
    for (uint32_t i = 0; i < n; i++)
        sd_wave(k, *(const uint16_t *)(k->s + off + 4 + i * 8),
                *(const uint16_t *)(k->s + off + 6 + i * 8));
}

/* One sound's 20-byte record: the waves it can play (header comment).
 * Returns 0 simple, 1 random takes, 2 events. */
static int sd_sound(Sdbk *k, const uint8_t *r)
{
    const uint32_t v = *(const uint32_t *)r;
    uint32_t e, q;

    if (r[11] & 8) {
        sd_wave(k, *(const uint16_t *)r, *(const uint16_t *)(r + 2));
        return 0;
    }
    if (r[11] & 0x10) {
        sd_takes(k, v);
        return 1;
    }
    if (!sd_ok(k, v, 4))
        return 2;
    e = *(const uint32_t *)(k->s + v);
    q = e >> 8;
    for (uint32_t i = 0; i < (e & 0xFF) && sd_ok(k, q, 8); i++) {
        const uint8_t *ev = k->s + q;
        const uint8_t size = ev[4], flags = ev[5];
        if (!sd_ok(k, q + 8, size))
            break;
        /* Play events: flags bit 2 = the wave is a random-takes table. */
        if ((ev[0] == 0 || ev[0] == 1) && size >= 4) {
            if (flags & 4)
                sd_takes(k, *(const uint32_t *)(ev + 8));
            else
                sd_wave(k, *(const uint16_t *)(ev + 8), *(const uint16_t *)(ev + 10));
        }
        q += 8u + size;
    }
    return 2;
}

/* conker_sb.bin kept (to-do 216: a mixed track's cue is looked up at its
 * start), and where each SDBK lies in it, in file order = the sound table's
 * bank numbers. */
static uint8_t *s_sb;
static uint32_t s_sdbk[MAX_SDBK][2];    /* offset, size */
static int s_nsdbk;

/* conker_sb.bin: every bank's sounds that are his, by wave bank and wave. */
static void parse_sound_banks(HANDLE f)
{
    DWORD size;
    uint8_t *d;
    int sounds[3] = { 0, 0, 0 }, waves = 0;

    s_parsed = -1;
    size = GetFileSize(f, NULL);
    d = size != INVALID_FILE_SIZE ? malloc(size) : NULL;
    if (!d || !read_at(f, 0, d, size)) {
        free(d);
        fprintf(stderr, "[VOICE] conker_sb.bin unreadable (error %lu)\n", GetLastError());
        fflush(stderr);
        return;
    }
    for (DWORD p = 0; p + 0x40 <= size; p++) {
        uint16_t ncue, nsnd;
        uint32_t recs;
        Sdbk k;

        if (memcmp(d + p, "SDBK", 4))
            continue;
        /* This SDBK ends where the next one starts (or at the file's end). */
        k.s = d + p;
        k.end = size - p;
        for (DWORD n = p + 4; n + 4 <= size; n++)
            if (!memcmp(d + n, "SDBK", 4)) {
                k.end = n - p;
                break;
            }
        if (s_nsdbk < MAX_SDBK) {
            s_sdbk[s_nsdbk][0] = p;
            s_sdbk[s_nsdbk++][1] = k.end;
        }
        ncue = *(uint16_t *)(d + p + 0x1A);
        nsnd = *(uint16_t *)(d + p + 0x1C);
        k.nbank = *(uint16_t *)(d + p + 0x22);
        k.list = *(uint32_t *)(d + p + 8);
        k.waves = 0;
        k.obank = NULL;
        recs = 0x38u + ncue * 20u;
        if (!sd_ok(&k, recs, nsnd * 20u) || !sd_ok(&k, k.list, k.nbank * 16u))
            continue;
        for (uint32_t i = 0; i < ncue; i++) {
            const uint8_t *cue = d + p + 0x38 + i * 20;
            uint32_t no = *(const uint32_t *)(cue + 4);
            uint16_t snd = *(const uint16_t *)(cue + 2);  /* the sound it plays */
            char name[64];

            if (no >= k.end || snd >= nsnd)
                continue;
            snprintf(name, sizeof name, "%.63s", (const char *)(d + p + no));
            if (his(name))
                sounds[sd_sound(&k, d + p + recs + snd * 20u)]++;
        }
        waves += k.waves;
        p += k.end - 1;
    }
    s_sb = d;
    s_parsed = 1;
    /* Every wave bank is followed from its first open (mixed tracks, to-do
     * 216), so one opened before this file may have read its header with
     * none of his waves known: it reads it again at its next read. */
    {
        int with = 0;
        for (int i = 0; i < s_nbank; i++)
            if (s_bank[i].nwant) {
                with++;
                if (s_bank[i].ready > 0 && !s_bank[i].nwaves)
                    s_bank[i].ready = 0;
            }
        fprintf(stderr, "[VOICE] Conker's sounds: %d simple, %d random takes, %d events; "
                "%d wave uses in %d wave banks\n", sounds[0], sounds[1], sounds[2], waves, with);
    }
    fflush(stderr);
}

/* The waves cue `cue` of SDBK `sdbk` plays, when its name is `name`; else
 * those of the first cue named so in any SDBK. How many (0: not found). */
static int cue_waves(int sdbk, int cue, const char *name, char (*bank)[17], uint16_t *wave, int max)
{
    for (int pass = 0; pass < 2 && s_sb; pass++)
        for (int i = pass ? 0 : sdbk; i < (pass ? s_nsdbk : sdbk + 1); i++) {
            const uint8_t *s;
            uint16_t ncue, nsnd;
            uint32_t recs;
            Sdbk k;

            if (i < 0 || i >= s_nsdbk)
                continue;
            s = s_sb + s_sdbk[i][0];
            memset(&k, 0, sizeof k);
            k.s = s;
            k.end = s_sdbk[i][1];
            ncue = *(const uint16_t *)(s + 0x1A);
            nsnd = *(const uint16_t *)(s + 0x1C);
            k.nbank = *(const uint16_t *)(s + 0x22);
            k.list = *(const uint32_t *)(s + 8);
            k.obank = bank;
            k.owave = wave;
            k.omax = max;
            recs = 0x38u + ncue * 20u;
            if (!sd_ok(&k, recs, nsnd * 20u) || !sd_ok(&k, k.list, k.nbank * 16u))
                continue;
            for (uint32_t c = pass ? 0 : (uint32_t)cue; c < (pass ? ncue : (uint32_t)cue + 1u) && c < ncue; c++) {
                const uint8_t *r = s + 0x38 + c * 20;
                uint32_t no = *(const uint32_t *)(r + 4);
                uint16_t snd = *(const uint16_t *)(r + 2);
                if (no >= k.end || snd >= nsnd
                    || _strnicmp((const char *)s + no, name, k.end - no) != 0)
                    continue;
                sd_sound(&k, s + recs + snd * 20u);
                return k.on;
            }
        }
    return 0;
}

/* The wave bank's header: where each of his waves lies in the file. */
static int wave_cmp(const void *a, const void *b)
{
    const Wave *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void read_bank(Bank *b, HANDLE f)
{
    uint8_t hdr[0x60], *ent = NULL;
    uint32_t seg[4][2], count, esz, data;

    /* Read again (parse_sound_banks): the old tables go. */
    free(b->waves);
    free(b->ent);
    b->waves = NULL;
    b->ent = NULL;
    b->nwaves = 0;
    b->ready = -1;
    if (!read_at(f, 0, hdr, sizeof hdr) || memcmp(hdr, "WBND", 4)
        || *(uint32_t *)(hdr + 4) != 3)
        goto out;
    memcpy(seg, hdr + 8, sizeof seg);
    if (seg[0][0] + 0x1C > sizeof hdr)
        goto out;
    count = *(uint32_t *)(hdr + seg[0][0] + 4);
    esz = *(uint32_t *)(hdr + seg[0][0] + 0x18);
    data = seg[3][0];
    if (esz < 16 || count > 10000)
        goto out;
    ent = malloc((size_t)count * esz + 1);
    if (!ent || !read_at(f, seg[1][0], ent, count * esz))
        goto out;
    b->waves = calloc((size_t)b->nwant + 1, sizeof *b->waves);
    if (!b->waves)
        goto out;
    for (int i = 0; i < b->nwant; i++) {
        uint32_t w = b->want[i], fmt, rate;
        if (w >= count)
            continue;
        fmt = *(uint32_t *)(ent + w * esz + 4);
        rate = (fmt >> 5) & 0x3FFFFFFu;
        /* Plain PCM (tag 0), mono, 16-bit only. */
        if ((fmt & 3) || ((fmt >> 2) & 7) != 1 || !(fmt >> 31) || rate < 8000)
            continue;
        b->waves[b->nwaves].off = data + *(uint32_t *)(ent + w * esz + 8);
        b->waves[b->nwaves].len = *(uint32_t *)(ent + w * esz + 0xC) & ~1u;
        b->waves[b->nwaves].rate = rate;
        b->nwaves++;
    }
    qsort(b->waves, (size_t)b->nwaves, sizeof *b->waves, wave_cmp);
    b->ready = 1;
    b->ent = ent;                       /* kept: a mixed track's wave is found later */
    b->count = count;
    b->esz = esz;
    b->data = data;
    ent = NULL;
    if (b->nwant) {
        fprintf(stderr, "[VOICE] %s: %d of his waves\n", b->name, b->nwaves);
        fflush(stderr);
    }
out:
    if (b->ready < 0) {
        fprintf(stderr, "[VOICE] %s: header unreadable\n", b->name);
        fflush(stderr);
    }
    free(ent);
}

/* Wave w of bank b: where it lies, when it is plain 16-bit mono PCM. */
static int wave_loc(const Bank *b, uint32_t w, uint32_t *off, uint32_t *len, uint32_t *rate)
{
    uint32_t fmt;

    if (b->ready <= 0 || !b->ent || w >= b->count)
        return 0;
    fmt = *(const uint32_t *)(b->ent + w * b->esz + 4);
    *rate = (fmt >> 5) & 0x3FFFFFFu;
    if ((fmt & 3) || ((fmt >> 2) & 7) != 1 || !(fmt >> 31) || *rate < 8000)
        return 0;
    *off = b->data + *(const uint32_t *)(b->ent + w * b->esz + 8);
    *len = *(const uint32_t *)(b->ent + w * b->esz + 0xC) & ~1u;
    return 1;
}

/* ---- the pitch --------------------------------------------------------- */

/* in (n samples) stretched to about n * k by WSOLA into a new buffer. */
static float *wsola(const float *in, int n, float k, int rate, int *nout)
{
    const int N = (rate / 25) & ~1, H = N / 2, tol = N / 4;
    const int outn = (int)((float)n * k) + N;
    float *out = calloc((size_t)outn + N, sizeof *out), *win = malloc((size_t)N * sizeof *win);
    int prev = 0;

    if (!out || !win) {
        free(out);
        free(win);
        return NULL;
    }
    for (int i = 0; i < N; i++)
        win[i] = 0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)N);
    for (int o = 0; o + N <= outn; o += H) {
        int nominal = (int)((float)o / k), best = nominal;

        if (o > 0) {
            /* Where the last frame would have gone on: the frame that best
             * matches it, within tol of where this one should start. */
            const int nat = prev + H;
            double bestc = -1e30;
            for (int d = -tol; d <= tol; d += 2) {
                int c = nominal + d;
                double s = 0.0;
                if (c < 0 || c + N > n || nat + N > n)
                    continue;
                for (int i = 0; i < N; i += 4)
                    s += (double)in[c + i] * in[nat + i];
                if (s > bestc) {
                    bestc = s;
                    best = c;
                }
            }
        }
        if (best < 0)
            best = 0;
        if (best + N > n)
            best = n - N > 0 ? n - N : 0;
        for (int i = 0; i < N && best + i < n; i++)
            out[o + i] += in[best + i] * win[i];
        prev = best;
    }
    free(win);
    *nout = outn;
    return out;
}

static int16_t *pitch_wave(const int16_t *src, int n, float k, int rate)
{
    float *in = malloc((size_t)n * sizeof *in), *st;
    int16_t *out = malloc((size_t)n * sizeof *out);
    int sn = 0;

    if (!in || !out || n < rate / 10) {
        free(in);
        if (out)
            memcpy(out, src, (size_t)n * sizeof *out);
        return out;
    }
    for (int i = 0; i < n; i++)
        in[i] = (float)src[i];
    st = wsola(in, n, k, rate, &sn);
    if (!st) {
        free(in);
        memcpy(out, src, (size_t)n * sizeof *out);
        return out;
    }
    for (int i = 0; i < n; i++) {
        float p = (float)i * k;
        int a = (int)p;
        float f = p - (float)a, v;
        v = a + 1 < sn ? st[a] * (1.0f - f) + st[a + 1] * f : 0.0f;
        if (v > 32767.0f)
            v = 32767.0f;
        if (v < -32768.0f)
            v = -32768.0f;
        out[i] = (int16_t)lrintf(v);
    }
    free(st);
    free(in);
    return out;
}

/* RECOMP_VOICE_DUMP=<folder>: each wave pitched here also as WAV files there,
 * <offset>_orig.wav and <offset>_mouse.wav / _bear.wav (debug). */
static void dump_wav(const char *tag, const Wave *w, const int16_t *pcm)
{
    const char *dir = getenv("RECOMP_VOICE_DUMP");
    char path[MAX_PATH];
    uint32_t hdr[11];
    FILE *f;

    if (!dir || !*dir)
        return;
    snprintf(path, sizeof path, "%s/%08X_%s.wav", dir, w->off, tag);
    if (!(f = fopen(path, "wb")))
        return;
    hdr[0] = 0x46464952u;               /* RIFF */
    hdr[1] = 36u + w->len;
    hdr[2] = 0x45564157u;               /* WAVE */
    hdr[3] = 0x20746D66u;               /* fmt */
    hdr[4] = 16u;
    hdr[5] = 1u | (1u << 16);           /* PCM, mono */
    hdr[6] = w->rate;
    hdr[7] = w->rate * 2u;
    hdr[8] = 2u | (16u << 16);
    hdr[9] = 0x61746164u;               /* data */
    hdr[10] = w->len;
    fwrite(hdr, 4, 11, f);
    fwrite(pcm, 1, w->len, f);
    fclose(f);
}

static const int16_t *pitched(Bank *b, Wave *w, int which, HANDLE f)
{
    if (!w->raw) {
        int16_t *raw = malloc(w->len ? w->len : 2);
        if (raw && read_at(f, w->off, raw, w->len))
            w->raw = raw;
        else
            free(raw);
    }
    if (!w->pitched[which] && w->raw) {
        const int16_t *raw = w->raw;
        {
            w->pitched[which] = pitch_wave(raw, (int)(w->len / 2),
                                           which ? BEAR_PITCH : MOUSE_PITCH, (int)w->rate);
            dump_wav("orig", w, raw);
            if (w->pitched[which])
                dump_wav(which ? "bear" : "mouse", w, w->pitched[which]);
            fprintf(stderr, "[VOICE] %s wave at %08X (%.1f s) pitched for the %s\n", b->name,
                    w->off, (double)w->len / 2.0 / w->rate, which ? "Bear" : "Mouse");
            fflush(stderr);
        }
    }
    return w->pitched[which];
}

/* ---- conversations: his stretches of a mixed track (to-do 216) --------- */

#define MAX_MIX      6                  /* mixed waves kept (a scene's few tracks) */
#define MAX_RUNS     128
#define MIX_RAMP_S   0.015f             /* fade in / out at a stretch's edges */
#define MIX_GAP_F    8                  /* a pause of 8 frames (160 ms) ends a reach */
#define MIX_FWD_F    75                 /* reach on after a line: 1.5 s at most */
#define MIX_BACK_F   25                 /* and back before one: 0.5 s */

typedef struct {
    char cue[32];                       /* "" = a free slot */
    char bank[17];
    uint16_t wave;
    const char *src;                    /* "lip" or "table" */
    float runs[MAX_RUNS][2];            /* seconds, as found */
    uint8_t fixed[MAX_RUNS];            /* 1 = our own time: snapped, not reached on */
    int nruns, table;                   /* table: every run is ours */
    uint32_t off, len, rate;
    int located;                        /* 1 known, -1 not plain PCM */
    int16_t *raw, *out[2];
    unsigned stamp;
    unsigned gen;                       /* this registration (a worker's result fits it) */
    int building;                       /* a worker is making out[] */
} MixWave;

static MixWave s_mix[MAX_MIX];
static unsigned s_mix_stamp, s_mix_gen;
static CONDITION_VARIABLE s_built = CONDITION_VARIABLE_INIT;   /* a worker finished */

/* The lip table (caff_cache.h): each track's record (name[32], u32 n,
 * n x 12 bytes), sorted by name. */
static uint8_t *s_lips;
static const uint8_t **s_lip;
static int s_nlip;

/* Scenes that switch the lip-sync talker (scene action 0x7E: haunted house
 * 1, 4, 14, end arena 18, 19): their lip runs may be someone else's, so these
 * tracks stay as recorded until checked (conkturnback and zombrise there are
 * his own lines, pitched whole). */
static const char *const s_switch[] = {
    "enterhouse", "dinning1", "alien7", "alien8", "alien9", "alien10", "alien11", "alien12",
    "alien13",
};

/* Our own times of his lines (seconds, by ear and by the waves; no game data)
 * for tracks whose lip data misses lines. add 0: these times instead of the
 * lip data; add 1: added to the lip runs (lines heard while his mouth is off
 * screen). hung_new04: the gargoyle scene (TrainingLevel 11, scene 9 after
 * it); its lip data covers only the third (docs/research/r3-gargoyle.md,
 * r4-216). intro1 / intro1b: the New Game opening (MainMenu 3, to-do 216 (b)):
 * his voice from Berri's answering machine while the camera is in her room;
 * intro1 has no lip data (her greeting, the beep at 36.6-37.8 s, then his
 * message), intro1b's lip runs stop at the cut to her room ("Anyway... I'll
 * see ya. Love you!") and start again back in the bar at 17 s. */
static const struct {
    const char *cue;
    int n;
    float s[8][2];
    int add;
} s_table[] = {
    { "hung_new04", 5, { { 0.0f, 1.9f }, { 7.1f, 10.7f }, { 13.9f, 15.5f }, { 37.6f, 42.2f },
                         { 47.0f, 50.7f } }, 0 },
    { "intro1", 1, { { 38.2f, 46.3f } }, 0 },
    { "intro1b", 1, { { 9.0f, 15.6f } }, 1 },
};

static int mixed_mode(void)             /* RECOMP_VOICE_MIXED: 1 on, 0 off, 2 log only */
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_VOICE_MIXED");
        m = !e || !*e ? 1 : *e == '0' ? 0 : *e == '2' ? 2 : 1;
    }
    return m;
}

static int extend_on(void)              /* RECOMP_VOICE_MIXED_EXTEND=0: no reaching on */
{
    static int x = -1;
    if (x < 0) {
        const char *e = getenv("RECOMP_VOICE_MIXED_EXTEND");
        x = !(e && *e == '0');
    }
    return x;
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void lips_load(const char *save_dir)
{
    size_t n = 0, at = 12;
    uint8_t *d = save_dir && *save_dir ? caff_cache_lips(save_dir, &n) : NULL;
    uint32_t cnt;

    if (!d) {
        fprintf(stderr, "[VOICEMIX] no lip table in the asset cache: conversations stay as recorded "
                "with the Mouse / Bear cheat this run (the launcher's Preparing game assets makes it; the "
                "game at its start once the cheat menu or either cheat is unlocked)\n");
        return;
    }
    cnt = u32le(d + 8);
    s_lip = malloc((size_t)cnt * sizeof *s_lip + 1);
    for (uint32_t i = 0; s_lip && i < cnt; i++) {
        uint32_t e;
        if (at + 36 > n || (e = u32le(d + at + 32)) > (n - at - 36) / 12)
            break;
        s_lip[s_nlip++] = d + at;
        at += 36 + (size_t)e * 12;
    }
    s_lips = d;
    fprintf(stderr, "[VOICEMIX] lip table: %d tracks\n", s_nlip);
}

static const uint8_t *lip_find(const char *name)
{
    char low[32];
    size_t i;
    int lo = 0, hi = s_nlip;

    for (i = 0; i + 1 < sizeof low && name[i]; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    while (lo < hi) {
        const int mid = (lo + hi) / 2, c = strncmp((const char *)s_lip[mid], low, 32);
        if (!c)
            return s_lip[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* The runs of a lip timeline with a mouth shape (a shape ending in 0 is the
 * mouth closed): each entry lasts its length, 0.5 s at most (a mouth parked
 * on a shape while the other one talks), 0.4167 s when none; gaps under
 * 0.25 s joined. */
static int lip_runs(const uint8_t *rec, float (*out)[2], int max)
{
    const uint32_t n = u32le(rec + 32);
    int k = 0;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = rec + 36 + i * 12;
        const uint16_t shape = (uint16_t)(e[2] | e[3] << 8);
        float t, len, end;

        memcpy(&t, e + 4, 4);
        memcpy(&len, e + 8, 4);
        if (shape % 10 == 0 || !(t >= 0.0f) || t > 3600.0f)
            continue;
        if (!(len > 0.0f))
            len = 0.4167f;
        if (len > 0.5f)
            len = 0.5f;
        end = t + len;
        if (k && t - out[k - 1][1] < 0.25f) {
            if (end > out[k - 1][1])
                out[k - 1][1] = end;
        } else if (k < max) {
            out[k][0] = t;
            out[k][1] = end;
            k++;
        }
    }
    return k;
}

static void mix_free(MixWave *m)
{
    free(m->raw);
    free(m->out[0]);
    free(m->out[1]);
    memset(m, 0, sizeof *m);
}

/* A mixed track's wave with his stretches, kept for its first read. The same
 * wave again keeps what was built; the oldest of MAX_MIX goes. */
static MixWave *mix_register(const char *cue, const char *bank, uint16_t wave, const char *src,
                             float (*runs)[2], const uint8_t *fixed, int nr, int table)
{
    MixWave *m = NULL;
    int i;

    for (i = 0; i < MAX_MIX && !m; i++)
        if (s_mix[i].cue[0] && s_mix[i].wave == wave && !_stricmp(s_mix[i].bank, bank))
            m = &s_mix[i];
    if (m && m->nruns == nr && !memcmp(m->runs, runs, (size_t)nr * sizeof *runs)
        && !memcmp(m->fixed, fixed, (size_t)nr)) {
        m->stamp = ++s_mix_stamp;
        return m;
    }
    if (!m) {
        for (i = 0; i < MAX_MIX && s_mix[i].cue[0]; i++)
            ;
        if (i == MAX_MIX)
            for (int j = i = 0; j < MAX_MIX; j++)
                if (s_mix[j].stamp < s_mix[i].stamp)
                    i = j;
        m = &s_mix[i];
    }
    mix_free(m);
    snprintf(m->cue, sizeof m->cue, "%s", cue);
    snprintf(m->bank, sizeof m->bank, "%s", bank);
    m->wave = wave;
    m->src = src;
    m->nruns = nr;
    memcpy(m->runs, runs, (size_t)nr * sizeof *runs);
    memcpy(m->fixed, fixed, (size_t)nr);
    m->table = table;
    m->stamp = ++s_mix_stamp;
    m->gen = ++s_mix_gen;
    return m;
}

/* The quietest 20-ms frame from f + lo to f + hi (a slight pull toward f). */
static int snap_frame(const float *db, int nf, int f, int lo, int hi)
{
    int best = f;
    float bv = 1e30f;

    for (int g = f + lo; g <= f + hi; g++) {
        float v;
        if (g < 0 || g >= nf)
            continue;
        v = db[g] + 0.05f * (float)abs(g - f);
        if (v < bv) {
            bv = v;
            best = g;
        }
    }
    return best;
}

/* From a stretch's edge f outward (dir +1 / -1): on through sound and pauses
 * shorter than MIX_GAP_F, up to `most` frames; the frame just past the last
 * sound, or f when there was none. */
static int reach(const float *db, int nf, float thr, int f, int dir, int most)
{
    int last = -1, quiet = 0;

    for (int g = f + dir, k = 1; k <= most && g >= 0 && g < nf; g += dir, k++) {
        if (db[g] > thr) {
            last = g;
            quiet = 0;
        } else if (++quiet >= MIX_GAP_F) {
            break;
        }
    }
    return last < 0 ? f : last + dir;
}

/* Whose voice a reach takes in (the reach guard): his lines' pitch against the
 * other speakers'. A reach into the next speaker's line (frankbroke2 23.0 s:
 * 364 Hz, his lines 172-291, the others' 375-403) is left out. */
#define MIX_SHORT_F  20                 /* 0.4 s: taken without a pitch to go by */
#define MIX_APART    1.12f              /* voices told apart by pitch from 12 % */

typedef struct {
    float *x;                           /* the wave at ~11 kHz (dec samples averaged) */
    int n, rate, dec, F;                /* its samples and rate; the wave's frame length */
    float *f0;                          /* per frame: Hz, 0 = none, -1 = not yet */
    const float *db;
    float thr;
    int nf;
} Pitch;

static float frame_f0(Pitch *p, int f)
{
    const int L = p->rate / 25, lo = p->rate / 500, hi = p->rate / 70;
    int s0;
    double e = 0.0, best = 0.0;
    int bk = 0;

    if (f < 0 || f >= p->nf)
        return 0.0f;
    if (p->f0[f] >= 0.0f)
        return p->f0[f];
    p->f0[f] = 0.0f;
    s0 = (int)(((long long)f * p->F + p->F / 2) / p->dec) - L / 2;
    if (p->db[f] <= p->thr || s0 < 0 || s0 + L > p->n || lo < 2 || hi >= L)
        return 0.0f;
    double ac[512];

    if (hi - lo >= 512)
        return 0.0f;
    for (int i = 0; i < L; i++)
        e += (double)p->x[s0 + i] * p->x[s0 + i];
    for (int k = lo; k <= hi; k++) {
        double s = 0.0;
        for (int i = 0; i + k < L; i++)
            s += (double)p->x[s0 + i] * p->x[s0 + i + k];
        ac[k - lo] = s;
        if (s > best)
            best = s;
    }
    /* The shortest lag nearly as strong as the best: two periods match about
     * as well as one, and taking them halves the pitch (an octave low). */
    for (int k = lo + 1; k < hi; k++) {
        const double s = ac[k - lo];
        if (s >= 0.85 * best && s >= ac[k - lo - 1] && s >= ac[k - lo + 1]) {
            bk = k;
            break;
        }
    }
    if (bk && ac[bk - lo] > 0.35 * e)
        p->f0[f] = (float)p->rate / (float)bk;
    return p->f0[f];
}

static int float_cmp(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* Median pitch of the frames [a, b) that `want` says (1 = his runs, 0 =
 * well away from them, -1 = all); how many had one in *cnt. */
static float median_f0(Pitch *p, int a, int b, const uint8_t *near_run, int want, int *cnt)
{
    float *v = malloc((size_t)(b - a > 0 ? b - a : 1) * sizeof *v), m = 0.0f;
    int k = 0;

    for (int f = a > 0 ? a : 0; v && f < b && f < p->nf; f++) {
        float x;
        if (want >= 0 && near_run[f] != (want ? 2 : 0))
            continue;
        if ((x = frame_f0(p, f)) > 0.0f)
            v[k++] = x;
    }
    if (k) {
        qsort(v, (size_t)k, sizeof *v, float_cmp);
        m = v[k / 2];
    }
    free(v);
    *cnt = k;
    return m;
}

/* Take in the reached frames [a, b)? Their pitch closer to his lines' than
 * to the others' (when the two differ and they have one); else only short. */
static int reach_ok(Pitch *p, int a, int b, float his, float oth)
{
    int c;
    float fr;

    if (b <= a)
        return 1;
    fr = median_f0(p, a, b, NULL, -1, &c);
    if (his > 0.0f && oth > 0.0f && (his > oth ? his / oth : oth / his) >= MIX_APART && c >= 3)
        return fabsf(logf(fr / his)) < fabsf(logf(fr / oth));
    return b - a <= MIX_SHORT_F;
}

static float ramp(float x)              /* raised cosine, 0 at x 0, 1 at x 1 */
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    return 0.5f - 0.5f * cosf(3.14159265f * x);
}

/* The wave (raw, as recorded) with only his stretches pitched by k. Any
 * thread; touches nothing shared (m is the caller's to read). */
static int16_t *mix_build(const MixWave *m, const int16_t *raw, float k, const char *who)
{
    const int n = (int)(m->len / 2), rate = (int)m->rate, F = rate / 50;
    const int nf = F > 0 ? n / F : 0, R = (int)(MIX_RAMP_S * (float)rate), ctx = rate / 10;
    int16_t *out = malloc((size_t)n * sizeof *out + 2);
    float *db = malloc((size_t)(nf > 0 ? nf : 1) * sizeof *db), maxdb = 0.0f, thr;
    int (*st)[2] = malloc(MAX_RUNS * sizeof *st);
    int ns = 0, voiced = 0, covered = 0;
    double pitched_s = 0.0;
    char list[512];
    size_t ll = 0;
    DWORD t0 = GetTickCount();
    Pitch pc;
    uint8_t *near_run = NULL;
    float his = 0.0f, oth = 0.0f;
    int reach_on = 0, kept = 0, dropped = 0, nh = 0, no = 0;

    /* Reaching on (and the pitch guard it needs) only from lip runs. */
    for (int r = 0; r < m->nruns && extend_on(); r++)
        reach_on |= !m->fixed[r];
    memset(&pc, 0, sizeof pc);
    if (!out || !db || !st || nf < 4) {
        free(db);
        free(st);
        if (out)
            memcpy(out, raw, (size_t)n * sizeof *out);
        return out;
    }
    memcpy(out, raw, (size_t)n * sizeof *out);
    for (int f = 0; f < nf; f++) {
        double s = 0.0;
        for (int i = 0; i < F; i++)
            s += (double)raw[f * F + i] * raw[f * F + i];
        db[f] = 20.0f * log10f(sqrtf((float)(s / F)) + 1.0f);
        if (db[f] > maxdb)
            maxdb = db[f];
    }
    /* Sound = within 25 dB of the loudest frame: the tracks' levels have two
     * humps, voices ~64-84 dB and the room, breaths and pauses ~43-57 (trainin1);
     * 35 dB down took the second hump for sound and reached into Birdy. */
    thr = maxdb - 25.0f > 30.0f ? maxdb - 25.0f : 30.0f;

    /* The reach guard's pitch: his (inside the lip runs) and the others'
     * (voiced frames 0.3 s or more from any run). */
    if (reach_on) {
        pc.dec = rate >= 16000 ? rate / 11025 : 1;
        pc.F = F;
        pc.rate = rate / pc.dec;
        pc.n = n / pc.dec;
        pc.db = db;
        pc.thr = thr;
        pc.nf = nf;
        pc.x = malloc((size_t)(pc.n > 0 ? pc.n : 1) * sizeof *pc.x);
        pc.f0 = malloc((size_t)nf * sizeof *pc.f0);
        near_run = calloc((size_t)nf, 1);
        if (!pc.x || !pc.f0 || !near_run) {
            reach_on = 0;
        } else {
            for (int i = 0; i < pc.n; i++) {
                float s = 0.0f;
                for (int j = 0; j < pc.dec; j++)
                    s += (float)raw[i * pc.dec + j];
                pc.x[i] = s / (float)pc.dec;
            }
            for (int f = 0; f < nf; f++)
                pc.f0[f] = -1.0f;
            for (int r = 0; r < m->nruns; r++) {
                const int a = (int)(m->runs[r][0] * (float)rate) / F, b = (int)(m->runs[r][1] * (float)rate) / F;
                for (int f = a - 15; f < b + 15; f++)
                    if (f >= 0 && f < nf)
                        /* Our own times (his voice through a phone: its
                         * pitch reads an octave up) count as neither. */
                        near_run[f] = f >= a && f < b && !m->fixed[r] ? 2 : near_run[f] == 2 ? 2 : 1;
            }
            his = median_f0(&pc, 0, nf, near_run, 1, &nh);
            oth = median_f0(&pc, 0, nf, near_run, 0, &no);
        }
    }

    /* Stretches in frames: edges snapped to the quietest frame near them
     * (lips lead his voice by 0.05-0.1 s: a start looks mostly before, an
     * end mostly after), then reached on over his laugh or grunt. */
    for (int r = 0; r < m->nruns; r++) {
        int a = (int)(m->runs[r][0] * (float)rate) / F, b = (int)(m->runs[r][1] * (float)rate) / F;

        if (a >= nf)
            continue;
        if (b >= nf)
            b = nf - 1;
        a = snap_frame(db, nf, a, -12, 4);
        b = snap_frame(db, nf, b, -5, 12);
        if (reach_on && !m->fixed[r]) {
            const int floor_f = ns ? st[ns - 1][1] : 0;
            int na = reach(db, nf, thr, a, -1, MIX_BACK_F), nb = reach(db, nf, thr, b, +1, MIX_FWD_F);
            /* A reached edge sits on the pause's first frame under thr, which
             * can still hold sound (a step at the fade, bullshit1 26.6 s):
             * moved to the pause's quietest frame. */
            if (na != a)
                na = snap_frame(db, nf, na, -(MIX_GAP_F - 1), 0);
            if (nb != b)
                nb = snap_frame(db, nf, nb, 0, MIX_GAP_F - 1);
            if (na < a) {
                if (reach_ok(&pc, na, a, his, oth))
                    kept++;
                else
                    na = a, dropped++;
            }
            if (nb > b) {
                if (reach_ok(&pc, b + 1, nb + 1, his, oth))
                    kept++;
                else
                    nb = b, dropped++;
            }
            b = nb;
            a = na > floor_f ? na : floor_f;
        }
        if (b <= a)
            continue;
        if (ns && a <= st[ns - 1][1]) {
            if (b > st[ns - 1][1])
                st[ns - 1][1] = b;
        } else if (ns < MAX_RUNS) {
            st[ns][0] = a;
            st[ns++][1] = b;
        }
    }

    /* Each stretch pitched with 0.1 s around it (the pitch settles there),
     * then blended in: full inside, fading over R samples outside its edges
     * (in the pause the edge was snapped to). */
    for (int s = 0; s < ns; s++) {
        const int sa = st[s][0] * F + F / 2, sb = st[s][1] * F + F / 2;
        const int ra = sa - R - ctx > 0 ? sa - R - ctx : 0, rb = sb + R + ctx < n ? sb + R + ctx : n;
        int16_t *p = pitch_wave(raw + ra, rb - ra, k, rate);

        if (!p)
            continue;
        for (int i = sa - R > 0 ? sa - R : 0; i < (sb + R < n ? sb + R : n); i++) {
            const float w = i < sa ? ramp((float)(i - (sa - R)) / (float)R)
                          : i >= sb ? ramp((float)(sb + R - i) / (float)R) : 1.0f;
            float v = (float)raw[i] * (1.0f - w) + (float)p[i - ra] * w;
            if (v > 32767.0f)
                v = 32767.0f;
            if (v < -32768.0f)
                v = -32768.0f;
            out[i] = (int16_t)lrintf(v);
        }
        free(p);
        pitched_s += (double)(sb - sa) / rate;
        if (ll + 32 < sizeof list)
            ll += (size_t)snprintf(list + ll, sizeof list - ll, "%s%.2f-%.2f", s ? " " : "",
                                   (double)sa / rate, (double)sb / rate);
    }
    for (int f = 0, s = 0; f < nf; f++) {
        while (s < ns && st[s][1] <= f)
            s++;
        if (db[f] > thr) {
            voiced++;
            covered += s < ns && f >= st[s][0];
        }
    }
    list[ll] = 0;
    fprintf(stderr, "[VOICEMIX] %s pitched for the %s (%s): %d stretch(es) %s s; %.1f of %.1f s, "
            "%d %% of the voiced time, in %lu ms", m->cue, who, m->src, ns, list, pitched_s,
            (double)n / rate, voiced ? covered * 100 / voiced : 0, GetTickCount() - t0);
    if (reach_on)
        fprintf(stderr, "; reached %d, left out %d (his pitch %.0f Hz from %d frames, the others' %.0f "
                "from %d)", kept, dropped, his, nh, oth, no);
    fprintf(stderr, "\n");
    fflush(stderr);
    free(db);
    free(st);
    free(pc.x);
    free(pc.f0);
    free(near_run);
    return out;
}

/* Where mixed wave m lies in bank b's file (once its header was read). */
static void mix_locate(Bank *b, MixWave *m)
{
    m->located = wave_loc(b, m->wave, &m->off, &m->len, &m->rate) ? 1 : -1;
    if (m->located < 0) {
        fprintf(stderr, "[VOICEMIX] %s: wave %s/%u isn't plain PCM: as recorded\n", m->cue,
                m->bank, m->wave);
        fflush(stderr);
    }
}

/* A worker making a mixed wave's pitched copy from the speech start (the
 * stream's first read comes ~0.5 s later; building it in that read held the
 * scene, whose clock waits for the line, 0.36 s for Birdy's 53-s track). It
 * reads the wave through its own handle on the bank's file and hands the
 * result over only if the slot still holds the same registration. */
typedef struct {
    MixWave copy;                       /* runs, place, names; no buffers */
    int slot, which;
    WCHAR path[MAX_PATH];
} MixJob;

static DWORD WINAPI mix_worker(void *p)
{
    MixJob *j = p;
    const DWORD len = j->copy.len;
    int16_t *raw = malloc(len ? len : 2), *out = NULL;
    HANDLE f = CreateFileW(j->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER at;
    DWORD got = 0;
    int ok;

    at.QuadPart = j->copy.off;
    ok = raw && f != INVALID_HANDLE_VALUE && SetFilePointerEx(f, at, NULL, FILE_BEGIN)
      && ReadFile(f, raw, len, &got, NULL) && got == len;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (ok)
        out = mix_build(&j->copy, raw, j->which ? BEAR_PITCH : MOUSE_PITCH, j->which ? "Bear" : "Mouse");
    EnterCriticalSection(&s_lock);
    {
        MixWave *m = &s_mix[j->slot];
        if (m->gen == j->copy.gen) {
            m->building = 0;
            if (ok && !m->raw) {
                m->raw = raw;
                raw = NULL;
            }
            if (out && !m->out[j->which]) {
                m->out[j->which] = out;
                out = NULL;
            }
        }
    }
    WakeAllConditionVariable(&s_built);
    LeaveCriticalSection(&s_lock);
    if (!ok) {
        fprintf(stderr, "[VOICEMIX] %s: the worker couldn't read the wave (error %lu): made at its first read\n",
                j->copy.cue, GetLastError());
        fflush(stderr);
    }
    free(raw);
    free(out);
    free(j);
    return 0;
}

/* Start the worker for m (s_lock held) when its bank's header is known. */
static void mix_prebuild(MixWave *m, const char *bank)
{
    const float k = conker_SizeFactor();
    Bank *b = bank_named(bank, 0);
    MixJob *j;
    HANDLE t;

    if (k == 1.0f || !b || b->ready <= 0 || !b->path[0] || m->building)
        return;
    if (!m->located)
        mix_locate(b, m);
    if (m->located < 0 || m->out[k > 1.0f] || !(j = calloc(1, sizeof *j)))
        return;
    j->copy = *m;
    j->copy.raw = j->copy.out[0] = j->copy.out[1] = NULL;
    j->slot = (int)(m - s_mix);
    j->which = k > 1.0f;
    wcsncpy(j->path, b->path, MAX_PATH - 1);
    m->building = 1;
    if ((t = CreateThread(NULL, 0, mix_worker, j, 0, NULL)) != NULL) {
        CloseHandle(t);
    } else {
        m->building = 0;
        free(j);
    }
}

/* The bytes to hand out for mixed wave m of bank b (read through h; s_lock
 * held). A worker still at it is waited for (3 s at most); without one, or
 * when it failed, the copy is made here. */
static const int16_t *mix_out(Bank *b, MixWave *m, int which, HANDLE h)
{
    const unsigned gen = m->gen;
    DWORD t0 = GetTickCount();

    if (!m->located)
        mix_locate(b, m);
    if (m->located < 0)
        return NULL;
    if (m->building && !m->out[which]) {
        while (m->building && m->gen == gen && GetTickCount() - t0 < 3000)
            SleepConditionVariableCS(&s_built, &s_lock, 100);
        if (m->gen != gen)
            return NULL;                /* replaced meanwhile: as recorded */
        fprintf(stderr, "[VOICEMIX] %s: its first read waited %lu ms for the worker\n", m->cue,
                GetTickCount() - t0);
        fflush(stderr);
    }
    if (!m->raw) {
        int16_t *raw = malloc(m->len ? m->len : 2);
        if (raw && read_at(h, m->off, raw, m->len))
            m->raw = raw;
        else
            free(raw);
    }
    if (!m->out[which] && m->raw)
        m->out[which] = mix_build(m, m->raw, which ? BEAR_PITCH : MOUSE_PITCH, which ? "Bear" : "Mouse");
    return m->out[which];
}

int voice_mixed_wanted(void)
{
    const int mode = mixed_mode();
    return mode && on() && (mode == 2 || conker_SizeFactor() != 1.0f);
}

void voice_mixed_start(unsigned sound, int sdbk, int cue, const char *cue_name,
                       const char *lip_name, const char *talker)
{
    static float runs[MAX_RUNS][2];
    static uint8_t fixed[MAX_RUNS];
    char bank[4][17], why[80] = "", low[32];
    uint16_t wave[4];
    const uint8_t *rec = NULL;
    const char *src = "";
    int nw = 0, nr = 0, table = 0, te = -1, ours = 0;
    size_t i;

    if (!voice_mixed_wanted() || !cue_name || !*cue_name)
        return;
    for (i = 0; i + 1 < sizeof low && cue_name[i]; i++)
        low[i] = (char)tolower((unsigned char)cue_name[i]);
    low[i] = 0;
    EnterCriticalSection(&s_lock);
    if (s_parsed <= 0)
        snprintf(why, sizeof why, "sound banks not read");
    else if ((nw = cue_waves(sdbk, cue, cue_name, bank, wave, 4)) == 0)
        snprintf(why, sizeof why, "no wave found");
    else if (his(cue_name))
        snprintf(why, sizeof why, "his own line, pitched whole");
    else {
        for (i = 0; i < sizeof s_switch / sizeof *s_switch; i++)
            if (!strcmp(low, s_switch[i]))
                snprintf(why, sizeof why, "a talker-switch scene: as recorded");
        if (!why[0] && (!talker || strcmp(talker, "Conker")))
            snprintf(why, sizeof why, "talker %s: as recorded", talker && *talker ? talker : "?");
    }
    if (!why[0]) {
        for (i = 0; i < sizeof s_table / sizeof *s_table && te < 0; i++)
            if (!strcmp(low, s_table[i].cue))
                te = (int)i;
        if (te >= 0 && !s_table[te].add) {
            nr = s_table[te].n;
            memcpy(runs, s_table[te].s, (size_t)nr * sizeof *runs);
            memset(fixed, 1, (size_t)nr);
            ours = nr;
            src = "table";
            table = 1;
        }
        if (!nr) {
            char alt[40];
            if (lip_name && *lip_name)
                rec = lip_find(lip_name);
            if (!rec)
                rec = lip_find(low);
            if (!rec && snprintf(alt, sizeof alt, "%s_22", low) > 0)
                rec = lip_find(alt);
            if (!rec && snprintf(alt, sizeof alt, "%s-22", low) > 0)
                rec = lip_find(alt);
            if (rec && (nr = lip_runs(rec, runs, MAX_RUNS)) > 0) {
                src = "lip";
                memset(fixed, 0, (size_t)nr);
            }
            /* Our own times added to the lip runs, in order of start. */
            if (nr && te >= 0 && s_table[te].add) {
                for (int t = 0; t < s_table[te].n && nr < MAX_RUNS; t++) {
                    int at = nr;
                    while (at > 0 && runs[at - 1][0] > s_table[te].s[t][0]) {
                        runs[at][0] = runs[at - 1][0];
                        runs[at][1] = runs[at - 1][1];
                        fixed[at] = fixed[at - 1];
                        at--;
                    }
                    runs[at][0] = s_table[te].s[t][0];
                    runs[at][1] = s_table[te].s[t][1];
                    fixed[at] = 1;
                    nr++;
                    ours++;
                }
                src = "lip and our times";
            }
        }
        if (!nr)
            snprintf(why, sizeof why, "%s: as recorded",
                     !s_nlip ? "no lip table until the next start" : rec ? "no mouth shapes" : "no lip data");
    }
    fprintf(stderr, "[VOICEMIX] speech 0x%03X %s (bank %d cue %d) wave %s/%u lip %s talker %s: ",
            sound, cue_name, sdbk, cue, nw ? bank[0] : "-", nw ? wave[0] : 0,
            rec ? (const char *)rec : lip_name && *lip_name ? lip_name : "-",
            talker && *talker ? talker : "-");
    if (why[0])
        fprintf(stderr, "%s\n", why);
    else {
        fprintf(stderr, "%d run(s) from the %s%s", nr, src, mixed_mode() == 2 ? " (log only)" : "");
        for (int r = 0; r < nr && r < 40; r++)
            fprintf(stderr, " %.2f-%.2f%s", runs[r][0], runs[r][1], fixed[r] && !table ? "*" : "");
        fprintf(stderr, "%s%s\n", nr > 40 ? " ..." : "", ours && !table ? " (* = ours)" : "");
        if (mixed_mode() == 1)
            for (int w = 0; w < nw; w++)
                mix_prebuild(mix_register(low, bank[w], wave[w], src, runs, fixed, nr, table), bank[w]);
    }
    fflush(stderr);
    LeaveCriticalSection(&s_lock);
}

/* ---- the hooks --------------------------------------------------------- */

static void opened(const wchar_t *path, HANDLE h)
{
    const wchar_t *base = path;
    char name[40];
    size_t n;
    Bank *b;

    /* A handle value being handed out again means its old file is gone,
     * whether or not its close reached us (some don't: the game's cache
     * check opens and drops banks; a stale entry made the next bank's reads
     * look like that one's, to-do 215). */
    EnterCriticalSection(&s_lock);
    for (int i = 0; i < MAX_OPEN; i++)
        if (s_open[i].h == h) {
            s_open[i].h = NULL;
            if (voice_log())
                fprintf(stderr, "[VOICE] %p reused without a close (slot %d)\n", (void *)h, i);
        }
    LeaveCriticalSection(&s_lock);
    for (const wchar_t *q = path; *q; q++)
        if (*q == L'\\' || *q == L'/')
            base = q + 1;
    n = wcslen(base);
    if (n >= sizeof name)
        return;
    for (size_t i = 0; i <= n; i++)
        name[i] = (char)base[i];
    if (!_stricmp(name, "conker_sb.bin")) {
        EnterCriticalSection(&s_lock);
        if (!s_parsed)
            parse_sound_banks(h);
        LeaveCriticalSection(&s_lock);
        return;
    }
    if (n < 5 || _stricmp(name + n - 4, ".xwb"))
        return;
    name[n - 4] = 0;
    /* The bank's own name, from its header (hd_sfx.xwb is "sfx"). A cache
     * copy the game is still writing has no header yet: it goes by the name
     * its disc file had (the disc file is always opened first). */
    {
        uint8_t hdr[0x60];
        char own[17] = "";
        uint32_t s0;

        if (read_at(h, 0, hdr, sizeof hdr) && !memcmp(hdr, "WBND", 4)
            && (s0 = *(uint32_t *)(hdr + 8)) + 24 <= sizeof hdr) {
            memcpy(own, hdr + s0 + 8, 16);
            own[16] = 0;
        }
        EnterCriticalSection(&s_lock);
        if (own[0]) {
            int i;
            for (i = 0; i < s_nalias && _stricmp(s_alias[i].file, name); i++)
                ;
            if (i == s_nalias && i < MAX_ALIAS)
                s_nalias++;
            if (i < MAX_ALIAS) {
                snprintf(s_alias[i].file, sizeof s_alias[i].file, "%s", name);
                snprintf(s_alias[i].bank, sizeof s_alias[i].bank, "%s", own);
            }
            snprintf(name, sizeof name, "%s", own);
        } else {
            for (int i = 0; i < s_nalias; i++)
                if (!_stricmp(s_alias[i].file, name)) {
                    snprintf(name, sizeof name, "%s", s_alias[i].bank);
                    break;
                }
        }
        LeaveCriticalSection(&s_lock);
    }
    /* The disc's file and the cache partition's copy (Z:) alike: the game
     * streams from either. What it writes into the copy gets his original
     * voice back (written, below), so the cache never keeps a pitched one. */
    /* Every wave bank: his waves (b->nwant) or a mixed track's (to-do 216). */
    EnterCriticalSection(&s_lock);
    b = bank_named(name, 1);
    if (b) {
        int i;
        if (!b->path[0])
            wcsncpy(b->path, path, MAX_PATH - 1);
        for (i = 0; i < MAX_OPEN && s_open[i].h; i++)
            ;
        if (i < MAX_OPEN) {
            s_open[i].h = h;
            s_open[i].bank = (int)(b - s_bank);
        }
        if (voice_log())
            fprintf(stderr, "[VOICE] open %p = %s (slot %d) %S\n", (void *)h, b->name, i, path);
    }
    LeaveCriticalSection(&s_lock);
}

static void closed(HANDLE h)
{
    EnterCriticalSection(&s_lock);
    for (int i = 0; i < MAX_OPEN; i++)
        if (s_open[i].h == h) {
            s_open[i].h = NULL;
            if (voice_log())
                fprintf(stderr, "[VOICE] close %p (slot %d)\n", (void *)h, i);
        }
    LeaveCriticalSection(&s_lock);
}

static void read_done(HANDLE h, unsigned long long off, void *buf, unsigned long bytes)
{
    const float k = conker_SizeFactor();
    const unsigned long long end = off + bytes;
    int which, i;
    Bank *b;

    if (k == 1.0f || !on())
        return;
    which = k > 1.0f;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < MAX_OPEN && s_open[i].h != h; i++)
        ;
    if (i == MAX_OPEN) {
        LeaveCriticalSection(&s_lock);
        return;
    }
    b = &s_bank[s_open[i].bank];
    if (!b->ready)
        read_bank(b, h);
    for (int w = 0; b->ready > 0 && w < b->nwaves; w++) {
        Wave *v = &b->waves[w];
        unsigned long long a = v->off, z = (unsigned long long)v->off + v->len, lo, hi;
        const int16_t *p;
        if (z <= off)
            continue;
        if (a >= end)
            break;
        lo = a > off ? a : off;
        hi = z < end ? z : end;
        if ((p = pitched(b, v, which, h)) != NULL)
            memcpy((uint8_t *)buf + (lo - off), (const uint8_t *)p + (lo - a), (size_t)(hi - lo));
    }
    /* Mixed tracks marked at their speech start (to-do 216). */
    for (int x = 0; b->ready > 0 && mixed_mode() == 1 && x < MAX_MIX; x++) {
        MixWave *m = &s_mix[x];
        unsigned long long a, z, lo, hi;
        const int16_t *p;

        if (!m->cue[0] || _stricmp(m->bank, b->name))
            continue;
        if (!m->located)
            mix_locate(b, m);
        if (m->located < 0)
            continue;
        a = m->off;
        z = (unsigned long long)m->off + m->len;
        if (z <= off || a >= end)
            continue;
        lo = a > off ? a : off;
        hi = z < end ? z : end;
        if ((p = mix_out(b, m, which, h)) != NULL)
            memcpy((uint8_t *)buf + (lo - off), (const uint8_t *)p + (lo - a), (size_t)(hi - lo));
    }
    LeaveCriticalSection(&s_lock);
}

/* A write into a bank's copy (the cache fill): where it covers his waves, the
 * bytes as recorded (a read just before may have been pitched). Only waves
 * already read here can have been changed. */
static void written(HANDLE h, unsigned long long off, void *buf, unsigned long bytes)
{
    const unsigned long long end = off + bytes;
    int i;
    Bank *b;

    EnterCriticalSection(&s_lock);
    for (i = 0; i < MAX_OPEN && s_open[i].h != h; i++)
        ;
    b = i < MAX_OPEN ? &s_bank[s_open[i].bank] : NULL;
    for (int w = 0; b && b->ready > 0 && w < b->nwaves; w++) {
        Wave *v = &b->waves[w];
        unsigned long long a = v->off, z = (unsigned long long)v->off + v->len, lo, hi;
        if (z <= off)
            continue;
        if (a >= end)
            break;
        if (!v->raw)
            continue;
        lo = a > off ? a : off;
        hi = z < end ? z : end;
        memcpy((uint8_t *)buf + (lo - off), (const uint8_t *)v->raw + (lo - a), (size_t)(hi - lo));
    }
    for (int x = 0; b && x < MAX_MIX; x++) {
        MixWave *m = &s_mix[x];
        unsigned long long a, z, lo, hi;

        if (!m->cue[0] || !m->raw || m->located <= 0 || _stricmp(m->bank, b->name))
            continue;
        a = m->off;
        z = (unsigned long long)m->off + m->len;
        if (z <= off || a >= end)
            continue;
        lo = a > off ? a : off;
        hi = z < end ? z : end;
        memcpy((uint8_t *)buf + (lo - off), (const uint8_t *)m->raw + (lo - a), (size_t)(hi - lo));
    }
    LeaveCriticalSection(&s_lock);
}

void voice_pitch_init(const char *save_dir)
{
    InitializeCriticalSection(&s_lock);
    lips_load(save_dir);
    g_xbox_file_opened = opened;
    g_xbox_file_read = read_done;
    g_xbox_file_closed = closed;
    g_xbox_file_write = written;
}
