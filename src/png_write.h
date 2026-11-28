/*
 * PNG files without a library (to-do 238): 8 bits per channel, grey, grey +
 * alpha, RGB or RGBA, every row filter 0, not interlaced, one IDAT. A C port
 * of a texture extractor's PNG writer, compressed by src/deflate.c.
 */
#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stddef.h>
#include <stdint.h>

/* The bytes of a PNG file (malloc'd) of `pixels`: rows top to bottom, each
 * w * channels bytes. channels 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA.
 * 0, or -1 (bad size or channels, out of memory). */
int png_encode(uint32_t w, uint32_t h, int channels, const uint8_t *pixels,
               uint8_t **out, size_t *n_out);

#endif /* PNG_WRITE_H */
