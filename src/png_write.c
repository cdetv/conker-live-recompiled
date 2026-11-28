/*
 * PNG files without a library (png_write.h): a texture extractor's PNG writer in C.
 * A zlib stream is a 2-byte header, the deflate data and the Adler-32 of
 * the raw bytes (RFC 1950); every chunk carries a CRC-32 of its type and
 * body (PNG spec 5.3).
 */
#include "png_write.h"

#include <stdlib.h>
#include <string.h>

#include "deflate.h"

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;

    while (n) {
        size_t k = n < 5552 ? n : 5552;     /* no overflow before the mod */

        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* One chunk at out: length, type, body, CRC of type + body. */
static uint8_t *chunk(uint8_t *out, const char *type, const uint8_t *body, uint32_t n)
{
    uint32_t crc;

    be32(out, n);
    memcpy(out + 4, type, 4);
    if (n)
        memcpy(out + 8, body, n);
    crc = deflate_crc32(0, out + 4, (size_t)n + 4);
    be32(out + 8 + n, crc);
    return out + 12 + n;
}

int png_encode(uint32_t w, uint32_t h, int channels, const uint8_t *pixels,
               uint8_t **out, size_t *n_out)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    static const uint8_t colour[5] = { 0, 0, 4, 2, 6 };  /* channels -> grey, grey + alpha, RGB, RGBA */
    uint8_t *raw, *z = NULL, *file, *p, ihdr[13];
    size_t stride, nraw, nz, nidat, y;
    uint32_t adler;

    *out = NULL;
    *n_out = 0;
    if (channels < 1 || channels > 4 || !w || !h || w > 65536 || h > 65536)
        return -1;
    stride = (size_t)w * (size_t)channels;
    nraw = (stride + 1) * h;
    if (!(raw = malloc(nraw)))
        return -1;
    for (y = 0; y < h; y++) {               /* filter 0 (none) on every row */
        raw[y * (stride + 1)] = 0;
        memcpy(raw + y * (stride + 1) + 1, pixels + y * stride, stride);
    }
    adler = adler32(raw, nraw);
    if (deflate_fixed(raw, nraw, &z, &nz)) {
        free(raw);
        return -1;
    }
    free(raw);
    nidat = 2 + nz + 4;
    if (nidat > 0x7FFFFFFFu || !(file = malloc(8 + 25 + 12 + nidat + 12))) {
        free(z);
        return -1;
    }
    memcpy(file, sig, 8);
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;                            /* bits per channel */
    ihdr[9] = colour[channels];
    ihdr[10] = ihdr[11] = ihdr[12] = 0;     /* deflate, filter set 0, not interlaced */
    p = chunk(file + 8, "IHDR", ihdr, 13);
    /* IDAT: built in place (the zlib stream is its body) */
    be32(p, (uint32_t)nidat);
    memcpy(p + 4, "IDAT", 4);
    p[8] = 0x78;                            /* deflate, 32 KB window */
    p[9] = 0x01;                            /* fastest; (0x7801 % 31) == 0 */
    memcpy(p + 10, z, nz);
    be32(p + 10 + nz, adler);
    be32(p + 8 + nidat, deflate_crc32(0, p + 4, nidat + 4));
    p += 12 + nidat;
    free(z);
    p = chunk(p, "IEND", NULL, 0);
    *out = file;
    *n_out = (size_t)(p - file);
    return 0;
}
