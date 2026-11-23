/**
 * Speaker test (to-do 231, 0.5.0; F2 Sound): a game sound on each speaker of the chosen Output,
 * so a player can check which speaker is where.
 *
 * The sound (the user's pick 2026-11-22, by ear from four candidates): the
 * cash register, cue "cashtill" of the effects sound bank, which plays only
 * wave 0 of the effects wave bank dvddata\Audio\hd_sfx.xwb (bank "sfx"):
 * 16-bit PCM, mono, 22,050 Hz, 60,756 bytes (1.38 s). Read from the game
 * folder at the first test (nothing of it is in our source): the WBND v3
 * header gives the segments (+8: offset / length pairs; 0 = the bank data:
 * wave count +4, entry size +0x18; 1 = the entries: format +4, offset +8
 * from segment 3, length +0xC). A wave that is not that one (another disc)
 * is not played: the test logs it and does nothing.
 *
 * The toolkit plays it (apu.h xbox_ApuSpeakerTest): on that speaker, the
 * game's own sound faded out under it, the master volume applied, the
 * subwoofer an octave down below 120 Hz. F2 Sound "Speaker test",
 * console speakertest, RECOMP_SPEAKER_TEST=<speaker> once at start
 * (scripts); [SPKTEST] lines.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "speaker_test.h"

void xbox_ApuSpeakerTestSound(const int16_t *pcm, int frames, int rate);   /* toolkit apu.h */
int  xbox_ApuSpeakerTest(int speaker);
int  xbox_ApuOutputChannels(void);
#define APU_SPEAKER_ASK (-2)

#define SPK_FILE   "dvddata\\Audio\\hd_sfx.xwb"
#define SPK_WAVE   0u
#define SPK_RATE   22050u
#define SPK_BYTES  60756u

static char s_game_dir[MAX_PATH];
static volatile LONG s_loaded;          /* 0 not tried, 1 handed over, -1 failed */

void speaker_test_init(const char *game_dir)
{
    if (game_dir)
        strcpy_s(s_game_dir, sizeof s_game_dir, game_dir);
}

static uint32_t u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* The cash register's samples, handed to the toolkit once. */
static int load(void)
{
    char path[MAX_PATH];
    uint8_t hdr[0x60], ent[0x18];
    uint32_t seg0, count, esz, fmt, off, len;
    int16_t *pcm = NULL;
    FILE *f = NULL;
    const char *why = NULL;

    if (s_loaded)
        return s_loaded > 0;
    sprintf_s(path, sizeof path, "%s\\%s", s_game_dir, SPK_FILE);
    if (fopen_s(&f, path, "rb") || !f) {
        why = "file missing";
        goto out;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "WBND", 4) || u32(hdr + 4) != 3) {
        why = "not a version 3 wave bank";
        goto out;
    }
    seg0 = u32(hdr + 8);
    if (seg0 + 0x1C > sizeof hdr) {
        why = "header";
        goto out;
    }
    count = u32(hdr + seg0 + 4);
    esz = u32(hdr + seg0 + 0x18);
    if (SPK_WAVE >= count || esz < 0x10 || esz > sizeof ent
        || fseek(f, (long)(u32(hdr + 16) + SPK_WAVE * esz), SEEK_SET)
        || fread(ent, 1, esz, f) != esz) {
        why = "wave table";
        goto out;
    }
    fmt = u32(ent + 4);
    off = u32(hdr + 32) + u32(ent + 8);
    len = u32(ent + 0xC);
    if ((fmt & 3) || ((fmt >> 2) & 7) != 1 || !(fmt >> 31)
        || ((fmt >> 5) & 0x3FFFFFFu) != SPK_RATE || len != SPK_BYTES) {
        why = "not the cash register (another version of the game?)";
        goto out;
    }
    pcm = malloc(len);
    if (!pcm || fseek(f, (long)off, SEEK_SET) || fread(pcm, 1, len, f) != len) {
        why = "read";
        goto out;
    }
    xbox_ApuSpeakerTestSound(pcm, (int)(len / 2), (int)SPK_RATE);   /* copied there */
out:
    if (f)
        fclose(f);
    free(pcm);
    InterlockedExchange(&s_loaded, why ? -1 : 1);
    if (why)
        fprintf(stderr, "[SPKTEST] %s: %s; no speaker test\n", path, why);
    else
        fprintf(stderr, "[SPKTEST] the cash register (%s wave %u) handed to the APU\n",
                SPK_FILE, SPK_WAVE);
    return !why;
}

int speaker_test_start(int speaker)
{
    if (speaker < 0)
        return speaker_test_stop(), 1;
    if (!load())
        return 0;
    xbox_ApuSpeakerTest(speaker);
    return 1;
}

void speaker_test_stop(void)
{
    xbox_ApuSpeakerTest(-1);
}

int speaker_test_playing(void)
{
    return xbox_ApuSpeakerTest(APU_SPEAKER_ASK);
}

int speaker_test_channels(void)
{
    return xbox_ApuOutputChannels();
}

static const struct { const char *name; int spk; } k_names[] = {
    { "left", SPK_FRONT_LEFT }, { "fl", SPK_FRONT_LEFT }, { "right", SPK_FRONT_RIGHT },
    { "fr", SPK_FRONT_RIGHT }, { "centre", SPK_CENTRE }, { "center", SPK_CENTRE },
    { "c", SPK_CENTRE }, { "subwoofer", SPK_SUBWOOFER }, { "lfe", SPK_SUBWOOFER },
    { "sub", SPK_SUBWOOFER }, { "backleft", SPK_BACK_LEFT }, { "bl", SPK_BACK_LEFT },
    { "backright", SPK_BACK_RIGHT }, { "br", SPK_BACK_RIGHT }, { "both", SPK_BOTH },
    { "all", SPK_ALL },
};

int speaker_test_parse(const char *name)
{
    if (!name)
        return -1;
    if (name[0] >= '0' && name[0] <= '7' && !name[1])
        return name[0] - '0';
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (!_stricmp(name, k_names[i].name))
            return k_names[i].spk;
    return -1;
}

const char *speaker_test_name(int speaker)
{
    static const char *const n[8] = {
        "front left", "front right", "centre", "subwoofer", "back left", "back right",
        "both front", "all in turn"
    };
    return speaker >= 0 && speaker < 8 ? n[speaker] : "none";
}

void speaker_test_frame(void)
{
    static ULONGLONG first;
    static int done;
    const char *e;
    if (done)
        return;
    if (!first)
        first = GetTickCount64();
    if (GetTickCount64() - first < 3000)
        return;
    done = 1;
    e = getenv("RECOMP_SPEAKER_TEST");
    if (e && *e) {
        int s = speaker_test_parse(e);
        fprintf(stderr, "[SPKTEST] RECOMP_SPEAKER_TEST=%s: %s\n", e, speaker_test_name(s));
        if (s >= 0)
            speaker_test_start(s);
    }
}
