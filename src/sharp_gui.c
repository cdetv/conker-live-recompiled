/*
 * Sharper text and buttons (to-do 53 step 5; plan in docs/todo.md 53): the
 * C port of the two prototype scripts, scripts/make_font_sheets.py (step 1)
 * and scripts/make_button_pictures.py (step 4), with the same maths, so the
 * pictures each player's PC makes are the ones the user saw (checked against
 * the scripts' output: docs/conker-notes.md 2026-11-27).
 *
 * The game finds a replacement picture by the hash of the original's bytes
 * and draws it over the same rectangle, so a bigger picture with the same
 * layout only makes it sharper. The four fonts' sheets (8-bit grey) and the
 * 24 controller button pictures (DXT1 / DXT5, 32x32 or 64x64) are enlarged
 * x4 and cleaned up:
 *  - enlarged by a cubic B-spline (no blocky steps, no ringing; a curve
 *    through the pixels, Catmull-Rom, made the fonts' slanted edges wavy);
 *  - fonts: each spot over the brightest value within one original pixel (at
 *    least 0.3: a hairline too thin to reach white counts as a stroke), then
 *    the edge ~1.25 new pixels wide round the 0.5 line;
 *  - buttons: colour weighted by alpha (no dark rim from the clear pixels),
 *    the outline as the fonts' edges (an on/off DXT1 outline smoothed half an
 *    original pixel first, or its stairs stay as waves), the colour inside
 *    sharpened a little, the edge colours carried into the clear area (the
 *    game's filtering and the mip levels read them).
 * Written as <save>\assets\sharp\<HASH>_<name>.png by the carve
 * (caff_cache.c), loaded as the lowest texture pack (tex_packs.c).
 *
 * Plain memory, caff.h and texkit.h only (no guest, no window): it links into
 * the stand-alone test (tests/sharp_gui). Floats where the scripts use
 * doubles: the bytes come out the same or 1 apart.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_write.h"
#include "sharp_gui.h"
#include "texkit.h"

/* ── The B-spline ──────────────────────────────────────────────────────── */

/* Four taps per new pixel along one axis (make_font_sheets.py
 * bspline_weights): new pixel o sits at (o + 0.5) / s - 0.5, as the GPU
 * samples; taps past the edge repeat the edge pixel. */
static void taps(uint32_t n_in, uint32_t n_out, int *idx, float *wt)
{
    double s = (double)n_out / (double)n_in;
    uint32_t o;

    for (o = 0; o < n_out; o++) {
        double c = ((double)o + 0.5) / s - 0.5;
        int i0 = (int)floor(c), k;

        for (k = 0; k < 4; k++) {
            int i = i0 - 1 + k;
            double x = fabs((double)i - c), w;

            if (x < 1.0)
                w = (3.0 * x * x * x - 6.0 * x * x + 4.0) / 6.0;
            else if (x < 2.0)
                w = (2.0 - x) * (2.0 - x) * (2.0 - x) / 6.0;
            else
                w = 0.0;
            idx[o * 4 + k] = i < 0 ? 0 : i >= (int)n_in ? (int)n_in - 1 : i;
            wt[o * 4 + k] = (float)w;
        }
    }
}

