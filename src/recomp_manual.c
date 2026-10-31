/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <intrin.h>

/* The generated header: the register globals (thread-local, RECOMP_TLS),
 * MEM32/XBOX_PTR, recomp_func_t and the ICALL trace ring, all declared there.
 * The template declared its own copies, and its plain `extern uint32_t g_eax`
 * named the wrong (non-thread-local) variable. Without RECOMP_GENERATED_CODE
 * it does not define eax/ecx/... as macros, so registers are g_eax etc. */
#include "recomp_types.h"
#include "settings.h"
#include "controls.h"
#include "xbox_kbm.h"
#include "photo_mode.h"

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces.
 *
 * One line per site, not the first 50 lines: boot's cli/sti at 0x0055FC40 and
 * 0x0055FC51 alone used all 50, so the cmovnp that ended every multiplayer
 * round at once (0x00266E2B, 2026-10-21) never printed. Unlocked: two threads
 * hitting a new site together at worst print it twice. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static uint32_t seen[256];
    static int nseen;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';
    int i;

    for (i = 0; i < nseen; i++)
        if (seen[i] == va && !stop)
            return;
    if (nseen < 256)
        seen[nseen++] = va;
    else if (!stop)
        return;
    fprintf(stderr,
            "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
            " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
            text, va);
    fflush(stderr);
    if (stop) abort();
}

/* ── Conker: data the disassembler took for code ──────────────────────────
 *
 * Defined here so `--exclude-manual src/recomp_manual.c` (scripts/pipeline.ps1)
 * stops the recompiler generating a body for them. Each is a block of data
 * that the gap scan ("gap_prologue") picked up as a function: no callers, no
 * function in Ghidra. Its generated body was nonsense, and at least one did
 * not compile. Reaching one means guest control flow went badly wrong, so say
 * where and stop. */

static void conker_not_code(uint32_t va)
{
    fprintf(stderr, "[CONKER] jumped into data at 0x%08X (not a function, "
            "see src/recomp_manual.c)\n", va);
    fflush(stderr);
    abort();
}

/* 0x00389F5A: 2 bytes of mov edi,edi filler, then the jump table and byte
 * index table of the switch in sub_00389F20 (entries point back into it).
 * Its generated body failed to compile: `_cf` undeclared, from lahf after
 * dec (a toolkit lifter bug, docs/architecture.md gotchas). */
void sub_00389F5A(void)
{
    conker_not_code(0x00389F5Au);
}

/* ── Conker: widescreen (RECOMP_WIDESCREEN=1, or RECOMP_RENDER_RES) ──────
 *
 * Conker has no 16:9 mode of its own. The community "60fps + widescreen"
 * XBE patch (moddb, same base XBE as ours; docs/conker-notes.md 2026-10-13)
 * inserts one `mulss xmm1, 4/3` at 0x00064468 in sub_00064330, just before
 * its tail jump to sub_000645C0. That function takes a viewport's width
 * (xmm1, from +0x260) and height (xmm0, +0x264), stores the height and
 * width/height (the projection's aspect ratio) in the camera at +0x104 and
 * +0x108. Widening the width by 4/3 turns 4:3 into 16:9; the window then
 * shows the 640x480 frame at 16:9 (toolkit fb_present.c, same switch).
 *
 * Hooked at the entry of sub_000645C0 instead, so its other two callers
 * (0x00064529, 0x0006459B, which recompute the same aspect) widen too: the
 * whole frame is stretched, so every 3D viewport must be. The patch leaves
 * them at 4:3. The HUD and menus are 2D and stay stretched, as on Xbox.
 *
 * Any other shape the same way: the toolkit's xbox_DisplayShape says what
 * the window shows (RECOMP_RENDER_RES=2560x1080 is 64:27, 21:9), and the
 * width is widened by that over 4:3. 16:9 keeps the patch's exact constant;
 * 4:3 (1600x1200, or nothing set) is left alone. */
extern int xbox_DisplayShape(unsigned *w, unsigned *h);

/* RECOMP_ASPECT_LOG=1 (debug): who recomputes a camera's aspect and how
 * often. The first 40 calls one line each ([ASPECT] time, return address,
 * viewport, width x height before widening), then every 5 s the calls per
 * return address since the last summary. For a tail jump from sub_00064330
 * the return address is in sub_00064330's caller. */
static void aspect_log(void)
{
    static int on = -1;
    static struct { uint32_t ret; unsigned n; } site[32];
    static unsigned nsite, shown;
    static clock_t last;
    uint32_t ret;
    unsigned i;
    if (on < 0) {
        const char *e = getenv("RECOMP_ASPECT_LOG");
        on = e && *e && *e != '0';
        last = clock();
    }
    if (!on)
        return;
    ret = MEM32(g_esp);
    if (shown < 40) {
        shown++;
        fprintf(stderr, "[ASPECT] t=%ld ms ret=0x%08X vp=%u %.1f x %.1f\n",
                (long)(clock() * 1000 / CLOCKS_PER_SEC), ret, g_edx,
                g_xmm1.f[0], g_xmm0.f[0]);
    }
    for (i = 0; i < nsite && site[i].ret != ret; i++)
        ;
    if (i == nsite && nsite < 32)
        site[nsite++].ret = ret;
    if (i < nsite)
        site[i].n++;
    if (clock() - last >= 5 * CLOCKS_PER_SEC) {
        fprintf(stderr, "[ASPECT] t=%ld ms calls per return address since "
                "t=%ld ms:", (long)(clock() * 1000 / CLOCKS_PER_SEC),
                (long)(last * 1000 / CLOCKS_PER_SEC));
        last = clock();
        for (i = 0; i < nsite; i++)
            if (site[i].n)
                fprintf(stderr, " 0x%08X=%u", site[i].ret, site[i].n);
        fprintf(stderr, "\n");
        for (i = 0; i < nsite; i++)
            site[i].n = 0;
    }
}

/* The shape is asked for on every call (once per game frame, conker-notes
 * 2026-10-14) and the factor worked out again when it changed, so a render
 * resolution changed in the F2 menu (xbox_SetRenderRes) reaches the camera
 * on the next frame. Only the game thread calls this. */
static float display_widen(void)
{
    static unsigned last_w, last_h;
    static float widen = 1.0f;
    unsigned w = 4, h = 3;
    if (!xbox_DisplayShape(&w, &h)) {
        w = 4;
        h = 3;
    }
    if (w != last_w || h != last_h) {
        last_w = w;
        last_h = h;
        if ((uint64_t)w * 3 == (uint64_t)h * 4)
            widen = 1.0f;
        else
            widen = (uint64_t)w * 9 == (uint64_t)h * 16
                  ? 1.33333337f         /* the patch's constant, 0x3FAAAAAB */
                  : (float)((double)w * 3.0 / ((double)h * 4.0));
    }
    return widen;
}

void sub_000645C0_enter(void)
{
    float widen = display_widen();
    aspect_log();
    if (widen != 1.0f)
        g_xmm1.f[0] *= widen;
}

/* ── Conker: the main menu's save frame at wide shapes ─────────────────────
 *
 * 0x003FB030 (ecx = camera) builds a camera's projection from its field of
 * view (+0x104) and aspect (+0x108), unless flag bits 0-1 ask for another
 * kind. Besides viewport 0's camera (sub_000645C0 above), some objects have
 * a camera of their own, set up each time with a fixed aspect of 4/3
 * (0x3FAAAAAB, written straight into +0x108 by 0x00202250, 0x00202AC0,
 * 0x0021F9C0, 0x001F6440, 0x002A6EA0), so the widescreen hook never sees
 * them and what they draw is stretched with the picture.
 *
 * 0x00202AC0's camera (one shared camera, rebuilt every frame) draws the
 * overlay layer: the save slot's picture frame on the main menu, and the
 * pause menu in a level, both through 0x002A5F30. On the main menu the
 * picture inside the frame belongs to the main 3D scene (widened), so a
 * stretched frame left the picture at its left side with a gap on the right
 * and the playing time off centre (user report 2026-10-15). It is widened
 * here like viewport 0, but only in the front end, level [0x007FA1F8] = 0x1D
 * (intro movies and main menu; a level such as the field is 0x0C): the pause
 * menu's backdrop is drawn for 4:3 and leaves a black strip when widened,
 * and stretched it looks as it did on a 16:9 TV (user: keep it as it is).
 * 0x00202AC0 writes 4/3 again before every build, so this never stacks.
 *
 * RECOMP_ASPECT_LOG=1 also lists the fixed 4:3 builds: return address (for
 * 0x00202AC0 its caller's, at esp+0x44) and camera, the first time each pair
 * is seen, then every 5 s the calls per pair and the level number. */
#define CONKER_LEVEL_FRONT_END 0x1Du

static void fixed_camera_log(uint32_t cam)
{
    static int on = -1;
    static struct { uint32_t ret, cam; unsigned n; } site[32];
    static unsigned nsite;
    static clock_t last;
    uint32_t ret = MEM32(g_esp);
    unsigned i;
    if (on < 0) {
        const char *e = getenv("RECOMP_ASPECT_LOG");
        on = e && *e && *e != '0';
        last = clock();
    }
    if (!on)
        return;
    if (ret == 0x00202BE7u)
        ret = MEM32(g_esp + 0x44);
    for (i = 0; i < nsite && (site[i].ret != ret || site[i].cam != cam); i++)
        ;
    if (i == nsite && nsite < 32) {
        site[nsite].ret = ret;
        site[nsite++].cam = cam;
        fprintf(stderr, "[ASPECT] t=%ld ms fixed 4:3 camera 0x%08X built from "
                "ret=0x%08X\n", (long)(clock() * 1000 / CLOCKS_PER_SEC), cam,
                ret);
    }
    if (i < nsite)
        site[i].n++;
    if (clock() - last >= 5 * CLOCKS_PER_SEC) {
        last = clock();
        fprintf(stderr, "[ASPECT] t=%ld ms level 0x%02X, fixed 4:3 builds per "
                "caller/camera:", (long)(clock() * 1000 / CLOCKS_PER_SEC),
                MEM32(0x007FA1F8));
        for (i = 0; i < nsite; i++)
            if (site[i].n)
                fprintf(stderr, " 0x%08X/0x%08X=%u", site[i].ret, site[i].cam,
                        site[i].n);
        fprintf(stderr, "\n");
        for (i = 0; i < nsite; i++)
            site[i].n = 0;
    }
}

void sub_003FB030_enter(void)
{
    uint32_t cam = g_ecx;
    if ((MEM8(cam + 8) & 3) || MEM32(cam + 0x108) != 0x3FAAAAABu)
        return;
    fixed_camera_log(cam);
    if (MEM32(g_esp) == 0x00202BE7u                  /* built by 0x00202AC0 */
        && MEM32(0x007FA1F8) == CONKER_LEVEL_FRONT_END)
        MEMF(cam + 0x108) *= display_widen();
}

/* ── Conker: the game's own audio debug text (RECOMP_AUDIO_LOG=1) ─────────
 *
 * Conker's audio code is full of debug prints ("CMusicTune::StopTune :
 * Stopping cue %d (%s)", "CAudioCue::SmartStop ... audioSoundBankStopCue
 * issued for %s"), each behind a category byte at 0x00846FE8..0x00847007
 * (cleared at boot by 0x00015FC0) and all going through one printf,
 * 0x00018B30 audio_debug_printf. That one prints only when 0x0084725D is set,
 * and 0x0084725D is the game's whole debug mode (pad-combo menus, profiler
 * overlay, conker-notes 2026-10-15), so it stays off: the entry hook prints
 * the text itself and the original then does nothing.
 *
 * The categories turned on, set again on every audio_update (0x00017CF0)
 * because boot clears them: 0x00846FF1 cue lifecycle (play, SmartStop, the
 * AUDIO_NOTIFICATION_* states), 0x00846FF7 music tunes (StopTune,
 * UpdateFadeEffect, tune list report), 0x00846FF9 music manager (play/stop/
 * crossfade sequence), 0x00846FFD music commands, 0x00846FFF music tune list,
 * 0x00846FF3 wave banks (Register/UnRegister, "adWaveBankCallback on tick %d :
 * Primed %s"). All six only gate prints. Added for the barn pit music
 * that never stopped (run_20261015_005522); wave banks for the Berri cutscene
 * that plays the menu tune (2026-10-16).
 *
 * Also a [recomp] line whenever the level-music gate changes. A level load
 * queues its default music (0x00018560: mgr+0x10 = the level's music block,
 * mgr+0x14 = 1, mgr+0x15 = 0); level init (0x002A96E0) sets mgr+0x15 = 1;
 * audio_update then plays it (ad_gamemus_newlevel 0x0001ED00, which first
 * stops the current tune) once every wave bank in the block's list
 * (block+0x18 ids, block+0x1C count, banks at [mgr+4][id]) is primed
 * (bank+0x15) or needs no priming (bank+0x22), check 0x00017FF0. */
static int audio_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_AUDIO_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

/* XACT cue watch: the barn pit music (AD_CUE_windy_wires_inter, an
 * interactive cue) got its stop and kept playing (run_20261015_015553). The
 * XACT cue stop, 0x0054FAB1 (thiscall, ecx = cue instance, [esp+4] = flags),
 * stops an interactive cue at once only when no transition is under way
 * (+0x2C and +0x30 empty); otherwise it just sets "stop after the transition"
 * (+0x38 bit 0x10). Each stop of an interactive cue is logged with the cue's
 * state, and the cue is then watched from audio_update: one [XACT] line each
 * time that state changes, for 120 s. Fields, as far as known: +0x28/+0x2C/
 * +0x30 sound instances (current/next/transition; +0x30 of each a state
 * word, 8 = stopped, +0x38 its flags), +0x34 cue state, +0x38 cue flags,
 * +0x3A/+0x3C variation indices. */
#define XACT_WATCH 4
static struct { uint32_t cue; clock_t until; char last[200]; } xact_watch[XACT_WATCH];

static int xact_cue_interactive(uint32_t cue)
{
    uint32_t sb = MEM32(cue + 0x14), data;
    if (sb < 0x00010000u || sb >= 0x04000000u)
        return 0;
    data = MEM32(sb + 4);
    if (data < 0x00010000u || data >= 0x04000000u)
        return 0;
    return (MEM8(data + 0x38 + (uint32_t)MEM16(cue + 0x18) * 0x14) & 8) != 0;
}

static void xact_cue_state(uint32_t cue, char *out, size_t n)
{
    size_t o;
    int i;
    o = (size_t)snprintf(out, n, "state=%u flags=%04X var=%04X/%04X snd",
                         MEM32(cue + 0x34), MEM16(cue + 0x38),
                         MEM16(cue + 0x3A), MEM16(cue + 0x3C));
    for (i = 0; i < 3 && o < n; i++) {
        uint32_t s = MEM32(cue + 0x28 + 4 * i);
        if (s >= 0x00010000u && s < 0x04000000u)
            o += (size_t)snprintf(out + o, n - o, " [%08X s%u f%08X]", s,
                                  MEM16(s + 0x30), MEM32(s + 0x38));
        else
            o += (size_t)snprintf(out + o, n - o, " [%08X]", s);
    }
}

void sub_0054FAB1_enter(void)
{
    uint32_t cue = g_ecx;
    char st[200];
    int i, free_slot = -1;

    if (!audio_log_on() || cue < 0x00010000u || cue >= 0x04000000u ||
        !xact_cue_interactive(cue))
        return;
    xact_cue_state(cue, st, sizeof st);
    fprintf(stderr, "[XACT] t=%ld ms stop interactive cue %08X flags=%X (ret %08X): %s\n",
            (long)(clock() * 1000 / CLOCKS_PER_SEC), cue, MEM32(g_esp + 4),
            MEM32(g_esp), st);
    fflush(stderr);
    for (i = 0; i < XACT_WATCH; i++) {
        if (xact_watch[i].cue == cue)
            break;
        if (free_slot < 0 && clock() > xact_watch[i].until)
            free_slot = i;
    }
    if (i == XACT_WATCH)
        i = free_slot >= 0 ? free_slot : 0;
    xact_watch[i].cue = cue;
    xact_watch[i].until = clock() + 120 * CLOCKS_PER_SEC;
    strcpy(xact_watch[i].last, st);
}

static void xact_watch_tick(void)
{
    char st[200];
    int i;
    for (i = 0; i < XACT_WATCH; i++) {
        if (!xact_watch[i].cue || clock() > xact_watch[i].until)
            continue;
        xact_cue_state(xact_watch[i].cue, st, sizeof st);
        if (strcmp(st, xact_watch[i].last)) {
            strcpy(xact_watch[i].last, st);
            fprintf(stderr, "[XACT] t=%ld ms cue %08X now: %s\n",
                    (long)(clock() * 1000 / CLOCKS_PER_SEC),
                    xact_watch[i].cue, st);
            fflush(stderr);
        }
    }
}

/* ── Conker: short XACT waves were cut off (running footsteps, dung beetle
 * squash, clang goblin clangs, clock ticks) ────────────────────────────
 *
 * XACT streams these from the Z: cache wave banks like any other wave. A
 * track's reader has two packet slots (reader +0x10 + slot * 0x20: +0 slot
 * status, +8 marker 0xAAAA "read issued, not handed to the stream yet" /
 * 0xFFFF handed / 0xDEAD no data, +0xC the read's IO status). The packet
 * function 0x00552AEB issues the read for one slot; when the wave has no
 * bytes left (track +0x44 = 0) it marks the slot 0xDEAD and asks
 * 0x0054BBC0 "is a read still in flight?" (IO status 0x103) -- if not, it
 * clears track flag 8, and without flag 8 the update (0x00552825) never
 * hands a finished read to the stream.
 *
 * A wave under one packet (a ~8 KB footstep) takes that branch on its
 * second slot right after slot 0's read was issued. On the Xbox (and xemu)
 * that read is still pending then, so flag 8 stays and the data is handed
 * over a frame later. Our NtReadFile finishes every read before it returns,
 * so XACT saw nothing in flight, dropped the stream, and the sound ended
 * one frame after AUDIO_NOTIFICATION_START with no APU voice ever switched
 * on. Fix: right after that clear (the call to 0x0054E95D at 0x00552E17,
 * edi = track), put flag 8 back while a slot still holds a finished read
 * that was never handed over (0xAAAA); the next update hands it over, and
 * the following "no data" pass clears flag 8 for real. Found 2026-10-15
 * (conker-notes). RECOMP_XACT_SHORT_WAVES=old: the old behaviour, for A/B. */
static int xact_short_wave_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_XACT_SHORT_WAVES");
        on = !(e && !strcmp(e, "old"));
    }
    return on;
}

void sub_0054E95D_enter(void)
{
    uint32_t trk = g_edi, rd;
    int slot;

    if (MEM32(g_esp) != 0x00552E1Cu || !xact_short_wave_fix_on())
        return;
    if (trk < 0x00010000u || trk >= 0x84000000u || (MEM8(trk) & 8))
        return;
    rd = MEM32(trk + 0x40);
    if (rd < 0x00010000u || rd >= 0x84000000u)
        return;
    for (slot = 0; slot < 2; slot++) {
        if (MEM32(rd + 0x10 + slot * 0x20 + 8) == 0xAAAAu &&
            MEM32(rd + 0x10 + slot * 0x20 + 0xC) != 0x103u) {
            MEM8(trk) |= 8;
            return;
        }
    }
}

/* XACT sound/track trace (RECOMP_XACT_LOG=1), kept from the short-wave hunt
 * above: one [XACTSND]/[XACTEV]/[XACTTRK] line per sound state change, track
 * event, track voice setup, stream start and finished track. Sound: +0x10
 * sound data (+8 track count), +0x30 state (1 new, 2 playing, 5 waiting for
 * a prime, 7 stopping, 8 stopped; set by 0x0055324F, esi = sound, ebx = new
 * state), +0x34 tracks (0x88 bytes each), +0x44 XACT cue entry. Track: +0
 * flags (1 paused, 4 waiting for data, 8 streaming, 0x20 voice started,
 * 0x80 done), +4 voice wrapper (+0x1C DirectSound buffer, else +0x20
 * stream), +8 sound, +0x40 reader (packet slots), +0x44 bytes left to read,
 * +0x68 wave length, +0x74 wave bank. Track and sound objects live in the
 * 0x74xxxxxx CRT heap, XACT's voices in contiguous memory (0x8xxxxxxx). */
static int xact_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_XACT_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

static int xact_va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u;
}

static void xact_trk_line(const char *what, uint32_t trk)
{
    if (!xact_va_ok(trk))
        return;
    fprintf(stderr, "[XACTTRK] t=%ld ms %s trk %08X flags %04X voice %08X snd %08X "
            "rd %08X left %08X len %08X wb %08X (ret %08X)\n",
            (long)(clock() * 1000 / CLOCKS_PER_SEC), what, trk, MEM16(trk),
            MEM32(trk + 4), MEM32(trk + 8), MEM32(trk + 0x40), MEM32(trk + 0x44),
            MEM32(trk + 0x68), MEM32(trk + 0x74), MEM32(g_esp));
}

/* 0x0055324F: a sound's state change. */
void sub_0055324F_enter(void)
{
    uint32_t snd = g_esi;
    if (!xact_log_on() || !xact_va_ok(snd))
        return;
    fprintf(stderr, "[XACTSND] t=%ld ms snd %08X cue %08X s%u -> s%u tracks %u trk0 %08X "
            "(ret %08X)\n", (long)(clock() * 1000 / CLOCKS_PER_SEC), snd,
            MEM32(snd + 0x44), MEM16(snd + 0x30), g_ebx,
            xact_va_ok(MEM32(snd + 0x10)) ? MEM8(MEM32(snd + 0x10) + 8) : 0,
            MEM32(snd + 0x34), MEM32(g_esp));
    /* Who resumed or stopped it: return addresses further up the guest
     * stack (dwords in the code sections that follow a call instruction). */
    if (g_ebx == 2 || g_ebx == 7) {
        int i, n = 0;
        fprintf(stderr, "[XACTSND]   callers:");
        for (i = 1; i < 96 && n < 12; i++) {
            uint32_t a = MEM32(g_esp + 4u * (uint32_t)i);
            if (a < 0x00011000u || a >= 0x00556000u)
                continue;
            if (MEM8(a - 5) == 0xE8 || MEM8(a - 2) == 0xFF ||
                MEM8(a - 3) == 0xFF || MEM8(a - 6) == 0xFF) {
                fprintf(stderr, " %08X", a);
                n++;
            }
        }
        fprintf(stderr, "\n");
    }
}

/* 0x00554CD4: a track event fires ([esp+8] = context: +0x10 track, +0x14
 * event; event type 0 play, 1 play with variation, both start the voice
 * unless the track is paused). */
void sub_00554CD4_enter(void)
{
    uint32_t ctx = MEM32(g_esp + 8), ev;
    if (!xact_log_on() || !xact_va_ok(ctx) || !xact_va_ok(MEM32(ctx + 0x14)))
        return;
    ev = MEM32(ctx + 0x14);
    fprintf(stderr, "[XACTEV] t=%ld ms event type %u (%08X %08X %08X)\n",
            (long)(clock() * 1000 / CLOCKS_PER_SEC), MEM8(ev), MEM32(ev),
            MEM32(ev + 4), MEM32(ev + 8));
    xact_trk_line("event", MEM32(ctx + 0x10));
}

/* 0x0055220F: a track gets its voice ([esp+8] = track); 0x0055253F: a
 * stream track starts reading (esi = track); 0x005531E1: a track is done
 * (esi = track). */
void sub_0055220F_enter(void)
{
    if (xact_log_on())
        xact_trk_line("setup", MEM32(g_esp + 8));
}

void sub_0055253F_enter(void)
{
    if (xact_log_on())
        xact_trk_line("stream start", g_esi);
}

void sub_005531E1_enter(void)
{
    if (xact_log_on())
        xact_trk_line("done", g_esi);
}

static const char *audio_log_str(uint32_t va, char *buf, size_t n);

