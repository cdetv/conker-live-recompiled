/**
 * Sharper shadows (to-do 17; SPEC 10.1 "Shadow quality").
 *
 * Conker casts each character's shadow by drawing the character from the
 * light into a small picture (colour R5G6B5, depth Z16: 128x128, and 64x64
 * for a third, lesser light), blurring it in place with a 4-tap pass, then
 * projecting it onto the ground with a multiply blend
 * (conker-notes 2026-10-11, 2026-11-03). At 128 pixels a character is about
 * 20 x 45 of them, so the shadows are blocky and crawl as he moves. The
 * renderer can draw chosen small targets 2, 4 or 8 times bigger in each
 * direction without more video memory (the toolkit's xbox_SetSurfaceBoost);
 * this file chooses the shadow pictures, by their format: no other target
 * in the game uses R5G6B5 with Z16 (every surface made in the November
 * logs). Their addresses move between levels, so the format, not an
 * address, picks them.
 *
 * shadow_resolution in settings.ini / RECOMP_SHADOW_RES: original (1x),
 * 2x, 4x (default), 8x or 16x; F2 Picture changes it at once.
 *
 * Softer shadows (to-do 105): shadow_softness / RECOMP_SHADOW_SOFTNESS,
 * 0-100 %, a slider under Shadows. The toolkit lays a Gaussian over the
 * blur pass's reads of a boosted picture (xbox_SetSurfaceBoostBlur): 100 %
 * is SOFT_MAX_PX picture pixels (sigma), the same softness at every size;
 * a picture blurred that much is drawn no bigger than the blur needs, so
 * soft costs little GPU at 16x. Original (1x) is the game's own: the slider
 * does nothing there.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned (*xbox_SurfaceBoostFn)(uint32_t addr, uint32_t format,
                                        uint32_t pitch, uint32_t w,
                                        uint32_t h);
void xbox_SetSurfaceBoost(xbox_SurfaceBoostFn fn);
void xbox_SetSurfaceBoostBlur(float sigma_guest_px);

/* Softness at 100 %: sigma in shadow-picture pixels (Conker's body is about
 * 20 of them across). */
#define SOFT_MAX_PX 2.0f

/* SURFACE_FORMAT: colour bits 0-3 (3 = R5G6B5), zeta bits 4-7 (1 = Z16),
 * type bits 8-11. The characters are drawn into a picture set up linear
 * (type 1) and the blur pass draws over it set up swizzled (type 2, the
 * texture it reads is swizzled R5G6B5): both are the same picture, so both
 * must get the same factor, or it is remade at the other size twice a frame
 * and the blur runs at 1x (run_20261106_114328). */
#define SHADOW_FORMAT 0x13u
#define SHADOW_MAX    128u

static volatile unsigned g_shadow_factor = 4;

static unsigned shadow_boost(uint32_t addr, uint32_t format, uint32_t pitch,
                             uint32_t w, uint32_t h)
{
    uint32_t type = (format >> 8) & 0xFu;
    (void)addr;
    (void)pitch;
    if ((format & 0xFFu) != SHADOW_FORMAT || (type != 1 && type != 2)
        || w > SHADOW_MAX || h > SHADOW_MAX)
        return 1;
    return g_shadow_factor;
}

/* 1, 2, 4, 8 or 16 from a setting's value: "original" or "1x" is 1. */
static unsigned shadow_factor_from(const char *v)
{
    unsigned n;
    if (!v || !*v || !_stricmp(v, "original"))
        return 1;
    n = (unsigned)strtoul(v, NULL, 10);
    return n == 2 || n == 4 || n == 8 || n == 16 ? n : 1;
}

/* The F2 menu's choice, at once: 1 = the original size. 16x (user
 * 2026-11-06) is 2048 x 2048 a picture, the only one that takes more video
 * memory (16 MB a picture). */
void conker_SetShadowRes(unsigned factor)
{
    if (factor != 2 && factor != 4 && factor != 8 && factor != 16)
        factor = 1;
    g_shadow_factor = factor;
    xbox_SetSurfaceBoost(factor > 1 ? shadow_boost : NULL);
    fprintf(stderr, "[SHADOWS] shadow pictures drawn at %ux (%u x %u)\n",
            factor, SHADOW_MAX * factor, SHADOW_MAX * factor);
}

/* The F2 slider's softness, 0-100 %, at once (from each shadow's next
 * blur); 0 = the game's own blur only. */
void conker_SetShadowSoftness(float pct)
{
    static float said = -100.0f;
    if (!(pct > 0.0f))                  /* NaN too */
        pct = 0.0f;
    if (pct > 100.0f)
        pct = 100.0f;
    xbox_SetSurfaceBoostBlur(pct * SOFT_MAX_PX / 100.0f);
    /* The slider calls this every frame it moves: log whole percents. */
    if ((int)(pct + 0.5f) != (int)(said + 0.5f)) {
        said = pct;
        fprintf(stderr, "[SHADOWS] softness %.0f %% (blur %.2f picture "
                "pixels)\n", pct, pct * SOFT_MAX_PX / 100.0f);
    }
}

/* At start-up, from RECOMP_SHADOW_RES and RECOMP_SHADOW_SOFTNESS
 * (settings.ini's shadow_resolution, shadow_softness); unset = 4x, 50 %
 * (user 2026-11-06). */
void shadows_init(void)
{
    const char *e = getenv("RECOMP_SHADOW_RES");
    conker_SetShadowRes(e && *e ? shadow_factor_from(e) : 4);
    e = getenv("RECOMP_SHADOW_SOFTNESS");
    conker_SetShadowSoftness(e && *e ? (float)atof(e) : 50.0f);
}