/* One plane (h x w floats) enlarged into out (H x W), through tmp (h x W). */
static void enlarge(const float *src, uint32_t w, uint32_t h, uint32_t W, uint32_t H,
                    const int *ix, const float *wx, const int *iy, const float *wy,
                    float *tmp, float *out)
{
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        const float *row = src + (size_t)y * w;
        float *t = tmp + (size_t)y * W;
        for (x = 0; x < W; x++) {
            const int *i = ix + x * 4;
            const float *k = wx + x * 4;
            t[x] = k[0] * row[i[0]] + k[1] * row[i[1]] + k[2] * row[i[2]] + k[3] * row[i[3]];
        }
    }
    for (y = 0; y < H; y++) {
        const int *i = iy + y * 4;
        const float *k = wy + y * 4;
        const float *r0 = tmp + (size_t)i[0] * W, *r1 = tmp + (size_t)i[1] * W;
        const float *r2 = tmp + (size_t)i[2] * W, *r3 = tmp + (size_t)i[3] * W;
        float *o = out + (size_t)y * W;
        for (x = 0; x < W; x++)
            o[x] = k[0] * r0[x] + k[1] * r1[x] + k[2] * r2[x] + k[3] * r3[x];
    }
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t to_u8(float v)
{
    return (uint8_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/* ── Fonts ─────────────────────────────────────────────────────────────── */

int sharp_font_sheet(const uint8_t *sheet, uint32_t w, uint32_t h, int scale, float edge,
                     uint8_t *out)
{
    uint32_t W = w * (uint32_t)scale, H = h * (uint32_t)scale, x, y;
    size_t big_n = (size_t)W * H;
    int *ix = NULL, *iy = NULL, r = scale, rc = -1;
    float *wx = NULL, *wy = NULL, *src = NULL, *tmp = NULL, *big = NULL, *peak = NULL, *row = NULL;
    const float gain = (float)scale / edge;

    if (!w || !h || scale < 1 || scale > 16 || edge <= 0.0f)
        return -1;
    ix = malloc(W * 4 * sizeof *ix);
    iy = malloc(H * 4 * sizeof *iy);
    wx = malloc(W * 4 * sizeof *wx);
    wy = malloc(H * 4 * sizeof *wy);
    src = malloc((size_t)w * h * sizeof *src);
    tmp = malloc((size_t)h * W * sizeof *tmp);
    big = malloc(big_n * sizeof *big);
    peak = malloc(big_n * sizeof *peak);
    row = malloc((W > H ? W : H) * sizeof *row);
    if (!ix || !iy || !wx || !wy || !src || !tmp || !big || !peak || !row)
        goto done;
    for (x = 0; x < w * h; x++)
        src[x] = (float)sheet[x] / 255.0f;
    taps(w, W, ix, wx);
    taps(h, H, iy, wy);
    enlarge(src, w, h, W, H, ix, wx, iy, wy, tmp, big);
    free(tmp);
    tmp = NULL;

    /* The brightest value within r pixels (a square; edges repeated), down
     * the columns into peak, then along each row of peak. */
    for (x = 0; x < W; x++)
        for (y = 0; y < H; y++) {
            int a = (int)y - r, b = (int)y + r, i;
            float m = 0.0f;
            for (i = a; i <= b; i++) {
                int j = i < 0 ? 0 : i >= (int)H ? (int)H - 1 : i;
                float v = big[(size_t)j * W + x];
                if (v > m)
                    m = v;
            }
            peak[(size_t)y * W + x] = m;
        }
    for (y = 0; y < H; y++) {
        float *p = peak + (size_t)y * W;
        memcpy(row, p, W * sizeof *row);
        for (x = 0; x < W; x++) {
            int a = (int)x - r, b = (int)x + r, i;
            float m = 0.0f;
            for (i = a; i <= b; i++) {
                int j = i < 0 ? 0 : i >= (int)W ? (int)W - 1 : i;
                if (row[j] > m)
                    m = row[j];
            }
            p[x] = m;
        }
    }
    for (x = 0; x < big_n; x++) {
        float pk = clampf(peak[x], 0.3f, 1.0f);
        out[x] = to_u8((big[x] / pk - 0.5f) * gain + 0.5f);
    }
    rc = 0;
done:
    free(ix); free(iy); free(wx); free(wy);
    free(src); free(tmp); free(big); free(peak); free(row);
    return rc;
}

int sharp_font_sheet_of(const uint8_t *file, size_t n, const uint8_t **sheet, uint32_t *w,
                        uint32_t *h)
{
    uint32_t data_off, data_size;
    uint64_t at;

    if (n < 0x504)
        return -1;
    data_off = (uint32_t)file[0x38] | (uint32_t)file[0x39] << 8 | (uint32_t)file[0x3A] << 16
               | (uint32_t)file[0x3B] << 24;
    data_size = (uint32_t)file[0x50] | (uint32_t)file[0x51] << 8 | (uint32_t)file[0x52] << 16
                | (uint32_t)file[0x53] << 24;
    *w = (uint32_t)file[0x500] | (uint32_t)file[0x501] << 8;
    *h = (uint32_t)file[0x502] | (uint32_t)file[0x503] << 8;
    at = (uint64_t)data_off + data_size;
    if (!*w || !*h || *w % 64 || at + (uint64_t)*w * *h > n)
        return -1;
    *sheet = file + at;
    return 0;
}

/* ── Buttons ───────────────────────────────────────────────────────────── */

/* Mean over a (2r+1) square, one axis at a time, edges repeated
 * (make_button_pictures.py box_blur), in place through tmp (W x H). */
static void box_blur(float *img, uint32_t W, uint32_t H, int r, float *tmp)
{
    uint32_t x, y;
    const float k = 1.0f / (float)(2 * r + 1);

    for (x = 0; x < W; x++)             /* axis 0: down the columns */
        for (y = 0; y < H; y++) {
            float s = 0.0f;
            int i;
            for (i = (int)y - r; i <= (int)y + r; i++) {
                int j = i < 0 ? 0 : i >= (int)H ? (int)H - 1 : i;
                s += img[(size_t)j * W + x];
            }
            tmp[(size_t)y * W + x] = s * k;
        }
    for (y = 0; y < H; y++)             /* axis 1: along the rows */
        for (x = 0; x < W; x++) {
            float s = 0.0f;
            int i;
            for (i = (int)x - r; i <= (int)x + r; i++) {
                int j = i < 0 ? 0 : i >= (int)W ? (int)W - 1 : i;
                s += tmp[(size_t)y * W + j];
            }
            img[(size_t)y * W + x] = s * k;
        }
}

/* Three box passes: close to a Gaussian of about r / 1.7 pixels. */
static void soft_blur(float *img, uint32_t W, uint32_t H, int r, float *tmp)
{
    box_blur(img, W, H, r, tmp);
    box_blur(img, W, H, r, tmp);
    box_blur(img, W, H, r, tmp);
}

int sharp_picture(const uint8_t *rgba, uint32_t w, uint32_t h, int scale, float edge,
                  float sharpen, uint8_t *out)
{
    uint32_t W = w * (uint32_t)scale, H = h * (uint32_t)scale, x, y;
    size_t n = (size_t)W * H, i;
    int c, rc = -1, binary = 1, sr = scale / 2 > 1 ? scale / 2 : 1;
    int *ix = NULL, *iy = NULL;
    float *wx = NULL, *wy = NULL, *src = NULL, *tmp = NULL, *buf = NULL;
    float *a, *col[3], *shape, *wb, *blur, *acc[3], *accw, *fill[3];
    uint8_t *known = NULL, *hole = NULL;
    const float gain = (float)scale / edge;
    int seen[256] = { 0 }, levels = 0;

    if (!w || !h || scale < 1 || scale > 16 || edge <= 0.0f)
        return -1;
    for (i = 0; i < (size_t)w * h; i++)
        if (!seen[rgba[i * 4 + 3]]++)
            levels++;
    binary = levels <= 2;
    ix = malloc(W * 4 * sizeof *ix);
    iy = malloc(H * 4 * sizeof *iy);
    wx = malloc(W * 4 * sizeof *wx);
    wy = malloc(H * 4 * sizeof *wy);
    src = malloc((size_t)w * h * sizeof *src);
    tmp = malloc((n > (size_t)h * W ? n : (size_t)h * W) * sizeof *tmp);
    buf = malloc(n * 16 * sizeof *buf);
    known = malloc(n);
    hole = malloc(n);
    if (!ix || !iy || !wx || !wy || !src || !tmp || !buf || !known || !hole)
        goto done;
    a = buf;
    col[0] = buf + n; col[1] = buf + 2 * n; col[2] = buf + 3 * n;
    shape = buf + 4 * n;
    wb = buf + 5 * n;
    blur = buf + 6 * n;
    acc[0] = buf + 7 * n; acc[1] = buf + 8 * n; acc[2] = buf + 9 * n;
    accw = buf + 10 * n;
    fill[0] = buf + 11 * n; fill[1] = buf + 12 * n; fill[2] = buf + 13 * n;
    taps(w, W, ix, wx);
    taps(h, H, iy, wy);

    /* Alpha, then each colour times alpha (premultiplied), enlarged. */
    for (i = 0; i < (size_t)w * h; i++)
        src[i] = (float)rgba[i * 4 + 3] / 255.0f;
    enlarge(src, w, h, W, H, ix, wx, iy, wy, tmp, a);
    for (c = 0; c < 3; c++) {
        for (i = 0; i < (size_t)w * h; i++)
            src[i] = (float)rgba[i * 4 + c] / 255.0f * ((float)rgba[i * 4 + 3] / 255.0f);
        enlarge(src, w, h, W, H, ix, wx, iy, wy, tmp, col[c]);
    }
    for (i = 0; i < n; i++) {
        a[i] = clampf(a[i], 0.0f, 1.0f);
        for (c = 0; c < 3; c++)
            col[c][i] = clampf(col[c][i] / (a[i] > 1e-4f ? a[i] : 1e-4f), 0.0f, 1.0f);
    }

    /* The outline, crisp round the 50 % line. */
    memcpy(shape, a, n * sizeof *shape);
    if (binary)
        soft_blur(shape, W, H, sr, tmp);

    /* The colour inside, sharpened (the blur weighted by coverage). */
    if (sharpen > 0.0f) {
        memcpy(wb, a, n * sizeof *wb);
        soft_blur(wb, W, H, sr, tmp);
        for (c = 0; c < 3; c++) {
            for (i = 0; i < n; i++)
                blur[i] = col[c][i] * a[i];
            soft_blur(blur, W, H, sr, tmp);
            for (i = 0; i < n; i++) {
                float b = blur[i] / (wb[i] > 1e-4f ? wb[i] : 1e-4f);
                col[c][i] = clampf(col[c][i] + sharpen * (col[c][i] - b), 0.0f, 1.0f);
            }
        }
    }

    /* The edge colours carried on into the clear area. */
    {
        int it, any = 0;

        for (i = 0; i < n; i++) {
            known[i] = a[i] > 0.02f;
            hole[i] = !known[i];
            any |= hole[i];
            accw[i] = known[i] ? 1.0f : 0.0f;
            for (c = 0; c < 3; c++) {
                acc[c][i] = col[c][i] * accw[i];
                fill[c][i] = col[c][i];
            }
        }
        for (it = 0; any && it < 6 * scale; it++) {
            for (c = 0; c < 3; c++)
                box_blur(acc[c], W, H, 1, tmp);
            box_blur(accw, W, H, 1, tmp);
            any = 0;
            for (i = 0; i < n; i++) {
                if (!hole[i])
                    continue;
                if (accw[i] > 1e-6f) {
                    for (c = 0; c < 3; c++) {
                        fill[c][i] = acc[c][i] / accw[i];
                        acc[c][i] = fill[c][i];
                    }
                    accw[i] = 1.0f;
                    hole[i] = 0;
                } else {
                    any = 1;
                }
            }
        }
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            size_t p = (size_t)y * W + x;
            for (c = 0; c < 3; c++)
                out[p * 4 + c] = to_u8(known[p] ? col[c][p] : fill[c][p]);
            out[p * 4 + 3] = to_u8((shape[p] - 0.5f) * gain + 0.5f);
        }
    rc = 0;
done:
    free(ix); free(iy); free(wx); free(wy);
    free(src); free(tmp); free(buf); free(known); free(hole);
    return rc;
}

/* ── The set ───────────────────────────────────────────────────────────── */

static const char *const s_fonts[] = {
    "ConkerFont", "FrontendTitle", "ConkerFontJapanese", "FrontendTitleJapanese",
};

/* The game's controller button pictures (r2-swing-hud.md, r3-looks.md C1,
 * and the multiplayer and training ones the texture list shows;
 * docs/conker-notes.md 2026-11-27). */
typedef struct {
    const char *package;        /* for caff_resolve_package's first form */
    const char *names;          /* '|' between */
} SharpButtons;

static const SharpButtons s_buttons[] = {
    { "General\\Boot", "A_BUTT|B_BUTT|X_BUTT|Y_BUTT|L_BUTT|R_BUTT|D_BUTT|G_BUTT|I_BUTT|S_BUTT|"
      "joystick100|joystick101|joystick102|joystick103|joystick104|joystick105" },
    { "General\\Multiplayer", "A_BUTT|B_BUTT|BLACK_BUTT|triggerG|triggerI|triggerL|triggerS" },
    { "Level\\TrainingLevel", "l_but_1" },
};

/* One PNG into <save>\assets\sharp\<HASH>_<name>.png; the file callback
 * either way. 0, or -1. */
static int put_png(const char *save, uint64_t hash, const char *name, uint32_t w, uint32_t h,
                   int channels, const uint8_t *px, SharpFileFn file, void *ctx,
                   SharpResult *res)
{
    char rel[256], path[1024];
    uint8_t *png = NULL;
    size_t len = 0;
    int ok;

    snprintf(rel, sizeof rel, SHARP_DIR "\\%016llX_%s.png", (unsigned long long)hash, name);
    snprintf(path, sizeof path, "%s\\assets\\%s", save, rel);
    ok = !png_encode(w, h, channels, px, &png, &len)
         && !caff_make_parent_dirs(path) && !caff_write_file(path, png, len);
    free(png);
    if (!ok)
        fprintf(stderr, "[SHARP] can't write %s\n", path);
    if (file)
        file(rel, ok ? len : 0, !ok, ctx);
    if (ok) {
        res->made++;
        res->bytes += len;
    } else {
        res->failed++;
    }
    return ok ? 0 : -1;
}

static void make_fonts(const char *game, const char *save, SharpStepFn step, SharpFileFn file,
                       void *ctx, SharpResult *res)
{
    size_t f;

    for (f = 0; f < sizeof s_fonts / sizeof *s_fonts; f++) {
        char path[1024], what[64];
        uint8_t *data = NULL, *big = NULL;
        const uint8_t *sheet;
        size_t n = 0;
        uint32_t w, h;

        snprintf(what, sizeof what, "font\\%s", s_fonts[f]);
        if (step)
            step(what, ctx);
        snprintf(path, sizeof path, "%s\\dvddata\\aid\\font\\%s\\default.bin", game, s_fonts[f]);
        if (caff_read_file(path, &data, &n) || sharp_font_sheet_of(data, n, &sheet, &w, &h)) {
            fprintf(stderr, "[SHARP] %s: no font sheet in %s\n", s_fonts[f], path);
            res->failed++;
            free(data);
            continue;
        }
        big = malloc((size_t)w * SHARP_SCALE * h * SHARP_SCALE);
        if (!big || sharp_font_sheet(sheet, w, h, SHARP_SCALE, SHARP_EDGE, big)) {
            fprintf(stderr, "[SHARP] %s: out of memory (%ux%u)\n", s_fonts[f], w, h);
            res->failed++;
        } else {
            put_png(save, texkit_hash(sheet, (size_t)w * h, TEXKIT_HASH_SEED), s_fonts[f],
                    w * SHARP_SCALE, h * SHARP_SCALE, 1, big, file, ctx, res);
        }
        free(big);
        free(data);
    }
}

/* The button names of one package, each found once. */
static void make_buttons(const char *game, const char *save, CaffUnpackFn unpack,
                         const SharpButtons *b, SharpFileFn file, void *ctx, SharpResult *res)
{
    char path[1024], rel[1024], why[256], names[512], *name, *next;
    CaffPkg *k = NULL;
    TexkitPkg tp;
    int want = 0;

    snprintf(names, sizeof names, "%s", b->names);
    for (name = names; name; name = next) {
        if ((next = strchr(name, '|')))
            next++;
        want++;
    }
    snprintf(path, sizeof path, "%s\\dvddata\\aid\\zpackage\\%s\\default.rbm", game, b->package);
    snprintf(rel, sizeof rel, "zpackage\\%s\\default.rbm", b->package);
    memset(&tp, 0, sizeof tp);
    if (caff_open(path, unpack, &k) != CAFF_OK
        || texkit_read(k, rel, &tp, why, sizeof why) != CAFF_OK) {
        fprintf(stderr, "[SHARP] %s: can't read its pictures\n", b->package);
        res->failed += want;
        caff_close(k);
        return;
    }
    for (name = names; name; name = next) {
        int i;

        if ((next = strchr(name, '|')))
            *next++ = 0;
        for (i = 0; i < tp.nimg && strcmp(tp.img[i].name, name); i++)
            ;
        if (i == tp.nimg || tp.img[i].replace != TEXKIT_REPLACE_YES || !tp.img[i].hash) {
            fprintf(stderr, "[SHARP] %s: no %s there\n", b->package, name);
            res->failed++;
            continue;
        }
        {
            uint8_t *rgba = NULL, *big = NULL;
            uint32_t w = 0, h = 0;

            if (texkit_decode(&tp, i, &rgba, &w, &h, why, sizeof why) != CAFF_OK) {
                fprintf(stderr, "[SHARP] %s %s: %s\n", b->package, name, why);
                res->failed++;
            } else if (!(big = malloc((size_t)w * SHARP_SCALE * h * SHARP_SCALE * 4))
                       || sharp_picture(rgba, w, h, SHARP_SCALE, SHARP_EDGE, SHARP_SHARPEN, big)) {
                fprintf(stderr, "[SHARP] %s %s: out of memory\n", b->package, name);
                res->failed++;
            } else {
                put_png(save, tp.img[i].hash, name, w * SHARP_SCALE, h * SHARP_SCALE, 4, big,
                        file, ctx, res);
            }
            free(big);
            free(rgba);
        }
    }
    texkit_free(&tp);
    caff_close(k);
}

int sharp_make(const char *game, const char *save, CaffUnpackFn unpack, SharpStepFn step,
               SharpFileFn file, void *ctx, SharpResult *result)
{
    char probe[1024];
    size_t b;
    SharpResult res = { 0, 0, 0 };

    snprintf(probe, sizeof probe, "%s\\assets\\" SHARP_DIR "\\x", save);
    if (caff_make_parent_dirs(probe)) {
        fprintf(stderr, "[SHARP] can't make %s\\assets\\" SHARP_DIR "\n", save);
        if (result)
            *result = res;
        return -1;
    }
    make_fonts(game, save, step, file, ctx, &res);
    for (b = 0; b < sizeof s_buttons / sizeof *s_buttons; b++) {
        if (step)
            step(s_buttons[b].package, ctx);
        make_buttons(game, save, unpack, &s_buttons[b], file, ctx, &res);
    }
    if (result)
        *result = res;
    return 0;
}
