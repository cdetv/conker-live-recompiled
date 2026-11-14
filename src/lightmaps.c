/**
 * Smoother world shadows (to-do 106; SPEC 10.1 item 9).
 *
 * The shadows of fences, plants and buildings are not drawn at run time:
 * they are baked into each level's light maps, low-resolution pictures of
 * the light stretched over the level (the field's island: 512 x 512
 * greyscale, about 256 texels across it), multiplied onto the textures.
 * Read bilinear, every light-map texel shows, and a shadow edge is a
 * staircase of blocks. The toolkit can read chosen textures as a cubic
 * B-spline instead (xbox_SetTextureSmooth: four bilinear reads, a smooth
 * rounded edge); this file chooses the light maps.
 *
 * Which textures are light maps (conker-notes 2026-11-06): the artists
 * named them, and the level's shaders keep the names.
 *   - [0x0076FE00] is the level's scene graph ("bfdlevel"): +0x08 the root
 *     node, +0x34 the number of its textures, +0x38 their table of records
 *     (+0x00 D3DFMT code, +0x14 the data, +0x04 kind: 4 = animated, with
 *     +0x20 frames and +0x1C an array of their D3DTextures).
 *   - A node: +0x10 its type (registry [0x00822670] + n x 0x54), +0x14 its
 *     first child, +0x18 its next sibling. Types 10 and 11
 *     (ndShaderParam2[WithVisibility]) hold a shader at +0x30 and at +0x38
 *     {count, array of 0x28-byte entries starting with a shader}.
 *   - A shader: +0x08 its texture slots (0x2C bytes; +0x00 the index in the
 *     level's texture table, as FUN_003f4ce0 binds it), +0x0C how many,
 *     +0x28 its parameters ({name, 0, slot, kind}, 16 bytes), +0x2C how
 *     many. Each slot has a parameter named "colour" + its texture's name,
 *     kind 3, and the light maps' names say so ("colourfile430light",
 *     "colourbits_test_file186light", "..._lightmap1"; lm_light_name).
 *   So: every texture a light-map parameter's slot points at. Plus the
 *   characters' own top-down light map, [0x0084720C] (FUN_00083de0 binds
 *   it; it alternates with a 4x4 default as characters draw, so it adds).
 * The walk runs when the level changes and once more 2 s later (a level
 * still loading at its first draw); the toolkit asks per draw and stage,
 * on the executor thread, with the stage's texture address. Guest reads
 * are guarded: a half-built graph can point anywhere.
 *
 * smooth_world_shadows in settings.ini / RECOMP_SMOOTH_LIGHTMAPS: 1 (on,
 * default) or 0 (the original bilinear look); F2 Picture changes it at
 * once. RECOMP_LIGHTMAP_LOG=1: [LIGHTMAP] per level (nodes, shaders, the
 * light maps found and their names), for surveys with RECOMP_SOAK.
 */

#include "dev_logs.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

typedef int (*xbox_TextureSmoothFn)(uint32_t addr, uint32_t format,
                                    uint32_t w, uint32_t h);
void xbox_SetTextureSmooth(xbox_TextureSmoothFn fn);

#define LEVEL_SCENEGRAPH 0x0076FE00u    /* the level's scene graph */
#define NODE_REGISTRY    0x00822670u    /* node types, 0x54 bytes each */
#define CHAR_LIGHTMAP    0x0084720Cu    /* the characters' light map record */
#define TYPE_SHADERPARAM 10u            /* ndShaderParam2; 11 WithVisibility */
#define KIND_ANIMATED    4u
#define PARAM_TEXTURE    3u
#define LM_MAX           512            /* pictures (animation frames count) */
#define LM_RECS          256            /* light-map records per level (the
                                         * survey's most: 64) */
#define WALK_MAX         100000         /* nodes, against a looping graph */
#define RESCAN_MS        2000u

static uint32_t s_lm[LM_MAX];           /* data addresses, & 0x03FFFFFF */
static int      s_lm_n;
static uint32_t s_key[3];               /* the level the list was built for */
static ULONGLONG s_rescan_at;           /* GetTickCount64 of the 2nd walk */
static uint32_t s_ch_seen[8];           /* characters' records added */
static int      s_ch_n;
static uint32_t s_ch_last;
static uint32_t s_stack[4096];

static int lm_logging(void)
{
    return dev_log(DLOG_LIGHTMAP);
}

/* A guest pointer into main memory or the heap, where the level lives. */
static int lm_ptr_ok(uint32_t p)
{
    return !(p & 3u) && ((p >= 0x00010000u && p < 0x04000000u)
                         || (p >= 0x74000000u && p < 0x78000000u));
}

static void lm_add(uint32_t data)
{
    int i;
    data &= 0x03FFFFFFu;
    if (!data)
        return;
    for (i = 0; i < s_lm_n; i++)
        if (s_lm[i] == data)
            return;
    if (s_lm_n < LM_MAX)
        s_lm[s_lm_n++] = data;
}

