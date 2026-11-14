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

extern void (*g_xbox_file_opened)(const wchar_t *host_path, HANDLE h);
extern void (*g_xbox_file_read)(HANDLE h, unsigned long long offset, void *buf,
                                unsigned long bytes);
extern void (*g_xbox_file_closed)(HANDLE h);
extern void (*g_xbox_file_write)(HANDLE h, unsigned long long offset, void *buf,
                                 unsigned long bytes);

float conker_SizeFactor(void);          /* recomp_manual.c: 0.5, 2 or 1 */

#define MAX_BANKS   64
#define MAX_OPEN    64
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

/* One SDBK being read: its bytes [0, end) and its wave bank list. */
typedef struct {
    const uint8_t *s;
    uint32_t end, list;
    uint16_t nbank;
    int waves;                          /* his waves added from it */
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
        ncue = *(uint16_t *)(d + p + 0x1A);
        nsnd = *(uint16_t *)(d + p + 0x1C);
        k.nbank = *(uint16_t *)(d + p + 0x22);
        k.list = *(uint32_t *)(d + p + 8);
        k.waves = 0;
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
    free(d);
    s_parsed = 1;
    fprintf(stderr, "[VOICE] Conker's sounds: %d simple, %d random takes, %d events; "
            "%d wave uses in %d wave banks\n", sounds[0], sounds[1], sounds[2], waves, s_nbank);
    fflush(stderr);
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
    ent = malloc((size_t)count * esz);
    if (!ent || !read_at(f, seg[1][0], ent, count * esz))
        goto out;
    b->waves = calloc((size_t)b->nwant, sizeof *b->waves);
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
    fprintf(stderr, "[VOICE] %s: %d of his waves\n", b->name, b->nwaves);
    fflush(stderr);
out:
    if (b->ready < 0) {
        fprintf(stderr, "[VOICE] %s: header unreadable\n", b->name);
        fflush(stderr);
    }
    free(ent);
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
    EnterCriticalSection(&s_lock);
    b = bank_named(name, 0);
    if (b && b->nwant) {
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
    LeaveCriticalSection(&s_lock);
}

void voice_pitch_init(void)
{
    InitializeCriticalSection(&s_lock);
    g_xbox_file_opened = opened;
    g_xbox_file_read = read_done;
    g_xbox_file_closed = closed;
    g_xbox_file_write = written;
}
