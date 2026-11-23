/**
 * Full DSP (to-do 50, SPEC 10.1 Audio): the game's I3DL2 listener to the APU.
 *
 * The effects image's reverb (toolkit apu_fx.c) takes its parameters from the
 * I3DL2 listener, which DirectSound keeps in its own object where the toolkit
 * can't find it. Every listener change goes through XACT (0x0054EE0C), which
 * keeps a copy at 0x005557A4: 12 dwords, a DSI3DL2LISTENER (lRoom, lRoomHF,
 * flRoomRolloffFactor, flDecayTime, flDecayHFRatio, lReflections,
 * flReflectionsDelay, lReverb, flReverbDelay, flDiffusion, flDensity,
 * flHFReference; docs/research/r4-0.5.0-sound-plan.md). Sent to the APU
 * whenever it changes. Story levels hold preset 25 (no reverb, -10000 mB), so
 * the reverb stays silent there, as on the console.
 *
 * Processing (step 3; public from 0.5.0, Full DSP the default, user
 * 2026-11-22): settings.ini [audio] processing = full_dsp | standard goes to
 * the APU at start and from the F2 menu, the console's processing command at
 * any time; the toolkit crossfades a switch (xbox_ApuSetAudioMode).
 *
 * Output (step 4, the same rules): settings.ini [audio] output = stereo |
 * headphones | surround | mono as RECOMP_APU_OUTPUT. Stereo, Headphones
 * (Full DSP without the speaker crosstalk) and Mono (both sides the same,
 * to-do 230; the game is not told) switch at once; surround (5.1) only at
 * start: the kernel's XC_AUDIO tells the game at boot and the toolkit picks
 * the channels then, and it always plays Full DSP.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "audio_fx.h"
#include "settings.h"

void xbox_ApuSetI3dl2Listener(const void *listener48);   /* toolkit apu.h */
void xbox_ApuSetAudioMode(int processing, int output);   /* toolkit apu.h */
#define APU_PROCESSING_STANDARD 0
#define APU_PROCESSING_FULL     1

static volatile int s_full = -1;   /* asked: 1 Full DSP, 0 Standard, -1 not yet */
static int s_env_user;             /* RECOMP_APU_PROCESSING was set before the game */
static volatile int s_out = AUDIO_OUT_STEREO;   /* asked: AUDIO_OUT_* */
static int s_out_started = AUDIO_OUT_STEREO;    /* this run's start (5.1 is fixed) */
static int s_out_env_user;         /* RECOMP_APU_OUTPUT was set before the game */
static const char *const k_out_key[AUDIO_OUT_COUNT] = {
    "stereo", "headphones", "surround", "mono"
};

static int setting_full(void)
{
    return !_stricmp(settings_get("processing"), "full_dsp");
}

static int out_of(const char *v)
{
    for (int i = 0; i < AUDIO_OUT_COUNT; i++)
        if (v && !_stricmp(v, k_out_key[i]))
            return i;
    return AUDIO_OUT_STEREO;
}

static int setting_out(void)
{
    return out_of(settings_get("output"));
}

/* At start-up, after settings.ini: the toolkit reads RECOMP_APU_PROCESSING
 * and RECOMP_APU_OUTPUT at the APU's first frame, so the boot movie already
 * plays as chosen; the kernel answers the game's XC_AUDIO from the second. */