/* The queued level music's wave banks not yet primed (gate described above),
 * their names into `names`; -1 if no level music is queued. */
static int level_music_banks_waiting(uint32_t mgr, char *names, size_t size)
{
    uint32_t blk = MEM32(mgr + 0x10), ids, banks, bank, i, n;
    int wait = 0;
    size_t o = 0;
    char nm[64];

    if (size)
        names[0] = 0;
    if (!MEM8(mgr + 0x14) || !xact_va_ok(blk))
        return -1;
    ids = MEM32(blk + 0x18);
    n = MEM32(blk + 0x1C);
    banks = MEM32(mgr + 4);
    for (i = 0; xact_va_ok(ids) && xact_va_ok(banks) && i < n && i < 32; i++) {
        bank = MEM32(banks + MEM32(ids + i * 4) * 4);
        if (!xact_va_ok(bank) || MEM8(bank + 0x15) || MEM8(bank + 0x22))
            continue;
        wait++;
        if (o + 70 < size)
            o += snprintf(names + o, size - o, " %s",
                          audio_log_str(MEM32(bank + 4), nm, sizeof nm));
    }
    return wait;
}

/* The level-music gate (see above), one line when it changes. */
static void level_music_gate_watch(void)
{
    static int last_q = -1, last_s = -1, last_wait = -1;
    static uint32_t last_blk = 0xFFFFFFFFu;
    uint32_t mgr = MEM32(0x00847024), blk;
    int q, st, wait;
    char names[512];

    if (!xact_va_ok(mgr))
        return;
    q = MEM8(mgr + 0x14);
    st = MEM8(mgr + 0x15);
    blk = MEM32(mgr + 0x10);
    wait = level_music_banks_waiting(mgr, names, sizeof names);
    if (wait < 0)
        wait = 0;
    if (q == last_q && st == last_s && wait == last_wait && blk == last_blk)
        return;
    last_q = q; last_s = st; last_wait = wait; last_blk = blk;
    fprintf(stderr, "[AUDIO] t=%ld ms [recomp] level music gate: queued=%d "
            "level started=%d block=%08X, %d bank(s) not primed:%s\n",
            (long)(clock() * 1000 / CLOCKS_PER_SEC), q, st, blk, wait,
            wait ? names : " none");
    fflush(stderr);
}

/* Level music must find its wave banks primed when the level starts
 * (RECOMP_LEVEL_MUSIC_WAIT, on by default; =0 turns it off).
 *
 * On the Xbox the game's loaders wait for each read with
 * `while (pending) frame_background_update();` (0x0005EF70, 0x0005EFE0 and
 * ~20 more callers of 0x002A37B0), which runs audio_update and so XACT's
 * DoWork, so a level's music banks finish priming while the level loads.
 * Our reads finish at once, the loops never run, and the priming only
 * finishes in audio_update after the level has started. The level-music
 * gate then opens a few frames late, and ad_gamemus_newlevel (0x0001ED00,
 * which stops the current tune first) cuts off music the level already
 * started: Berri's dance cutscene after the Great Mighty Poo (frontend level,
 * default tune AD_SEQ_pub = the menu tune) played the menu tune instead of
 * AD_SEQ_berrikid (run_20261016_020323; conker-notes findings 2026-10-16).
 *
 * So when audio_update is about to check the gate (level queued and started,
 * a bank still priming), the hook first runs the game's own sound work,
 * audio_dsound_update (0x00401190: DirectSoundDoWork + XACT DoWork, no
 * arguments, saves its registers) until the banks are primed, at most 1 s,
 * once per queued level. The gate then opens in this same audio_update,
 * before the music commands are processed, as on the Xbox: the level tune
 * starts and a tune the level asked for replaces it. */
extern void sub_00401190(void);

static int level_music_wait_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_LEVEL_MUSIC_WAIT");
        on = !(e && *e == '0');
    }
    return on;
}

__declspec(dllimport) void __stdcall Sleep(unsigned long ms);

static void level_music_wait_for_banks(void)
{
    static uint32_t done_blk;
    uint32_t mgr = MEM32(0x00847024), blk;
    uint32_t r_eax, r_ecx, r_edx, r_ebx, r_esi, r_edi, r_esp;
    char names[512];
    long t0, t1;
    int wait, calls = 0;

    if (!level_music_wait_on() || !xact_va_ok(mgr))
        return;
    blk = MEM32(mgr + 0x10);
    if (!MEM8(mgr + 0x14)) {
        done_blk = 0;
        return;
    }
    if (!MEM8(mgr + 0x15) || blk == done_blk)
        return;
    wait = level_music_banks_waiting(mgr, names, sizeof names);
    if (wait <= 0)
        return;
    done_blk = blk;

    r_eax = g_eax; r_ecx = g_ecx; r_edx = g_edx; r_ebx = g_ebx;
    r_esi = g_esi; r_edi = g_edi; r_esp = g_esp;
    t0 = t1 = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    while (wait > 0 && t1 - t0 < 1000) {
        g_esp -= 4;
        MEM32(g_esp) = 0;               /* return address, popped by the callee */
        sub_00401190();
        g_esp = r_esp;
        calls++;
        wait = level_music_banks_waiting(mgr, NULL, 0);
        if (wait > 0)
            Sleep(2);
        t1 = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    }
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi;

    fprintf(stderr, "[AUDIO] t=%ld ms [recomp] level music wait: %d sound "
            "update(s), %ld ms, %s (banks:%s)\n", t1, calls, t1 - t0,
            wait > 0 ? "gave up, still priming" : "all primed", names);
    fflush(stderr);
}

/*
 * Half-second freezes when a sound stops: let the APU interrupt in while
 * DirectSound waits for a voice to go idle.
 *
 * Stopping a voice sets bit 0x8000 ("stop pending") in its flags (+0x12) and
 * notes the time (+0x78); 0x004CB1E7 then spins until the APU's interrupt
 * (SE2FE_IDLE_VOICE -> dsound_on_voice_idle) clears it, and each pass of
 * 0x004CA56A gives up after 5,000,000 x 100 ns = 0.5 s and forces the voice
 * off. The game thread spins at raised IRQL (DirectSound's public calls
 * hold it), which on the console a device interrupt still preempts. Our APU
 * thread holds its interrupt off while any guest thread is raised (the gate
 * against an ISR running beside the game thread, conker-notes run 62), so
 * every such stop waited the full 0.5 s: the stutters when moving fast in
 * open areas (2026-10-16, run_20261016_084822: 2-3 frames of 520-560 ms per
 * 10 s, game thread in 0x004CA56A + KeQuerySystemTime).
 *
 * Here, at the loop's entry, the game thread runs the APU's connected ISR
 * itself until the bit clears (or 0.5 s pass): one thread, at a point where
 * it edits nothing, which is what preemption on the console amounts to. The
 * ISR reads ISTS and does nothing when no interrupt is pending. Then the
 * generated loop runs as before: it exits at once, or times out as it always
 * did. RECOMP_VOICE_OFF_ISR=0 turns it off (on by default). With
 * RECOMP_AUDIO_LOG=1, "[recomp] voice-off wait" lines (first 20, then every
 * 100th).
 */
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern int xbox_IrqlEnterInterrupt(int level);
extern void xbox_IrqlLeaveInterrupt(int saved);
extern int xbox_IsrGateTryEnter(void);
extern void xbox_IsrGateLeave(void);
extern void (*recomp_lookup(uint32_t xbox_va))(void);

static int voice_off_isr_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VOICE_OFF_ISR");
        on = !(e && *e == '0');
    }
    return on;
}

/* Run the APU's interrupt routine (vector 5) on this thread, as the console
 * would between two of its instructions. 0 if it could not run now. */
static int apu_isr_here(void)
{
    uint32_t kint = xbox_GetConnectedInterrupt(5), routine, context;
    uint32_t r_eax, r_ecx, r_edx, r_ebx, r_esi, r_edi, r_esp;
    void (*fn)(void);
    int irql;

    if (!kint)
        return 0;
    routine = MEM32(kint + 0);
    context = MEM32(kint + 4);
    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn)
        return 0;
    /* Held already if this thread is raised; otherwise keeps the APU thread
     * from running the same ISR beside us. */
    if (!xbox_IsrGateTryEnter())
        return 0;
    r_eax = g_eax; r_ecx = g_ecx; r_edx = g_edx; r_ebx = g_ebx;
    r_esi = g_esi; r_edi = g_edi; r_esp = g_esp;
    g_esp -= 4; MEM32(g_esp) = context;
    g_esp -= 4; MEM32(g_esp) = kint;
    g_esp -= 4; MEM32(g_esp) = 0;      /* return address, popped by the ISR */
    irql = xbox_IrqlEnterInterrupt(16);
    fn();
    xbox_IrqlLeaveInterrupt(irql);
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi; g_esp = r_esp;
    xbox_IsrGateLeave();
    return 1;
}

void sub_004CB1E7_enter(void)
{
    static unsigned waits;
    uint32_t voice = g_ecx;
    long t0, t1;
    int runs = 0;

    if (!voice_off_isr_on() || !(MEM8(voice + 0x12) & 1)
            || !(MEM16(voice + 0x12) & 0x8000))
        return;
    t0 = t1 = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    while ((MEM16(voice + 0x12) & 0x8000) && t1 - t0 < 500) {
        runs += apu_isr_here();
        if (MEM16(voice + 0x12) & 0x8000)
            Sleep(0);
        t1 = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    }
    waits++;
    if (audio_log_on() && (waits <= 20 || waits % 100 == 0)) {
        fprintf(stderr, "[AUDIO] t=%ld ms [recomp] voice-off wait #%u: voice "
                "%08X, %ld ms, %d ISR run(s), %s\n", t1, waits, voice,
                t1 - t0, runs, (MEM16(voice + 0x12) & 0x8000)
                ? "still pending (the game's 0.5 s timeout takes over)"
                : "idle");
        fflush(stderr);
    }
}

/* RECOMP_EMITTER_WATCH=<cue name> (with RECOMP_AUDIO_LOG=2): follow one
 * sound emitter from CAudioSoundEmitterContainer::Add to its removal, and
 * print each frame which dwords of the emitter (first 0x100 bytes) and of
 * its CAudioCue (emitter +4, 0x170 bytes) changed. A dword that keeps
 * changing (positions, countdowns) is muted after 20 lines. Made for the
 * It's War boss beep (`sonar`) that its emitter ends after 37 s of real
 * time even when the game is paused (2026-10-20). */
#define EW_EDW 64
#define EW_CDW 92
static uint32_t s_ew_emitter, s_ew_cue;
static uint32_t s_ew_snap_e[EW_EDW], s_ew_snap_c[EW_CDW];
static unsigned s_ew_n_e[EW_EDW], s_ew_n_c[EW_CDW];

static void emitter_watch_snap(void)
{
    int i;
    for (i = 0; i < EW_EDW; i++)
        s_ew_snap_e[i] = MEM32(s_ew_emitter + 4u * (uint32_t)i);
    for (i = 0; i < EW_CDW; i++)
        s_ew_snap_c[i] = s_ew_cue ? MEM32(s_ew_cue + 4u * (uint32_t)i) : 0;
}

static void emitter_watch_tick(void);

/* From the audio_debug_printf hook: fmt = the format text, ap = guest args. */
static void emitter_watch_msg(const char *fmt, uint32_t ap)
{
    const char *want = getenv("RECOMP_EMITTER_WATCH");
    char name[208];
    if (!want || !*want)
        return;
    if (strstr(fmt, "CAudioSoundEmitterContainer::Add (") && !s_ew_emitter) {
        audio_log_str(MEM32(ap + 8), name, sizeof name);
        if (strcmp(name, want))
            return;
        s_ew_emitter = MEM32(ap + 4);
        s_ew_cue = MEM32(s_ew_emitter + 4);
        memset(s_ew_n_e, 0, sizeof s_ew_n_e);
        memset(s_ew_n_c, 0, sizeof s_ew_n_c);
        emitter_watch_snap();
        fprintf(stderr, "[EWATCH] t=%ld ms frame %u: watching emitter %08X (vtable "
                "%08X) cue %08X (%s)\n", (long)(clock() * 1000 / CLOCKS_PER_SEC),
                MEM32(0x007F9F60), s_ew_emitter, MEM32(s_ew_emitter), s_ew_cue, name);
        fprintf(stderr, "[EWATCH]   emitter:");
        for (int i = 0; i < EW_EDW; i++)
            fprintf(stderr, "%s%08X", (i % 8) ? " " : (i ? "\n[EWATCH]           " : " "),
                    s_ew_snap_e[i]);
        fprintf(stderr, "\n[EWATCH]   cue:    ");
        for (int i = 0; i < EW_CDW; i++)
            fprintf(stderr, "%s%08X", (i % 8) ? " " : (i ? "\n[EWATCH]           " : " "),
                    s_ew_snap_c[i]);
        fprintf(stderr, "\n");
    } else if (s_ew_emitter && strstr(fmt, "RemoveEmitter (") &&
               MEM32(ap + 4) == s_ew_emitter) {
        emitter_watch_tick();
        fprintf(stderr, "[EWATCH] t=%ld ms frame %u: emitter %08X removed\n",
                (long)(clock() * 1000 / CLOCKS_PER_SEC), MEM32(0x007F9F60),
                s_ew_emitter);
        s_ew_emitter = s_ew_cue = 0;
    }
}

static void emitter_watch_tick(void)
{
    char line[2048];
    size_t o = 0;
    int i;
    if (!s_ew_emitter)
        return;
    if (MEM32(s_ew_emitter + 4) != s_ew_cue)
        o += (size_t)snprintf(line + o, sizeof line - o, " CUE %08X -> %08X",
                              s_ew_cue, MEM32(s_ew_emitter + 4));
    for (i = 0; i < EW_EDW && o < sizeof line - 64; i++) {
        uint32_t v = MEM32(s_ew_emitter + 4u * (uint32_t)i);
        if (v != s_ew_snap_e[i] && ++s_ew_n_e[i] <= 20)
            o += (size_t)snprintf(line + o, sizeof line - o, " e+%02X %08X->%08X%s",
                                  4 * i, s_ew_snap_e[i], v,
                                  s_ew_n_e[i] == 20 ? "(muted)" : "");
    }
    for (i = 0; s_ew_cue && i < EW_CDW && o < sizeof line - 64; i++) {
        uint32_t v = MEM32(s_ew_cue + 4u * (uint32_t)i);
        if (v != s_ew_snap_c[i] && ++s_ew_n_c[i] <= 20)
            o += (size_t)snprintf(line + o, sizeof line - o, " c+%03X %08X->%08X%s",
                                  4 * i, s_ew_snap_c[i], v,
                                  s_ew_n_c[i] == 20 ? "(muted)" : "");
    }
    s_ew_cue = MEM32(s_ew_emitter + 4);
    emitter_watch_snap();
    /* A looped-sound emitter (vtable 0x005D2B34, update 0x00030730) counts
     * down [+0x48]+4 by the frame's 60 Hz ticks (0x007FA208); 0x007FA275 is
     * the game's pause (time frozen). Shown every 30 frames and when the
     * pause flag changes. */
    if (MEM32(s_ew_emitter) == 0x005D2B34u && xact_va_ok(MEM32(s_ew_emitter + 0x48))) {
        static uint8_t last_pause = 0xFF;
        uint8_t p = MEM8(0x007FA275);
        if (p != last_pause || MEM32(0x007F9F60) % 30 == 0) {
            last_pause = p;
            o += (size_t)snprintf(line + o, sizeof line - o,
                                  " | loop count %d, ticks %u, paused %u",
                                  (int32_t)MEM32(MEM32(s_ew_emitter + 0x48) + 4),
                                  MEM32(0x007FA208), p);
        }
    }
    if (o)
        fprintf(stderr, "[EWATCH] t=%ld ms frame %u:%s\n",
                (long)(clock() * 1000 / CLOCKS_PER_SEC), MEM32(0x007F9F60), line);
}

/* ── Conker: sound lifetimes keep counting in the pause menu ──────────────
 *
 * Looped and timed sound emitters end after a set number of 60 Hz ticks: a
 * looped one (vtable 0x005D2B34, update 0x00030730) counts down [+0x48]+4, a
 * timed one (vtable 0x005D2BFC, update 0x00030BC0) its own +0x48, both by the
 * frame's ticks [0x007FA208]. The game's pause [0x007FA275] (the pause menu)
 * freezes game time but not that tick count, and the emitter updates run on
 * in the menu, so a long sound ends early: the It's War boss missile beep
 * (37 s) stopped at 40-45 s after a few pauses, and never came back after a
 * 37 s pause (user, 2026-10-20). Probably the original game does the same;
 * the user wants it fixed anyway (an improvement, conker-notes 2026-10-20).
 *
 * Fix (RECOMP_PAUSE_SOUND_FIX=0 turns it off): while paused, give the
 * countdown back this frame's ticks before the update takes them, so it
 * holds still. The timed update always subtracts first. The looped one
 * skips the subtraction only on paths that free the countdown and end the
 * emitter, so the extra ticks can never pile up. */
static int pause_sound_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PAUSE_SOUND_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

/* 0x00030730: looped-sound emitter update (thiscall, ecx = emitter). */
void sub_00030730_enter(void)
{
    uint32_t desc;
    if (!MEM8(0x007FA275) || !pause_sound_fix_on())
        return;
    desc = MEM32(g_ecx + 0x48);
    if (xact_va_ok(desc) && (int32_t)MEM32(desc + 4) >= 1)
        MEM32(desc + 4) += MEM32(0x007FA208);
}

/* 0x00030BC0: timed-sound emitter update (thiscall, ecx = emitter). */
void sub_00030BC0_enter(void)
{
    if (!MEM8(0x007FA275) || !pause_sound_fix_on())
        return;
    if ((int32_t)MEM32(g_ecx + 0x48) >= 1)
        MEM32(g_ecx + 0x48) += MEM32(0x007FA208);
}

void sub_00017CF0_enter(void)
{
    /* RECOMP_HITCH_MS: one call per game frame (src/sample_profiler.c). */
    extern void sample_profiler_frame(void);
    sample_profiler_frame();
    level_music_wait_for_banks();
    if (!audio_log_on())
        return;
    MEM8(0x00846FF1) = 1;
    MEM8(0x00846FF3) = 1;
    MEM8(0x00846FF7) = 1;
    MEM8(0x00846FF9) = 1;
    MEM8(0x00846FFD) = 1;
    MEM8(0x00846FFF) = 1;
    /* RECOMP_AUDIO_LOG=2: also 0x00846FF4, sound emitters (add/remove,
     * "not active and is being removed", who stops one). */
    if (getenv("RECOMP_AUDIO_LOG")[0] == '2')
        MEM8(0x00846FF4) = 1;
    emitter_watch_tick();
    xact_watch_tick();
    level_music_gate_watch();
}

/* A guest string for %s: only from main RAM, at most 200 characters. */
static const char *audio_log_str(uint32_t va, char *buf, size_t n)
{
    size_t i;
    if (va < 0x00010000u || va >= 0x04000000u) {
        snprintf(buf, n, "(ptr %08X)", va);
        return buf;
    }
    for (i = 0; i + 1 < n && i < 200; i++)
        if (!(buf[i] = (char)MEM8(va + i)))
            return buf;
    buf[i] = 0;
    return buf;
}

/* printf over the guest's varargs ([esp+4] format, arguments from [esp+8]):
 * each conversion is handed to the CRT on its own with its flags, width and
 * precision; a double takes 8 bytes of stack, everything else 4. */
static void audio_log_format(uint32_t fmt, uint32_t ap, char *out, size_t n)
{
    size_t o = 0, k;
    char f, spec[16], arg[208];
    int guard = 0;

    while (o + 1 < n && guard++ < 1000) {
        f = (char)MEM8(fmt++);
        if (!f)
            break;
        if (f != '%') {
            out[o++] = f;
            continue;
        }
        k = 0;
        spec[k++] = '%';
        for (;;) {
            f = (char)MEM8(fmt++);
            if (!f || k + 2 >= sizeof spec)
                break;
            if (f == '*') {                 /* width/precision from an argument */
                k += snprintf(spec + k, sizeof spec - k, "%d", (int)MEM32(ap));
                ap += 4;
                continue;
            }
            if (strchr("-+ #0123456789.lhI", f)) {
                if (f != 'l' && f != 'h' && f != 'I')   /* all 4 bytes on x86 */
                    spec[k++] = f;
                continue;
            }
            break;
        }
        if (!f)
            break;
        spec[k++] = f;
        spec[k] = 0;
        switch (f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            o += snprintf(out + o, n - o, spec, MEM32(ap));
            ap += 4;
            break;
        case 'f': case 'e': case 'g': case 'E': case 'G': {
            double d;
            uint64_t bits = (uint64_t)MEM32(ap) | ((uint64_t)MEM32(ap + 4) << 32);
            memcpy(&d, &bits, sizeof d);
            o += snprintf(out + o, n - o, spec, d);
            ap += 8;
            break;
        }
        case 's': case 'S':
            spec[k - 1] = 's';
            o += snprintf(out + o, n - o, spec,
                          audio_log_str(MEM32(ap), arg, sizeof arg));
            ap += 4;
            break;
        case 'p':
            o += snprintf(out + o, n - o, "%08X", MEM32(ap));
            ap += 4;
            break;
        default:                            /* %% and anything unknown */
            out[o++] = f;
            break;
        }
        if (o >= n)
            o = n - 1;
    }
    out[o] = 0;
}

/* One [AUDIO] line per message, with ms since start; a message repeated
 * back to back is counted, not printed again. Lines the game splits over
 * several calls ("Tune name : %s\n" then "Tune ID : %u\n") come out as
 * several lines. */
void sub_00018B30_enter(void)
{
    static char last[1024];
    static unsigned repeats;
    char text[1024];
    size_t len;

    if (!audio_log_on())
        return;
    emitter_watch_msg(audio_log_str(MEM32(g_esp + 4), text, sizeof text), g_esp + 8);
    audio_log_format(MEM32(g_esp + 4), g_esp + 8, text, sizeof text);
    len = strlen(text);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        text[--len] = 0;
    if (!strcmp(text, last)) {
        repeats++;
        return;
    }
    if (repeats)
        fprintf(stderr, "[AUDIO] (previous line %u more times)\n", repeats);
    repeats = 0;
    strcpy(last, text);
    fprintf(stderr, "[AUDIO] t=%ld ms %s\n",
            (long)(clock() * 1000 / CLOCKS_PER_SEC), text);
    fflush(stderr);
}

/* ── Conker: functions whose x86 relied on running on one core ────────────
 *
 * The Xbox has one CPU, so D3D's GPU interrupt (ISR + DPC) can only run
 * between two of the main thread's instructions. Here the ISR and DPC run on
 * the timer thread (kernel_bridge.c, kernel_pgraph_swm_tick) while the main
 * thread keeps going, so a read-then-write that was safe there is a race. */

/* 0x0053AD90 d3d_pushbuffer_patch_return (docs/conker-notes.md), eax = record
 * {return|flags, next, last-dword VA, fixups, data VA}. For each record in the
 * `next` chain: write the jump back, apply the fixups, mark the record done
 * (next = 0xFFFFFFFF) and clear its in-use bit 30 if bit 31 is set.
 *
 * The race: D3DDevice_RunPushBuffer (main thread) links a new record onto the
 * last busy one with `cmpxchg [last+4], new` (0x0053AFFC), expecting 0 = still
 * pending. This function read `next` and then stored 0xFFFFFFFF as two moves
 * (0x0053ADFD, 0x0053AE00). A cmpxchg landing between them saw 0, linked its
 * record, and the store wiped the link: that record's jump back was never
 * written, the GPU ran stale ring words after it, and a later trap walked a
 * garbage chain -- the write fault here at 0x0053ADB7 (run 20261011_231602,
 * Windmill, Birdy's manual cutscene, once). Fix: one atomic exchange, so either
 * the link lands first and is walked now, or the cmpxchg sees 0xFFFFFFFF and
 * RunPushBuffer queues its own NOP, as on one core.
 *
 * Same registers out as the original (eax = last next + 1, ecx = last flags);
 * ebx/esi/edi/ebp are preserved, which is all its three callers rely on. */
