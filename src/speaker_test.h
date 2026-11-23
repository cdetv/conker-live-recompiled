/**
 * Speaker test (to-do 231, 0.5.0; F2 Sound): the game's own cash register
 * on one speaker at a time (speaker_test.c).
 */
#ifndef CONKER_SPEAKER_TEST_H
#define CONKER_SPEAKER_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* The toolkit's speaker numbers (apu.h xbox_ApuSpeakerTest). */
#define SPK_FRONT_LEFT   0
#define SPK_FRONT_RIGHT  1
#define SPK_CENTRE       2
#define SPK_SUBWOOFER    3
#define SPK_BACK_LEFT    4
#define SPK_BACK_RIGHT   5
#define SPK_BOTH         6   /* both front speakers at once (Mono) */
#define SPK_ALL          7   /* each speaker of the output in turn */

/* main.c, before the game runs: where the game's files are. */
void speaker_test_init(const char *game_dir);

/* Once a game frame (recomp_manual.c, next to audio_fx_frame):
 * RECOMP_SPEAKER_TEST=<speaker> plays once, 3 s after the first frame. */
void speaker_test_frame(void);

/* Plays the sound on that speaker (SPK_*; any thread). 0 = nothing to play
 * (the game's wave not found; logged). */
int speaker_test_start(int speaker);
void speaker_test_stop(void);

/* The speaker playing now (SPK_*), or -1. */
int speaker_test_playing(void);

/* The channels the output makes: 2, or 6 for 5.1 on a six-speaker device. */
int speaker_test_channels(void);

/* "left", "fl", "all" ... -> SPK_*, -1 for none of them. */
int speaker_test_parse(const char *name);
const char *speaker_test_name(int speaker);

#ifdef __cplusplus
}
#endif

#endif
