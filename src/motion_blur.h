/**
 * Camera motion blur (to-do 235, 0.7.0; motion_blur.c): how the render
 * camera moved since the last frame, handed to the runtime's motion blur
 * (toolkit host effect NV_HOST_FX_MBLUR = 11) during quick turns.
 */
#ifndef CONKER_MOTION_BLUR_H
#define CONKER_MOTION_BLUR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBLUR_PARAMS 15

/* The end of view 0's 3D picture (the dof_update wrapper): 1 when the
 * runtime should blur this frame, with its parameters in prm; gpu: a back end
 * that draws host effects. Called once a frame, on the game's thread. */
int mblur_frame(int gpu, uint32_t prm[MBLUR_PARAMS]);

/* Mode 0 off, 1 on, 2 the motion as colour (testing); strength 0-100 %. A
 * value out of range keeps the current one. Get: the values in effect. */
void conker_SetMotionBlur(int mode, float strength);
void conker_GetMotionBlur(int *mode, float *strength);

/* Tuning (the console; this run only): none under lo, all from hi degrees a
 * second; the longest smear (guest pixels, 1-128); the middle kept sharp
 * (share of the half diagonal, 0-1). Out of range keeps the current one. */
void conker_SetMotionBlurTuning(float lo, float hi, float max_px,
                                float middle);
void conker_GetMotionBlurTuning(float *lo, float *hi, float *max_px,
                                float *middle);

#ifdef __cplusplus
}
#endif

#endif
