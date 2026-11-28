/*
 * Texture pack kit test (to-do 238): src/texkit.c's pieces on their own,
 * no game data: the hash, level and chain sizes, the frame sizes of
 * textures.md 3.3 (the five textures on the disc whose frames have another
 * size, from the extractor's docs), the naming rules of texture-names.md,
 * the swizzle order and texel colours of textures.md 5-6, the DXT maths of
 * 6.1, and the PNG writer (src/png_write.c, src/deflate.c). The walk over real packages is checked against the
 * extractor's own listing (docs/test-recipes/textures.md).
 *
 * In a VS dev shell:
 *   cmake -S tests/texkit -B _work/texkit-test -G Ninja -DCMAKE_BUILD_TYPE=Release
 *   cmake --build _work/texkit-test && ctest --test-dir _work/texkit-test --output-on-failure
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texkit.h"
#include "png_write.h"
#include "deflate.h"

static int s_fail;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static void str_is(const char *got, const char *want, int line)
{
    if (strcmp(got, want)) {
        printf("FAIL line %d: \"%s\", want \"%s\"\n", line, got, want);
        s_fail++;
    }
}
#define STR(got, want) str_is(got, want, __LINE__)

static void test_hash(void)
{
    uint8_t b[64];
    int i;

    for (i = 0; i < 64; i++)
        b[i] = (uint8_t)i;
    /* values from the toolkit's hash_bytes, worked out in Python */
    CHECK(texkit_hash(NULL, 0, TEXKIT_HASH_SEED) == 0xCBF29CE484222325ull);
    CHECK(texkit_hash((const uint8_t *)"abcdefghij", 10, TEXKIT_HASH_SEED) == 0x4A1BFD9130F2BC4Full);
    CHECK(texkit_hash(b, 64, TEXKIT_HASH_SEED) == 0x4B378235660F547Aull);
}

static void test_sizes(void)
{
    CHECK(texkit_level_size(0x0C, 4, 4, 1) == 8);
    CHECK(texkit_level_size(0x0C, 2, 2, 1) == 8);           /* whole blocks */
    CHECK(texkit_level_size(0x0F, 64, 64, 1) == 4096);
    CHECK(texkit_level_size(0x00, 16, 16, 1) == 256);
    CHECK(texkit_level_size(0x3A, 64, 64, 16) == 64 * 64 * 16 * 4);
    CHECK(texkit_level_size(0x0C, 64, 64, 4) == 2048 * 4);
    CHECK(texkit_level_size(0x13, 64, 64, 1) == 0);         /* not on the disc */
    /* textures.md 3.2: a 128x128 DXT1 face with 8 levels is 0x2AB8 */
    CHECK(texkit_chain_size(0x0C, 128, 128, 8) == 0x2AB8);
}

static void layout_is(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t count,
                      uint32_t own, uint32_t want_fmt, uint32_t want_w, uint32_t want_h, int line)
{
    TexkitLayout l;
    int ok = texkit_layout(fmt, TEXKIT_KIND_VARIANTS, w, h, mips, count, own, &l);

    if (!ok || l.format != want_fmt || l.width != want_w || l.height != want_h || l.depth != 1) {
        printf("FAIL line %d: %d 0x%02X %ux%u, want 0x%02X %ux%u\n", line, ok, l.format,
               l.width, l.height, want_fmt, want_w, want_h);
        s_fail++;
    }
}

static void test_layout(void)
{
    TexkitLayout l;

    /* textures.md 3.3's table: each frame's own bytes -> its real size */
    layout_is(0x0C, 512, 512, 1, 8, 0x80, 0x0C, 16, 16, __LINE__);   /* labProjectedShadow.1@1 */
    layout_is(0x0C, 32, 32, 6, 4, 0x100, 0x0C, 16, 16, __LINE__);    /* demolisher_yellowEye.1@3 */
    layout_is(0x0C, 16, 16, 5, 4, 0x300, 0x0C, 32, 32, __LINE__);    /* demolisher_RedEye.1@1 */
    layout_is(0x0C, 8, 8, 4, 16, 0x300, 0x0C, 32, 32, __LINE__);     /* Piosoner_eye.4@1 */
    layout_is(0x0C, 32, 32, 6, 11, 0x580, 0x3A, 16, 16, __LINE__);   /* Detpack_texturefile2@10 */
    /* a normal frame: the header's chain, or that padded to 0x80 */
    layout_is(0x0C, 32, 32, 6, 9, (uint32_t)texkit_chain_size(0x0C, 32, 32, 6), 0x0C, 32, 32, __LINE__);
    layout_is(0x0C, 32, 32, 6, 9, 0x300, 0x0C, 32, 32, __LINE__);   /* 0x2B8 padded */
    /* other kinds: the header's (a volume with its depth) */
    CHECK(texkit_layout(0x0C, TEXKIT_KIND_IMAGE, 64, 32, 7, 0, 1, &l) && l.width == 64
          && l.height == 32 && l.depth == 1);
    CHECK(texkit_layout(0x3A, TEXKIT_KIND_VOLUME, 64, 64, 1, 16, 1, &l) && l.depth == 16);
    /* nothing fits */
    CHECK(!texkit_layout(0x0C, TEXKIT_KIND_VARIANTS, 32, 32, 6, 4, 0x123, &l) && l.width == 32);
}

