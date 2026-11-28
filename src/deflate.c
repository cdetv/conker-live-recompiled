/*
 * CRC-32 and deflate with fixed Huffman codes (deflate.h). Moved out of
 * src/bug_report.c unchanged (2026-11-26, to-do 238), with its own byte
 * buffer and the tables made once for every thread.
 */
#include "deflate.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int fail;
} Buf;

static int buf_reserve(Buf *b, size_t extra)
{
    size_t cap;
    uint8_t *q;

    if (b->fail)
        return -1;
    if (b->n + extra <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    q = (uint8_t *)realloc(b->p, cap);
    if (!q) {
        b->fail = 1;
        return -1;
    }
    b->p = q;
    b->cap = cap;
    return 0;
}

static void buf_add(Buf *b, const void *d, size_t n)
{
    if (buf_reserve(b, n))
        return;
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

/* ── Tables ──────────────────────────────────────────────── */

static uint32_t s_crc[256];
static uint16_t s_lit_code[288];            /* bit-reversed, LSB first */
static uint8_t s_lit_bits[288];
static uint8_t s_dist_code[30];
static uint8_t s_len_sym[259];              /* length 3..258 -> 0..28 */
static uint8_t s_dist_sym[32769];           /* distance 1..32768 -> 0..29 */

static const uint16_t s_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t s_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0 };
static const uint16_t s_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t s_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13 };

static uint32_t rev_bits(uint32_t v, int n)
{
    uint32_t r = 0;
    while (n--) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static void tables_make(void)
{
    uint32_t i, c, k;

    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc[i] = c;
    }
    for (i = 0; i < 288; i++) {
        uint32_t code;
        int len;
        if (i < 144)      { code = 0x30 + i;          len = 8; }
        else if (i < 256) { code = 0x190 + (i - 144); len = 9; }
        else if (i < 280) { code = i - 256;           len = 7; }
        else              { code = 0xC0 + (i - 280);  len = 8; }
        s_lit_code[i] = (uint16_t)rev_bits(code, len);
        s_lit_bits[i] = (uint8_t)len;
    }
    for (i = 0; i < 30; i++)
        s_dist_code[i] = (uint8_t)rev_bits(i, 5);
    for (i = 0; i < 28; i++)
        for (k = s_len_base[i]; k < s_len_base[i] + (1u << s_len_extra[i])
                                && k <= 258; k++)
            s_len_sym[k] = (uint8_t)i;
    s_len_sym[258] = 28;
    for (i = 0; i < 30; i++)
        for (k = s_dist_base[i]; k < s_dist_base[i] + (1u << s_dist_extra[i])
                                 && k <= 32768; k++)
            s_dist_sym[k] = (uint8_t)i;
}

#ifdef _WIN32
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tables_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    tables_make();
    return TRUE;
}

static void tables_init(void)
{
    InitOnceExecuteOnce(&s_once, tables_once, NULL, NULL);
}
#else
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void tables_init(void)
{
    pthread_once(&s_once, tables_make);
}
#endif

uint32_t deflate_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    uint32_t c = crc ^ 0xFFFFFFFFu;

    tables_init();
    while (n--)
        c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── Deflate ─────────────────────────────────────────────── */

typedef struct {
    Buf *out;
    uint64_t acc;
    int bits;
} Bits;

static void put_bits(Bits *w, uint32_t v, int n)
{
    w->acc |= (uint64_t)v << w->bits;
    w->bits += n;
    if (w->bits >= 32) {
        if (!buf_reserve(w->out, 4)) {
            uint8_t b[4] = { (uint8_t)w->acc, (uint8_t)(w->acc >> 8),
                             (uint8_t)(w->acc >> 16), (uint8_t)(w->acc >> 24) };
            memcpy(w->out->p + w->out->n, b, 4);
            w->out->n += 4;
        }
        w->acc >>= 32;
        w->bits -= 32;
    }
}

#define WSIZE    32768u
#define WMASK    (WSIZE - 1)
#define HBITS    15
#define MAXCHAIN 48

static uint32_t hash3(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return (v * 2654435761u) >> (32 - HBITS);
}

/* One final fixed-Huffman block (BTYPE 01). */
int deflate_fixed(const uint8_t *in, size_t n, uint8_t **out_p, size_t *n_out)
{
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * WSIZE);
    Buf b = { 0 }, *out = &b;
    Bits w = { out, 0, 0 };
    size_t i = 0;

    *out_p = NULL;
    *n_out = 0;
    tables_init();
    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    memset(head, 0xFF, sizeof(int32_t) << HBITS);
    buf_reserve(out, n + n / 8 + 64);
    put_bits(&w, 1, 1);                     /* BFINAL */
    put_bits(&w, 1, 2);                     /* BTYPE = 01, fixed codes */
    while (i < n) {
        size_t best = 0, bdist = 0;

        if (i + 3 <= n) {
            uint32_t h = hash3(in + i);
            int32_t cand = head[h];
            int chain = MAXCHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;

            while (cand >= 0 && chain-- > 0) {
                size_t d = i - (size_t)cand, len = 0;
                int32_t next;
                if (d >= WSIZE)
                    break;
                if (in[cand + best] == in[i + best]) {
                    while (len < maxlen && in[cand + len] == in[i + len])
                        len++;
                    if (len > best) {
                        best = len;
                        bdist = d;
                        if (len == maxlen)
                            break;
                    }
                }
                next = prev[cand & WMASK];
                if (next >= cand)
                    break;
                cand = next;
            }
            prev[i & WMASK] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= 3) {
            unsigned ls = s_len_sym[best], ds = s_dist_sym[bdist], k;
            put_bits(&w, s_lit_code[257 + ls], s_lit_bits[257 + ls]);
            if (s_len_extra[ls])
                put_bits(&w, (uint32_t)(best - s_len_base[ls]), s_len_extra[ls]);
            put_bits(&w, s_dist_code[ds], 5);
            if (s_dist_extra[ds])
                put_bits(&w, (uint32_t)(bdist - s_dist_base[ds]), s_dist_extra[ds]);
            for (k = 1; k < best; k++) {
                size_t p = i + k;
                if (p + 3 <= n) {
                    uint32_t h = hash3(in + p);
                    prev[p & WMASK] = head[h];
                    head[h] = (int32_t)p;
                }
            }
            i += best;
        } else {
            put_bits(&w, s_lit_code[in[i]], s_lit_bits[in[i]]);
            i++;
        }
    }
    put_bits(&w, s_lit_code[256], s_lit_bits[256]);         /* end of block */
    while (w.bits > 0) {
        uint8_t byte = (uint8_t)w.acc;
        buf_add(out, &byte, 1);
        w.acc >>= 8;
        w.bits -= 8;
    }
    free(head);
    free(prev);
    if (out->fail) {
        free(out->p);
        return -1;
    }
    *out_p = out->p;
    *n_out = out->n;
    return 0;
}