void audio_fx_init(void)
{
    const char *env = getenv("RECOMP_APU_PROCESSING");
    const char *oenv = getenv("RECOMP_APU_OUTPUT");
    s_env_user = env && *env;
    if (s_env_user)
        fprintf(stderr, "[AUDIO] processing: RECOMP_APU_PROCESSING=%s decides\n", env);
    else if (setting_full())
        _putenv_s("RECOMP_APU_PROCESSING", "full");
    s_full = s_env_user ? !strcmp(env, "full") : setting_full();
    s_out_env_user = oenv && *oenv;
    if (s_out_env_user)
        fprintf(stderr, "[AUDIO] output: RECOMP_APU_OUTPUT=%s decides\n", oenv);
    else if (setting_out() != AUDIO_OUT_STEREO)
        _putenv_s("RECOMP_APU_OUTPUT", k_out_key[setting_out()]);
    s_out = s_out_env_user ? out_of(oenv) : setting_out();
    s_out_started = s_out;
    fprintf(stderr, "[AUDIO] processing: %s at start\n",
            s_out == AUDIO_OUT_SURROUND ? "full_dsp (5.1 surround always uses it)"
            : s_full ? "full_dsp" : "standard");
    fprintf(stderr, "[AUDIO] output: %s at start\n", k_out_key[s_out]);
}

static void send_mode(void)
{
    xbox_ApuSetAudioMode(s_full > 0 ? APU_PROCESSING_FULL : APU_PROCESSING_STANDARD, s_out);
}

void audio_fx_set_processing(int full)
{
    s_full = full != 0;
    send_mode();
    fprintf(stderr, "[AUDIO] processing: %s%s\n", full ? "full_dsp" : "standard",
            s_out_started == AUDIO_OUT_SURROUND ? " (5.1 surround plays full_dsp anyway)" : "");
}

void audio_fx_set_output(int out)
{
    s_out = out >= AUDIO_OUT_STEREO && out < AUDIO_OUT_COUNT ? out : AUDIO_OUT_STEREO;
    send_mode();
    fprintf(stderr, "[AUDIO] output: %s%s\n", k_out_key[s_out],
            audio_fx_output_waits() ? " (5.1 surround changes from the next start)" : "");
}

void audio_fx_apply_settings(void)
{
    if (!s_env_user)
        audio_fx_set_processing(setting_full());
    if (!s_out_env_user)
        audio_fx_set_output(setting_out());
}

int audio_fx_output(void)
{
    return s_out;
}

int audio_fx_output_started(void)
{
    return s_out_started;
}

int audio_fx_output_waits(void)
{
    return (s_out == AUDIO_OUT_SURROUND) != (s_out_started == AUDIO_OUT_SURROUND);
}

int audio_fx_output_env_decides(void)
{
    return s_out_env_user;
}

const char *audio_fx_output_key(int out)
{
    return k_out_key[out >= AUDIO_OUT_STEREO && out < AUDIO_OUT_COUNT ? out : 0];
}

int audio_fx_processing(void)
{
    return s_full > 0;
}

int audio_fx_env_decides(void)
{
    return s_env_user;
}

#define XACT_I3DL2_LISTENER 0x005557A4u

static float f32_of(uint32_t w)
{
    float f;
    memcpy(&f, &w, 4);
    return f;
}

/* Before XACT sets one the copy is zeros, which would read as a reverb at
 * full level: only a listener inside DirectSound's ranges is sent. */
static int listener_valid(const uint32_t l[12])
{
    float decay = f32_of(l[3]), ratio = f32_of(l[4]), diffusion = f32_of(l[9]);
    float density = f32_of(l[10]), ref = f32_of(l[11]);
    return (int32_t)l[0] <= 0 && (int32_t)l[0] >= -10000 &&
           decay >= 0.1f && decay <= 20.0f && ratio >= 0.1f && ratio <= 2.0f &&
           diffusion >= 0.0f && diffusion <= 100.0f &&
           density >= 0.0f && density <= 100.0f && ref >= 20.0f && ref <= 20000.0f;
}

void audio_fx_frame(void)
{
    static uint32_t last[12];
    static int sent;
    uint32_t now[12];

    for (int i = 0; i < 12; i++)
        now[i] = MEM32(XACT_I3DL2_LISTENER + 4u * (uint32_t)i);
    if (sent && !memcmp(now, last, sizeof now))
        return;
    if (!listener_valid(now))
        return;
    memcpy(last, now, sizeof now);
    sent = 1;
    xbox_ApuSetI3dl2Listener(now);
}