void sub_0053AD90(void)
{
    uint32_t rec = g_eax, flags = 0, next;

    do {
        flags = MEM32(rec);
        MEM32(MEM32(rec + 8)) = (flags & 0x0FFFFFFFu) + 1;   /* JUMP back */
        if (flags & 0x20000000u) {
            uint32_t base = MEM32(rec + 0x10), p = MEM32(rec + 0x0C), n, i;
            /* {size, offset, bytes[size]} ... 0xFFFFFFFF */
            while ((n = MEM32(p)) != 0xFFFFFFFFu) {
                uint32_t dst = MEM32(p + 4) + base;
                p += 8;
                for (i = 0; i < n; i++)
                    MEM8(dst + i) = MEM8(p + i);
                p += n;
            }
        }
        next = (uint32_t)_InterlockedExchange(
            (volatile long *)XBOX_PTR(rec + 4), (long)0xFFFFFFFFu);
        if (flags & 0x80000000u) {
            flags &= 0xBFFFFFFFu;
            MEM32(rec) = flags;
        }
        rec = next;
    } while (rec + 1u > 1u);        /* 0 = end of chain, 0xFFFFFFFF = done */

    g_eax = rec + 1u;
    g_ecx = flags;
    g_esp += 4;                     /* ret */
}

/* ── Conker: character attachments (RECOMP_ATTACH_LOG=1) ──────────────────
 *
 * Props and face pieces glued to a character's bone are "attachments": 0xF0
 * byte records in a list at 0x00847200 (next +0xE0, prev +0xE4). Fields seen
 * so far: +0x00 character id (char +0x10B), +0x02 model id, +0x04 bone,
 * +0x0A attachment type (1..0x119, names at 0x006CACA8), +0x0C per-frame
 * callback index (table 0x0058DFA8; nonzero return = remove), +0x10 up to
 * four animation ids (kept only while the character's animation, char +0xA4,
 * is one of them; all zero = no check), +0x26 frames left (0xFFFF = forever).
 * 0x002B37D0 adds every part of a type ([esp+4] = character, [esp+8] = type,
 * [esp+0x10] = frames; animation ids come from the part's definition, table
 * 0x0059BBD0, part +0x0E), 0x00076AD0 removes one, 0x00076E40 removes by type
 * (ebx; eax = 0xFF for all, from 0x002B38F0) and 0x00076EC0 all of a
 * character's (edi); 0x00076F10 is the
 * per-frame update. Type 11 CONKER_SHOCK_EYES_AND_TONGUE = shock eyes (model
 * 0x84, callback 6: gone when its own animation ends) + sick tongue (model
 * 0x12, no callback). Added for Conker's mouth stuck open after the Uga Buga
 * eye-pop (run_20261016_120107). */
static int attach_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_ATTACH_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

static long attach_ms(void)
{
    return (long)(clock() * 1000 / CLOCKS_PER_SEC);
}

/* The live character with this id (characters at [0x0084A5F8], 0x81 of
 * 0x6D0 bytes, in use when +0x64 != 0), or 0. */
static uint32_t attach_char(uint8_t id)
{
    uint32_t c = MEM32(0x0084A5F8u);
    int i;
    if (!id || !xact_va_ok(c))
        return 0;
    for (i = 0; i < 0x81; i++, c += 0x6D0)
        if (MEM32(c + 0x64) && MEM8(c + 0x10B) == id)
            return c;
    return 0;
}

static void attach_line(const char *what, uint32_t a)
{
    uint32_t c = attach_char(MEM8(a));
    fprintf(stderr, "[ATTACH] t=%ld ms %s %08X char %02X type %u model %02X cb %u "
            "anims %u,%u,%u,%u frames %04X | char anim %u (ret %08X)\n",
            attach_ms(), what, a, MEM8(a), MEM16(a + 0x0A), MEM16(a + 2),
            MEM8(a + 0x0C), MEM16(a + 0x10), MEM16(a + 0x12), MEM16(a + 0x14),
            MEM16(a + 0x16), MEM16(a + 0x26), c ? MEM32(c + 0xA4) : 0xFFFFFFFFu,
            MEM32(g_esp));
}

void sub_002B37D0_enter(void)
{
    uint32_t ch = MEM32(g_esp + 4);
    if (attach_log_on())
        fprintf(stderr, "[ATTACH] t=%ld ms add type %u to char %08X (id %02X, anim %u) "
                "frames %u (ret %08X)\n", attach_ms(), MEM32(g_esp + 8), ch,
                xact_va_ok(ch) ? MEM8(ch + 0x10B) : 0,
                xact_va_ok(ch) ? MEM32(ch + 0xA4) : 0, MEM32(g_esp + 0x10),
                MEM32(g_esp));
}

void sub_00076AD0_enter(void)
{
    if (attach_log_on())
        attach_line("remove", MEM32(g_esp + 4));
}

void sub_00076E40_enter(void)
{
    uint32_t ch = MEM32(g_esp + 4);
    if (attach_log_on())
        fprintf(stderr, "[ATTACH] t=%ld ms remove type %u (count %d) from char id %02X "
                "(ret %08X)\n", attach_ms(), g_ebx & 0xFFFF, (int)g_eax,
                xact_va_ok(ch) ? MEM8(ch + 0x10B) : 0, MEM32(g_esp));
}

void sub_00076EC0_enter(void)
{
    if (attach_log_on())
        fprintf(stderr, "[ATTACH] t=%ld ms remove all from char id %02X (ret %08X)\n",
                attach_ms(), xact_va_ok(g_edi) ? MEM8(g_edi + 0x10B) : 0,
                MEM32(g_esp));
}

/* ── Conker: character animations and positions (RECOMP_CHAR_LOG=1) ──────
 *
 * Diagnostic for Berri sinking into the floor in the final arena cutscene
 * (2026-10-21). char_set_animation 0x002F1790 (eax = character) starts an
 * animation by name ([esp+4], formatted into aid\anim\%s\default.rba) and
 * index ([esp+8], stored in char +0xA4). A character's position is the vec3 at
 * +0x78 (char_fur_shell_count measures the camera distance from it); +0x68 is
 * the model kind. [ANIMREQ]: a request for another animation than the one
 * playing (the same one is asked for every frame). [CHAR]: per frame, a character's
 * line when it appears, its animation changes, its height (y) moves by more
 * than 1, or it moves more than 20 in one frame. */
static int char_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CHAR_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

static uint32_t s_char_frame;

static float char_f(uint32_t va)
{
    uint32_t v = MEM32(va);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void char_guest_str(uint32_t s, char *out, int size)
{
    int i;
    out[0] = 0;
    for (i = 0; xact_va_ok(s) && i < size - 1; i++)
        if (!(out[i] = (char)MEM8(s + i)))
            break;
    out[size - 1] = 0;
}

/* Berri head-down in the floor (final arena, after she is shot; 2026-10-21).
 * A character's world matrix is rebuilt before each draw from three angles in
 * degrees (0x000808F0, [esp+4] = character index: x +0x1C0, y +0x114, z
 * +0x1CC). The cutscene lays her body down twice by playing "stance" (her
 * standing idle) with x = -90, the standing pose tipped flat. The first time
 * it then switches her to "deadpose2", already lying, and leaves x at -90 for
 * the whole "Right, squirrel, I think you're coming vit me" stretch (387-455 s
 * into run_20261021_014518, CDUMP): the lying pose tipped again, head down
 * through the floor, feet up. It clears x when it moves her (455 s); the
 * second time it clears x in the same frame as the switch. The real game does
 * the same (user, from gameplay videos); fixed as an improvement. While her
 * animation is a "deadpose", x = -90 is drawn as 0. The animation's name is
 * entry +0xA4 of her table [[char+0x74]+0x50], 12 bytes each, +0 name.
 * RECOMP_BERRI_FIX=0 turns it off. */
static int berri_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_BERRI_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

static long s_berri_ms = -10000;

void sub_000808F0_enter(void)
{
    char cur[96];
    uint32_t base = MEM32(0x0084A5F8u), idx = MEM32(g_esp + 4), c, tbl, anim;
    if (!berri_fix_on() || !xact_va_ok(base) || idx > 0x80)
        return;
    c = base + idx * 0x6D0;
    if (MEM32(c + 0x68) != 0x97u || MEM32(c + 0x1C0) != 0xC2B40000u)     /* -90.0f */
        return;
    anim = MEM32(c + 0xA4);
    if (anim >= 0x100 || !xact_va_ok(MEM32(c + 0x74))
        || !xact_va_ok(tbl = MEM32(MEM32(c + 0x74) + 0x50)))
        return;
    char_guest_str(MEM32(tbl + anim * 12), cur, sizeof cur);
    if (!strstr(cur, "berimatrix\\deadpose"))
        return;
    MEM32(c + 0x1C0) = 0;               /* the cutscene sets it again every frame */
    if (attach_ms() - s_berri_ms > 1000)
        fprintf(stderr, "[BERRI] t=%ld ms \"%s\" tipped -90: drawn flat (each frame until "
                "the line repeats)\n", attach_ms(), cur);
    s_berri_ms = attach_ms();
}

void sub_002F1790_enter(void)
{
    char name[96];
    uint32_t c = g_eax;
    if (!char_log_on() || !xact_va_ok(c) || MEM32(c + 0xA4) == MEM32(g_esp + 8))
        return;                         /* most calls re-ask every frame */
    char_guest_str(MEM32(g_esp + 4), name, sizeof name);
    fprintf(stderr, "[ANIMREQ] t=%ld ms f%u char %08X id %02X kind %X \"%s\" index %u -> %u "
            "a3 %08X a5 %08X a6 %.2f a8 %u pos %.1f %.1f %.1f (ret %08X)\n", attach_ms(),
            s_char_frame, c, MEM8(c + 0x10B), MEM32(c + 0x68), name, MEM32(c + 0xA4),
            MEM32(g_esp + 8), MEM32(g_esp + 0xC), MEM32(g_esp + 0x14), char_f(g_esp + 0x18),
            MEM32(g_esp + 0x20), char_f(c + 0x78), char_f(c + 0x7C), char_f(c + 0x80),
            MEM32(g_esp));
}

/* RECOMP_CHAR_DUMP=<id hex>: every 60 frames, [CDUMP] lines with that
 * character's whole record (0x6D0 bytes) and its first model object
 * ([char+0x4B0], 0x200 bytes), as dwords, to compare a frame that looks
 * right with one that doesn't (screenshots name the log line). */
static void dump_dwords(const char *what, uint32_t va, uint32_t len)
{
    uint32_t o;
    fprintf(stderr, "[CDUMP] f%u %s %08X:", s_char_frame, what, va);
    for (o = 0; o < len; o += 4) {
        if (!(o & 0x3F))
            fprintf(stderr, "\n[CDUMP]   +%03X", o);
        fprintf(stderr, " %08X", MEM32(va + o));
    }
    fprintf(stderr, "\n");
}

static void char_dump(void)
{
    static int id = -2;
    uint32_t base = MEM32(0x0084A5F8u), c;
    int i;
    if (id == -2) {
        const char *e = getenv("RECOMP_CHAR_DUMP");
        id = e && *e ? (int)strtoul(e, NULL, 16) : -1;
    }
    if (id < 0 || s_char_frame % 60 || !xact_va_ok(base))
        return;
    for (i = 0, c = base; i < 0x81; i++, c += 0x6D0) {
        uint32_t obj;
        if (!MEM32(c + 0x64) || MEM8(c + 0x10B) != (uint32_t)id)
            continue;
        fprintf(stderr, "[CDUMP] t=%ld ms f%u #%02X anim %u pos %.1f %.1f %.1f\n", attach_ms(),
                s_char_frame, i, MEM32(c + 0xA4), char_f(c + 0x78), char_f(c + 0x7C),
                char_f(c + 0x80));
        dump_dwords("char", c, 0x6D0);
        if (xact_va_ok(obj = MEM32(c + 0x4B0)))
            dump_dwords("obj", obj, 0x200);
    }
}

static void char_log_frame(void)
{
    static struct { uint32_t live, anim; float x, y, z; } last[0x81];
    uint32_t base = MEM32(0x0084A5F8u), c;
    int i;
    s_char_frame++;
    if (!xact_va_ok(base))
        return;
    for (i = 0, c = base; i < 0x81; i++, c += 0x6D0) {
        uint32_t live = MEM32(c + 0x64) != 0, anim = MEM32(c + 0xA4);
        float x = char_f(c + 0x78), y = char_f(c + 0x7C), z = char_f(c + 0x80);
        float dx = x - last[i].x, dy = y - last[i].y, dz = z - last[i].z;
        const char *why = NULL;
        if (!live) {
            last[i].live = 0;
            continue;
        }
        if (!last[i].live)
            why = "new";
        else if (anim != last[i].anim)
            why = "anim";
        else if (dy > 1.0f || dy < -1.0f)
            why = "height";
        else if (dx * dx + dy * dy + dz * dz > 400.0f)
            why = "jump";
        if (why)
            fprintf(stderr, "[CHAR] t=%ld ms f%u #%02X id %02X kind %X %s anim %u -> %u pos "
                    "%.1f %.1f %.1f (was %.1f %.1f %.1f)\n", attach_ms(), s_char_frame, i,
                    MEM8(c + 0x10B), MEM32(c + 0x68), why, last[i].anim, anim, x, y, z,
                    last[i].x, last[i].y, last[i].z);
        last[i].live = 1;
        last[i].anim = anim;
        last[i].x = x; last[i].y = y; last[i].z = z;
    }
    char_dump();
}

/* Per frame: one line per face-piece attachment (types 5-7, 10, 11) when it
 * first shows up and whenever its character's animation or frames left change. */
void sub_00076F10_enter(void)
{
    static struct { uint32_t a, anim, frames; } seen[8];
    uint32_t a;
    int guard = 0;
    if (char_log_on())
        char_log_frame();
    if (!attach_log_on())
        return;
    for (a = MEM32(0x00847200u); xact_va_ok(a) && guard < 512; a = MEM32(a + 0xE0), guard++) {
        uint16_t t = MEM16(a + 0x0A);
        uint32_t c, anim, frames;
        int i, slot = -1;
        if (!((t >= 5 && t <= 7) || t == 10 || t == 11))
            continue;
        c = attach_char(MEM8(a));
        anim = c ? MEM32(c + 0xA4) : 0xFFFFFFFFu;
        frames = MEM16(a + 0x26);
        for (i = 0; i < 8; i++)
            if (seen[i].a == a) { slot = i; break; }
        if (slot >= 0 && seen[slot].anim == anim && seen[slot].frames == frames)
            continue;
        if (slot < 0) {
            static int next;
            slot = next++ & 7;
            seen[slot].a = a;
        }
        seen[slot].anim = anim;
        seen[slot].frames = frames;
        attach_line("tick", a);
    }
}

/* 0x002CEFC0 sets a character's facial expression (edi = character,
 * [esp+4] = expression, [esp+8] = priority, [esp+0xC] = frames, 0xFFFF =
 * until changed, [esp+0x10] = mouth blend frames, 0 = the entry's +3).
 * Character fields: +0x140 current, +0x141 default, +0x142
 * priority, +0x144 frames left; the face piece of an expression is byte +4 of
 * its 10-byte entry ([0x00743010 + slot*4] + expression*10, slot from the
 * list at 0x00742F98 matching char +0x70), attachment type =
 * 0x0059C497[piece] (1 shock eyes = type 5, 2 grin = 6, 3 mad = 7, 4 sick
 * tongue = 10, 5 shock eyes + tongue = 11). The old piece is removed and the
 * new one added only while the character holds nothing, 0x111 or 0x114
 * ("held" = [[char+0x50C] + (char+0x3A9 - 1)*4], or [[char+0x50C]] when
 * +0x3A9 is 0). Logged for character id 01 (Conker) only. */
void sub_002CEFC0_enter(void)
{
    uint32_t c = g_edi, held = 0xFFFFFFFFu, list;
    uint8_t slot;
    if (!attach_log_on() || !xact_va_ok(c) || MEM8(c + 0x10B) != 1)
        return;
    list = MEM32(c + 0x50C);
    slot = MEM8(c + 0x3A9);
    if (xact_va_ok(list))
        held = slot ? MEM32(list + (slot - 1u) * 4) : MEM32(list);
    fprintf(stderr, "[ATTACH] t=%ld ms expression %u -> %u (prio %u -> %u, frames %04X -> "
            "%04X, mouth blend %u, default %u) held %X anim %u (ret %08X)\n", attach_ms(),
            MEM8(c + 0x140), MEM8(g_esp + 4), MEM8(c + 0x142), MEM8(g_esp + 8),
            MEM16(c + 0x144), MEM16(g_esp + 0xC), MEM8(g_esp + 0x10), MEM8(c + 0x141),
            held, MEM32(c + 0xA4), MEM32(g_esp));
}

/* 0x000FEA00 blends a character's mouth (morph targets) toward a shape, once
 * per frame from 0x0007EB22: [esp+4] = target shape (char +0x248, set from
 * byte +2 of the expression entry), [esp+8] = blend frames (char +0x249),
 * [esp+0xC] = character index. Blend state at [char+0x46C]: +0 float t
 * (0..1), +4 float step (1/frames), +8 previous shape, +9 current shape, +10
 * frames left to keep writing weights after t reaches 1 (3), +11 skip once.
 * Each frame it writes weight t to the current shape and 1 - t to the
 * previous one, by name (0x000FEE20(model, instance weights, name, weight):
 * every morph channel whose name contains it). Shape names for model kinds
 * 0, 0x111, 0x114, 0x96 (table 0x0059581C: kind, count, names): 0 none,
 * 1 EEE, 2 AAA, 3 OOO, 4 FFF, 5 MMM, 6 HAPPY, 7 SAD, 8 SHOCK, 9 MAD, 10 GRIN,
 * 11 BREATH. 0x000FED10 snaps the mouth to one shape (all weights 0/1; a
 * cutscene start does this).
 *
 * Mouth stuck open (Uga Buga, 2026-10-16, run_20261016_135510): when the
 * target changes before a fade is done, the game makes the current shape the
 * previous one and never writes the old previous shape again, so it keeps
 * its last weight (1 - t) until it happens to be used again. Lip sync
 * (0x0001B2C0: shapes from the voice line's timeline, in seconds; a 10-frame
 * fade back to shape 0 between them, 0x0030F4D0) changes shape every 4-6
 * frames, so a quick line can leave AAA 0.6 + OOO 0.4 + EEE 0.8 on for good:
 * an open mouth until the next snap. The eye-pop left MMM 0.6-0.8 in two of
 * four runs, depending on whether two expression changes fell in one frame.
 * Fix (RECOMP_MOUTH_FIX=0 turns it off): at that moment, write 0 to the
 * shape being dropped, on every model the fade writes to, before the game
 * moves on; that is the weight it would have reached. All characters with
 * this mouth system get it.
 *
 * RECOMP_ATTACH_LOG=1: one [MOUTH] line for character id 01 whenever the
 * state changes, t reaches 1, or 5 s pass, and one per dropped shape. */
extern void sub_000FEE20(void);

static int mouth_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MOUTH_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

/* The game's own weight write for one name on each of the character's
 * models, as 0x000FEA00's loop does it. */
static void mouth_set_weight(uint32_t c, uint32_t name, float w)
{
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_ebx = g_ebx;
    uint32_t r_esi = g_esi, r_edi = g_edi, r_esp = g_esp;
    uint32_t n, count = MEM8(c + 0x511), wbits;
    memcpy(&wbits, &w, 4);
    if (!count)
        count = 1;
    for (n = 0; n < count; n++) {
        uint32_t obj = MEM32(c + 0x4B0 + 4 * n), m;
        if (!xact_va_ok(obj) || !xact_va_ok(m = MEM32(obj + 0x24)))
            continue;
        g_esp = r_esp - 0x10;
        MEM32(g_esp) = MEM32(m + 8);
        MEM32(g_esp + 4) = MEM32(c + 0x4C4 + 4 * n);
        MEM32(g_esp + 8) = name;
        MEM32(g_esp + 0xC) = wbits;
        g_esp -= 4;
        MEM32(g_esp) = 0;               /* return address, popped by the callee */
        sub_000FEE20();
    }
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi; g_esp = r_esp;
}

void sub_000FEA00_enter(void)
{
    static uint32_t last_key = 0xFFFFFFFFu;
    static long last_ms;
    uint32_t idx = MEM32(g_esp + 0xC), base = MEM32(0x0084A5F8u), c, b, key;
    uint32_t list, kind, names = 0, shapes = 0;
    uint8_t target = MEM8(g_esp + 4), prev, cur;
    int log, writes, k;
    float t, step;
    long now;
    if (!xact_va_ok(base) || idx > 0x80)
        return;
    c = base + idx * 0x6D0;
    b = MEM32(c + 0x46C);
    list = MEM32(c + 0x50C);
    if (!xact_va_ok(b) || !xact_va_ok(list))
        return;
    log = attach_log_on() && MEM8(c + 0x10B) == 1;
    key = MEM32(b);
    memcpy(&t, &key, 4);
    key = MEM32(b + 4);
    memcpy(&step, &key, 4);
    prev = MEM8(b + 8);
    cur = MEM8(b + 9);

    /* Same tests as 0x000FEA00: a known model kind, and the branch that
     * writes weights. */
    kind = MEM8(c + 0x3A9) ? MEM32(list + (MEM8(c + 0x3A9) - 1u) * 4) : MEM32(list);
    for (k = 0; k < 5; k++)
        if (MEM16(0x0059581Cu + k * 8) == kind) {
            names = MEM32(0x00595820u + k * 8);
            shapes = MEM8(0x0059581Eu + k * 8);
            break;
        }
    if (MEM8(c + 0x3A8) == 0 || idx == 0)
        writes = MEM32(c + 0x4B0) != 0;
    else
        writes = MEM32(c + 0x488 + 4 * (uint32_t)(int32_t)(int8_t)MEM8(0x007FA230u)) != 0 &&
                 MEM32(c + 0x4B0) != 0;
    if (xact_va_ok(names) && writes && target != cur && cur != 0xFF && prev != cur &&
        prev != target && prev < shapes && t < 1.0f) {
        uint32_t name = MEM32(names + prev * 4u);
        if (xact_va_ok(name)) {
            if (mouth_fix_on())
                mouth_set_weight(c, name, 0.0f);
            if (log)
                fprintf(stderr, "[MOUTH] t=%ld ms target %u while fading %u -> %u at %.2f: "
                        "%s left at %.2f%s\n", attach_ms(), target, prev, cur, t,
                        (const char *)XBOX_PTR(name), 1.0f - t,
                        mouth_fix_on() ? ", set to 0" : "");
        }
    }

    if (!log)
        return;
    key = target | MEM8(g_esp + 8) << 8 | (uint32_t)prev << 16 | (uint32_t)cur << 24;
    key ^= (uint32_t)MEM8(b + 10) * 0x9E3779B1u ^ (t == 1.0f ? 0x55u : 0u) ^
           (MEM32(c + 0x4B0) ? 0xAA00u : 0u);
    now = attach_ms();
    if (key == last_key && now - last_ms < 5000)
        return;
    last_key = key;
    last_ms = now;
    fprintf(stderr, "[MOUTH] t=%ld ms target %u frames %u | prev %u cur %u blend %.4f "
            "step %.4f keep %u skip %u | expr %u (default %u) +4B0 %08X +3A8 %u anim %u\n",
            now, target, MEM8(g_esp + 8), prev, cur, t, step,
            MEM8(b + 10), MEM8(b + 11), MEM8(c + 0x140), MEM8(c + 0x141),
            MEM32(c + 0x4B0), MEM8(c + 0x3A8), MEM32(c + 0xA4));
}

/* ── Models drawn after they were freed (RECOMP_MODEL_LOG=1) ──
 * Multiplayer character select, 2026-10-21 (conker-notes): scrolling fast drew
 * a character whose model memory had been freed and reloaded with other data
 * (rainbow textures, missing parts, crashes in the model loader). Cause: our
 * reads finished before NtReadFile returned; fixed by RECOMP_IO_LATENCY_MS.
 * These lines show such a case again:
 *   [MFREE]    XMemFree of a block in the frontend model area, with callers
 *   [MRUN]     RunPushBuffer of a buffer that is not all commands when the game
 *              submits it, with the character being drawn (table index, type)
 *   [MSWAP]    frontend slot swap (0x001FE4B0): the character it replaces
 *   [MDESTROY] character destroyed (0x002F50C0) */
static int model_log_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_MODEL_LOG") != NULL;
    return on;
}