/* Every picture of texture record rec: its frames when animated, from the
 * frames' D3DTextures (+0x1C, an array of them: what texset_pick_layer
 * binds; Data at +4). Not from the data list at +0x24: its first entry is
 * not frame 0 (field: 0x2761F380 where frame 0 is 0x0159FE80), so frame 0
 * stayed blocky, one picture in 20 of the lamp's swing (user 2026-11-06). */
static void lm_add_record(uint32_t rec)
{
    lm_add(MEM32(rec + 0x14u));
    if (MEM32(rec + 4u) == KIND_ANIMATED) {
        uint32_t n = MEM32(rec + 0x20u), arr = MEM32(rec + 0x1Cu), i;
        if (!lm_ptr_ok(arr) || n > 64u)
            return;
        for (i = 0; i < n; i++) {
            uint32_t tex = MEM32(arr + 4u * i);
            if (lm_ptr_ok(tex))
                lm_add(MEM32(tex + 4u));
        }
        if (lm_logging())
            fprintf(stderr, "[LIGHTMAP]   animated 0x%08X: %u frames, frame 0"
                    " 0x%08X\n", rec, n,
                    lm_ptr_ok(MEM32(arr)) ? MEM32(MEM32(arr) + 4u) : 0u);
    }
}

/* Parameter name at p names a light map: "colour" + a texture name that has
 * "lightmap" in it, or ends in "light" once a "tga" and trailing digits
 * and underscores are dropped ("file430light", "file921light1",
 * "house256_tgalight", "..._lambert2sg_light_tga"). Not a texture that is
 * only about lamps or light colours ("light_arm_01_tga",
 * "cobblefloorlighter_tga", "berrilightwoodfloor128_tga", "lights1_256_
 * normal_rgb"): the 2026-11-06 survey's 841 "light" names, 809 kept. */
static int lm_light_name(uint32_t p, char *out, size_t outn)
{
    char s[96];
    size_t i, n;
    for (i = 0; i + 1 < sizeof s; i++) {
        s[i] = (char)MEM8(p + (uint32_t)i);
        if (!s[i])
            break;
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = (char)(s[i] - 'A' + 'a');
    }
    s[i] = 0;
    if (strncmp(s, "colour", 6) != 0)
        return 0;
    if (!strstr(s + 6, "lightmap")) {
        char t[96];
        strncpy_s(t, sizeof t, s + 6, _TRUNCATE);
        n = strlen(t);
        if (n >= 3 && !strcmp(t + n - 3, "tga")) {
            n -= 3;
            if (n && (t[n - 1] == '_' || t[n - 1] == '.'))
                n--;
        }
        while (n && ((t[n - 1] >= '0' && t[n - 1] <= '9') || t[n - 1] == '_'))
            n--;
        t[n] = 0;
        if (n <= 5 || strcmp(t + n - 5, "light") != 0)
            return 0;
    }
    strncpy_s(out, outn, s, _TRUNCATE);
    return 1;
}

/* Shader sh's light-map slots: their texture indices into recs (n so far,
 * deduplicated), names to the log. */
static int lm_shader(uint32_t sh, uint32_t count, uint32_t *recs, int n)
{
    uint32_t slots, nslots, params, nparams, i;
    if (!lm_ptr_ok(sh))
        return n;
    slots = MEM32(sh + 0x08u);
    nslots = MEM32(sh + 0x0Cu);
    params = MEM32(sh + 0x28u);
    nparams = MEM32(sh + 0x2Cu);
    if (!lm_ptr_ok(slots) || !lm_ptr_ok(params) || nslots > 8u
        || nparams > 256u)
        return n;
    for (i = 0; i < nparams; i++) {
        uint32_t e = params + 16u * i, name = MEM32(e);
        uint32_t slot = MEM32(e + 8u), kind = MEM32(e + 12u), idx;
        char nm[80];
        int k;
        if (kind != PARAM_TEXTURE || slot >= nslots || !lm_ptr_ok(name & ~3u)
            || !lm_light_name(name, nm, sizeof nm))
            continue;
        idx = MEM32(slots + 0x2Cu * slot);
        if (idx >= count)
            continue;
        for (k = 0; k < n && recs[k] != idx; k++)
            ;
        if (k == n && n < LM_RECS) {
            recs[n++] = idx;
            if (lm_logging())
                fprintf(stderr, "[LIGHTMAP]   %s: slot %u, texture #%u\n",
                        nm, slot, idx);
        }
    }
    return n;
}

/* The level's light maps: walk its scene graph from the root. 0 when a
 * read failed (a graph still being built). */
