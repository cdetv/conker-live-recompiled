/*
 * The package reader's unpacker (to-do 43): a packed package's sections go
 * through the game's own decompressor, sub_00416170 (Ghidra:
 * caff_unpack_blocks), called the way the game's loader reaches it
 * (caff_decode_sections 0x004067B0 -> codec slot 0x0084BD50 = 0x00063100 ->
 * 0x00416170: ecx source, edx destination, eax stored size, with the three
 * loader globals 0x84BE2C / 0x84BE30 / 0x84BE38 set around it). It is plain
 * computation on guest memory: no indirect calls, no kernel.
 *
 * Only before the game runs, on the thread that will run it: the stored
 * section is copied into a scratch part of guest RAM the game has not touched
 * yet, unpacked there, copied out. caff_guest_begin() checks that the scratch
 * is still all zero (else it refuses: something else lives there now) and
 * caff_guest_end() zeroes it again, so the game starts on the RAM it expects.
 *
 * Scratch: 0x01800000 (stored, up to 14 MB) and 0x02600000 (unpacked, up to
 * 26 MB, then a 64 KB guard) to 0x04000000, the end of the retail 64 MB. Not
 * Txukie's 0x04000000+ (written for 128 MB): our runtime maps the 64 MB again
 * every 64 MB, so that range aliases the XBE image and the stack (the Berri
 * research saw 156 of 266 packages come out wrong there).
 *
 * The game decodes a section only when the packed flag is set and its stored
 * size is below its unpacked size (caff_decode_sections, checked 2026-11-18);
 * caff.c's rule (flag set and stored != unpacked, block chain checked) gives
 * the same answer for every package on the disc.
 * The approach after the Berri research's berri_pkg.c and Txukie's
 * conker_caff.c (MIT); this code is ours. That the game's own decompressor
 * can be called this way was first published in birdytsc's clr_unpack
 * (knowledge only: no code from it is used).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "caff.h"
#include "recomp_types.h"

void sub_00416170(void);

#define GUEST_SRC    0x01800000u
#define GUEST_DST    0x02600000u
#define GUEST_END    0x04000000u
#define GUARD        0x10000u
#define GUARD_BYTE   0xA5u

static int s_ready;

/* Is guest [from, to) all zero? */
static int guest_zero(uint32_t from, uint32_t to)
{
    const uint64_t *p = (const uint64_t *)XBOX_PTR(from);
    size_t i, n = (to - from) / 8;

    for (i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

int caff_guest_begin(void)
{
    if (!g_xbox_mem_offset)
        return CAFF_E_NEED_GUEST;
    if (!guest_zero(GUEST_SRC, GUEST_END)) {
        fprintf(stderr, "[CAFF] guest scratch 0x%08X-0x%08X is in use: "
                "packed packages can't be unpacked now\n", GUEST_SRC, GUEST_END);
        return CAFF_E_NEED_GUEST;
    }
    s_ready = 1;
    return CAFF_OK;
}

void caff_guest_end(void)
{
    if (!s_ready)
        return;
    memset((void *)XBOX_PTR(GUEST_SRC), 0, GUEST_END - GUEST_SRC);
    s_ready = 0;
}

int caff_guest_unpack(const uint8_t *src, size_t n_src, uint8_t *dst, size_t n_dst)
{
    uint32_t save[7], loader[3], i;
    const uint8_t *guard;

    if (!s_ready)
        return CAFF_E_NEED_GUEST;
    if (n_src > GUEST_DST - GUEST_SRC || n_dst > GUEST_END - GUEST_DST - GUARD)
        return CAFF_E_SECTION_TOO_BIG;
    memcpy((void *)XBOX_PTR(GUEST_SRC), src, n_src);
    memset((void *)XBOX_PTR(GUEST_DST), 0, n_dst);
    memset((void *)XBOX_PTR(GUEST_DST + (uint32_t)n_dst), GUARD_BYTE, GUARD);

    save[0] = g_eax; save[1] = g_ebx; save[2] = g_ecx; save[3] = g_edx;
    save[4] = g_esi; save[5] = g_edi; save[6] = g_esp;
    loader[0] = MEM32(0x84BE2C);
    loader[1] = MEM32(0x84BE30);
    loader[2] = MEM32(0x84BE38);
    MEM32(0x84BE2C) = 0x630A0u;              /* as 0x00063100 sets them */
    MEM32(0x84BE30) = 0x630C0u;
    MEM32(0x84BE38) = 1u;
    g_ecx = GUEST_SRC;
    g_edx = GUEST_DST;
    g_eax = (uint32_t)n_src;
    PUSH32(g_esp, 0u);                        /* return address, popped by ret */
    sub_00416170();
    MEM32(0x84BE2C) = loader[0];
    MEM32(0x84BE30) = loader[1];
    MEM32(0x84BE38) = loader[2];
    g_eax = save[0]; g_ebx = save[1]; g_ecx = save[2]; g_edx = save[3];
    g_esi = save[4]; g_edi = save[5]; g_esp = save[6];

    /* The blocks add up to n_dst (caff_header_read checked the chain): a
     * write past it means the decoder ran off on damaged data. */
    guard = (const uint8_t *)XBOX_PTR(GUEST_DST + (uint32_t)n_dst);
    for (i = 0; i < GUARD; i++)
        if (guard[i] != GUARD_BYTE)
            return CAFF_E_UNPACK;
    memcpy(dst, (const void *)XBOX_PTR(GUEST_DST), n_dst);
    return CAFF_OK;
}
