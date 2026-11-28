/*
 * Sharper text and buttons (to-do 53 step 5): src/sharp_gui.c's clean-up
 * against the prototype scripts' own results on made-up pictures
 * (expected.h, made by make_expected.py; no game data). The C port uses
 * floats where the scripts use doubles: a byte may be 1 off, never more.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sharp_gui.h"
#include "expected.h"

static int s_fail;

static void compare(const char *what, const unsigned char *got, const unsigned char *want, size_t n)
{
    size_t i, off1 = 0, worse = 0;
    int maxd = 0;

    for (i = 0; i < n; i++) {
        int d = abs((int)got[i] - (int)want[i]);
        if (d > maxd)
            maxd = d;
        off1 += d == 1;
        worse += d > 1;
    }
    printf("%-22s %6u bytes: %u one off, %u more than one off (largest %d)\n", what, (unsigned)n,
           (unsigned)off1, (unsigned)worse, maxd);
    if (worse)
        s_fail++;
}

static void test_font(void)
{
    size_t n = (size_t)FONT_W * EXP_SCALE * FONT_H * EXP_SCALE;
    unsigned char *out = malloc(n);

    if (!out || sharp_font_sheet(font_in, FONT_W, FONT_H, EXP_SCALE, EXP_EDGE, out)) {
        printf("font: failed to run\n");
        s_fail++;
    } else {
        compare("font sheet", out, font_out, n);
    }
    free(out);
}

static void test_picture(const char *what, const unsigned char *in, const unsigned char *want,
                         unsigned w, unsigned h)
{
    size_t n = (size_t)w * EXP_SCALE * h * EXP_SCALE * 4;
    unsigned char *out = malloc(n);

    if (!out || sharp_picture(in, w, h, EXP_SCALE, EXP_EDGE, EXP_SHARPEN, out)) {
        printf("%s: failed to run\n", what);
        s_fail++;
    } else {
        compare(what, out, want, n);
    }
    free(out);
}

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* The font file layout make_font_sheets.py read_sheet reads. */
static void test_font_file(void)
{
    unsigned char *f = calloc(1, 0x520 + 64 * 2);
    const uint8_t *sheet = NULL;
    uint32_t w = 0, h = 0;
    int ok;

    if (!f) {
        s_fail++;
        return;
    }
    put32(f + 0x38, 0x510);             /* .data at */
    put32(f + 0x50, 0x10);              /* .data size: the sheet right after */
    f[0x500] = 64; f[0x502] = 2;        /* 64 x 2 */
    f[0x520] = 7;
    ok = !sharp_font_sheet_of(f, 0x520 + 128, &sheet, &w, &h) && sheet == f + 0x520 && w == 64
         && h == 2 && sheet[0] == 7;
    ok = ok && sharp_font_sheet_of(f, 0x520 + 127, &sheet, &w, &h) == -1;  /* cut short */
    f[0x500] = 60;
    ok = ok && sharp_font_sheet_of(f, 0x520 + 128, &sheet, &w, &h) == -1;  /* not x64 wide */
    printf("%-22s %s\n", "font file layout", ok ? "ok" : "FAILED");
    s_fail += !ok;
    free(f);
}

int main(void)
{
    test_font();
    test_picture("button, soft outline", soft_in, soft_out, SOFT_W, SOFT_H);
    test_picture("button, on/off outline", bin_in, bin_out, BIN_W, BIN_H);
    test_font_file();
    printf(s_fail ? "FAILED (%d)\n" : "all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