static int lm_walk(uint32_t sg, uint32_t count, uint32_t table)
{
    uint32_t recs[LM_RECS], reg = MEM32(NODE_REGISTRY);
    int nrec = 0, sp = 0, nodes = 0, shaders = 0, i;

    s_lm_n = 0;
    if (!lm_ptr_ok(table) || !lm_ptr_ok(reg))
        return 1;
    s_stack[sp++] = MEM32(sg + 0x08u);
    while (sp > 0 && nodes < WALK_MAX) {
        uint32_t nd = s_stack[--sp], t, ti;
        if (!lm_ptr_ok(nd))
            continue;
        nodes++;
        t = MEM32(nd + 0x10u);
        ti = t >= reg && (t - reg) % 0x54u == 0 ? (t - reg) / 0x54u : ~0u;
        if (ti == TYPE_SHADERPARAM || ti == TYPE_SHADERPARAM + 1) {
            uint32_t var = MEM32(nd + 0x38u);
            nrec = lm_shader(MEM32(nd + 0x30u), count, recs, nrec);
            shaders++;
            if (lm_ptr_ok(var)) {
                uint32_t vn = MEM32(var), va = MEM32(var + 4u), j;
                for (j = 0; j < vn && j < 16u && lm_ptr_ok(va); j++)
                    nrec = lm_shader(MEM32(va + 0x28u * j), count, recs,
                                     nrec);
            }
        }
        if (sp + 2 <= (int)(sizeof s_stack / sizeof s_stack[0])) {
            s_stack[sp++] = MEM32(nd + 0x18u);
            s_stack[sp++] = MEM32(nd + 0x14u);
        }
    }
    for (i = 0; i < nrec; i++) {
        uint32_t rec = MEM32(table + 4u * recs[i]);
        if (lm_ptr_ok(rec))
            lm_add_record(rec);
    }
    if (lm_logging())
        fprintf(stderr, "[LIGHTMAP] level 0x%08X (id 0x%02X): %d nodes, %d"
                " shader nodes, %d light maps of %u textures, %d pictures\n",
                sg, MEM8(0x007FA1F8u), nodes, shaders, nrec, count, s_lm_n);
    return 1;
}

static void lm_rebuild(uint32_t sg, uint32_t count, uint32_t table)
{
    int ok = 0;
    __try {
        ok = lm_walk(sg, count, table);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (!ok) {
        s_lm_n = 0;
        if (lm_logging())
            fprintf(stderr, "[LIGHTMAP] level 0x%08X: graph not readable"
                    " yet\n", sg);
    }
    /* The characters' maps seen in the last level are forgotten. */
    s_ch_n = 0;
    s_ch_last = 0;
}

/* The characters' light map record ch: its pictures, once. */
static void lm_character(uint32_t ch)
{
    int i;
    s_ch_last = ch;
    if (!lm_ptr_ok(ch))
        return;
    for (i = 0; i < s_ch_n; i++)
        if (s_ch_seen[i] == ch)
            return;
    if (s_ch_n < (int)(sizeof s_ch_seen / sizeof s_ch_seen[0]))
        s_ch_seen[s_ch_n++] = ch;
    __try {
        lm_add_record(ch);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (lm_logging())
        fprintf(stderr, "[LIGHTMAP] characters' light map 0x%08X: fmt %02X"
                " %ux%u, %d pictures now\n", ch, MEM32(ch) & 0xFFu,
                MEM32(ch + 0x0Cu) & 0xFFFFu, MEM32(ch + 0x0Cu) >> 16,
                s_lm_n);
}

static int lm_smooth(uint32_t addr, uint32_t format, uint32_t w, uint32_t h)
{
    uint32_t sg = MEM32(LEVEL_SCENEGRAPH), count = 0, table = 0;
    uint32_t ch = MEM32(CHAR_LIGHTMAP);
    int i;

    (void)format;
    (void)w;
    (void)h;
    if (lm_ptr_ok(sg)) {
        count = MEM32(sg + 0x34u);
        table = MEM32(sg + 0x38u);
        if (count > 4096u)
            count = 0;
    } else {
        sg = 0;
    }
    if (sg != s_key[0] || count != s_key[1] || table != s_key[2]) {
        s_key[0] = sg;
        s_key[1] = count;
        s_key[2] = table;
        s_rescan_at = sg ? GetTickCount64() + RESCAN_MS : 0;
        if (sg)
            lm_rebuild(sg, count, table);
        else
            s_lm_n = 0;
    } else if (s_rescan_at && GetTickCount64() >= s_rescan_at) {
        s_rescan_at = 0;
        lm_rebuild(sg, count, table);
    }
    if (ch != s_ch_last)
        lm_character(ch);
    addr &= 0x03FFFFFFu;
    for (i = 0; i < s_lm_n; i++)
        if (s_lm[i] == addr)
            return 1;
    return 0;
}

/* The F2 menu's choice, at once: 1 = smooth light maps, 0 = bilinear. */
void conker_SetSmoothLightmaps(int on)
{
    xbox_SetTextureSmooth(on ? lm_smooth : NULL);
    fprintf(stderr, "[LIGHTMAP] light maps read %s\n",
            on ? "smoothly (B-spline)" : "bilinear (original)");
}

/* At start-up, from RECOMP_SMOOTH_LIGHTMAPS (settings.ini's
 * smooth_world_shadows); unset = on. */
void lightmaps_init(void)
{
    const char *e = getenv("RECOMP_SMOOTH_LIGHTMAPS");
    conker_SetSmoothLightmaps(!(e && e[0] == '0'));
}
