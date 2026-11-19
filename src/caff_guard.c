/*
 * Guards for features that load carved assets (to-do 43, step 7; plan
 * docs/research/r4-0.3.4-package-reader.md section 4, r3-asset-loading.md
 * "For the package reader"). The game halts on a model or xml it can't find,
 * and its graphics pool is a fixed ~34 MB with only 2-5 MB free after a level
 * load, so a feature asks first, before the request:
 *
 *   conker_model_status(path)    the asset table's status for a model path
 *                                (src/recomp_manual.c, the attachment
 *                                guard's): 0 / 0x3E4 / 0x3E5 usable, -1 none;
 *                                a model only the asset cache holds is 0
 *   caff_pool_free()             free bytes in the graphics pool,
 *                                [[0x0084A188]+0x34]; 0 when there is none yet
 *   caff_bundle_fits(name, gpu)  does the pool, less a 1 MB margin, hold a
 *                                bundle's .gpu? One [BUNDLE] line either way.
 *
 * A bundle's .data goes to the main heap (in our runtime the 191 MB extended
 * area: no practical limit), so only .gpu is checked. Nothing in 0.3.4 loads
 * a bundle; the first users are to-dos 61, 192 and 243.
 *
 * Checks without a user: RECOMP_CAFF_LOG=1 prints the pool after every level
 * load ([BUNDLE] pool line), RECOMP_BUNDLE_TEST=<bytes> asks caff_bundle_fits
 * about that many bytes then.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

#define POOL_PTR       0x0084A188u      /* the graphics pool's header */
#define POOL_FREE      0x34u            /* its free bytes */
#define BUNDLE_MARGIN  (1u << 20)       /* kept free for the level's own needs */

static int va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u && !(va & 3u);
}

uint32_t caff_pool_free(void)
{
    uint32_t pool = MEM32(POOL_PTR);

    return va_ok(pool) ? MEM32(pool + POOL_FREE) : 0;
}

int caff_bundle_fits(const char *name, uint32_t gpu_bytes)
{
    uint32_t free_b = caff_pool_free();
    int fits = free_b > BUNDLE_MARGIN && gpu_bytes <= free_b - BUNDLE_MARGIN;

    fprintf(stderr, "[BUNDLE] %s: .gpu %u bytes, graphics pool %u bytes free (%.2f MB, 1 MB kept): %s\n",
            name, gpu_bytes, free_b, free_b / 1e6, fits ? "fits" : "does not fit, not loaded");
    return fits;
}

static int flag_on(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0");
}

/* After every level load (src/recomp_manual.c sub_002AADC0). */
void caff_guard_after_load(uint32_t level)
{
    static int log = -1;
    static uint32_t test;

    if (log < 0) {
        const char *t = getenv("RECOMP_BUNDLE_TEST");
        log = flag_on("RECOMP_CAFF_LOG");
        test = t && t[0] ? (uint32_t)strtoul(t, NULL, 0) : 0;
    }
    if (log) {
        uint32_t pool = MEM32(POOL_PTR);
        fprintf(stderr, "[BUNDLE] pool after level 0x%02X: header 0x%08X, %u bytes free (%.2f MB)\n",
                level, pool, caff_pool_free(), caff_pool_free() / 1e6);
    }
    if (test)
        caff_bundle_fits("RECOMP_BUNDLE_TEST", test);
}