static uint32_t s_model_draw_idx = 0xFFFFFFFFu;   /* character being drawn */

static void model_log_callers(void)
{
    uint32_t i, shown = 0;
    for (i = 0; i < 160 && shown < 12; i++) {
        uint32_t v = MEM32(g_esp + i * 4);
        if (v >= 0x00011000u && v < 0x00561000u) {
            fprintf(stderr, " %08X", v);
            shown++;
        }
    }
    fprintf(stderr, "\n");
}

/* XMemFree(ptr) */
void sub_002AF670_enter(void)
{
    static unsigned n;
    uint32_t a = MEM32(g_esp + 4);
    if (!model_log_on() || (a & 0x0FFFFFFFu) < 0x01E00000u
        || (a & 0x0FFFFFFFu) >= 0x02200000u || n++ > 3000)
        return;
    fprintf(stderr, "[MFREE] t=%ld %08X from", attach_ms(), a);
    model_log_callers();
}

/* D3DDevice_RunPushBuffer(pb, fixups): pb +4 data, +12 size */
void sub_0053AE30_enter(void)
{
    static unsigned n;
    uint32_t pb = MEM32(g_esp + 4), data, size, va, end, bad = 0, w;
    if (!model_log_on() || !xact_va_ok(pb))
        return;
    data = MEM32(pb + 4);
    size = MEM32(pb + 12);
    if (!xact_va_ok(data) || size < 8 || size > 0x100000u)
        return;
    end = data + size - 4;                      /* the jump back */
    for (va = data; va < end; ) {
        w = MEM32(va);
        if ((w & 3u) == 1u) {                   /* a jump inside the buffer */
            uint32_t t = (w & 0x0FFFFFFCu) | (data & 0xF0000000u);
            if (t < data || t > end) { bad = va; break; }
            va = t;
            continue;
        }
        if ((w & 0x00030003u) != 0u) { bad = va; break; }
        va += 4 + ((w >> 18) & 0x7FFu) * 4u;
    }
    if (!bad && va != end)
        bad = va;
    if (!bad || n++ > 400)
        return;
    fprintf(stderr, "[MRUN] t=%ld draw idx %d type %08X pb %08X data %08X size %08X"
            " bad at %08X (%08X) from", attach_ms(), (int)s_model_draw_idx,
            s_model_draw_idx < 0x80
                ? MEM32(MEM32(0x0084A5F8u) + s_model_draw_idx * 0x6D0u + 0x68) : 0,
            pb, data, size, bad, MEM32(bad));
    model_log_callers();
}

/* character draw (index, pass) */
void sub_0007F4D0_enter(void)
{
    s_model_draw_idx = MEM32(g_esp + 4);
}

/* character destroy (char, mode, flag) */
void sub_002F50C0_enter(void)
{
    uint32_t c = MEM32(g_esp + 4), base = MEM32(0x0084A5F8u);
    if (!model_log_on())
        return;
    fprintf(stderr, "[MDESTROY] t=%ld char %08X idx %d type %08X mode %u\n", attach_ms(),
            c, (int)((c - base) / 0x6D0u), MEM32(c + 0x68), MEM32(g_esp + 8));
}

/* frontend slot swap (slot, choice) */
void sub_001FE4B0_enter(void)
{
    uint32_t slot = MEM32(g_esp + 4), old, base = MEM32(0x0084A5F8u);
    if (!model_log_on() || slot > 16)
        return;
    old = MEM32(0x007761E0u + slot * 4);
    fprintf(stderr, "[MSWAP] t=%ld slot %u choice %u old char %08X idx %d type %08X\n",
            attach_ms(), slot, MEM32(g_esp + 8), old,
            old ? (int)((old - base) / 0x6D0u) : -1, old ? MEM32(old + 0x68) : 0);
}


/* ── Conker: glow strength (F2 Picture > Glow) ───────────────────────────
 *
 * The game's own glow is one function per frame, 0x001D95D0 (in game after
 * the scene, before the HUD; also the menus), with two parts that add light
 * onto the frame (conker-notes 2026-10-21):
 *   haze  0x001D9790: the frame shrunk to 128x128 and drawn back over it,
 *         colour = texture, alpha = vertex alpha (the level's strength x
 *         255, about 0.2), blend SRC_ALPHA/ONE; only where a level sets it.
 *   glare 0x001DA7D0: the stencil-tagged bright pixels (depth/stencil read
 *         back), four diagonal streak passes, then 0x001DA3F0 adds them:
 *         colour = texture x TFACTOR (0xFFFFFFFF), blend ONE/ONE.
 * Both adds are linear in those values, so the strength scales them:
 * 100 % (default) changes nothing, 0 % adds nothing. RECOMP_GLOW (0-100)
 * at start, conker_SetGlow from the F2 menu. */
static volatile float g_glow = -1.0f;   /* 0..1; < 0 = not read yet */

static float glow_scale(void)
{
    float v = g_glow;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_GLOW");
        v = e && *e ? (float)atof(e) / 100.0f : 1.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_glow = v;
    }
    return v;
}

/* For the F2 menu (overlay.cpp): strength in percent, 0-100. */
void conker_SetGlow(float pct)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    g_glow = pct / 100.0f;
}

/* 0x0053A600: D3DDevice_DrawVerticesUP (stdcall: type, count, data,
 * stride). From the haze (returns to 0x001D9C73): 4 vertices of 0x18
 * bytes, the D3DCOLOR at +0x0C, only its alpha set. */
void sub_0053A600_enter(void)
{
    float k;
    uint32_t data, i, a;
    if (MEM32(g_esp) != 0x001D9C73u)
        return;
    k = glow_scale();
    if (k >= 1.0f)
        return;
    data = MEM32(g_esp + 12);
    for (i = 0; i < 4; i++) {
        a = MEM32(data + i * 0x18u + 0x0C) >> 24;
        a = (uint32_t)((float)a * k + 0.5f);
        MEM32(data + i * 0x18u + 0x0C) =
            (MEM32(data + i * 0x18u + 0x0C) & 0x00FFFFFFu) | (a << 24);
    }
}

/* 0x00537360: D3DDevice_SetRenderState_TextureFactor (stdcall: value).
 * From the glare add (returns to 0x001DA610): white scaled to grey. */
void sub_00537360_enter(void)
{
    float k;
    uint32_t g;
    if (MEM32(g_esp) != 0x001DA610u)
        return;
    k = glow_scale();
    if (k >= 1.0f)
        return;
    g = (uint32_t)(255.0f * k + 0.5f);
    MEM32(g_esp + 4) = 0xFF000000u | (g << 16) | (g << 8) | g;
}

/* ── Conker: grass, flowers and characters at full detail (F2 Display) ───
 *
 * Grass patches (and the flowers in them) are CHAREFFECTS_TYPE_GRASS (0x91,
 * also UNDERWATER_GRASS 0x59): one object per patch, a vertex buffer of up
 * to 8000 blades (+0x60 the count, +0x68 the buffer; built by 0x001A89B0).
 * Each patch is drawn by 0x001A9540 (cdecl: patch, short player), which
 * measures the patch centre (+0x2C x, +0x30 y, +0x34 z) against that
 * player's camera ([[0x008470DC] + player * 0x770 + 0x3E0] + 0x78):
 *   up to 800 ([0x00648EBC])  every blade, alpha +0x5C = 0xFF, the swaying
 *                             vertex program [0x006B89A4];
 *   800 to 2500               fewer blades (+0x64 = count x the fade), alpha
 *                             fading, a still vertex program [0x006B89DC]:
 *                             the grass stops waving and the flowers thin
 *                             out as the camera backs away;
 *   past 2500 ([0x00649234])  not drawn.
 * Both limits are float constants shared with many other functions, so they
 * stay; instead, with grass_full_detail on, the patch's centre is put on the
 * camera for the distance test (entry hook below) and back before anything
 * else reads it: the function's next read of it is the position pushed for
 * the patch matrix, 0x004288F5 (D3DX translation, returns to 0x001A96A4),
 * whose entry hook writes the real centre back to the patch and to the
 * pushed arguments. Every patch in range of the level's own culling is then
 * drawn as if near. RECOMP_GRASS_FULL=0 (or grass_full_detail = 0) = the
 * original; conker_SetGrassFull from the F2 menu. */
static volatile int g_grass_full = -1;
static uint32_t s_grass_patch, s_grass_pos[3];

static int grass_full_on(void)
{
    if (g_grass_full < 0) {
        const char *e = getenv("RECOMP_GRASS_FULL");
        g_grass_full = !(e && *e == '0');
    }
    return g_grass_full;
}

/* For the F2 menu (overlay.cpp): applies from the next frame. */
void conker_SetGrassFull(int on)
{
    g_grass_full = on != 0;
}

static void grass_put_back(void)
{
    if (s_grass_patch) {
        MEM32(s_grass_patch + 0x2C) = s_grass_pos[0];
        MEM32(s_grass_patch + 0x30) = s_grass_pos[1];
        MEM32(s_grass_patch + 0x34) = s_grass_pos[2];
        s_grass_patch = 0;
    }
}

void sub_001A9540_enter(void)
{
    uint32_t patch = MEM32(g_esp + 4), players = MEM32(0x008470DCu), cam;
    int16_t player = (int16_t)MEM16(g_esp + 8);
    grass_put_back();                   /* never left moved by a missed call */
    if (!grass_full_on() || !xact_va_ok(patch) || !xact_va_ok(players)
        || player < 0 || player > 3)
        return;
    cam = MEM32(players + (uint32_t)player * 0x770u + 0x3E0u);
    if (!xact_va_ok(cam))
        return;
    s_grass_patch = patch;
    s_grass_pos[0] = MEM32(patch + 0x2C);
    s_grass_pos[1] = MEM32(patch + 0x30);
    s_grass_pos[2] = MEM32(patch + 0x34);
    MEM32(patch + 0x2C) = MEM32(cam + 0x78);
    MEM32(patch + 0x30) = MEM32(cam + 0x7C);
    MEM32(patch + 0x34) = MEM32(cam + 0x80);
}

/* Characters too, under the same switch: 0x00080DA0 (cdecl: character,
 * viewport mask; ebx = viewport; from 0x00081190 in the world update) hides
 * a character whose distance from that viewport's camera (less its radius
 * +0x9C) is past its ushort +0xCA: the off-screen fade kind 8 fades it
 * out, kind 9 back in (0x002F5D20), so characters pop out and in as the
 * camera backs away. +0xCA is 1000 for every character in the front end's
 * pub. A limit of 0 means none, which is what the function itself uses
 * while the cutscene camera runs ([0x0076F0E0] == 1): then a character in
 * the frustum is shown at full opacity at once. With grass_full_detail on,
 * +0xCA reads 0 for the call and is put back after it. The frustum and the
 * GPU occlusion box still apply. */
extern void sub_00080DA0_gen(void);
void sub_00080DA0(void)
{
    uint32_t c = MEM32(g_esp + 4);
    uint16_t lim;

    if (!grass_full_on() || !xact_va_ok(c)) {
        sub_00080DA0_gen();
        return;
    }
    lim = MEM16(c + 0xCAu);
    MEM16(c + 0xCAu) = 0;
    sub_00080DA0_gen();
    MEM16(c + 0xCAu) = lim;
}

/* Fur too (user 2026-10-29: Mrs. Bee bald from afar, her fur growing back
 * as Conker walks up). char_render_pass's fur pass asks 0x00081FC0 (cdecl:
 * viewport, &shell scale; edi = character) for its shell count: the
 * distance from the character (+0x78) to that player's camera (player
 * record +0x330), measured by 0x003E1EE0 (cdecl: two points; result in
 * xmm0, returns to 0x0008210C), against R in xmm3 (1000; 3000 for model
 * 0x6D). Up to 0.4 R every shell (at most 8; +0xCF caps it), the shells
 * 1 to 2 times as long from 100 to 0.4 R; past that fewer shells, down to
 * one (no fur) at about R. With grass_full_detail on the distance reads at
 * most 0.4 R there: full fur at any distance, as long as at 0.4 R. */
extern void sub_003E1EE0_gen(void);
void sub_003E1EE0(void)
{
    float full;
    int fur = MEM32(g_esp) == 0x0008210Cu;

    sub_003E1EE0_gen();
    if (!fur || !grass_full_on())
        return;
    full = g_xmm3.f[0] > 0.0f ? g_xmm3.f[0] * 0.4f : 400.0f;
    if (g_xmm0.f[0] > full)
        g_xmm0.f[0] = full;
}

/* 0x004288F5: D3DX matrix translation (out in EAX; x, y, z on the stack). */
void sub_004288F5_enter(void)
{
    if (MEM32(g_esp) != 0x001A96A4u || !s_grass_patch)
        return;
    MEM32(g_esp + 4) = s_grass_pos[0];
    MEM32(g_esp + 8) = s_grass_pos[1];
    MEM32(g_esp + 12) = s_grass_pos[2];
    grass_put_back();
}

/* ── Conker: cutscene blur (F2 Picture > Cutscene blur) ──────────────────
 *
 * The cutscene depth of field is one function per frame, 0x001C71A0
 * (dof_update, called from 0x002A5D62). Only while the cutscene camera
 * runs ([0x0076F0E0] == 1) and the shot has a focus ([0x0084947C] != 0)
 * with its flag set ([0x00849480], written by the camera spline code
 * 0x000569A0) does it draw: near = focus x 25 (at least 1, at most far),
 * far = focus x 100, and 0x00403040 (dof_draw) gets those with the
 * camera's near and far planes ([0x0076F7AC], [0x0076F7B0]). dof_draw
 * shrinks the frame to 256x256 and 128x128 and mixes that over the frame
 * by the depth: in depth-buffer units d = (1 - zn/z) zf/(zf - zn), the
 * ramp a = saturate(slope (d - start)) runs from near's depth to far's
 * (slope at most 16: then from far's - 1/16), and the mix is (2a - 1)^2:
 * none halfway, all at both ends (conker-notes 2026-10-23).
 *
 * cutscene_blur (RECOMP_DOF; conker_SetCutsceneBlur from the F2 menu):
 *   off       skips dof_update when it would draw: no copy, no pass
 *   original  the game's own
 *   updated   (default) skips it and asks the runtime for its depth of
 *             field instead, at the same point of the frame: a host
 *             method in the pushbuffer (toolkit nv2a_pb_state.h,
 *             NV_HOST_FX_DOF) with the same focus (the ramp's middle) and
 *             range (half the ramp), the blur growing with the same curve
 *             up to DOF_BLUR guest pixels, smooth at any resolution
 *   cinematic the same, blurred as a lens would and more, with bokeh
 * The runtime draws it on the GPU path only; with RECOMP_GPU=sw updated
 * and cinematic give the original. RECOMP_DOF_BLUR (guest pixels, test
 * switch) changes the most blur. RECOMP_DOF_LOG=1 prints a [DOF] line
 * whenever the shot's values change. */
enum { DOF_OFF, DOF_ORIGINAL, DOF_UPDATED, DOF_CINEMATIC };
static const char *const s_dof_names[] = {
    "off", "original", "updated", "cinematic",
};
static volatile int g_dof_mode = -1;    /* < 0 = not read yet */

/* The original's blur is the frame shrunk 5 x 3.75 times and four taps:
 * about this radius, in guest pixels. */
#define DOF_BLUR 4.0f
/* Cinematic: its lens curve is only ~1.3 times the most blur behind the
 * focus; this much more makes it the clearly stronger look. */
#define DOF_CINEMATIC_MORE 1.75f

static int dof_mode(void)
{
    int m = g_dof_mode;
    if (m < 0) {
        const char *e = getenv("RECOMP_DOF");
        m = DOF_UPDATED;
        if (e)
            for (int i = 0; i < 4; i++)
                if (!_stricmp(e, s_dof_names[i]))
                    m = i;
        g_dof_mode = m;
    }
    return m;
}

/* For the F2 menu (overlay.cpp): 0 off, 1 original, 2 updated,
 * 3 cinematic. */
void conker_SetCutsceneBlur(int mode)
{
    g_dof_mode = mode >= DOF_OFF && mode <= DOF_CINEMATIC ? mode
                                                          : DOF_UPDATED;
}

int conker_CutsceneBlurOn(void)
{
    return dof_mode() != DOF_OFF;
}

/* The mode in effect: photo mode's B toggles between off and the F2 choice
 * (updated when that is off), for photo mode only. */
static int dof_mode_now(void)
{
    int m = dof_mode(), t = photo_cut_blur();
    if (t == 0)
        return DOF_OFF;
    if (t == 1 && m == DOF_OFF)
        return DOF_UPDATED;
    return m;
}

/* The runtime draws host effects on the GPU back ends only. */
static int dof_host_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        const char *e = getenv("RECOMP_GPU");
        ok = e && (!_stricmp(e, "d3d11") || !_stricmp(e, "both"));
    }
    return ok;
}

static float dof_max_blur(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_DOF_BLUR");
        v = e && *e ? (float)atof(e) : DOF_BLUR;
        if (v < 0.5f || v > 32.0f)
            v = DOF_BLUR;
    }
    return v;
}

extern void sub_0053C300(void);

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* The depth of field as a host method, where dof_draw would have drawn:
 * focus and range in depth-buffer units, from near/far as dof_draw works
 * them out (0x00403040, 0x00403410). The pushbuffer is D3D's: its put
 * pointer is device[0] (device at [0x005499E8]); below device[1] there
 * are at least 0x204 bytes free, else 0x0053C300 (MakeRequestedSpace, eax
 * and a pushed half of [0x0054A8B8], ret 4) makes room and returns the
 * new put pointer, as the game's own code does it. */
static void dof_host_emit(float focus, float range, float cn, float cf,
                          int style, float blur)
{
    uint32_t dev = MEM32(0x005499E8u), p, w[9];
    int i;

    w[0] = (6u << 18) | (7u << 13) | 0x0800u;   /* NV_HOST_PARAM(0..5) */
    w[1] = f2u(focus);
    w[2] = f2u(range);
    w[3] = f2u(cn);
    w[4] = f2u(cf);
    w[5] = (uint32_t)style;
    w[6] = f2u(blur);
    w[7] = (1u << 18) | (7u << 13) | 0x0880u;   /* NV_HOST_EFFECT */
    w[8] = 1u;                                  /* NV_HOST_FX_DOF */

    p = MEM32(dev);
    if (p >= MEM32(dev + 4)) {
        uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_ebx = g_ebx,
                 r_esi = g_esi, r_edi = g_edi, r_esp = g_esp;
        g_eax = MEM32(0x0054A8B8u);
        g_esp -= 4;
        MEM32(g_esp) = g_eax >> 1;
        g_esp -= 4;
        MEM32(g_esp) = 0;               /* return address, popped by the callee */
        sub_0053C300();
        p = g_eax;
        g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
        g_esi = r_esi; g_edi = r_edi; g_esp = r_esp;
    }
    for (i = 0; i < 9; i++)
        MEM32(p + 4u * i) = w[i];
    MEM32(dev) = p + 4u * 9;
}

static void dof_host_draw(float nr, float fr, float cn, float cf, int style)
{
    float q = cf / (cf - cn), dn, df, slope, start;

    dn = (1.0f - cn / nr) * q;
    df = (1.0f - cn / fr) * q;
    dn = dn < 0.0f ? 0.0f : dn > 1.0f ? 1.0f : dn;
    df = df < 0.0f ? 0.0f : df > 1.0f ? 1.0f : df;
    if (df < dn) {
        float t = dn;
        dn = df;
        df = t;
    }
    slope = df > dn ? 1.0f / (df - dn) : 16.0f;
    start = dn;
    if (slope > 16.0f) {
        slope = 16.0f;
        start = df - 1.0f / 16.0f;
    }
    dof_host_emit(start + 0.5f / slope, 0.5f / slope, cn, cf, style,
                  dof_max_blur() * (style ? DOF_CINEMATIC_MORE : 1.0f));
}

/* Photo mode's depth of field (photo_mode.c): a lens focused focus units
 * away (the cinematic style), blur guest pixels at twice that distance:
 * the focus depth and the depth distance to there, d = (1 - cn/z) cf /
 * (cf - cn) as dof_draw makes them. */
static void dof_photo_draw(float focus, float blur, float cn, float cf)
{
    float q = cf / (cf - cn), z2, d0, d2;

    if (focus < cn * 1.01f)
        focus = cn * 1.01f;
    z2 = focus * 2.0f;
    if (z2 > cf * 0.999f)
        z2 = cf * 0.999f;
    d0 = (1.0f - cn / focus) * q;
    d2 = (1.0f - cn / z2) * q;
    if (d2 > 0.9999f)
        d2 = 0.9999f;
    if (d2 <= d0)
        return;
    dof_host_emit(d0, d2 - d0, cn, cf, 1, blur);
}

static int dof_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DOF_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

extern void sub_001C71A0_gen(void);
void sub_001C71A0(void)
{
    int draws = MEM8(0x0076F0E0u) == 1 && MEMF(0x0084947Cu) != 0.0f
             && MEM8(0x00849480u) != 0;
    int mode;

    if (dof_log_on()) {
        static int last_draws = -1;
        static float last_focus, last_cn, last_cf;
        float focus = MEMF(0x0084947Cu);
        float cn = MEMF(0x0076F7ACu), cf = MEMF(0x0076F7B0u);
        if (draws != last_draws || (draws && (focus != last_focus
                || cn != last_cn || cf != last_cf))) {
            float nr = focus * 25.0f, fr = focus * 100.0f;
            if (nr < 1.0f) nr = 1.0f;
            if (nr > fr) nr = fr;
            if (draws)
                fprintf(stderr, "[DOF] t=%ld focus %g: near %g far %g, camera"
                        " %g..%g (%s)\n", attach_ms(), focus, nr, fr, cn, cf,
                        s_dof_names[dof_mode_now()]);
            else
                fprintf(stderr, "[DOF] t=%ld none\n", attach_ms());
            last_draws = draws;
            last_focus = focus;
            last_cn = cn;
            last_cf = cf;
        }
    }
    /* Photo mode's own, in place of the shot's (frozen: the cutscene
     * camera's focus no longer moves anyway). */
    {
        float focus, blur, cn, cf;
        if (photo_dof(&focus, &blur, &cn, &cf) && dof_host_ok()) {
            static float last_focus = -1.0f, last_blur;
            if (dof_log_on() && (focus != last_focus || blur != last_blur)) {
                fprintf(stderr, "[DOF] t=%ld photo: focus %g, blur %g, camera"
                        " %g..%g\n", attach_ms(), focus, blur, cn, cf);
                last_focus = focus;
                last_blur = blur;
            }
            dof_photo_draw(focus, blur, cn, cf);
            g_esp += 4;                 /* ret */
            return;
        }
    }
    mode = dof_mode_now();
    if (draws && mode != DOF_ORIGINAL && (mode == DOF_OFF || dof_host_ok())) {
        if (mode != DOF_OFF) {
            float focus = MEMF(0x0084947Cu);
            float nr = focus * 25.0f, fr = focus * 100.0f;
            if (nr < 1.0f) nr = 1.0f;
            if (nr > fr) nr = fr;
            dof_host_draw(nr, fr, MEMF(0x0076F7ACu), MEMF(0x0076F7B0u),
                          mode == DOF_CINEMATIC);
        }
        g_esp += 4;                     /* ret */
        return;
    }
    sub_001C71A0_gen();
}

