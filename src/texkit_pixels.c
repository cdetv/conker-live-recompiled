/*
 * The texture pack kit's pixels (to-do 238; texkit.h): level 0 of a picture
 * as RGBA, a C port of a texture extractor's pixel decoding (the
 * "Correct" column of its texture format notes, section 6):
 *
 *   swizzled  texel (x, y) at the index whose bits come from x, y (and z)
 *             in turn, lowest first, x first, each axis while it has bits;
 *             slice 0 of a volume = the texels with z = 0
 *   DXT       4x4 blocks row by row, endpoints rounded to 8 bits, the 1/3
 *             and 2/3 points rounded, DXT1's 3-colour mode when c0 <= c1
 *             (its 4th colour transparent black), DXT5 alpha with "+ 1"
 *   texels    L8 grey opaque; A8L8 grey + alpha; X8R8G8B8 bytes B G R X;
 *             V8U8 each byte ^ 0x80, blue 128; A8B8G8R8 bytes R G B A as
 *             they are; A4R4G4B4 x 17; R5G6B5 widened by copying top bits
 */
#include "texkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t dxt_bytes(uint32_t f)
{
    return f == 0x0C ? 8 : f == 0x0E || f == 0x0F ? 16 : 0;
}

static uint32_t texel_size(uint32_t f)
{
    switch (f) {
    case 0x00: return 1;
    case 0x04: case 0x05: case 0x1A: case 0x28: return 2;
    case 0x07: case 0x3A: return 4;
    default:   return 0;
    }
}

int texkit_png_channels(uint32_t format)
{
    switch (format) {
    case 0x00: return 1;                            /* L8: grey */
    case 0x1A: return 2;                            /* A8L8: grey + alpha */
    case 0x05: case 0x07: case 0x28: return 3;      /* no alpha in the texture */
    default:   return 4;
    }
}

