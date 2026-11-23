/**
 * Full DSP (to-do 50): Conker's side of the toolkit's native GP effects
 * (audio_fx.c): the I3DL2 listener and the Processing setting.
 */
#ifndef CONKER_AUDIO_FX_H
#define CONKER_AUDIO_FX_H

#ifdef __cplusplus
extern "C" {
#endif

void audio_fx_frame(void);   /* game thread, once a frame */

/* Start-up, after settings.ini and --debug are known: settings.ini [audio]
 * processing (full_dsp by default) as RECOMP_APU_PROCESSING, unless
 * that is already set (then it decides). */
void audio_fx_init(void);

/* settings.ini processing to the APU at once (crossfaded); nothing while
 * RECOMP_APU_PROCESSING was set before the game. The F2 menu; any thread. */
void audio_fx_apply_settings(void);

/* The console's processing command: 1 = Full DSP, 0 = Standard, whatever
 * RECOMP_APU_PROCESSING said. Any thread. */
void audio_fx_set_processing(int full);

/* 1 = Full DSP is asked for. */
int audio_fx_processing(void);

/* 1 = RECOMP_APU_PROCESSING was set before the game (settings.ini unused). */
int audio_fx_env_decides(void);

/* Output (step 4; settings.ini [audio] output):
 * the toolkit's APU_OUTPUT_* values. */
#define AUDIO_OUT_STEREO     0
#define AUDIO_OUT_HEADPHONES 1   /* Full DSP without the speaker crosstalk */
#define AUDIO_OUT_SURROUND   2   /* 5.1, from the start only; always Full DSP */
#define AUDIO_OUT_MONO       3   /* both sides the same (to-do 230) */
#define AUDIO_OUT_COUNT      4

/* The console's output command and the F2 row (through
 * audio_fx_apply_settings): Stereo / Headphones / Mono at once (crossfaded),
 * 5.1 from the next start. Any thread. */
void audio_fx_set_output(int out);
int audio_fx_output(void);          /* asked */
int audio_fx_output_started(void);  /* this run's (5.1 or not is fixed) */
int audio_fx_output_waits(void);    /* 1 = 5.1 asked or left: next start */
int audio_fx_output_env_decides(void);   /* RECOMP_APU_OUTPUT was set */
const char *audio_fx_output_key(int out);   /* settings.ini value */

#ifdef __cplusplus
}
#endif

#endif
