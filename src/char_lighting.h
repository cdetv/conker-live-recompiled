/**
 * Per-pixel character lighting (to-do 51, 0.7.0; char_lighting.c): Conker's
 * lit vertex programs' rows handed to the toolkit, the setting and its
 * debug views.
 */
#ifndef CONKER_CHAR_LIGHTING_H
#define CONKER_CHAR_LIGHTING_H

#ifdef __cplusplus
extern "C" {
#endif

/* At start-up, before anything draws (main.c). */
void char_lighting_init(void);

/* 0 off (per vertex, the original), 1 on, 2 even frames only (the
 * held-picture check), at once. Get: the mode in effect. */
void conker_SetPerPixel(int mode);
int conker_GetPerPixel(void);

/* A debug view by its number (g_lit_view_names: off, vertex, key, ambient,
 * blobs, term, face), at once. */
extern const char *const g_lit_view_names[7];
void conker_SetLitView(int view);
int conker_GetLitView(void);

/* The blob shadows' softness per pixel (ramp 0-255, knee 0-100 hundredths;
 * tuning, this run only). */
void conker_SetBlobSoft(int ramp, int knee);
void conker_GetBlobSoft(int *ramp, int *knee);

#ifdef __cplusplus
}
#endif

#endif
