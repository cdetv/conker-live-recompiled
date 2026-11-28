/*
 * CRC-32 and deflate with fixed Huffman codes (RFC 1951 3.2.6), plain C, no
 * library: the bug report's zip (src/bug_report.c) and the texture pack
 * kit's PNGs (src/png_write.c, to-do 238). Moved out of bug_report.c
 * unchanged (2026-11-26). Logs shrink about 5x; one final block, LZ77 with
 * a 32 KB window and hash chains of 48.
 */
#ifndef DEFLATE_H
#define DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* zlib's convention: start from 0, chain calls over pieces. */
uint32_t deflate_crc32(uint32_t crc, const uint8_t *p, size_t n);

/* Raw deflate of in[0..n) into a malloc'd *out (*n_out bytes). 0 or -1 (out
 * of memory). Safe from several threads at once. */
int deflate_fixed(const uint8_t *in, size_t n, uint8_t **out, size_t *n_out);

#endif /* DEFLATE_H */