static int log2u(uint32_t v)
{
    int n = 0;

    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

/* ── Swizzled ───────────────────────────────────────────── */

static void texel_rgba(uint32_t f, const uint8_t *t, uint8_t *o)
{
    uint32_t v, r, g, b;

    switch (f) {
    case 0x00:                                      /* L8 */
        o[0] = o[1] = o[2] = t[0];
        o[3] = 255;
        break;
    case 0x1A:                                      /* A8L8: L, A */
        o[0] = o[1] = o[2] = t[0];
        o[3] = t[1];
        break;
    case 0x07:                                      /* X8R8G8B8: B, G, R, X */
        o[0] = t[2];
        o[1] = t[1];
        o[2] = t[0];
        o[3] = 255;
        break;
    case 0x28:                                      /* V8U8: signed U, V */
        o[0] = (uint8_t)(t[0] ^ 0x80);
        o[1] = (uint8_t)(t[1] ^ 0x80);
        o[2] = 128;
        o[3] = 255;
        break;
    case 0x3A:                                      /* A8B8G8R8: R, G, B, A */
        memcpy(o, t, 4);
        break;
    case 0x04:                                      /* A4R4G4B4 */
        v = (uint32_t)t[0] | (uint32_t)t[1] << 8;
        o[0] = (uint8_t)((v >> 8 & 15) * 17);
        o[1] = (uint8_t)((v >> 4 & 15) * 17);
        o[2] = (uint8_t)((v & 15) * 17);
        o[3] = (uint8_t)((v >> 12 & 15) * 17);
        break;
    default:                                        /* 0x05 R5G6B5 */
        v = (uint32_t)t[0] | (uint32_t)t[1] << 8;
        r = v >> 11;
        g = v >> 5 & 63;
        b = v & 31;
        o[0] = (uint8_t)(r << 3 | r >> 2);
        o[1] = (uint8_t)(g << 2 | g >> 4);
        o[2] = (uint8_t)(b << 3 | b >> 2);
        o[3] = 255;
        break;
    }
}

static int decode_swizzled(uint32_t f, const uint8_t *data, size_t n, uint32_t w, uint32_t h,
                           uint32_t d, uint8_t *rgba)
{
    uint32_t *wx, *wy, size = texel_size(f), x, y, count;
    int bits[3], level, axis, top, out = 0;

    if (!w || !h || !d || (w & (w - 1)) || (h & (h - 1)) || (d & (d - 1)))
        return -1;                      /* a swizzled texture is a power of two */
    bits[0] = log2u(w);
    bits[1] = log2u(h);
    bits[2] = log2u(d);
    top = bits[0] > bits[1] ? bits[0] : bits[1];
    if (bits[2] > top)
        top = bits[2];
    wx = calloc(w, sizeof *wx);
    wy = calloc(h, sizeof *wy);
    if (!wx || !wy) {
        free(wx);
        free(wy);
        return -2;
    }
    for (level = 0; level < top; level++)
        for (axis = 0; axis < 3; axis++) {
            if (level >= bits[axis])
                continue;
            if (axis == 0)
                for (x = 0; x < w; x++)
                    wx[x] |= ((x >> level) & 1u) << out;
            else if (axis == 1)
                for (y = 0; y < h; y++)
                    wy[y] |= ((y >> level) & 1u) << out;
            out++;                      /* z: slice 0 has 0 there */
        }
    count = (wx[w - 1] | wy[h - 1]) + 1;
    if ((uint64_t)count * size > n) {
        free(wx);
        free(wy);
        return -1;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            texel_rgba(f, data + (size_t)(wx[x] | wy[y]) * size, rgba + ((size_t)y * w + x) * 4);
    free(wx);
    free(wy);
    return 0;
}

/* ── DXT ────────────────────────────────────────────────── */

static void endpoint(uint32_t c, int o[3])
{
    o[0] = (int)(((c >> 11) * 527 + 23) >> 6);         /* round(v * 255 / 31) */
    o[1] = (int)((((c >> 5) & 63) * 259 + 33) >> 6);   /* round(v * 255 / 63) */
    o[2] = (int)(((c & 31) * 527 + 23) >> 6);
}

/* 16 texels (i = row * 4 + column) of an 8-byte colour block */
static void colour_block(const uint8_t *b, int three_colour_mode, uint8_t px[16][4])
{
    uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8, c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8;
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    int a[3], e[3], pal[4][4], k, four = !three_colour_mode || c0 > c1;

    endpoint(c0, a);
    endpoint(c1, e);
    for (k = 0; k < 3; k++) {
        pal[0][k] = a[k];
        pal[1][k] = e[k];
        pal[2][k] = four ? (2 * a[k] + e[k] + 1) / 3 : (a[k] + e[k] + 1) / 2;
        pal[3][k] = four ? (a[k] + 2 * e[k] + 1) / 3 : 0;
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = four ? 255 : 0;             /* DXT1's 4th colour with c0 <= c1 */
    for (k = 0; k < 16; k++) {
        const int *c = pal[idx >> (2 * k) & 3];

        px[k][0] = (uint8_t)c[0];
        px[k][1] = (uint8_t)c[1];
        px[k][2] = (uint8_t)c[2];
        px[k][3] = (uint8_t)c[3];
    }
}

static void dxt3_alpha(const uint8_t *b, uint8_t px[16][4])
{
    int k;

    for (k = 0; k < 16; k++)
        px[k][3] = (uint8_t)(((b[k / 2] >> (k & 1 ? 4 : 0)) & 15) * 17);
}

static void dxt5_alpha(const uint8_t *b, uint8_t px[16][4])
{
    int a0 = b[0], a1 = b[1], table[8], k;
    uint64_t bits = 0;

    for (k = 0; k < 6; k++)
        bits |= (uint64_t)b[2 + k] << (8 * k);
    table[0] = a0;
    table[1] = a1;
    for (k = 2; k < 8; k++) {
        if (a0 > a1)
            table[k] = ((8 - k) * a0 + (k - 1) * a1 + 1) / 7;
        else
            table[k] = k <= 5 ? ((6 - k) * a0 + (k - 1) * a1 + 1) / 5 : k == 6 ? 0 : 255;
    }
    for (k = 0; k < 16; k++)
        px[k][3] = (uint8_t)table[(bits >> (3 * k)) & 7];
}

/* `step` = g: only every g-th block (slice 0 of a DXT volume) */
static int decode_dxt(uint32_t f, const uint8_t *data, size_t n, uint32_t w, uint32_t h,
                      uint32_t step, uint8_t *rgba)
{
    uint32_t bs = dxt_bytes(f), across = (w + 3) / 4, down = (h + 3) / 4, bx, by;
    uint8_t px[16][4];

    if ((uint64_t)across * down * step * bs > n)
        return -1;
    for (by = 0; by < down; by++)
        for (bx = 0; bx < across; bx++) {
            const uint8_t *b = data + ((size_t)(by * across + bx) * step) * bs;
            uint32_t r, c;

            colour_block(b + bs - 8, f == 0x0C, px);
            if (f == 0x0E)
                dxt3_alpha(b, px);
            else if (f == 0x0F)
                dxt5_alpha(b, px);
            for (r = 0; r < 4; r++)
                for (c = 0; c < 4; c++) {
                    uint32_t x = bx * 4 + c, y = by * 4 + r;

                    if (x < w && y < h)
                        memcpy(rgba + ((size_t)y * w + x) * 4, px[r * 4 + c], 4);
                }
        }
    return 0;
}

int texkit_decode_raw(uint32_t format, const uint8_t *data, size_t n, uint32_t w, uint32_t h,
                      uint32_t depth, uint8_t *rgba)
{
    if (dxt_bytes(format))
        return decode_dxt(format, data, n, w, h, depth < 4 ? (depth ? depth : 1) : 4, rgba);
    if (texel_size(format))
        return decode_swizzled(format, data, n, w, h, depth ? depth : 1, rgba);
    return -1;
}

/* ── One picture ────────────────────────────────────────── */

int texkit_decode(const TexkitPkg *p, int i, uint8_t **rgba, uint32_t *w, uint32_t *h,
                  char *why, size_t cap)
{
    const TexkitImage *m;
    const TexkitTexture *t;
    const TexkitLayout *l;
    uint64_t size, end;
    int rc;

    *rgba = NULL;
    if (why && cap)
        why[0] = 0;
    if (i < 0 || i >= p->nimg)
        return CAFF_E_NOT_FOUND;
    m = &p->img[i];
    t = &p->tex[m->texture];
    l = &m->layout;
    if (!dxt_bytes(t->format) && !texel_size(t->format)) {
        snprintf(why, cap, "%s: format %s isn't supported", m->name, texkit_format_name(t->format));
        return CAFF_E_TOC;
    }
    if (t->kind != TEXKIT_KIND_IMAGE && t->kind != TEXKIT_KIND_CUBE
        && t->kind != TEXKIT_KIND_VOLUME && t->kind != TEXKIT_KIND_VARIANTS) {
        snprintf(why, cap, "%s: texture kind %u isn't supported", m->name, t->kind);
        return CAFF_E_TOC;
    }
    if (!m->layout_ok) {
        snprintf(why, cap, "%s: frame %u has %u bytes, which fit no size of its texture", m->name,
                 m->variant, m->own);
        return CAFF_E_TOC;
    }
    size = dxt_bytes(l->format)
         ? (uint64_t)((l->width + 3) / 4) * ((l->height + 3) / 4) * dxt_bytes(l->format) * l->depth
         : (uint64_t)l->width * l->height * l->depth * texel_size(l->format);
    end = (uint64_t)m->at + m->own;
    if (m->at < t->gpu_offset || m->at + size > end || end > p->gpu_size || !p->gpu) {
        snprintf(why, cap, "%s: level 0 (%ux%u %s, %llu bytes) runs past the end of its bytes",
                 m->name, l->width, l->height, texkit_format_name(l->format),
                 (unsigned long long)size);
        return CAFF_E_TOC;
    }
    if (!(*rgba = malloc((size_t)l->width * l->height * 4)))
        return CAFF_E_NOMEM;
    rc = texkit_decode_raw(l->format, p->gpu + m->at, (size_t)(end - m->at), l->width, l->height,
                           l->depth, *rgba);
    if (rc) {
        free(*rgba);
        *rgba = NULL;
        if (rc == -2)
            return CAFF_E_NOMEM;
        snprintf(why, cap, "%s: a swizzled texture must be a power of two in every direction",
                 m->name);
        return CAFF_E_TOC;
    }
    *w = l->width;
    *h = l->height;
    return CAFF_OK;
}