static void test_names(void)
{
    char s[TEXKIT_NAME_CAP];

    texkit_package_name("zpackage\\Level\\Canyon\\default.rbm", s, sizeof s);
    STR(s, "Level_Canyon");
    texkit_package_name("dvddata\\aid\\texture\\Avatars\\Bang\\default.rba", s, sizeof s);
    STR(s, "Avatars_Bang");
    texkit_package_name("Bang\\default.rba", s, sizeof s);
    STR(s, "Bang");
    texkit_leaf("aid\\texture\\characters\\conker\\New_Conker_Eye.1\\default.rba", s, sizeof s);
    STR(s, "New_Conker_Eye.1");
    texkit_leaf("aid/bfdmodel//Thing\\DEFAULT.cut.bin", s, sizeof s);
    STR(s, "Thing");
    texkit_leaf("aid\\texture\\fire_1.123", s, sizeof s);
    STR(s, "fire_1.123");
    texkit_leaf("default.rba", s, sizeof s);              /* one part: itself */
    STR(s, "default.rba");
    texkit_standalone_name("Avatars_Bang", s, sizeof s);
    STR(s, "Bang");
    texkit_standalone_name("tips_Tip01", s, sizeof s);
    STR(s, "Tip01");
    texkit_standalone_name("avatars_Bang", s, sizeof s);  /* prefixes with their case */
    STR(s, "avatars_Bang");
    texkit_clean_name("  a:*b \t c. ", s, sizeof s);
    STR(s, "a_b c");
    texkit_clean_name("\tx", s, sizeof s);                /* strip() takes spaces only */
    STR(s, " x");
    texkit_clean_name(" .. ", s, sizeof s);
    STR(s, "Image");
    texkit_clean_name("ace marshal", s, sizeof s);
    STR(s, "ace marshal");
}

static void test_unique(void)
{
    /* texture-names.md 5 and 6: ".001" for a repeat (exact case), "_001" for
     * a file name equal ignoring case, the first created keeping its name */
    static const char *const base[] = {
        "snowdrop_1", "snowdrop_1", "SandShot_texturefile0", "sandshot_texturefile0",
        "X@1", "X@1", "default", "Default", "snowdrop_1",
    };
    static const char *const name[] = {
        "snowdrop_1", "snowdrop_1.001", "SandShot_texturefile0", "sandshot_texturefile0",
        "X@1", "X@1.001", "default", "Default", "snowdrop_1.002",
    };
    static const char *const file[] = {
        "snowdrop_1.png", "snowdrop_1.001.png", "SandShot_texturefile0.png",
        "sandshot_texturefile0_001.png", "X@1.png", "X@1.001.png", "default.png",
        "Default_001.png", "snowdrop_1.002.png",
    };
    enum { N = sizeof base / sizeof *base };
    TexkitImage img[N];
    char b[N][TEXKIT_NAME_CAP];
    int i;

    memset(img, 0, sizeof img);
    for (i = 0; i < N; i++)
        snprintf(b[i], sizeof b[i], "%s", base[i]);
    texkit_name_images(img, N, b);
    for (i = 0; i < N; i++) {
        str_is(img[i].name, name[i], 1000 + i);
        str_is(img[i].file_name, file[i], 2000 + i);
    }
}

/* textures.md 5.1: texel (x, y) of a swizzled level, through an L8 level
 * whose byte at index i is i */
