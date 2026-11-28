/**
 * Per-pixel character lighting (to-do 51, 0.7.0): Conker's side.
 *
 * Conker lights characters (and the props they hold, and placed objects)
 * per vertex in 128 vertex programs: an ambient term from the normal, a key
 * light, and "blobs", the soft round shadows characters cast on each other.
 * Interpolated over each triangle, the key light's edge crawls along tails
 * and arms as they move. The toolkit can work the same maths out per pixel
 * (xboxrecomp nv2a_vsh_lit.c) once it knows which constant rows the
 * programs light with; these are Conker's (hardware numbers; D3D's c0 is
 * c96, so the blob rows are D3D's c50-c61):
 *
 *   c179        key light direction (the programs dot the normal with -c179)
 *   c180        key light colour (w 0.1)
 *   c166-c177   ambient: 3 channels x 4 rows, two DPH stages (spherical
 *               harmonics irradiance)
 *   c146-c157   blobs: 3 groups of 4 rows (x, y, clip height, strength),
 *               one blob per component; x / y / height come from the
 *               vertex placed in the light's frame (c159-c161)
 *
 * Written per character by 0x0012FE60 / 0x0012F500 from the level's
 * lights.bin lights, zeroed by 0x0012FAC0 at the opaque pass (research
 * r3-looks [51] B1; conker-notes findings 2026-11-25 "lit vertex
 * programs"). The scan checks every program against the maths itself, so
 * a program that lights differently stays per vertex.
 */
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include "char_lighting.h"

void xbox_SetLitPattern(int key_dir, int key_col, int amb_first,
                        int blob_first, int blob_groups);
void xbox_SetPerPixelLighting(int mode);
int nv2a_vsh_lit_mode(void);
void xbox_SetLitView(int view);
int nv2a_vsh_lit_view(void);
void xbox_SetBlobSoftness(int ramp, int knee);
void nv2a_vsh_lit_soft(int *ramp, int *knee);

/* The toolkit's views (nv2a_vsh_lit.h NV2A_LIT_VIEW_*), by name. */
const char *const g_lit_view_names[7] = {
    "off", "vertex", "key", "ambient", "blobs", "term", "face"
};

/* 0 off, 1 on, 2 even frames only (the held-picture check), at once. */
void conker_SetPerPixel(int mode)
{
    xbox_SetPerPixelLighting(mode);
}

int conker_GetPerPixel(void)
{
    return nv2a_vsh_lit_mode();
}

/* A debug view by its number (g_lit_view_names), at once. */
void conker_SetLitView(int view)
{
    xbox_SetLitView(view);
}

int conker_GetLitView(void)
{
    return nv2a_vsh_lit_view();
}

/* The blob shadows' softness per pixel (xbox_SetBlobSoftness: ramp 0-255,
 * knee 0-100), at once; for tuning (console lit soft). */
void conker_SetBlobSoft(int ramp, int knee)
{
    xbox_SetBlobSoftness(ramp, knee);
}

void conker_GetBlobSoft(int *ramp, int *knee)
{
    nv2a_vsh_lit_soft(ramp, knee);
}

#define LIT_KEY_DIR    179
#define LIT_KEY_COLOUR 180
#define LIT_AMBIENT    166
#define LIT_BLOBS      146
#define LIT_BLOB_GROUPS  3

/* The blobs' softness per pixel (to-do 51 step 4; fixed, no setting): the
 * program's hard "at or above the clip height" step as a ramp BLOB_RAMP
 * hundredths of sqrt(strength) either side, the sum reaching full shadow
 * through a corner BLOB_KNEE hundredths either side of 1. Tuned by eye
 * 2026-11-26 (BeachRunup, cutscene TrainingLevel 2, the warp cp 10
 * cutscene): 25 still shows the step as a line, 75 and up wash the shadow
 * over the whole body; knee 0 leaves an edge where the sum reaches 1, 100
 * lifts the shadow's middle. */
#define BLOB_RAMP 50
#define BLOB_KNEE 50

/* At start-up, before anything draws (main.c). RECOMP_PERPIXEL=0|1|2 (2 =
 * even frames only, the held-picture check with cmp_alt.py); unset = on.
 * settings.ini [picture] per_pixel_lighting (F2 Graphics) sets it to 0 when
 * off (settings.c); the F2 checkbox and the presets switch it at once
 * through conker_SetPerPixel.
 * RECOMP_LIT_VIEW=vertex|key|ambient|blobs|term|face: a debug view from
 * the start (the console's lit view). RECOMP_BLOB_SOFT=<ramp>[,<knee>]:
 * the blobs' softness in place of BLOB_RAMP / BLOB_KNEE (tests; 0,0 = the
 * programs' hard blobs per pixel too). */
void char_lighting_init(void)
{
    const char *e = getenv("RECOMP_PERPIXEL");
    const char *v = getenv("RECOMP_LIT_VIEW");
    const char *s = getenv("RECOMP_BLOB_SOFT");
    int i, ramp = BLOB_RAMP, knee = BLOB_KNEE;

    xbox_SetLitPattern(LIT_KEY_DIR, LIT_KEY_COLOUR, LIT_AMBIENT, LIT_BLOBS,
                       LIT_BLOB_GROUPS);
    xbox_SetPerPixelLighting(e && e[0] ? atoi(e) : 1);
    if (s && s[0]) {
        const char *c = strchr(s, ',');
        ramp = atoi(s);
        if (c)
            knee = atoi(c + 1);
    }
    xbox_SetBlobSoftness(ramp, knee);
    for (i = 1; v && i < 7; i++)
        if (!strcmp(v, g_lit_view_names[i]))
            xbox_SetLitView(i);
}