/* ── Conker: frame rate (F2 Display > Frame rate) and game speed ─────────
 *
 * 0x002A7B60 (frame_timer_update) runs once a frame and sets the game's
 * time step: vblanks [0x007FA20C] = present interval [0x006C0268] + the
 * vblanks the last swap missed [0x007F9F30] (written by the D3D swap
 * callback 0x002A8020 from D3DSWAPDATA.MissedVBlanks), capped at 60; step
 * [0x007FA24C] = vblanks / 2 (1.0 = one 30 fps frame; ~100 readers); and
 * dt [0x007FA21C] = the rdtsc time since the last frame in seconds (at most
 * 1 s), 0 while paused ([0x007FA275]). So the game was built for other
 * intervals: at interval 1 the step is 0.5.
 *
 * Frame rate 60 (frame_rate, RECOMP_FRAME_RATE=60; conker_SetFrameRate
 * from the F2 menu) is the community 60 fps patch done live: where the game
 * asks for interval 2 (video_reset_device_640x480, 0x002A78D0, edx; the
 * patch changes `mov edx, 2` at 0x001F6B22 and 0x002A9DCB), it gets 1.
 * The reset stores the interval in [0x006C0268] and D3D's presentation
 * interval [0x00549BF4] (0x80000002 here: TWO_OR_IMMEDIATE); after it,
 * frame_timer_update's hook rewrites both at the start of a frame, on the
 * game thread, so a change from the menu applies from the next frame.
 * Other intervals pass through untouched (1 in the opening movies, 3 when
 * [0x0084A144] != 0).
 *
 * The dip fix: with the IMMEDIATE bit a late frame flips at once, but D3D
 * reports MissedVBlanks as if it had waited for the next vblank, so every
 * late frame adds one vblank too many to the step and the game runs fast
 * in a dip (1.35-1.5x in It's War at 30, up to 1.8x at 60; conker-notes
 * 2026-10-22). Such a swap is due on the vblank it came in (SwapVBlank ==
 * D3D's count); the hook takes one off its MissedVBlanks, so the step is
 * the vblanks between flips. Always on at 60; at 30 on by default too
 * (user 2026-10-22): slowdown_fix = 0 / RECOMP_DIP_FIX=0 (F2 with --debug,
 * conker_SetDipFix) gives the original, which runs fast in a dip.
 *
 * RECOMP_FRAMESTEP_LOG=1 prints once a second ([FSTEP]): frames, missed
 * vblanks per frame, the average step and dt, D3D's vblank count
 * (device +0x1DE8) per second, and game time against wall time (the step
 * counted as 1/30 s, and dt). */
static volatile int g_fps60 = -1;      /* frame rate 60; < 0 = not read yet */
static volatile int g_dip_fix = -1;    /* dip fix at 30; < 0 = not read yet */
static uint32_t g_asked_interval;      /* the game's own, from the reset */

static int framestep_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("RECOMP_FRAMESTEP_LOG");
        on = s ? (atoi(s) > 1 ? atoi(s) : 1) : 0;
    }
    return on;
}

static int fps60_on(void)
{
    if (g_fps60 < 0) {
        const char *e = getenv("RECOMP_FRAME_RATE");
        g_fps60 = e && atoi(e) == 60;
    }
    return g_fps60;
}

static int dip_fix_on(void)
{
    if (g_dip_fix < 0) {
        const char *e = getenv("RECOMP_DIP_FIX");
        g_dip_fix = !e || atoi(e) != 0;
    }
    return fps60_on() || g_dip_fix;
}

/* From the F2 menu (overlay.cpp): 30 or 60; applies from the next frame. */
void conker_SetFrameRate(int fps)
{
    g_fps60 = fps == 60;
}

void conker_SetDipFix(int on)
{
    g_dip_fix = on != 0;
}

/* The interval the game should run at: its own, or 1 for its 2 at 60. */
static uint32_t frame_interval(uint32_t asked)
{
    return asked == 2u && fps60_on() ? 1u : asked;
}

/* Conker's own soak test (conker-notes 2026-10-23; 0x000A54B0
 * soak_test_update): map after map, then the chapters and a list of scenes,
 * over and over, with the map's name in the corner (Back hides it). The
 * game starts it when LT + RT + Black go down in the same frame on
 * controller port 1, on the front end; our pad is port 3, so here it never
 * starts. This sets the game's own state byte to 1 ("start") instead, only
 * on the front end, as the combo would. RECOMP_SOAK=1 asks for it at start-up
 * (it starts once the front end's movies have closed); the F2 menu with
 * --debug through conker_StartSoak, at once. Every new map, mode and loop
 * is logged ([SOAK]). */
#define SOAK_STATE 0x0077111Cu          /* mode; 0 idle, 1 start, 2 maps,
                                           3 chapters, 4 scenes, 5 loop */
#define SOAK_LEVEL 0x007FA1F8u          /* current level; 0x1D front end */
static volatile int g_soak_ask = -1;    /* 1 when the movies close, 2 now */

void conker_StartSoak(void)
{
    g_soak_ask = 2;
}

/* For the F2 menu: 0 not on the front end, 1 can start, 2 running. */
int conker_SoakState(void)
{
    uint32_t level = MEM32(SOAK_LEVEL);
    if (MEM8(SOAK_STATE))
        return 2;
    return level == 0x1Du || level == 0x46u;
}

static void soak_poll(void)
{
    static uint8_t mode, map;
    static uint32_t loops;
    uint8_t m = MEM8(SOAK_STATE);

    if (g_soak_ask < 0) {
        const char *e = getenv("RECOMP_SOAK");
        g_soak_ask = e && atoi(e) != 0;
    }
    if (g_soak_ask > 0 && m)
        g_soak_ask = 0;                 /* already running */
    if (g_soak_ask > 0 && conker_SoakState() == 1
        && (g_soak_ask == 2
            || (!MEM32(0x00849C4Cu) && !MEM32(0x00849C58u)))) {  /* XMVs */
        MEM8(SOAK_STATE) = 1;
        g_soak_ask = 0;
        fprintf(stderr, "[SOAK] t=%ld started on level 0x%02X\n", attach_ms(),
                MEM32(SOAK_LEVEL));
    }
    if (m < 2)
        return;
    if (m != mode || (m == 2 && MEM8(0x0077111Eu) != map)
        || MEM32(0x00771118u) != loops) {
        char name[48] = "";
        uint32_t p = m == 2 ? MEM32(0x0069CF90u + 4u * MEM8(0x0077111Eu)) : 0;
        unsigned i;
        for (i = 0; p && i < sizeof name - 1 && MEM8(p + i); i++)
            name[i] = (char)MEM8(p + i);
        name[i] = 0;
        mode = m;
        map = MEM8(0x0077111Eu);
        loops = MEM32(0x00771118u);
        fprintf(stderr, "[SOAK] t=%ld mode %u%s map %u %s, level 0x%02X, loop %u\n",
                attach_ms(), m, m == 2 ? " (maps)" : m == 3 ? " (chapters)"
                : m == 4 ? " (scenes)" : "", map, name, MEM32(SOAK_LEVEL),
                loops);
    }
}

/* Shader pre-compilation (SPEC §10.1): the toolkit's memory scan is asked
 * for at the first frame and when the level [0x007FA1F8] changes; with
 * RECOMP_VSH_PRECOMPILE=1 every program it finds is compiled into the shader
 * cache before it is drawn, RECOMP_VSH_SCAN=1 counts what it found (both
 * unset: nothing happens). Scans 2 s and 6 s after a change found nothing
 * new in the soak test (2026-10-24) and were dropped; so did a scan every
 * second (programs are not in memory before the level number changes). */
extern void nv2a_vsh_prescan_request(const char *why);

static void vsh_scan_poll(void)
{
    static int started;
    static uint32_t level = 0xFFFFFFFFu;
    uint32_t lv = MEM32(0x007FA1F8u);
    char why[64];

    if (!started) {
        started = 1;
        nv2a_vsh_prescan_request("start");
    }
    if (lv != level) {
        level = lv;
        snprintf(why, sizeof why, "level 0x%02X, t=%ld", lv, attach_ms());
        nv2a_vsh_prescan_request(why);
    }
}

/* Which keyboard-and-mouse set is in use (src/controls.c), every frame.
 *
 * Multiplayer or single player by the level number: multiplayer on the
 * multiplayer front end (level 0x46, Mp_Frontend) and on every level
 * without a name in the soak test's table at 0x0069CF90 (one pointer per
 * level number, 0x00-0x4A): it names the story's levels, the front end
 * (0x1D) and GenericCutscenes (0x25), so the empty ones are multiplayer
 * maps, and so is every level past it (a bot match on Chapter X loaded
 * level 0x4B; conker-notes 2026-10-25). The table is in the XBE's .data,
 * always there.
 *
 * Over that, fixed keys: the front ends' menu keys on both front ends (Esc
 * goes back there); a level's menu keys whenever the game drew a menu list
 * in the last two frames (0x0020C3B0 and 0x0020C200, the list drawers: the
 * multiplayer pause menu; Esc closes it); the single player pause screen's
 * keys while the pause flag [0x007FA275] is set in a story level without a
 * list (its Options page and "Quit?" are no lists; run_20261025_160000);
 * the cutscene keys while the cutscene camera runs ([0x0076F0E0] == 1, as
 * for the cutscene blur) in a story level. */
static uint32_t s_ctl_frame;          /* frames counted here */
static uint32_t s_ctl_list_frame;     /* the last frame a list was drawn */

static void controls_list_drawn(void)
{
    s_ctl_list_frame = s_ctl_frame;
}

/* Mouse look: the game's camera turned by the mouse itself, past the
 * stick's top speed (user 2026-10-25: the stick emulation over-swung).
 * Camera 0 (the cameras at [0x008470DC], 0x770 bytes each, updated by
 * 0x0003C500) has a controller at +4. Angles in degrees; each one written
 * is the value the stick changes (its source): copies made from it every
 * frame overwrite a write (conker-notes 2026-10-26).
 *
 * Single player, normal view (camera mode +0xC0 = 1): yaw = the
 * controller's +0x60 (right stick right lowers it); pitch = the wanted
 * pitch +0x8EC, which the camera's +0x34 eases towards (writing +0x34 alone
 * eased back: user 2026-10-26).
 *
 * Head-cam (mode 0x100, camera byte 0 clear): yaw = the controller's
 * current yaw +0x5C (Conker's facing is set from it); pitch = +0x34.
 *
 * Aiming (mode 0x100 with camera byte 0 set: a weapon out in single
 * player; every multiplayer match): yaw = the aim offset +0x14 of the
 * object at [camera +0x3E0 (the character) + 0x568] (right stick right
 * raises it; the game keeps it within about 50 degrees and turns the body
 * by the rest); pitch = +0x40 of that object's aim data [+0x4D4], which
 * the game limits to the weapon's range (0x00039B40). A multiplayer
 * profile's own "Invert Y Look" flips only the stick, not this.
 *
 * Underwater (mode 0x80, the underwater keys): 0x002D3CF0 swims along the
 * aim data's yaw +4 and pitch +0x40 (the stick turns them, at most ~45
 * degrees a second underwater), easing Conker towards them through two
 * springs whose current values are 0x0087BF38 (yaw) and 0x0087BF10 (minus
 * the pitch). The camera follows Conker through two springs of its
 * controller, yaw at +0x3B8 (current +0x3BC, target +0x3C0; made into +0x60)
 * and pitch at +0x390 (current +0x394, target +0x398 = the pitch within
 * +-85; made into +0x38), then eases +0x5C and +0x34 towards those. All of
 * them move with the write, so the turn shows the same frame (conker-notes
 * 2026-10-26, swimming). Conker's facing (+0x170/+0x174, 65536 a turn, and
 * its degrees +0x114) is set too: the camera reads it before 0x002D3CF0
 * runs again, and the old one pulled the camera back for a few frames.
 *
 * Bat flight (Count Batula; Conker's +0x1B5 = 10, set only by the bat's
 * flight code 0x002BC180): camera mode 1 with byte +0 = 1, steered like
 * underwater by aim data +4/+0x40 (pitch +-89), eased through the bat's own
 * springs, yaw current 0x0075A1C4 and minus the pitch 0x0075A1EC. The
 * camera's pitch target +0x398 is the pitch - 5 within -85..85. While
 * 0x0084A5B0 is set the flight code holds the pitch at 0 (conker-notes
 * 2026-10-27, bat flight).
 *
 * Multiplayer scope (Long Ranger, right stick click; camera +0x5D0 = 0):
 * the stick turns the controller's springs, yaw +0x5C (current) / +0x60
 * (target) and pitch +0x34 / +0x38 (+-80, larger = down), and the game
 * keeps the aim object's +0x14, +0x258, +0x260, +0x25C, +0x264, aim data
 * +4 and the camera's +0x3BC in step with them; a write to only some of
 * them was put back the same frame, so the mouse moves them all
 * (conker-notes 2026-10-27, sniper scope).
 *
 * Tank (It's War "Chemical Warfare"): the tank is a character of its own
 * whose +0x250 is its driver's index + 1. Its movement code 0x002BAEC0
 * keeps the turret's yaw relative to the hull in its aim object's +0x2EC
 * (the stick's turn per frame, aim data +8, is taken off it: right lowers
 * it) and the pitch's source in its aim data +0x40 (the stick winds it to
 * +-89; the turret +0x2E8 is kept within -10..45, the camera's pitch is it
 * - 15 within -25..12); the camera is set from them each frame (0x0004B320)
 * and the turret model eases after the camera (0x000E41B0). The mouse
 * moves those two at RECOMP_KBM_TANK_SCALE (default 0.5) of its usual
 * speed, the pitch within the turret's range (conker-notes 2026-10-27,
 * tank).
 *
 * Called every frame from frame_timer_update, after the pad poll and
 * before the camera update: the whole movement since the last frame turns
 * the camera this frame. In any other mode (other vehicles, mounted guns, not
 * checked) the toolkit's stick emulation stays (conker-notes 2026-10-25). */
static float mouse_deg_per_count(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_KBM_MOUSE_DEG");
        v = e && atof(e) > 0.0 ? (float)atof(e) : 0.09f;  /* 4000 a turn */
    }
    return v;
}

/* RECOMP_CAM_DUMP=<file>: every frame of play, camera 0 (0x770 bytes), its
 * controller (0x900), the character it follows (0x6D0), that one's aim
 * object (0x40), the objects at camera +0x3E4 (0x400) and +0x3B4 (0x100)
 * and the aim data [aim +0x4D4] (0x100), after a 32-byte header, appended
 * to the file: for finding which value a stick or mouse movement changes
 * (conker-notes 2026-10-26). */
static void cam_dump(void)
{
    static FILE *f;
    static int tried;
    static uint32_t frame;
    uint32_t cam, ctl, chr, aim, st, hdr[8];
    static uint8_t zero[0x900];

    if (!tried) {
        const char *e = getenv("RECOMP_CAM_DUMP");
        tried = 1;
        f = e && *e ? fopen(e, "wb") : NULL;
    }
    if (!f)
        return;
    cam = MEM32(0x008470DCu);
    ctl = cam ? MEM32(cam + 4u) : 0;
    chr = cam ? MEM32(cam + 0x3E0u) : 0;
    aim = chr ? MEM32(chr + 0x568u) : 0;
    hdr[0] = 0x4D414343u;   /* "CCAM" */
    hdr[1] = frame++;
    hdr[2] = (uint32_t)attach_ms();
    hdr[3] = cam;
    hdr[4] = ctl;
    hdr[5] = chr;
    hdr[6] = aim;
    hdr[7] = MEM32(0x007FA1F8u);
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(cam ? (const void *)XBOX_PTR(cam) : zero, 0x770, 1, f);
    fwrite(ctl ? (const void *)XBOX_PTR(ctl) : zero, 0x900, 1, f);
    fwrite(chr ? (const void *)XBOX_PTR(chr) : zero, 0x6D0, 1, f);
    fwrite(aim ? (const void *)XBOX_PTR(aim) : zero, 0x40, 1, f);
    st = cam ? MEM32(cam + 0x3E4u) : 0;
    fwrite(st ? (const void *)XBOX_PTR(st) : zero, 0x400, 1, f);
    st = cam ? MEM32(cam + 0x3B4u) : 0;
    fwrite(st ? (const void *)XBOX_PTR(st) : zero, 0x100, 1, f);
    st = aim ? MEM32(aim + 0x4D4u) : 0;
    fwrite(st ? (const void *)XBOX_PTR(st) : zero, 0x100, 1, f);
    fflush(f);
}

#define SWIM_PITCH_LO (-89.0f)
#define SWIM_PITCH_HI 89.0f
#define SCOPE_PITCH_MAX 80.0f   /* stick held up or down while scoped */

static float swim_cam_pitch(float p)
{
    return p < -85.0f ? -85.0f : p > 85.0f ? 85.0f : p;
}

#define TANK_PITCH_LO (-10.0f)  /* the turret's own range (0x002BAEC0) */
#define TANK_PITCH_HI 45.0f

static float tank_mouse_scale(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_KBM_TANK_SCALE");
        v = e && atof(e) > 0.0 ? (float)atof(e) : 0.5f;
    }
    return v;
}

/* The vehicle character `chr` drives: the one in the character array
 * [0x0084A5F8] (0x81 x 0x6D0, in use when +0x64 is set) whose driver
 * +0x250 is chr's index + 1; 0 when on foot. */
static uint32_t driven_vehicle(uint32_t chr)
{
    uint32_t base = MEM32(0x0084A5F8u), i, me;

    if (!base || chr < base || (chr - base) % 0x6D0u
            || (me = (chr - base) / 0x6D0u) >= 0x81u)
        return 0;
    for (i = 0; i < 0x81u; i++) {
        uint32_t c = base + i * 0x6D0u;
        if (MEM32(c + 0x64u) && MEM8(c + 0x250u) == me + 1u)
            return c;
    }
    return 0;
}

/* Multiplayer vehicles the mouse steers or aims: the object's vtable and
 * the pitch range of +0x774 (a gun; smaller is up) or of the Steed. Not
 * here: the R-Hog (vtable 0x0063FEC8), whose right stick does nothing, and
 * the Mule 52's gunner seat (it aims with the left stick, in the air only;
 * keys: on_mule_gun). */
enum { MPV_TANKUS = 1, MPV_TOAD, MPV_DRAGON, MPV_TURRET, MPV_MULE,
       MPV_STEED, MPV_SNOOPA };
static const struct {
    uint32_t vtable;
    float lo, hi;
} s_mpv[] = {
    { 0 },
    { 0x00641260u, -45.0f, 4.0f },   /* Tankus's gun (update 0x003AEC40) */
    { 0x0063FE00u, -55.0f, 20.0f },  /* Toad MkII gun (update 0x003A8650) */
    { 0x0063F808u, -55.0f, 20.0f },  /* Dragon MkI gun (update 0x003A2990) */
    { 0x006416B8u, -55.0f, 20.0f },  /* the maps' seated turrets (update
                                        0x003B2A40; yaw not wrapped) */
    { 0x0063F2A8u, 0.0f, 0.0f },     /* Mule 52 driver (update 0x0039FE60,
                                        movement 0x0039E240): its facing;
                                        up/down climbs, left to the stick */
    { 0x0063F8D0u, -85.0f, 85.0f },  /* Steed (update 0x003A4B00): its
                                        character's pitch +0x1C0 */
    { 0x00641118u, -85.0f, 85.0f },  /* the Sneeker's Snoopa drone (update
                                        0x003ACCE0): like the Steed */
};

/* The multiplayer vehicle player character `chr` controls with the right
 * stick, or 0; *type gets its s_mpv index. Its object hangs off its own
 * character +0x570 and points back at +4. Seats are 0xB0 apart from
 * +0x1F0: driver +0x1F0, gunner +0x2A0, each with a type byte at +0xC (2 =
 * a player). A gunner aims; the driver does when the gunner seat is empty
 * (a truck puts its gun back once it drives). The Mule's, the Steed's and
 * the Snoopa's driver steers. */
static uint32_t mp_vehicle(uint32_t chr, int *type)
{
    uint32_t base = MEM32(0x0084A5F8u), i;
    int t;

    if (!base || !chr)
        return 0;
    for (i = 0; i < 0x81u; i++) {
        uint32_t c = base + i * 0x6D0u, o, seat;
        if (!MEM32(c + 0x64u))
            continue;
        o = MEM32(c + 0x570u);
        if (!xact_va_ok(o) || MEM32(o + 4u) != c)
            continue;
        for (t = MPV_TANKUS; t <= MPV_SNOOPA; t++)
            if (MEM32(o) == s_mpv[t].vtable)
                break;
        if (t > MPV_SNOOPA) {
            /* A vehicle the mouse does not know yet: say so once. */
            static uint32_t told;
            if ((MEM32(o + 0x1F0u) == chr || MEM32(o + 0x2A0u) == chr)
                    && told != MEM32(o)) {
                told = MEM32(o);
                fprintf(stderr, "[CONTROLS] t=%ld unknown vehicle: vtable "
                        "%08X, update %08X, seat %s\n", attach_ms(), told,
                        MEM32(told + 0x24u),
                        MEM32(o + 0x1F0u) == chr ? "+0x1F0" : "+0x2A0");
            }
            continue;
        }
        seat = t < MPV_MULE && MEM32(o + 0x2A0u) ? 0x2A0u : 0x1F0u;
        if (MEM32(o + seat) == chr && MEM8(o + seat + 0xCu) == 2u) {
            *type = t;
            return o;
        }
    }
    return 0;
}

