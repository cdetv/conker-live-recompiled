/* GPU busy (to-do 220): this process's share of the video card's 3D engine,
 * from Windows' GPU usage counter (gpu_busy.c). */
#ifndef CONKER_GPU_BUSY_H
#define CONKER_GPU_BUSY_H

#ifdef __cplusplus
extern "C" {
#endif

/* On while F1's "what limits" or Automatic resolution needs it: a thread
 * collects every 250 ms. Off: it idles. */
void gpu_busy_want(int on);

/* The 3D engine's busy share (0..1) since the last call; -1 when none was
 * read (just switched on, or no counter on this system). */
double gpu_busy_take(void);

#ifdef __cplusplus
}
#endif

#endif