static int swz(uint32_t w, uint32_t h, uint32_t d, uint32_t x, uint32_t y)
{
    static uint8_t lvl[4096], rgba[64 * 64 * 4];
    uint32_t i;

    for (i = 0; i < sizeof lvl; i++)
        lvl[i] = (uint8_t)i;
    if (texkit_decode_raw(0x00, lvl, (size_t)w * h * d, w, h, d, rgba))
        return -1;
    return rgba[((size_t)y * w + x) * 4];
}

static void test_swizzle(void)
{
    /* 4x4: x0 y0 x1 y1 */
    CHECK(swz(4, 4, 1, 1, 0) == 1);
    CHECK(swz(4, 4, 1, 0, 1) == 2);
    CHECK(swz(4, 4, 1, 1, 1) == 3);
    CHECK(swz(4, 4, 1, 2, 0) == 4);
    CHECK(swz(4, 4, 1, 0, 2) == 8);
    CHECK(swz(4, 4, 1, 3, 3) == 15);
    /* 8x2: x0 y0 x1 x2; 2x8: x0 y0 y1 y2 */
    CHECK(swz(8, 2, 1, 2, 0) == 4);
    CHECK(swz(8, 2, 1, 4, 1) == 10);
    CHECK(swz(2, 8, 1, 0, 2) == 4);
    CHECK(swz(2, 8, 1, 1, 4) == 9);
    /* 4x4x4 slice 0: x0 y0 z0 x1 y1 z1 */
    CHECK(swz(4, 4, 4, 1, 0) == 1);
    CHECK(swz(4, 4, 4, 0, 1) == 2);
    CHECK(swz(4, 4, 4, 2, 0) == 8);
    CHECK(swz(4, 4, 4, 0, 2) == 16);
    /* too few bytes, not a power of two */
    {
        uint8_t lvl[16] = { 0 }, rgba[6 * 4 * 4];

        CHECK(texkit_decode_raw(0x00, lvl, 15, 4, 4, 1, rgba) == -1);
        CHECK(texkit_decode_raw(0x00, lvl, 16, 6, 2, 1, rgba) == -1);
    }
}

static void test_texels(void)
{
    uint8_t px[4];
    /* one texel each */
    static const uint8_t x8r8g8b8[4] = { 0x10, 0x20, 0x30, 0x99 };     /* B G R X */
    static const uint8_t a8b8g8r8[4] = { 0x10, 0x20, 0x30, 0x40 };
    static const uint8_t v8u8[2] = { 0x7F, 0x80 };
    static const uint8_t a8l8[2] = { 0x55, 0x66 };
    static const uint8_t r5g6b5[2] = { 0x1F, 0xF8 };                   /* 0xF81F: R 31 G 0 B 31 */
    static const uint8_t a4r4g4b4[2] = { 0x21, 0x43 };                 /* A 4 R 3 G 2 B 1 */

    CHECK(!texkit_decode_raw(0x07, x8r8g8b8, 4, 1, 1, 1, px) && px[0] == 0x30 && px[1] == 0x20
          && px[2] == 0x10 && px[3] == 255);
    CHECK(!texkit_decode_raw(0x3A, a8b8g8r8, 4, 1, 1, 1, px) && px[0] == 0x10 && px[3] == 0x40);
    CHECK(!texkit_decode_raw(0x28, v8u8, 2, 1, 1, 1, px) && px[0] == 0xFF && px[1] == 0x00
          && px[2] == 128 && px[3] == 255);
    CHECK(!texkit_decode_raw(0x1A, a8l8, 2, 1, 1, 1, px) && px[0] == 0x55 && px[2] == 0x55
          && px[3] == 0x66);
    CHECK(!texkit_decode_raw(0x05, r5g6b5, 2, 1, 1, 1, px) && px[0] == 255 && px[1] == 0
          && px[2] == 255 && px[3] == 255);
    CHECK(!texkit_decode_raw(0x04, a4r4g4b4, 2, 1, 1, 1, px) && px[0] == 51 && px[1] == 34
          && px[2] == 17 && px[3] == 68);
    CHECK(texkit_png_channels(0x00) == 1 && texkit_png_channels(0x1A) == 2
          && texkit_png_channels(0x28) == 3 && texkit_png_channels(0x0F) == 4);
}