static void mouse_look(int set)
{
    static const char *const kinds[] = {
        "right stick", "camera turned directly", "head-cam turned directly",
        "aim turned directly", "aim left/right direct, up/down stick",
        "swimming turned directly", "bat flight turned directly",
        "sniper scope turned directly", "tank turned directly",
        "multiplayer tank turned directly", "Toad gun turned directly",
        "Dragon gun turned directly", "turret turned directly",
        "Mule turned directly (up/down stick)", "Steed turned directly",
        "Vault tail turned directly", "Snoopa turned directly",
    };
    static int was = -1;
    static uint32_t was_mode;
    uint32_t cam = 0, ctl = 0, mode = 0, aim = 0, aimd = 0, swim = 0, bat = 0;
    uint32_t scope = 0, tank = 0, tankd = 0, mpv = 0, tail = 0;
    int mpvt = 0;
    int story = set == CONTROLS_STORY || set == CONTROLS_SWIM_SURFACE
                || set == CONTROLS_SWIM_UNDER || set == CONTROLS_BAT_AIR;
    int direct = 0, kind;
    long dx, dy;

    if (story || set == CONTROLS_MP)
        cam_dump();

    /* RECOMP_WATCH_FILE=<path>: a guest address (hex) written to that file
     * arms RECOMP_WATCH on it once, while the game runs -- for heap values
     * whose address is only known once the state is reached. */
    {
        static int armed = -1;
        static const char *wf;
        FILE *f;
        char line[64];
        if (armed < 0) {
            wf = getenv("RECOMP_WATCH_FILE");
            armed = !wf || !*wf;
        }
        if (!armed && (f = fopen(wf, "r")) != NULL) {
            if (fgets(line, sizeof line, f) && strtoul(line, NULL, 16)) {
                char env[96];
                extern void xbox_WatchInit(void);
                snprintf(env, sizeof env, "RECOMP_WATCH=0x%08lX",
                         strtoul(line, NULL, 16));
                _putenv(env);
                fprintf(stderr, "[CONTROLS] t=%ld arming %s\n", attach_ms(), env);
                xbox_WatchInit();
                armed = 1;
            }
            fclose(f);
        }
    }

    /* The camera below is the first player's (the only one in single
     * player; split screen not looked into): with the keyboard on another
     * controller port (F3 > Slots) the mouse stays the right
     * stick of that port's pad. */
    if ((story || set == CONTROLS_MP) && controls_keyboard_port() == 0
        && !MEM8(0x007FA275u) && MEM8(0x0076F0E0u) != 1) {
        cam = MEM32(0x008470DCu);
        ctl = cam ? MEM32(cam + 4u) : 0;
        mode = cam ? MEM32(cam + 0xC0u) : 0;
        if (story && ctl && (mode == 1u || mode == 0x100u))
            direct = XBOX_KBM_DIRECT_X | XBOX_KBM_DIRECT_Y;
        /* Aiming: a weapon out in single player (camera byte 0 set), every
         * multiplayer match. */
        if (ctl && mode == 0x100u && (set == CONTROLS_MP || MEM8(cam))) {
            uint32_t chr = MEM32(cam + 0x3E0u);
            aim = chr ? MEM32(chr + 0x568u) : 0;
            aimd = aim ? MEM32(aim + 0x4D4u) : 0;
            if (aim)
                direct = XBOX_KBM_DIRECT_X | (aimd ? XBOX_KBM_DIRECT_Y : 0);
            /* A multiplayer scope (Long Ranger): aiming state +0x5D0 is 0
             * (2 while aiming unscoped), so 0x00039B40 skips its aim code. */
            if (aimd && set == CONTROLS_MP && !MEM32(cam + 0x5D0u))
                scope = aimd;
            /* The It's War tank: Conker drives a vehicle character. Not
             * the camera's aiming state +0x5D0, which wanders there (1, 2,
             * 0). Multiplayer vehicles: mp_vehicle below. */
            if (chr && set != CONTROLS_MP) {
                uint32_t v = driven_vehicle(chr);
                tank = v ? MEM32(v + 0x568u) : 0;
                tankd = tank ? MEM32(tank + 0x4D4u) : 0;
                if (!tankd)
                    tank = 0;
                else
                    direct = XBOX_KBM_DIRECT_X | XBOX_KBM_DIRECT_Y;
            }
        }
        if (ctl && set == CONTROLS_MP) {
            mpv = mp_vehicle(MEM32(cam + 0x3E0u), &mpvt);
            if (mpv)
                direct = XBOX_KBM_DIRECT_X
                       | (mpvt == MPV_MULE ? 0 : XBOX_KBM_DIRECT_Y);
        }
        if (ctl && mode == 0x80u && set == CONTROLS_SWIM_UNDER) {
            uint32_t chr = MEM32(cam + 0x3E0u);
            uint32_t a = chr ? MEM32(chr + 0x568u) : 0;
            swim = a ? MEM32(a + 0x4D4u) : 0;
            if (swim)
                direct = XBOX_KBM_DIRECT_X | XBOX_KBM_DIRECT_Y;
        }
        if (ctl && mode == 1u && set == CONTROLS_BAT_AIR) {
            uint32_t chr = MEM32(cam + 0x3E0u);
            uint32_t a = chr && MEM8(chr + 0x1B5u) == 10u
                       ? MEM32(chr + 0x568u) : 0;
            bat = a ? MEM32(a + 0x4D4u) : 0;
        }
    }
    /* Holding the alien's tail (The Vault): camera mode 0x100 like
     * aiming, but the camera turns with the values the scope keeps in
     * step, not with the aim alone (conker-notes 2026-10-28, Vault tail). */
    if (set == CONTROLS_VAULT_TAIL && controls_keyboard_port() == 0
        && !MEM8(0x007FA275u) && MEM8(0x0076F0E0u) != 1) {
        uint32_t chr;
        cam = MEM32(0x008470DCu);
        ctl = cam ? MEM32(cam + 4u) : 0;
        mode = cam ? MEM32(cam + 0xC0u) : 0;
        chr = cam ? MEM32(cam + 0x3E0u) : 0;
        aim = chr ? MEM32(chr + 0x568u) : 0;
        aimd = aim ? MEM32(aim + 0x4D4u) : 0;
        if (ctl && aimd) {
            tail = aimd;
            direct = XBOX_KBM_DIRECT_X | XBOX_KBM_DIRECT_Y;
        }
    }
    kind = !direct ? 0 : tail ? 15 : mpvt == MPV_SNOOPA ? 16 : mpv ? 8 + mpvt : tank ? 8 : scope ? 7 : bat ? 6 : swim ? 5 : aim ? (aimd ? 3 : 4)
         : mode == 0x100u ? 2 : 1;
    if (kind != was || (!direct && cam && mode != was_mode)) {
        fprintf(stderr, "[CONTROLS] t=%ld mouse look: %s (camera mode %08X)\n",
                attach_ms(), kinds[kind], mode);
        was = kind;
        was_mode = mode;
    }
    xbox_KbmMouseDirect(direct);
    if (!direct || !xbox_KbmTakeMouse(&dx, &dy))
        dx = dy = 0;
    if (mpv && (mpvt == MPV_STEED || mpvt == MPV_SNOOPA || mpvt == MPV_MULE)) {
        /* The Steed and the Snoopa: their character's facing (the game
         * derives the heading from it; stick right lowers it) and pitch
         * (smaller is nose up),
         * at full speed: it spins fast on the stick. The Mule: its facing
         * the same way; up/down stays with the stick. Uneven mouse steps
         * shook its model in first person (user), so each movement is
         * spread over a few frames: RECOMP_KBM_STEED_SMOOTH = the share
         * left for later each frame (default 0.75; 0 = off; 0.5 still
         * stuttered on quick jerks), RECOMP_KBM_STEED_MAX = degrees a
         * frame at most (default 8; 0 = no limit). */
        static float smooth = -1.0f, cap, py, pp;
        uint32_t c = MEM32(mpv + 4u);
        float sx, sy, k = mouse_deg_per_count(), y, q, a, b;
        int inv;
        if (smooth < 0.0f) {
            const char *e = getenv("RECOMP_KBM_STEED_SMOOTH");
            smooth = e && *e ? (float)atof(e) : 0.75f;
            smooth = smooth < 0.0f ? 0.0f : smooth > 0.95f ? 0.95f : smooth;
            e = getenv("RECOMP_KBM_STEED_MAX");
            cap = e && *e ? (float)atof(e) : 8.0f;
        }
        controls_mouse(set, &sx, &sy, &inv);
        py -= (float)dx * k * sx;
        pp += (float)(inv ? -dy : dy) * k * sy;
        a = py * (1.0f - smooth);
        b = pp * (1.0f - smooth);
        if (cap > 0.0f) {
            a = a > cap ? cap : a < -cap ? -cap : a;
            b = b > cap ? cap : b < -cap ? -cap : b;
        }
        if (py - a > -0.01f && py - a < 0.01f)
            a = py;
        if (pp - b > -0.01f && pp - b < 0.01f)
            b = pp;
        py -= a;
        pp -= b;
        y = MEMF(c + 0x114u) + a;
        y = y >= 360.0f ? y - 360.0f : y < 0.0f ? y + 360.0f : y;
        MEMF(c + 0x114u) = y;
        if (mpvt == MPV_MULE)
            return;
        q = MEMF(c + 0x1C0u) + b;
        MEMF(c + 0x1C0u) = q < s_mpv[mpvt].lo ? s_mpv[mpvt].lo
                         : q > s_mpv[mpvt].hi ? s_mpv[mpvt].hi : q;
        if (q < s_mpv[mpvt].lo || q > s_mpv[mpvt].hi)
            pp = 0.0f;
        return;
    }
    if (!dx && !dy)
        return;
    {
        float sx, sy, k = mouse_deg_per_count();
        int inv;
        controls_mouse(set, &sx, &sy, &inv);
        if (mpv) {
            /* A gun (Tankus, Toad, Dragon, turret): yaw to the hull
             * (stick right lowers it; the vehicles wrap it, a turret
             * leaves it as is) and pitch (smaller is up), at tank speed:
             * the slow aim is the game's. */
            float t = tank_mouse_scale();
            float p = MEMF(mpv + 0x774u), q;
            MEMF(mpv + 0x770u) -= (float)dx * k * sx * t;
            q = p + (float)(inv ? -dy : dy) * k * sy * t;
            MEMF(mpv + 0x774u) = q < s_mpv[mpvt].lo ? s_mpv[mpvt].lo
                               : q > s_mpv[mpvt].hi ? s_mpv[mpvt].hi : q;
            return;
        }
        if (tank) {
            /* The turret's yaw to the hull and the pitch's source, slower
             * than on foot; the game wraps the yaw itself. */
            float t = tank_mouse_scale();
            float p = MEMF(tankd + 0x40u), q;
            MEMF(tank + 0x2ECu) -= (float)dx * k * sx * t;
            q = p + (float)(inv ? dy : -dy) * k * sy * t;
            MEMF(tankd + 0x40u) = q < TANK_PITCH_LO ? TANK_PITCH_LO
                                : q > TANK_PITCH_HI ? TANK_PITCH_HI : q;
            return;
        }
        if (swim || bat) {
            /* Underwater and bat flight: the source, its springs (yaw
             * current, minus the pitch) and the camera's pitch offset. */
            uint32_t src = swim ? swim : bat;
            uint32_t ys = swim ? 0x0087BF38u : 0x0075A1C4u;
            uint32_t ps = swim ? 0x0087BF10u : 0x0075A1ECu;
            float off = swim ? 0.0f : -5.0f;
            uint32_t chr = MEM32(cam + 0x3E0u);
            float d = -(float)dx * k * sx, y, p, q;
            uint16_t h;
            y = MEMF(src + 4u) + d;
            MEMF(ys) += d;
            if (y >= 360.0f) {
                y -= 360.0f;
                MEMF(ys) -= 360.0f;
            } else if (y < 0.0f) {
                y += 360.0f;
                MEMF(ys) += 360.0f;
            }
            MEMF(src + 4u) = y;
            /* As 0x002D3CF0 and 0x002BC180 make it from the spring. */
            h = (uint16_t)(-0x4000 + (int32_t)(MEMF(ys)
                                               * (65536.0f / 360.0f)));
            MEM16(chr + 0x170u) = h;
            MEM16(chr + 0x174u) = h;
            MEMF(chr + 0x114u) = (float)(h + 0x4000) * (360.0f / 65536.0f);
            MEMF(ctl + 0x3BCu) += d;
            MEMF(ctl + 0x3C0u) += d;
            MEMF(ctl + 0x60u) += d;
            MEMF(ctl + 0x5Cu) += d;
            if (bat && MEM8(0x0084A5B0u))
                return;
            /* The game's own range (stick held each way). */
            p = MEMF(src + 0x40u);
            q = p + (float)(inv ? dy : -dy) * k * sy;
            q = q < SWIM_PITCH_LO ? SWIM_PITCH_LO
              : q > SWIM_PITCH_HI ? SWIM_PITCH_HI : q;
            MEMF(src + 0x40u) = q;
            MEMF(ps) -= q - p;
            d = swim_cam_pitch(q + off) - swim_cam_pitch(p + off);
            MEMF(ctl + 0x394u) += d;
            MEMF(ctl + 0x398u) += d;
            MEMF(ctl + 0x38u) += d;
            MEMF(ctl + 0x34u) += d;
            return;
        }
        if (tail) {
            /* The tail: the right stick moved aim data +4, aim +0x14
             * (the other way), aim +0x258, camera +0x3BC and controller
             * +0x5C / +0x60 by the same amount, as scoped; up/down aim
             * data +0x40, which the game keeps in range and makes the
             * controller's pitch from, as aiming. */
            float d = -(float)dx * k * sx;
            float y = MEMF(tail + 4u) + d;
            MEMF(tail + 4u) = y >= 360.0f ? y - 360.0f
                            : y < 0.0f ? y + 360.0f : y;
            MEMF(aim + 0x14u) -= d;
            MEMF(aim + 0x258u) += d;
            MEMF(cam + 0x3BCu) += d;
            MEMF(ctl + 0x5Cu) += d;
            MEMF(ctl + 0x60u) += d;
            MEMF(tail + 0x40u) += (float)(inv ? dy : -dy) * k * sy;
            return;
        }
        if (scope) {
            /* Scoped: every value the stick moves, by the same amount
             * (writing only some of them was put back the same frame). */
            float d = -(float)dx * k * sx;
            float y = MEMF(scope + 4u) + d;
            float p = MEMF(ctl + 0x34u), q;
            MEMF(scope + 4u) = y >= 360.0f ? y - 360.0f
                             : y < 0.0f ? y + 360.0f : y;
            MEMF(aim + 0x14u) -= d;
            MEMF(aim + 0x258u) += d;
            MEMF(aim + 0x260u) += d;
            MEMF(cam + 0x3BCu) += d;
            MEMF(ctl + 0x5Cu) += d;
            MEMF(ctl + 0x60u) += d;
            /* Pitch: +0x34 down is up on screen; the game's range +-80. */
            q = p - (float)(inv ? dy : -dy) * k * sy;
            q = q < -SCOPE_PITCH_MAX ? -SCOPE_PITCH_MAX
              : q > SCOPE_PITCH_MAX ? SCOPE_PITCH_MAX : q;
            MEMF(ctl + 0x34u) += q - p;
            MEMF(ctl + 0x38u) += q - p;
            MEMF(aim + 0x25Cu) += q - p;
            MEMF(aim + 0x264u) += q - p;
            return;
        }
        if (aim) {
            MEMF(aim + 0x14u) += (float)dx * k * sx;
            if (aimd)   /* the game keeps it within the weapon's range */
                MEMF(aimd + 0x40u) += (float)(inv ? dy : -dy) * k * sy;
            return;
        }
        {
            float dp = (float)(inv ? dy : -dy) * k * sy;
            float lo, hi, p, q;
            if (mode == 0x100u) {
                /* Head-cam: 0x00039B40 takes the stick's turn off +0x5C
                 * (+0x60 is made from it there) and sets Conker's facing
                 * from it; the pitch has no wanted value. */
                MEMF(ctl + 0x5Cu) -= (float)dx * k * sx;
                MEMF(ctl + 0x34u) += dp;
                return;
            }
            MEMF(ctl + 0x60u) -= (float)dx * k * sx;
            /* The wanted pitch +0x8EC, limited as 0x00047DC0 does; the
             * camera eases +0x34 (and +0x38) towards it, so they move by
             * the same amount or the camera eases back. */
            if (MEM8(ctl + 0x8C4u) || MEM8(ctl + 0x8C5u))
                lo = -65.0f, hi = 65.0f;
            else if (MEM16(cam + 0x220u) == 3u)
                lo = -30.0f, hi = -5.0f;
            else
                lo = -45.0f, hi = -2.0f;
            p = MEMF(ctl + 0x8ECu);
            q = p + dp < lo ? lo : p + dp > hi ? hi : p + dp;
            if (q == p)
                return;
            MEMF(ctl + 0x8ECu) = q;
            MEMF(ctl + 0x38u) += q - p;
            MEMF(ctl + 0x34u) += q - p;
        }
    }
}

/* Single player in the water (conker-notes 2026-10-26, swimming): Conker
 * (camera 0's +0x3E0) has +0x1B5 = 1 at the surface and underwater; his aim
 * object's +0x2D is set at the surface and clear underwater, where
 * 0x002D3CF0 swims. */
static int story_water(void)
{
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;
    uint32_t aim;

    if (!chr || MEM8(chr + 0x1B5u) != 1u)
        return CONTROLS_STORY;
    aim = MEM32(chr + 0x568u);
    if (!aim)
        return CONTROLS_STORY;
    return MEM8(aim + 0x2Du) ? CONTROLS_SWIM_SURFACE : CONTROLS_SWIM_UNDER;
}

/* BeeCavern ("Wasps' Revenge", level 0x27) starts on foot; the beehive gun
 * is a character whose +0x570 object (vtable 0x006416B8, +4 back to the
 * gun) holds whoever sits in it at +0x1F0, empty when nobody does
 * (conker-notes 2026-10-28, beehive gun). */
static int on_bee_gun(void)
{
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;
    uint32_t base = MEM32(0x0084A5F8u), i;

    if (!chr || !base)
        return 0;
    for (i = 0; i < 0x81u; i++) {
        uint32_t c = base + i * 0x6D0u, o;
        if (!MEM32(c + 0x64u) || !(o = MEM32(c + 0x570u)))
            continue;
        if (MEM32(o) == 0x006416B8u && MEM32(o + 0x1F0u) == chr)
            return 1;
    }
    return 0;
}

/* Multiplayer: the first player (the camera's character; the keyboard's
 * only on slot 1) sits in a Mule 52's gunner seat: +0x2A0 of the Mule's
 * +0x570 object (vtable 0x0063F2A8), seat type +0x2AC 2 (a player). The
 * gun aims with the left stick, forward = down (user 2026-10-28; seen in
 * the air with slot 2 flying). */
static int on_mule_gun(void)
{
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;
    uint32_t base = MEM32(0x0084A5F8u), i;

    if (!chr || !base || controls_keyboard_port() != 0)
        return 0;
    for (i = 0; i < 0x81u; i++) {
        uint32_t c = base + i * 0x6D0u, o;
        if (!MEM32(c + 0x64u) || !(o = MEM32(c + 0x570u)))
            continue;
        if (MEM32(o) == s_mpv[MPV_MULE].vtable && MEM32(o + 0x2A0u) == chr
            && MEM8(o + 0x2ACu) == 2u)
            return 1;
    }
    return 0;
}

/* The Vault (Heist, level 0x40) starts on foot; the final boss is fought
 * in a suit: Conker's model kind (+0x68) is 0x96 on foot, 0x82 in the suit
 * (conker-notes 2026-10-28, Vault suit). */
static int in_vault_suit(void)
{
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;

    return chr && MEM32(chr + 0x68u) == 0x82u;
}

/* Holding the alien's tail (enviro_holdstail, then the spin up to the
 * throw): Conker's +0x250 is the alien's character index + 1, 0 the rest
 * of the fight (conker-notes 2026-10-28, Vault tail). */
static int holding_tail(void)
{
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;

    return chr && MEM32(chr + 0x250u) != 0;
}

/* Count Batula (level 0x3C): Conker is the bat, model kind +0x68 0x9A.
 * Flying, the bat's movement code 0x002BC180 sets +0x1B5 to 10 (nothing
 * else does); walking it sets 0 (conker-notes 2026-10-28, bat keys). */
#define BAT_MODEL 0x9Au

static int story_bat(int set)
{
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;

    if (set != CONTROLS_STORY || !chr)
        return set;
    if (MEM8(chr + 0x1B5u) == 10u)
        return CONTROLS_BAT_AIR;
    if (MEM32(0x007FA1F8u) == 0x3Cu && MEM32(chr + 0x68u) == BAT_MODEL)
        return CONTROLS_BAT_GROUND;
    return set;
}

/* Single player's Modern scheme (SPEC 10.1, user 2026-10-25): the context
 * key, the aim key and Attack press B and the right trigger by the game's
 * own state (conker-notes 2026-10-28, Modern scheme). B is the context
 * button: 0x002F48A0 picks what it does from Conker's aim object, the
 * zone he stands in (+0x97, low 7 bits; a B pad such as Poo Cabin's
 * trapdoor) or else the level's weapon (+0xAC), and 0x002FD7D0 does it;
 * what it is doing now is +0xA0 (0 = nothing; the game puts a weapon away
 * itself when Conker walks onto a pad). The kinds that draw a weapon are
 * the cases there that run 0x0031A7A0, the weapon's own code and
 * 0x00301410 (camera to aiming): bat 0x75, shotgun 0x37, SMG 0x09. */
static int weapon_kind(uint32_t k)
{
    switch (k) {
    case 0x02: case 0x03: case 0x09: case 0x0A: case 0x0C: case 0x13:
    case 0x16: case 0x34: case 0x37: case 0x3B: case 0x75: case 0x76:
    case 0x77:
        return 1;
    }
    return 0;
}

#define KIND_SHOTGUN   0x37u
#define KIND_SLING     0x02u /* a slingshot pad (0x13 too: both 0x0032B260) */
#define KIND_SLING2    0x13u
#define LEVEL_VERTEX   0x36u /* Heist, "Enter the Vertex" */
#define MODERN_B_MS    100   /* one press of B, about three frames */
#define MODERN_WAIT_MS 700   /* then the game's answer, before another */
#define MODERN_FIRE_MS 150   /* the shotgun fires on letting go of RT */

/* Modern on the controller too (user 2026-10-30), player 1 in single
 * player only: on foot B is the context button (B pads only), holding LT
 * aims (the weapon out while held, as the keyboard's aim key), RT attacks
 * while aiming and crouches otherwise, LB is the head-cam (the game's LT,
 * held); the bat flying aims while LT is held; underwater RT also swims up
 * (B) and LT down (A). The toolkit's pad filter (xbox_InputSetPadFilter)
 * runs on the USB thread on the controller's own state, before the
 * keyboard is merged in: it keeps the pad's B, LT and RT for the polls
 * below (game thread, once a frame) and puts their answer in their place.
 * The swimming, beehive gun, The Vault, vehicles, cutscenes and menus keep
 * the controller as the game has it. */
#define PAD_NATIVE 0
#define PAD_FOOT   1     /* on foot, modern_poll */
#define PAD_UNDER  2     /* underwater */
#define PAD_BAT    3     /* the bat flying, bat_poll */

static volatile LONG s_pad_mode;
static volatile LONG s_pad_in_b, s_pad_in_lt, s_pad_in_rt;   /* pad, port 1 */
static volatile LONG s_pad_out_b, s_pad_out_rt, s_pad_out_lt;

static int pad_filter(DWORD port, XBOX_INPUT_STATE *st)
{
    BYTE *a = st->Gamepad.bAnalogButtons;
    int lt, rt;

    if (port != 0)
        return 0;
    lt = a[XBOX_BUTTON_LTRIGGER] > XBOX_ANALOG_BUTTON_THRESHOLD;
    rt = a[XBOX_BUTTON_RTRIGGER] > XBOX_ANALOG_BUTTON_THRESHOLD;
    InterlockedExchange(&s_pad_in_b,
                        a[XBOX_BUTTON_B] > XBOX_ANALOG_BUTTON_THRESHOLD);
    InterlockedExchange(&s_pad_in_lt, lt);
    InterlockedExchange(&s_pad_in_rt, rt);
    switch (s_pad_mode) {
    case PAD_FOOT:
        a[XBOX_BUTTON_B] = s_pad_out_b ? 255 : 0;
        /* Aiming, RT is modern_poll's (attack, the shotgun's laser);
         * otherwise the pad's own (crouch), or held while the shotgun goes
         * away. */
        if (lt)
            a[XBOX_BUTTON_RTRIGGER] = s_pad_out_rt ? 255 : 0;
        else if (s_pad_out_rt)
            a[XBOX_BUTTON_RTRIGGER] = 255;
        /* LB (the Xbox pad's Black) is the head-cam, the game's LT. */
        a[XBOX_BUTTON_LTRIGGER] = a[XBOX_BUTTON_BLACK];
        a[XBOX_BUTTON_BLACK] = 0;
        break;
    case PAD_UNDER:
        if (rt)
            a[XBOX_BUTTON_B] = 255;
        if (lt)
            a[XBOX_BUTTON_A] = 255;
        a[XBOX_BUTTON_LTRIGGER] = a[XBOX_BUTTON_RTRIGGER] = 0;
        break;
    case PAD_BAT:
        a[XBOX_BUTTON_LTRIGGER] = s_pad_out_lt ? 255 : 0;
        break;
    }
    return 0;
}

/* RECOMP_MODERN_LOG=1: a [MODERN] line whenever what modern_poll reads
 * changes (zone, level weapon, what B is doing, the inputs). */
static int modern_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MODERN_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

