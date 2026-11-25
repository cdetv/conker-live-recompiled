/*
 * The Milk Fix's glass (Extras, to-do 45; "Intro Fix" in the plans, named "Milk Fix" by
 * the user 2026-11-23; SPEC 10.4): the opening scene's goblet becomes a low glass half full
 * of milk, as on the N64. The glass is the front end's effects_models_whiskey_glass_filled,
 * carved from the player's own Level\Frontend package (caff_cache.c recipe "milk"); this
 * turns its drink into milk before it is written to the asset cache as
 * aid\bfdmodel\attach\effects_models_conker_mil2\default.rbm. The goblet's name then gets one
 * letter changed at each level load (addons.c), so the throne room asks for that file.
 *
 * milk_glass_make() is make_milk() of conker_intro.c in Txukie's xboxrecomp-conker (commit
 * d0edebcfeac54ea1e0cb099d5bf0a5be92e8c300), changed only to work on the carved bytes in
 * memory instead of a file. Its licence:
 *
 * MIT License
 *
 * Copyright (c) 2026 sp00nz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "milk_glass.h"

/* In the model's .gpu part. */
#define GLASS_COLOURS  0x0000u
#define GLASS_VERTS    161u
#define DRINK_COLOURS  0x08D0u
#define DRINK_POS      0x09B4u
#define DRINK_NORMALS  0x0B0Cu
#define NORMAL_UP      0x7FC00000u      /* 11:11:10 (0, 0, 1) */
#define DRINK_VERTS    57u
#define DRINK_TEXTURE  0x3500u          /* the second, 64x64 DXT1 with mips */
#define DRINK_TEX_SIZE 2816u
#define DRINK_Z_LOW    2188             /* its bottom */
#define DRINK_Z_HIGH   8397             /* its top, as made */
#define MILK_Z_HIGH    7151             /* halfway up the glass's inside */

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The drink becomes milk, in the glass's carved bytes. */
int milk_glass_make(uint8_t *file, size_t size)
{
    uint8_t *gpu;
    const uint32_t normal_up = NORMAL_UP;
    uint32_t i;

    if (!file || size < 0x180)
        return 0;
    {
        uint64_t off = (uint64_t)get32(file + 0x38) + get32(file + 0x40 + 0x10);
        if (off + DRINK_TEXTURE + DRINK_TEX_SIZE > (uint64_t)size)
            return 0;
        gpu = file + off;
    }
    /* The glass we looked at: its colours, the drink's, its amber texture. */
    if (get32(gpu + GLASS_COLOURS) != 0x99FFFFFFu || get32(gpu + DRINK_COLOURS) != 0xCCFFFFFFu
            || get32(gpu + DRINK_TEXTURE) != 0x00000820u)
        return 0;
    /* Clearer: as made it is dark over anything but a bright pub. */
    for (i = 0; i < GLASS_VERTS; i++)
        gpu[GLASS_COLOURS + 4 * i + 3] /= 3;
    for (i = 0; i < DRINK_VERTS; i++) {
        uint8_t *c = gpu + DRINK_COLOURS + 4 * i;
        uint8_t *p = gpu + DRINK_POS + 6 * i + 4;
        int16_t z;

        c[3] = 0xFF;
        memcpy(gpu + DRINK_NORMALS + 4 * i, &normal_up, 4);
        memcpy(&z, p, 2);
        z = (int16_t)(DRINK_Z_LOW + (z - DRINK_Z_LOW) * (MILK_Z_HIGH - DRINK_Z_LOW)
                                    / (DRINK_Z_HIGH - DRINK_Z_LOW));
        memcpy(p, &z, 2);
    }
    /* DXT1 blocks all of one white. */
    for (i = 0; i + 8 <= DRINK_TEX_SIZE; i += 8) {
        static const uint8_t white[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };
        memcpy(gpu + DRINK_TEXTURE + i, white, 8);
    }
    return 1;
}