static void test_dxt(void)
{
    /* c0 red > c1 blue: four colours; indices 0 1 2 3 on the first row */
    static const uint8_t four[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00 };
    /* c0 blue <= c1 red: DXT1's three colours + transparent black */
    static const uint8_t three[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0x00, 0x00, 0x00 };
    /* DXT5 alpha a0 255 > a1 0, texel 0 index 2, texel 1 index 7; colour block = four */
    static const uint8_t dxt5[16] = { 255, 0, 0x3A, 0, 0, 0, 0, 0,
                                      0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00 };
    /* DXT5 alpha a0 0 <= a1 255: index 2 = 51, 6 = 0, 7 = 255 (texels 0, 1, 2) */
    static const uint8_t dxt5b[16] = { 0, 255, 0xF2, 0x01, 0, 0, 0, 0,
                                       0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00 };
    static const uint8_t dxt3[16] = { 0x1F, 0, 0, 0, 0, 0, 0, 0,
                                      0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00 };
    uint8_t px[16 * 4];

    CHECK(!texkit_decode_raw(0x0C, four, 8, 4, 4, 1, px));
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);        /* c0 */
    CHECK(px[4] == 0 && px[6] == 255);                                      /* c1 */
    CHECK(px[8] == 170 && px[10] == 85 && px[11] == 255);                   /* (2a + b + 1) / 3 */
    CHECK(px[12] == 85 && px[14] == 170);                                   /* (a + 2b + 1) / 3 */
    CHECK(!texkit_decode_raw(0x0C, three, 8, 4, 4, 1, px));
    CHECK(px[8] == 128 && px[10] == 128 && px[11] == 255);                  /* (a + b + 1) / 2 */
    CHECK(px[12] == 0 && px[13] == 0 && px[14] == 0 && px[15] == 0);        /* transparent black */
    /* DXT3 / DXT5 colour blocks always use four colours */
    CHECK(!texkit_decode_raw(0x0E, dxt3, 16, 4, 4, 1, px));
    CHECK(px[3] == 255 && px[7] == 17 && px[11] == 0);                      /* nibbles x 17 */
    CHECK(!texkit_decode_raw(0x0F, dxt5, 16, 4, 4, 1, px));
    CHECK(px[3] == 218 && px[7] == 36);     /* (6*255 + 1) / 7, (1*255 + 6*0 + 1) / 7 */
    CHECK(!texkit_decode_raw(0x0F, dxt5b, 16, 4, 4, 1, px));
    CHECK(px[3] == 51 && px[7] == 0 && px[11] == 255);
    /* 2x2: one block, the top-left texels kept (row 1 is index 0 again) */
    CHECK(!texkit_decode_raw(0x0C, four, 8, 2, 2, 1, px) && px[4] == 0 && px[8] == 255);
    CHECK(texkit_decode_raw(0x0C, four, 7, 4, 4, 1, px) == -1);
}

static uint32_t be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void test_png(void)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t px[3 * 2 * 2], *f = NULL, *z = NULL;
    size_t n = 0, nz = 0;
    int i;

    CHECK(deflate_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(deflate_crc32(deflate_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5)
          == 0xCBF43926u);
    CHECK(!deflate_fixed((const uint8_t *)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 48, &z, &nz)
          && nz > 0 && nz < 16);
    free(z);
    for (i = 0; i < (int)sizeof px; i++)
        px[i] = (uint8_t)(i * 20);
    CHECK(!png_encode(3, 2, 2, px, &f, &n));
    if (f) {
        CHECK(n > 8 + 25 + 12 + 12 && !memcmp(f, sig, 8));
        CHECK(be(f + 8) == 13 && !memcmp(f + 12, "IHDR", 4));
        CHECK(be(f + 16) == 3 && be(f + 20) == 2 && f[24] == 8 && f[25] == 4);  /* grey + alpha */
        CHECK(be(f + 29) == deflate_crc32(0, f + 12, 17));
        CHECK(!memcmp(f + 37, "IDAT", 4) && f[41] == 0x78 && f[42] == 0x01);
        CHECK(be(f + n - 4) == 0xAE426082u && !memcmp(f + n - 8, "IEND", 4));
        CHECK(be(f + 33) + 12 + 33 + 12 == n);                                   /* IDAT length */
        CHECK(be(f + 41 + be(f + 33)) == deflate_crc32(0, f + 37, be(f + 33) + 4));
    }
    free(f);
    CHECK(png_encode(0, 2, 4, px, &f, &n) == -1);
    CHECK(png_encode(2, 2, 5, px, &f, &n) == -1);
}

int main(void)
{
    test_hash();
    test_swizzle();
    test_texels();
    test_dxt();
    test_png();
    test_sizes();
    test_layout();
    test_names();
    test_unique();
    if (s_fail) {
        printf("%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("texkit: all checks passed\n");
    return 0;
}