static void modern_log(long now, uint32_t aim, int ctx, int aiming, int att,
                       int owned, int foot, int mpad, int mjump, int b, int rt)
{
    static uint32_t was[8];
    uint32_t v[8];

    v[0] = MEM8(aim + 0x97u);
    v[1] = MEM32(aim + 0xACu);
    v[2] = MEM32(aim + 0xA0u);
    v[3] = MEM32(aim + 0x1F8u);
    v[4] = (uint32_t)(ctx | aiming << 1 | att << 2);
    v[5] = (uint32_t)(owned | foot << 1 | mpad << 2 | mjump << 3);
    v[6] = (uint32_t)(b | rt << 1);
    v[7] = MEM32(0x008470DCu) ? MEM8(MEM32(0x008470DCu)) : 0;
    if (!memcmp(v, was, sizeof v))
        return;
    memcpy(was, v, sizeof v);
    fprintf(stderr, "[MODERN] t=%ld zone 0x%02X weapon 0x%02X B-doing 0x%02X "
            "+1F8 %u cam0 %u | ctx %d aim %d att %d | owned %d foot %d "
            "vertex pad %d jump %d | out B %d RT %d\n", now, v[0], v[1], v[2],
            v[3], v[7], ctx, aiming, att, owned, foot, mpad, mjump, b, rt);
}

/* Returns 1 while the controller's Modern layout on foot applies. */
static int modern_poll(int set)
{
    static int on_was, att_was, rt_was, owned;
    static long b_until, wait_until, fire_until;
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;
    uint32_t aim = chr ? MEM32(chr + 0x568u) : 0;
    uint32_t act, cur;
    int ctx_k, aim_k, att_k, ctx, aiming, att, out, pad, foot;
    int vertex, mpad, mjump, sling;
    int b = 0, rt = 0;
    long now = attach_ms();

    if (!controls_modern(&ctx_k, &aim_k, &att_k) || set != CONTROLS_STORY
        || !aim) {
        if (on_was) {
            xbox_KbmForce(XBOX_KBM_B, 0);
            xbox_KbmForce(XBOX_KBM_RT, 0);
        }
        /* Photo mode and the pause menu keep whose the weapon is: the aim
         * key let go there puts it away on return (user 2026-10-30). */
        if (set != CONTROLS_PHOTO && set != CONTROLS_PAUSE)
            owned = 0;
        on_was = att_was = rt_was = 0;
        b_until = wait_until = fire_until = 0;
        /* Leaving photo mode stops the shotgun's laser while the game still
         * has RT held, and until RT is let go B does not put it away: aimed
         * on return, RT is let go for a moment (the laser is off, so no
         * shot) and the laser comes back. */
        if (set == CONTROLS_PHOTO)
            fire_until = now + MODERN_FIRE_MS;
        InterlockedExchange(&s_pad_out_b, 0);
        InterlockedExchange(&s_pad_out_rt, 0);
        return 0;
    }
    on_was = 1;
    ctx = ctx_k > 0 && xbox_KbmHeld((unsigned)ctx_k);
    aiming = aim_k > 0 && xbox_KbmHeld((unsigned)aim_k);
    att = att_k > 0 && xbox_KbmHeld((unsigned)att_k);
    act = MEM8(aim + 0x97u) & 0x7Fu;
    if (!act)
        act = MEM32(aim + 0xACu);
    cur = MEM32(aim + 0xA0u) & ~0x80u;
    out = weapon_kind(cur);
    pad = act && !weapon_kind(act);
    foot = !driven_vehicle(chr) && !MEM8(chr + 0x1B5u);
    /* On foot the controller's B, LT and RT are the context key, the aim
     * key and Attack (pad_filter); elsewhere the game has them. */
    if (foot) {
        ctx |= (int)s_pad_in_b;
        aiming |= (int)s_pad_in_lt;
        att |= (int)s_pad_in_rt;
    }
    /* Enter the Vertex's slow-motion jumps (conker-notes 2026-10-30,
     * Vertex): there the SMG's code (0x00329610) takes B with the gun out
     * as the jump when Conker stands on a pad (character +0x534 = 1; 2 off
     * them), and 0x0084A680 is set while the jump lasts. */
    vertex = MEM32(0x007FA1F8u) == LEVEL_VERTEX;
    mpad = vertex && MEM32(chr + 0x534u) == 1u;
    mjump = vertex && MEM32(0x0084A680u) != 0;
    /* Slingshot Aim = Toggle (story_slingshot_aim; user 2026-10-30): on a
     * slingshot pad the context key turns the slingshot on and off, as
     * Classic's B, and the aim key does nothing there. */
    sling = (act == KIND_SLING || act == KIND_SLING2)
            && controls_sling_toggle();
    /* A weapon out is the aim input's once it is held with the weapon out
     * (or took it out). Only that one is put away when it is let go, not
     * one the game or Classic's B took out (user 2026-10-30). In the
     * Vertex every weapon out is: the game hands Conker the gun aimed when
     * the level starts and after each jump (user 2026-10-30). */
    if (!out)
        owned = 0;
    else if (aiming || vertex)
        owned = 1;

    if (!foot) {
        /* Not on foot (It's War's tank; the bat has sets of its own): no weapon to
         * draw; the context key is B and Attack the right trigger. */
        b = ctx;
        rt = att;
        b_until = wait_until = fire_until = 0;
    } else if (now < b_until) {
        b = 1;
    } else if (mjump) {
        /* Nothing presses B during a slow-motion jump. */
    } else if (mpad && out) {
        /* A slow-motion pad with the gun out: the context key is the jump;
         * letting go of the aim key puts nothing away (that B would jump). */
        b = ctx;
    } else if (pad || sling) {
        /* A B pad: the context key, nothing else (the game has put a
         * weapon away by itself). A slingshot pad toggled the same way. */
        b = ctx;
    } else if (aiming != out && (aiming || owned) && now >= wait_until) {
        /* Take the weapon out or put it away: one press of B, then wait
         * for the game's answer in +0xA0. */
        b = 1;
        b_until = now + MODERN_B_MS;
        wait_until = now + MODERN_WAIT_MS;
        fprintf(stderr, "[CONTROLS] t=%ld modern: B to %s (kind 0x%02X, "
                "now 0x%02X)\n", now, aiming ? "take out" : "put away",
                act, cur);
    }
    if (foot && out && (aiming || mjump || sling)) {
        /* In a slow-motion jump, and with the slingshot toggled on, Attack
         * fires without the aim key. */
        if (cur == KIND_SHOTGUN) {
            /* Aiming holds RT (the laser); Attack lets go for a moment,
             * which fires; the laser comes back while aiming. */
            if (att && !att_was)
                fire_until = now + MODERN_FIRE_MS;
            rt = now >= fire_until;
        } else {
            rt = att;
        }
    } else if (foot && out && cur == KIND_SHOTGUN && rt_was) {
        /* Putting the shotgun away: RT stays held until it is away, or
         * letting go would fire it. */
        rt = 1;
    }
    att_was = att;
    rt_was = rt;
    if (modern_log_on())
        modern_log(now, aim, ctx, aiming, att, owned, foot, mpad, mjump, b,
                   rt);
    xbox_KbmForce(XBOX_KBM_B, b);
    xbox_KbmForce(XBOX_KBM_RT, rt);
    InterlockedExchange(&s_pad_out_b, b);
    InterlockedExchange(&s_pad_out_rt, rt);
    return foot;
}

/* Multiplayer zoom held (mp_zoom_mode = hold; user 2026-10-28): zoomed in
 * while the zoom key is held, on foot. The right stick click toggles the
 * zoom in 0x00038710 (controller +0x8C1); the Long Ranger's scope is its
 * own (camera +0x5D0 = 0, aim +0x2B0 = 2) and goes in, 2x, out, one step a
 * click. Zoomed as 0x0004A930 has it: +0x8C1 = 1 or aim +0x2B0 set
 * (conker-notes 2026-10-28, zoom). Only a zoom this made is taken out, so
 * a controller beside the keyboard keeps its own. The scope stays a toggle
 * in Hold too (user 2026-10-28: its 2x needs a second press): once it is
 * up, each press is one click and letting go does nothing. */
#define ZOOM_TAP_MS  70    /* one click of the right stick (two frames) */
#define ZOOM_IN_MS   400   /* after zooming in, the game's answer */
#define ZOOM_OUT_MS  170   /* after a click out, before another */

/* Is chr in any seat of a multiplayer vehicle (the R-Hog's and the Mule's
 * gunner seat included, unlike mp_vehicle)? Seats are 0xB0 apart from
 * +0x1F0 with a type byte at +0xC (2 = a player). */
static int in_mp_seat(uint32_t chr)
{
    uint32_t base = MEM32(0x0084A5F8u), i, s;

    if (!base || !chr)
        return 0;
    for (i = 0; i < 0x81u; i++) {
        uint32_t c = base + i * 0x6D0u, o;
        if (!MEM32(c + 0x64u))
            continue;
        o = MEM32(c + 0x570u);
        if (!xact_va_ok(o) || MEM32(o + 4u) != c)
            continue;
        for (s = 0x1F0u; s <= 0x350u; s += 0xB0u)
            if (MEM32(o + s) == chr && MEM8(o + s + 0xCu) == 2u)
                return 1;
    }
    return 0;
}

static void zoom_poll(int set)
{
    static int on_was, held_was, want_in, owned, taps;
    static long tap_until, wait_until;
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t ctl = cam ? MEM32(cam + 4u) : 0;
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;
    uint32_t aim = chr ? MEM32(chr + 0x568u) : 0;
    int key, held, zoomed, scoped, click = 0;
    long now = attach_ms();

    if (!controls_zoom_hold(&key) || set != CONTROLS_MP || !ctl || !aim
        || controls_keyboard_port() != 0) {
        if (on_was)
            xbox_KbmForce(XBOX_KBM_RTHUMB, 0);
        on_was = held_was = want_in = owned = taps = 0;
        tap_until = wait_until = 0;
        return;
    }
    on_was = 1;
    held = key > 0 && xbox_KbmHeld((unsigned)key);
    scoped = MEM32(aim + 0x2B0u) != 0;
    zoomed = MEM8(ctl + 0x8C1u) == 1u || scoped;

    if (in_mp_seat(chr)) {
        /* A vehicle: the key is the right stick click, as the controller. */
        xbox_KbmForce(XBOX_KBM_RTHUMB, held);
        held_was = held;
        want_in = owned = taps = 0;
        tap_until = wait_until = 0;
        return;
    }
    if (held && !held_was)
        want_in = 1;
    if (!held)
        want_in = 0;
    held_was = held;
    if (now < tap_until) {
        click = 1;
    } else if (now >= wait_until) {
        if (!zoomed || scoped)
            owned = 0;
        if (want_in && (!zoomed || scoped)) {
            /* In; or the scope's next step (2x, out), as Toggle. */
            click = 1;
            owned = !scoped;
            taps = 0;
        } else if (!held && owned && zoomed && taps < 2) {
            click = 1;
            taps++;
        }
        want_in = 0;
        if (click) {
            tap_until = now + ZOOM_TAP_MS;
            wait_until = now + (held ? ZOOM_IN_MS : ZOOM_OUT_MS);
            fprintf(stderr, "[CONTROLS] t=%ld zoom held: click %s "
                    "(+8C1 %u, aim +2B0 %u)\n", now,
                    scoped ? "(scope, as Toggle)" : held ? "to zoom in"
                    : "to zoom out", MEM8(ctl + 0x8C1u), MEM32(aim + 0x2B0u));
        }
    }
    xbox_KbmForce(XBOX_KBM_RTHUMB, click);
}

/* The bat's aiming view (user 2026-10-28): 0x002BC180 toggles 0x0084A5B0
 * on each press of the left trigger, and while the trigger is held (the
 * button table's +0x64 at 0x00763268, per player) it stops the bat. So the
 * aim key taps the trigger: Classic once per press of the key, Modern
 * whenever the key held and the aim flag differ, then waits for the game's
 * answer. Only while the flight code would take the press: the flying
 * camera's byte (Conker's +0x564 -> +0) set and nothing held (+0x250). */
#define BAT_TAP_MS  100   /* one press of the left trigger, about three frames */
#define BAT_WAIT_MS 300   /* then the game's answer, before another */

static void bat_poll(int set)
{
    static int on_was, held_was, want, owned;
    static long tap_until, wait_until;
    uint32_t cam = MEM32(0x008470DCu);
    uint32_t chr = cam ? MEM32(cam + 0x3E0u) : 0;
    uint32_t fc = chr ? MEM32(chr + 0x564u) : 0;
    int key, modern, held, aiming, ready, lt = 0;
    long now = attach_ms();

    modern = controls_bat_aim(&key);
    /* Modern: the controller's LT held aims too (pad_filter). */
    if (set != CONTROLS_BAT_AIR || (key <= 0 && !modern) || !chr) {
        if (on_was)
            xbox_KbmForce(XBOX_KBM_LT, 0);
        if (set != CONTROLS_PHOTO && set != CONTROLS_PAUSE)
            owned = 0;   /* as modern_poll's weapon */
        on_was = held_was = want = 0;
        tap_until = wait_until = 0;
        InterlockedExchange(&s_pad_out_lt, 0);
        return;
    }
    on_was = 1;
    held = key > 0 && xbox_KbmHeld((unsigned)key);
    if (modern)
        held |= (int)s_pad_in_lt;
    aiming = MEM8(0x0084A5B0u) != 0;
    ready = fc && MEM8(fc) && !MEM8(chr + 0x250u);
    if (held && !held_was)
        want = 1;
    held_was = held;
    /* Modern stops only an aim the key holds (as modern_poll's weapon): one
     * the controller's left trigger turned on stays on, as in Classic. */
    if (!aiming)
        owned = 0;
    else if (held)
        owned = 1;
    if (now < tap_until) {
        lt = 1;
    } else if (now >= wait_until && ready
               && (modern ? held != aiming && (held || owned) : want)) {
        lt = 1;
        want = 0;
        tap_until = now + BAT_TAP_MS;
        wait_until = now + BAT_WAIT_MS;
        fprintf(stderr, "[CONTROLS] t=%ld bat: left trigger to %s\n", now,
                aiming ? "stop aiming" : "aim");
    }
    xbox_KbmForce(XBOX_KBM_LT, lt);
    InterlockedExchange(&s_pad_out_lt, lt);
}

static void controls_poll(void)
{
    static const char *const names[] = {
        "single player", "multiplayer", "menu", "single player pause",
        "level menu", "cutscene", "single player swimming",
        "single player underwater", "single player beehive gun",
        "multiplayer Mule 52 gun", "single player Vault suit",
        "single player Vault tail", "single player bat",
        "single player bat flying", "photo mode",
    };
    static int last = -1;
    uint32_t lv = MEM32(0x007FA1F8u);
    int mp, set;

    s_ctl_frame++;
    mp = lv == 0x46u || lv > 0x4Au || MEM32(0x0069CF90u + lv * 4u) == 0;
    if (lv == 0x1Du || lv == 0x46u)
        set = CONTROLS_MENU;
    else if (s_ctl_list_frame && s_ctl_frame - s_ctl_list_frame <= 2)
        set = CONTROLS_MENU_GAME;
    else if (!mp && MEM8(0x007FA275u))
        set = CONTROLS_PAUSE;
    else if (!mp && MEM8(0x0076F0E0u) == 1)
        set = CONTROLS_CUTSCENE;
    else if (mp)
        set = CONTROLS_MP;
    else
        set = story_bat(story_water());
    if (lv == 0x27u && set == CONTROLS_STORY && on_bee_gun())
        set = CONTROLS_BEE_GUN;
    if (lv == 0x40u && set == CONTROLS_STORY && in_vault_suit())
        set = holding_tail() ? CONTROLS_VAULT_TAIL : CONTROLS_VAULT_SUIT;
    if (set == CONTROLS_MP && on_mule_gun())
        set = CONTROLS_MULE_GUN;
    /* Photo mode reads the keys and the mouse itself (photo_mode.c), and
     * the forced outputs below let go (none is for its set). */
    if (photo_keys_wanted())
        set = CONTROLS_PHOTO;
    /* Holding the tail the right stick turns the camera and Conker follows
     * (the aim is not the source there): the mouse stays the stick. */
    if (set != CONTROLS_PHOTO)
        mouse_look(set == CONTROLS_MULE_GUN ? CONTROLS_MP
                   : set == CONTROLS_VAULT_SUIT || set == CONTROLS_BAT_GROUND
                   ? CONTROLS_STORY : set);
    {
        static int hooked;
        int k, modern = controls_modern(&k, &k, &k);
        int foot = modern_poll(set);
        LONG mode = !modern ? PAD_NATIVE
                  : foot ? PAD_FOOT
                  : set == CONTROLS_SWIM_UNDER ? PAD_UNDER
                  : set == CONTROLS_BAT_AIR ? PAD_BAT : PAD_NATIVE;
        if (!hooked) {
            hooked = 1;
            xbox_InputSetPadFilter(pad_filter);
        }
        zoom_poll(set);
        bat_poll(set);
        if (InterlockedExchange(&s_pad_mode, mode) != mode)
            fprintf(stderr, "[CONTROLS] t=%ld controller layout: %s\n",
                    attach_ms(), mode == PAD_FOOT ? "modern, on foot"
                    : mode == PAD_UNDER ? "modern, underwater"
                    : mode == PAD_BAT ? "modern, bat flying" : "as the game has it");
    }
    if (set == last)
        return;
    last = set;
    controls_set_mode(set);
    fprintf(stderr, "[CONTROLS] t=%ld level 0x%02X: %s keys\n", attach_ms(),
            lv, names[set]);
}

void sub_002A7B60_enter(void)
{
    static long t0 = -1;
    static unsigned frames, missed[4], nlog;
    static uint32_t vb0;
    static double step, dt;
    long now;
    uint32_t m, vb, want;

    soak_poll();
    vsh_scan_poll();
    controls_poll();
    photo_frame_begin();
    if (g_asked_interval) {
        want = frame_interval(g_asked_interval);
        if (MEM8(0x006C0268u) != want) {
            MEM8(0x006C0268u) = (uint8_t)want;
            MEM32(0x00549BF4u) = (MEM32(0x00549BF4u) & 0x80000000u) | want;
            fprintf(stderr, "[FSTEP] frame rate: interval %u (%s fps)\n", want,
                    want == 1u ? "60" : want == 2u ? "30" : "other");
        }
    }
    if (!framestep_log_on())
        return;
    now = attach_ms();
    vb = MEM32(MEM32(0x005499E8u) + 0x1DE8u);   /* D3D's vblank count */
    if (t0 < 0) {
        t0 = now;
        vb0 = vb;
    }
    m = MEM32(0x007F9F30u);
    missed[m < 3 ? m : 3]++;
    if (framestep_log_on() >= 2 && now >= framestep_log_on() * 1000L
        && nlog++ < 3000)
        fprintf(stderr, "[FRAME] t=%ld frame %u missed %u now %u\n", now,
                MEM32(0x007F9F60u), m, vb);
    frames++;
    step += MEMF(0x007FA24Cu);
    dt += MEMF(0x007FA21Cu);
    if (now - t0 < 1000)
        return;
    fprintf(stderr, "[FSTEP] t=%ld %u frames (%.1f/s), interval %u, missed 0:%u 1:%u"
            " 2:%u 3+:%u, step %.3f, dt %.1f ms, vblanks %.1f/s, game time per wall"
            " second: step %.2f s, dt %.2f s\n", now, frames,
            frames * 1000.0 / (now - t0), MEM8(0x006C0268u), missed[0], missed[1],
            missed[2], missed[3],
            step / frames, dt * 1000.0 / frames, (vb - vb0) * 1000.0 / (now - t0),
            step / 30.0 * 1000.0 / (now - t0), dt * 1000.0 / (now - t0));
    t0 = now;
    vb0 = vb;
    frames = 0;
    memset(missed, 0, sizeof missed);
    step = dt = 0.0;
}

/* Photo mode (photo_mode.c): the in-game frame's draw (0x002A3FD0) gets
 * the photo camera; 0x002AF250 (game_frame, after the swap, just before the
 * pause flags are compared) gets the pause flag back. */
void sub_002A3FD0_enter(void)
{
    photo_render_begin();
}

void sub_002AF250_enter(void)
{
    photo_frame_end();
}

/* 0x00204060 (frontend_draw.cpp, "UIPause"; reached through the draw's
 * per-state table at 0x0059B448): the single player pause screen, drawn
 * whenever the pause flag is set. It copies the frame into a texture and
 * draws it greyed inside a picture frame with the menu. Photo mode's frozen
 * frames leave it out. */
extern void sub_00204060_gen(void);
void sub_00204060(void)
{
    if (photo_frozen()) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_00204060_gen();
}

/* Photo mode's HUD (photo_mode.c), hidden by default: 0x002A5DB0 draws
 * the HUD 0x00317780 only while the pause flag is 0, so a shown HUD gets
 * the flag clear for this call; the draw's other HUD call follows
 * 0x002A60B0 under the same test. */
extern void sub_002A5DB0_gen(void);
void sub_002A5DB0(void)
{
    if (photo_hud_shown() && MEM8(0x007FA275u)) {
        MEM8(0x007FA275u) = 0;
        sub_002A5DB0_gen();
        MEM8(0x007FA275u) = 1;
        return;
    }
    sub_002A5DB0_gen();
}

extern void sub_002A60B0_gen(void);
void sub_002A60B0(void)
{
    sub_002A60B0_gen();
    photo_hud_draw();
}

/* char_render_pass (0x0007FB00, cdecl, [esp+4] = character index): photo
 * mode's "hide Conker" leaves his every pass out. */
extern void sub_0007FB00_gen(void);
void sub_0007FB00(void)
{
    if (photo_hides_char(MEM32(g_esp + 4u))) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_0007FB00_gen();
}

/* 0x000780E0 (cdecl, [esp+4] = attachment record, byte +0 its character's
 * id): draws one attachment. The world draw 0x002A5AA0 draws them all in
 * a loop of its own, Conker's tail among them (type "ConkerTail"). */
extern void sub_000780E0_gen(void);
void sub_000780E0(void)
{
    uint32_t rec = MEM32(g_esp + 4u);
    if (rec && photo_hides_char_id(MEM8(rec))) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_000780E0_gen();
}

/* 0x0015CAF0 (cdecl): the draw of object types 0x7A (layer 3, drawn by
 * 0x000D9950 every frame, paused too) and 0x72: the weapon sight, e.g. the
 * SMG's green cross in Chemical Warfare. Part of photo mode's HUD. */
/* 0x001BCBC0 (cdecl): the draw of object type 0x78 (layer 3), the screen
 * haze some levels have: Chemical Warfare's green gas, the haunted house's
 * mist, a light haze elsewhere (9 of the soak's maps, conker-notes
 * 2026-10-29). RECOMP_SCREEN_HAZE=0 (or screen_haze = 0) leaves it out;
 * conker_SetScreenHaze from the F2 menu. */
static volatile int g_screen_haze = -1;

void conker_SetScreenHaze(int on)
{
    g_screen_haze = on != 0;
}

extern void sub_001BCBC0_gen(void);
void sub_001BCBC0(void)
{
    if (g_screen_haze < 0) {
        const char *e = getenv("RECOMP_SCREEN_HAZE");
        g_screen_haze = !(e && *e == '0');
    }
    if (!g_screen_haze) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_001BCBC0_gen();
}

/* Speech bubbles (cutscenes and in play): object type 0x7C of the draw
 * layers (layer 3), made by 0x000E0850 with the "speechbubble" textures.
 * Left out with the HUD in photo mode, like the weapon sight (user
 * 2026-10-29: they stayed on screen while the camera flew). */
extern void sub_000E1880_gen(void);
void sub_000E1880(void)
{
    if (photo_hud_hidden()) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_000E1880_gen();
}

extern void sub_0015CAF0_gen(void);
void sub_0015CAF0(void)
{
    if (photo_hud_hidden()) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_0015CAF0_gen();
}

/* 0x00205490 (frontend_draw.cpp): the front end's menu draw, front-end
 * state 3 of the table at 0x0059B400 ([0x00776574]; called by 0x00201E20
 * from 0x002A60B0). It draws the menu's UI scene: every main menu item
 * (Game 1-3, Multiplayer, Options, Chapters) and the pages they open (the
 * chapter list and picture frame, a game slot's frame and counts, the
 * options with the controller). Left out with the HUD in photo mode (user
 * 2026-10-29: the text stayed on screen while the camera flew). */
