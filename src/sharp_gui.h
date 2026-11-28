/*
 * Sharper text and buttons (to-do 53 step 5): the game's own font sheets and
 * controller button pictures, enlarged and cleaned up on the player's PC from
 * their own game files, and loaded as the lowest texture pack. See
 * sharp_gui.c. Nothing it makes leaves their PC (SPEC 3.3).
 */
#ifndef SHARP_GUI_H
#define SHARP_GUI_H

#include <stddef.h>
#include <stdint.h>

#include "caff.h"

#define SHARP_DIR    "sharp"    /* below <save dir>\assets\ */
/* The stamp's "sharp": a new number (other maths, other pictures) makes
 * every player's set again at the launcher's next check. */
#define SHARP_FORMAT 1
#define SHARP_SCALE  4          /* x4: a 32-pixel button 128, the fonts up to 4096 */
#define SHARP_EDGE   1.25f      /* an edge ~1.25 new pixels wide round its middle */
#define SHARP_SHARPEN 1.0f      /* the buttons' unsharp mask (0.5 soft, 2.5 grainy) */

/* ── The clean-up (scripts/make_font_sheets.py, make_button_pictures.py) ── */

/* A font sheet (8-bit grey, w x h) into out ((w*scale) x (h*scale) bytes):
 * enlarged by a cubic B-spline, each spot over the brightest value within one
 * original pixel (at least 0.3), the edges made `edge` new pixels wide round
 * the 0.5 line. 0, or -1 (bad size, out of memory). */
int sharp_font_sheet(const uint8_t *sheet, uint32_t w, uint32_t h, int scale, float edge,
                     uint8_t *out);

/* A picture (RGBA, w x h) into out ((w*scale) x (h*scale) x 4): colour
 * (weighted by alpha) and alpha enlarged by the B-spline; the outline crisp
 * round the 0.5 line (an on/off outline smoothed half an original pixel
 * first); colour sharpened by `sharpen` (unsharp mask over about one original
 * pixel); edge colours carried into the clear area. 0, or -1. */
int sharp_picture(const uint8_t *rgba, uint32_t w, uint32_t h, int scale, float edge,
                  float sharpen, uint8_t *out);

/* A font file (aid\font\<Font>\default.bin) -> its sheet: *sheet points into
 * `file`. 0, or -1 when its layout isn't the one the four fonts have. */
int sharp_font_sheet_of(const uint8_t *file, size_t n, const uint8_t **sheet, uint32_t *w,
                        uint32_t *h);

/* ── Making the set ─────────────────────────────────────────────────────── */

/* Each file made (rel = "sharp\<HASH>_<name>.png", below assets\) or failed. */
typedef void (*SharpFileFn)(const char *rel, uint64_t size, int failed, void *ctx);
/* Before each of the SHARP_STEPS pieces: what it reads ("font\ConkerFont",
 * "General\Boot"), for the launcher's progress line. */
typedef void (*SharpStepFn)(const char *what, void *ctx);
#define SHARP_STEPS 7           /* 4 fonts, 3 packages */

typedef struct {
    int made, failed;           /* pictures */
    unsigned long long bytes;   /* of the PNGs written */
} SharpResult;

/* The whole set from the game in `game` (its dvddata\aid) into
 * <save>\assets\sharp\ (the folder must be empty or hold only an older set:
 * the caller clears it). Packages are opened with `unpack` (the carve's
 * guest). A picture that can't be read or written is a [SHARP] line and
 * counted in `failed`; the rest go on. 0, or -1 when the folder can't be made. */
int sharp_make(const char *game, const char *save, CaffUnpackFn unpack, SharpStepFn step,
               SharpFileFn file, void *ctx, SharpResult *result);

/* ── In the game ────────────────────────────────────────────────────────── */

/* F2 Display "Sharper Text and Buttons" (sharp_gui, RECOMP_SHARP_GUI): on
 * (default) loads <save>\assets\sharp as the lowest texture pack. */
int  sharp_gui_on(void);
void conker_SetSharpGui(int on);

#endif