extern void sub_00205490_gen(void);
void sub_00205490(void)
{
    if (photo_hud_hidden()) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_00205490_gen();
}

void sub_002A78D0_enter(void)
{
    uint32_t want;
    g_asked_interval = g_edx & 0xFFu;
    want = frame_interval(g_asked_interval);
    fprintf(stderr, "[FSTEP] video reset: interval %u from %08X%s\n", g_edx,
            MEM32(g_esp), want != g_asked_interval ? " (60 fps: 1)" : "");
    g_edx = (g_edx & ~0xFFu) | want;
}

/* 0x002A8020: Conker's D3D swap callback (D3DSWAPDATA *: +0 Swap, +4
 * SwapVBlank, +8 MissedVBlanks, +0x0C TimeUntilSwapVBlank, +0x10
 * TimeBetweenSwapVBlanks); stores MissedVBlanks in [0x007F9F30]. The dip
 * fix (above) corrects MissedVBlanks here.
 * RECOMP_FRAMESTEP_LOG=<s> (2 or more): from <s> seconds after the start,
 * one [SWAP] line per swap and one [FRAME] line per frame_timer_update (at
 * most 3000 each) with D3D's vblank count and last due vblank (miniport
 * +0x1C0, +0x1C8); "fixed" when the dip fix took one off. */
void sub_002A8020_enter(void)
{
    static unsigned n;
    uint32_t d = MEM32(g_esp + 4), mp = MEM32(0x005499E8u) + 0x1C28u;
    uint32_t missed = MEM32(d + 8);
    int fixed = 0;

    if (missed && MEM32(d + 4) == MEM32(mp + 0x1C0u) && dip_fix_on()) {
        MEM32(d + 8) = missed - 1u;
        fixed = 1;
    }
    if (framestep_log_on() < 2 || attach_ms() < framestep_log_on() * 1000L
        || n >= 3000)
        return;
    n++;
    fprintf(stderr, "[SWAP] t=%ld swap %u due %u missed %u%s until %u between %u |"
            " now %u last due %u frame %u\n", attach_ms(), MEM32(d), MEM32(d + 4),
            missed, fixed ? " (fixed)" : "", MEM32(d + 12), MEM32(d + 16),
            MEM32(mp + 0x1C0u), MEM32(mp + 0x1C8u), MEM32(0x007F9F60u));
}

/* ── Conker: vertex program variants (RECOMP_VSHVAR_LOG=1) ───────────────
 *
 * Conker splices each vertex program at runtime (conker-notes 2026-10-23):
 * 0x003EC270 vsh_variant_create (esi = the material record from
 * vsh_material_register, ebx = the light key [0x0081B7C0], args lighting
 * mode 0-2 and fog 0/1) lists the material's base shader (+0) and the
 * fragments 0x003EC0A0 vsh_pick_fragments picks (edi = list, cdecl args
 * material, key, mode bits: 2 = mode 2, 4 = fog), then calls
 * XGSpliceVertexShaders 0x00529060 (eax = output, stdcall &size, 0, list,
 * count, optimise [0x0084B7FC]) and D3DDevice_CreateVertexShader. The
 * alternate picker ([0x0084B800]) and the optimise flag are never written
 * in the XBE, so both are 0.
 *
 * Shader pre-compilation step 1 (SPEC §10.1 plan A): the hook does the same
 * pick and splice itself before the game does, and prints one [VSHVAR] line
 * per variant made: the material (FNV-1a of its base shader), its flags,
 * the key, mode and fog, the fragments, and the spliced program's hash the
 * way the toolkit's decoder hashes a loaded program (dwords 1-3 of each
 * instruction up to the final one). That hash should match a "[VSHGPU]
 * program <hash>" line if the D3D runtime loads the splice unchanged. */
extern void sub_003EC0A0(void);
extern void sub_00529060(void);

static int vshvar_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("RECOMP_VSHVAR_LOG");
        on = s && atoi(s) > 0;
    }
    return on;
}

/* FNV-1a over a guest Xbox shader binary: header dword, then count x 16
 * bytes (count = the word at +2). */
static uint64_t vshvar_shader_hash(uint32_t sh, uint32_t *count)
{
    uint64_t h = 14695981039346656037ull;
    uint32_t n = MEM16(sh + 2), i;

    for (i = 0; i < n * 4u + 1u; i++)
        h = (h ^ MEM32(sh + 4u * i)) * 1099511628211ull;
    *count = n;
    return h;
}

void sub_003EC270_enter(void)
{
    static unsigned n;
    uint32_t mat = g_esi, key = g_ebx, mode = MEM32(g_esp + 4);
    uint32_t fog = MEM32(g_esp + 8), base, flags, bits, cnt, i, k, nins;
    uint32_t r_eax, r_ecx, r_edx, r_ebx, r_esi, r_edi, r_esp, r_ebp, r_seh;
    uint32_t scratch, size_at, list, out;
    uint64_t mat_hash, h = 14695981039346656037ull;
    char frags[160];
    int len = 0, final = 0;

    if (!vshvar_log_on())
        return;
    base = MEM32(mat);
    flags = MEM32(mat + 0xDC);
    mat_hash = vshvar_shader_hash(base, &nins);
    if (MEM32(0x0084B800u)) {
        fprintf(stderr, "[VSHVAR] alternate picker set (%08X): not checked\n",
                MEM32(0x0084B800u));
        return;
    }

    r_eax = g_eax; r_ecx = g_ecx; r_edx = g_edx; r_ebx = g_ebx;
    r_esi = g_esi; r_edi = g_edi; r_esp = g_esp;
    r_ebp = g_ebp; r_seh = g_seh_ebp;

    /* As the game lays it out: size, list[10], the 0x884-byte output, all
     * below the caller's stack and the callees' stack below that. */
    scratch = (g_esp - 0x900u) & ~0xFu;
    size_at = scratch;
    list = scratch + 4;
    out = scratch + 0x2C;
    MEM32(list) = base;
    if (flags & 8) {
        cnt = 1;
    } else {
        bits = (mode == 1 || mode == 2 ? mode : 0) | (fog ? 4u : 0u);
        g_esp = scratch - 0x10;
        g_esp -= 4; MEM32(g_esp) = bits;
        g_esp -= 4; MEM32(g_esp) = key;
        g_esp -= 4; MEM32(g_esp) = mat;
        g_esp -= 4; MEM32(g_esp) = 0;   /* return address */
        g_edi = list + 4;
        sub_003EC0A0();
        cnt = g_eax + 1;
    }
    MEM32(size_at) = 0x884;
    g_esp = scratch - 0x10;
    g_esp -= 4; MEM32(g_esp) = MEM32(0x0084B7FCu);
    g_esp -= 4; MEM32(g_esp) = cnt;
    g_esp -= 4; MEM32(g_esp) = list;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_esp -= 4; MEM32(g_esp) = size_at;
    g_esp -= 4; MEM32(g_esp) = 0;       /* return address, popped by the callee */
    g_eax = out;
    sub_00529060();

    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi; g_esp = r_esp;
    g_ebp = r_ebp; g_seh_ebp = r_seh;

    frags[0] = 0;
    for (i = 1; i < cnt && i < 10; i++)
        len += snprintf(frags + len, sizeof frags - len, "%s%08X",
                        i > 1 ? "," : "", MEM32(list + 4u * i));
    k = MEM16(out + 2);
    for (i = 0; i < k && i < 136; i++) {
        uint32_t ins = out + 4 + 16 * i;
        h = (h ^ MEM32(ins + 4)) * 1099511628211ull;
        h = (h ^ MEM32(ins + 8)) * 1099511628211ull;
        h = (h ^ MEM32(ins + 12)) * 1099511628211ull;
        if (MEM32(ins + 12) & 1) {
            final = 1;
            i++;
            break;
        }
    }
    n++;
    fprintf(stderr, "[VSHVAR] t=%ld #%u level 0x%02X material %016llX (%u ins) "
            "d8 %u dc %02X key %08X mode %u fog %u | %u parts base+%s | "
            "program %016llX %u ins%s (size %u)\n", attach_ms(), n,
            MEM32(0x007FA1F8u), (unsigned long long)mat_hash, nins,
            MEM32(mat + 0xD8), flags, key, mode, fog, cnt,
            cnt > 1 ? frags : "none", (unsigned long long)h, i,
            final ? "" : " NO FINAL", MEM32(size_at));
}

/* The two D3D ways a program reaches the GPU, for the same step: a [VSHSRC]
 * line the first time each program (hashed as above) comes through
 * D3DDevice_CreateVertexShader 0x005366A0 (stdcall decl, function, handle,
 * usage) or D3DDevice_LoadVertexShaderProgram 0x00536120 (function,
 * address), with the caller, so the GPU's compiles can be traced to where
 * and when the game made each program. */
static void vshsrc_log(const char *via, uint32_t fn)
{
    static uint64_t seen[4096];
    static unsigned nseen;
    uint64_t h = 14695981039346656037ull;
    uint32_t k, i, s;

    if (!vshvar_log_on() || !fn)
        return;
    k = MEM16(fn + 2);
    for (i = 0; i < k && i < 136; i++) {
        uint32_t ins = fn + 4 + 16 * i;
        h = (h ^ MEM32(ins + 4)) * 1099511628211ull;
        h = (h ^ MEM32(ins + 8)) * 1099511628211ull;
        h = (h ^ MEM32(ins + 12)) * 1099511628211ull;
        if (MEM32(ins + 12) & 1) {
            i++;
            break;
        }
    }
    for (s = 0; s < nseen; s++)
        if (seen[s] == h)
            return;
    if (nseen < 4096)
        seen[nseen++] = h;
    fprintf(stderr, "[VSHSRC] t=%ld %s caller %08X fn %08X type %04X count %u "
            "level 0x%02X program %016llX %u ins\n", attach_ms(), via,
            MEM32(g_esp), fn, MEM16(fn), k, MEM32(0x007FA1F8u),
            (unsigned long long)h, i);
}

void sub_005366A0_enter(void)
{
    vshsrc_log("create", MEM32(g_esp + 8));
}

void sub_00536120_enter(void)
{
    vshsrc_log("load", MEM32(g_esp + 4));
}

/* D3DDevice_SetVertexShader 0x00536930 (stdcall handle; odd = a shader
 * object + 1, even = FVF). D3DDevice_LoadVertexShader 0x00536090, which it
 * calls when the handle's byte 3 has 0x10, copies the object's ready-made
 * method stream (+0x0C dwords from +0x114: TRANSFORM_PROGRAM headers and
 * instruction words) into the push buffer. A [VSHOBJ] line the first time
 * each object is set, hashed from the instruction words in that stream,
 * with the object and the caller. */
void sub_00536930_enter(void)
{
    static uint32_t seen[8192];             /* open addressing, 0 = empty */
    uint64_t h = 14695981039346656037ull;
    uint32_t handle = MEM32(g_esp + 4), obj, n, i, s, words[4], nw = 0;
    uint32_t nins = 0, va;
    int final = 0;

    if (!vshvar_log_on() || !(handle & 1u))
        return;
    obj = handle - 1u;
    for (s = (obj >> 4) & 8191u; seen[s] && seen[s] != obj; s = (s + 1) & 8191u)
        ;
    if (seen[s])
        return;
    seen[s] = obj;
    n = MEM32(obj + 0x0C);
    va = obj + 0x114;
    for (i = 0; i < n && i < 4096 && !final; ) {
        uint32_t hdr = MEM32(va + 4 * i), cnt = (hdr >> 18) & 0x7FFu;
        uint32_t m = hdr & 0x1FFCu, k;
        i++;
        if ((hdr & 0x00030003u) != 0u)
            break;                          /* not a method header */
        for (k = 0; k < cnt && i < n; k++, i++) {
            if (m < 0x0B00u || m >= 0x0B80u)
                continue;
            words[nw++] = MEM32(va + 4 * i);
            if (nw == 4) {
                h = (h ^ words[1]) * 1099511628211ull;
                h = (h ^ words[2]) * 1099511628211ull;
                h = (h ^ words[3]) * 1099511628211ull;
                nw = 0;
                nins++;
                if (words[3] & 1) {
                    final = 1;
                    break;
                }
            }
        }
    }
    fprintf(stderr, "[VSHOBJ] t=%ld object %08X flags %08X dwords %u level 0x%02X "
            "caller %08X program %016llX %u ins%s\n", attach_ms(), obj,
            MEM32(obj + 4), n, MEM32(0x007FA1F8u), MEM32(g_esp),
            (unsigned long long)h, nins, final ? "" : " NO FINAL");
}

/* D3DDevice_BeginPushBuffer 0x0053B140 (edi = the push buffer object, +4
 * data, +0x10 size): a [VSHPB] line per recording, at most 2000, so the
 * recorded buffers in [VSHORIGIN] can be matched to who recorded them and
 * when. */
void sub_0053B140_enter(void)
{
    static unsigned n;
    if (!vshvar_log_on() || n >= 2000)
        return;
    n++;
    fprintf(stderr, "[VSHPB] t=%ld record #%u buffer %08X data %08X size %u "
            "level 0x%02X caller %08X\n", attach_ms(), n, g_edi,
            MEM32(g_edi + 4), MEM32(g_edi + 0x10), MEM32(0x007FA1F8u),
            MEM32(g_esp));
}

/* ── Conker: no way into Xbox Live (SPEC §9) ─────────────────────────────
 *
 * Every Live sign-in ends in the game handing off to the Xbox Dashboard,
 * which closes the .exe (conker-notes 2026-10-21). Without --debug, the ways
 * in are taken away (conker-notes 2026-10-25):
 * - Back anywhere: 0x001FA7D0 calls 0x0026AB20 (cdecl, pad) when a pad's
 *   Back (button byte +8 bit 0x20) goes down, which opens the Live overlay
 *   (sign-in box, "New Account"); it now returns 0 at once.
 * - The multiplayer "Welcome" menu (state 0, 0x00236030) draws one of the
 *   item tables {text id, handler, 0} at 0x0075E718 (with downloaded
 *   content) or 0x0075E760: Xbox Live (0xC9), System Link (0xC8), Dumbots
 *   (0xC7), Profiles (0x2FD)[, content 0x356]. The first two are cut out of
 *   the tables, so the rest move up; 0x002401F0 (items, &selected, title,
 *   flags, icons[]) gets its icon list moved up to match.
 * - The multiplayer pause menu (0x001F7D00..) fills 0x0087C828 every frame
 *   with Continue, Quit, Xbox Live, Controller[, class, team, restart] and
 *   the cursor [0x00849874] steps over items whose skip byte is set. The
 *   cursor is kept off Xbox Live, and for the draw (0x0020C200 called from
 *   0x001F8DA3) the item is left out of the list.
 * - The main menu's "Xbox Live" / "& Co" (UIFrontend keys LIVE, LIVE2, read
 *   by 0x001F4F40 through the text lookup) become the Menus text
 *   FT_Multiplayer (in the player's language) and a blank line.
 *
 * RECOMP_MENU_LOG=1: [TEXT] each new (key, caller) pair the text lookup
 * 0x003F9CB0 (cdecl: table, wide key, &out; edi = the key hash table;
 * returns the id or -1) resolves, with the text found; [MENU] each new
 * item list given to the menu list builders 0x0020C3B0 (items, count,
 * selected, flags; eax = menu id) and 0x0020C200. */
static int live_hidden(void)
{
    return !settings_debug();
}

static int menu_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MENU_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

/* A guest wide string as ASCII (others as '?'), at most n - 1 chars. */
static const char *guest_wstr(uint32_t va, char *buf, int n)
{
    int i = 0;
    if (va) {
        for (; i < n - 1; i++) {
            uint16_t c = MEM16(va + 2u * (uint32_t)i);
            if (!c)
                break;
            buf[i] = c < 0x80 && c >= 0x20 ? (char)c : '?';
        }
    }
    buf[i] = 0;
    return buf;
}

/* Remembers 64-bit hashes; 1 the first time one is seen. */
static int menu_log_new(uint64_t h)
{
    static uint64_t seen[16384];
    uint32_t s = (uint32_t)h & 16383u;
    if (!h)
        h = 1;
    while (seen[s] && seen[s] != h)
        s = (s + 1) & 16383u;
    if (seen[s])
        return 0;
    seen[s] = h;
    return 1;
}

static uint64_t menu_hash_wstr(uint64_t h, uint32_t va)
{
    uint32_t i;
    for (i = 0; va && i < 256; i++) {
        uint16_t c = MEM16(va + 2u * i);
        if (!c)
            break;
        h = (h ^ c) * 1099511628211ull;
    }
    return h;
}

/* A wide string at va equals the ASCII s. */
static int guest_wstr_is(uint32_t va, const char *s)
{
    uint32_t i;
    for (i = 0; s[i]; i++)
        if (MEM16(va + 2u * i) != (uint8_t)s[i])
            return 0;
    return MEM16(va + 2u * i) == 0;
}

extern void sub_003F9CB0_gen(void);
void sub_003F9CB0(void)
{
    uint32_t ret = MEM32(g_esp), tbl = MEM32(g_esp + 4);
    uint32_t key = MEM32(g_esp + 8), out = MEM32(g_esp + 12);
    char k[72], v[72];

    sub_003F9CB0_gen();
    /* The main menu's two lines. Menus' translated texts are a pointer
     * table at [0x00849800] by key index (0x001FCA30); FT_Multiplayer is
     * index 0x37. The blank line is a lone space (the int 0x20 at
     * 0x00563B70 in .rdata, read as a wide string), as a missing text would
     * draw "ERR: Unable To Find Text Entry". */
    if (ret == 0x001F4F5Du && live_hidden() && (int)g_eax != -1) {
        uint32_t tr = MEM32(0x00849800u), mp = tr ? MEM32(tr + 0x37u * 4) : 0;
        if (guest_wstr_is(key, "LIVE") && mp && MEM16(mp))
            MEM32(out) = mp;
        else if (guest_wstr_is(key, "LIVE2"))
            MEM32(out) = 0x00563B70u;
    }
    if (menu_log_on()) {
        uint64_t h = menu_hash_wstr(14695981039346656037ull ^ ret, key);
        if (menu_log_new(h))
            fprintf(stderr, "[TEXT] t=%ld key %s -> %d \"%s\" table %08X "
                    "caller %08X\n", attach_ms(), guest_wstr(key, k, sizeof k),
                    (int)g_eax, guest_wstr(MEM32(out), v, sizeof v), tbl, ret);
    }
}

static void menu_log_items(const char *who, uint32_t items, uint32_t count)
{
    uint64_t h = 14695981039346656037ull ^ MEM32(g_esp) ^ ((uint64_t)g_eax << 32);
    uint32_t i;
    char k[72];

    if (!menu_log_on())
        return;
    if (count > 32)
        count = 32;
    for (i = 0; i < count; i++)
        h = menu_hash_wstr(h * 31u, MEM32(items + 4 * i));
    if (!menu_log_new(h))
        return;
    fprintf(stderr, "[MENU] t=%ld %s id %08X caller %08X count %u selected %d:",
            attach_ms(), who, g_eax, MEM32(g_esp), count, (int)MEM32(g_esp + 12));
    for (i = 0; i < count; i++)
        fprintf(stderr, " %s", guest_wstr(MEM32(items + 4 * i), k, sizeof k));
    fprintf(stderr, "\n");
}

void sub_0020C3B0_enter(void)
{
    controls_list_drawn();
    menu_log_items("list", MEM32(g_esp + 4), MEM32(g_esp + 8));
}

/* The pause menu's list (see above): items 0x0087C828, count, selected, 0,
 * the caller's skip bytes. Its cursor has already moved this frame; one
 * that landed on Xbox Live goes on the way it was going, past skipped
 * items, and the list is drawn without it (then put back). */
#define PAUSE_ITEMS 0x0087C828u
#define PAUSE_CURSOR 0x00849874u

static uint32_t pause_step(uint32_t from, int dir, uint32_t count, uint32_t skip)
{
    uint32_t i, at = from;
    for (i = 0; i < count; i++) {
        at = (at + count + (uint32_t)dir) % count;
        if (at != 2 && !MEM8(skip + at))
            return at;
    }
    return 0;
}

extern void sub_0020C200_gen(void);
void sub_0020C200(void)
{
    static uint32_t last;
    uint32_t count = MEM32(g_esp + 8), skip = MEM32(g_esp + 20);
    uint32_t items[16], i, sel;
    uint8_t flags[16];

    controls_list_drawn();
    menu_log_items("list2", MEM32(g_esp + 4), count);
    if (MEM32(g_esp) != 0x001F8DA8u || MEM32(g_esp + 4) != PAUSE_ITEMS
        || !live_hidden() || count < 4 || count > 16 || !skip
        || MEM32(PAUSE_ITEMS + 8) != 0x005F74CCu) {     /* L"PT_XBOXLIVE" */
        sub_0020C200_gen();
        return;
    }
    sel = MEM32(PAUSE_CURSOR);
    if (sel == 2) {
        sel = pause_step(2, last == 3 ? -1 : 1, count, skip);
        MEM32(PAUSE_CURSOR) = sel;
    }
    last = sel;
    for (i = 0; i < count; i++) {
        items[i] = MEM32(PAUSE_ITEMS + 4 * i);
        flags[i] = MEM8(skip + i);
    }
    for (i = 2; i + 1 < count; i++) {
        MEM32(PAUSE_ITEMS + 4 * i) = items[i + 1];
        MEM8(skip + i) = flags[i + 1];
    }
    MEM32(PAUSE_ITEMS + 4 * (count - 1)) = 0;
    MEM8(skip + count - 1) = 0;
    MEM32(g_esp + 8) = count - 1;
    MEM32(g_esp + 12) = sel > 2 ? sel - 1 : sel;
    {
        uint32_t esp = g_esp;
        sub_0020C200_gen();
        MEM32(esp + 8) = count;
        MEM32(esp + 12) = sel;
    }
    for (i = 0; i < count; i++) {
        MEM32(PAUSE_ITEMS + 4 * i) = items[i];
        MEM8(skip + i) = flags[i];
    }
}

/* Back: the Live overlay never opens. */
extern void sub_0026AB20_gen(void);
void sub_0026AB20(void)
{
    if (live_hidden()) {
        g_eax = 0;
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_0026AB20_gen();
}

/* The Welcome menu's two item tables lose Xbox Live and System Link (the
 * first two items); done once, before the first draw. */
void sub_00236030_enter(void)
{
    static const uint32_t tables[2] = { 0x0075E718u, 0x0075E760u };
    int t;

    if (!live_hidden())
        return;
    for (t = 0; t < 2; t++) {
        uint32_t a = tables[t], i;
        if (MEM32(a) != 0xC9u || MEM32(a + 12) != 0xC8u)
            continue;
        for (i = 0; ; i++) {
            uint32_t from = a + 12u * (i + 2), to = a + 12u * i;
            MEM32(to) = MEM32(from);
            MEM32(to + 4) = MEM32(from + 4);
            MEM32(to + 8) = MEM32(from + 8);
            if (!MEM32(from))
                break;                  /* the terminator moved too */
        }
    }
}

/* ...and its icons (one per item, the caller's array of 6) move up alike. */
void sub_002401F0_enter(void)
{
    uint32_t icons = MEM32(g_esp + 20), i;

    if (!live_hidden() || MEM32(g_esp + 4) != 0x0075E718u || !icons)
        return;
    for (i = 0; i < 6; i++)
        MEM32(icons + 4 * i) = i + 2 < 6 ? MEM32(icons + 4 * (i + 2)) : 0;
}

/* The title launching another image (XLaunchNewImage / the Dashboard, the
 * kernel then quick-reboots and the .exe closes): tell the player why the
 * game is about to close. In main.c (a message box). */
extern void conker_dashboard_notice(void);
void sub_0042E562_enter(void)
{
    conker_dashboard_notice();
}
