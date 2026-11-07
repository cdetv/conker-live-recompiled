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
#include <math.h>

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
#include "save_names.h"
#include "precompile.h"

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

/* The widening in the camera's aspect: the one last applied here. */
static float s_widen_applied;

void sub_000645C0_enter(void)
{
    float widen = display_widen();
    aspect_log();
    s_widen_applied = widen;
    if (widen != 1.0f)
        g_xmm1.f[0] *= widen;
}

/* Photo mode freezes the world update, so the call above stops and a
 * render resolution changed meanwhile left the 4:3 picture stretched
 * (user 2026-10-31). The camera's aspect (+0x108) is brought to the shape
 * shown, before photo mode's projection build. */
void conker_AspectRefresh(unsigned cam)
{
    float widen = display_widen();
    if (s_widen_applied > 0.0f && widen != s_widen_applied) {
        MEMF(cam + 0x108u) *= widen / s_widen_applied;
        s_widen_applied = widen;
    }
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

/* XAPI turning a save name into its folder name (0x0042E6BB, stdcall: the
 * name as UTF-16, the 13-byte output): save_names.c learns which name the
 * hex is for, so a save the game creates gets a named folder at once. */
void sub_0042E6BB_enter(void)
{
    uint32_t p = MEM32(g_esp + 4);
    wchar_t name[64];
    int i;

    if (p < 0x00010000u || p >= 0x84000000u)
        return;
    for (i = 0; i < 63 && MEM16(p + 2 * i); i++)
        name[i] = (wchar_t)MEM16(p + 2 * i);
    if (MEM16(p + 2 * i))
        return;
    name[i] = L'\0';
    save_names_seen(name);
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

/* Return addresses further up the guest stack (dwords in the code sections
 * that follow a call instruction; the first `max` within `depth` dwords),
 * as one "<tag>   callers:" line. */
static void guest_callers_line(const char *tag, int depth, int max)
{
    int i, n = 0;
    fprintf(stderr, "%s   callers:", tag);
    for (i = 1; i < depth && n < max; i++) {
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
    /* Who resumed or stopped it. */
    if (g_ebx == 2 || g_ebx == 7)
        guest_callers_line("[XACTSND]", 96, 12);
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

/* ------------------------------------------------------------------------
 * Sound channels (to-do 13, SPEC 10.1 Sound): a volume and a mute for
 * music, effects, dialogue and other (ambience, menu sounds), a solo for
 * testing (--debug), a master volume and a cutscene volume.
 *
 * Every XACT voice's volume goes through one call, 0x0054E484 (eax = XACT's
 * voice wrapper, [esp+4] = millibels, 0 = full, -10000 = silent), mostly
 * from 0x00551690 (sound, track), which works it out from the sound's
 * volume, the track's and the game's variables whenever one changes
 * (conker-notes 2026-11-07 "Sound channels"). The entry hook adds the
 * channel's millibels to the argument. The channel comes from the track's
 * wave bank (track +0x74, an XACT handle): audiocore's bank list at
 * 0x0084B8EC (+4 next, +0x20 the XACT handle, +0x14 the game's
 * CAudioWaveBank, whose +4 is its path) gives the bank's file name, and the
 * name its kind, as the game's own bank table does (0x00687F80: am_
 * ambience, cut_ cutscene speech, m_ / multi_ music, the rest effects;
 * sfx_speech* in-game speech, sfx_frontend* menu sounds).
 *
 * XACT sets a voice's volume only when something changes, so a channel
 * change is re-sent: the voices seen (wrapper -> sound, track) are asked
 * again through 0x00551690 for every playing sound, from audio_update on
 * the game thread (where the game makes all its XACT calls).
 *
 * Master and cutscene volume are one gain on the output (toolkit
 * xbox_ApuSetOutputGain, which glides and limits above 1): movies
 * included; the cutscene volume applies while a cutscene plays
 * ([0x0076F0E0] == 1, the cutscene camera; dof_update's test).
 *
 * Volumes are percent on a squared curve (50 % = -12 dB, 0 = silent); the
 * cutscene volume goes to 200 % (+12 dB; user 2026-11-07: percent, the
 * original in the middle). At the defaults (100 %, no mute) the hook
 * changes nothing and the output gain is 1: the original sound.
 * RECOMP_VOLUME_LOG=1: [VOLUME] lines (classes, changes, re-sends);
 * =2 also each SetVolume call. RECOMP_AUDIO_SOLO=music|effects|dialogue|
 * other = the debug solo from the start. */
void xbox_ApuSetOutputGain(float gain);
extern void sub_00551690(void);

enum { SND_MUSIC, SND_EFFECTS, SND_DIALOGUE, SND_OTHER, SND_CHANNELS };
static const char *const s_snd_names[SND_CHANNELS] = {
    "music", "effects", "dialogue", "other",
};
static const char *const s_snd_vol_env[SND_CHANNELS] = {
    "RECOMP_MUSIC_VOLUME", "RECOMP_EFFECTS_VOLUME", "RECOMP_DIALOGUE_VOLUME",
    "RECOMP_OTHER_VOLUME",
};
static const char *const s_snd_mute_env[SND_CHANNELS] = {
    "RECOMP_MUSIC_MUTE", "RECOMP_EFFECTS_MUTE", "RECOMP_DIALOGUE_MUTE",
    "RECOMP_OTHER_MUTE",
};

static volatile float g_snd_vol[SND_CHANNELS];   /* percent */
static volatile int   g_snd_mute[SND_CHANNELS];
static volatile int   g_snd_solo = -1;           /* channel, -1 none */
static volatile float g_snd_master = 100.0f;     /* percent */
static volatile int   g_snd_master_mute;
static volatile float g_snd_cut = 100.0f;        /* percent, in cutscenes */
static volatile long  g_snd_gen;                 /* bumped by every change */
static int g_snd_ready;
static int g_snd_log = -1;
static unsigned g_snd_class_count[SND_CHANNELS + 1];   /* + not found */

static float snd_env_pct(const char *name, float def, float hi)
{
    const char *e = getenv(name);
    float v = e && *e ? (float)atof(e) : def;
    return v < 0.0f ? 0.0f : v > hi ? hi : v;
}

static int snd_channel_named(const char *s)
{
    int i;
    for (i = 0; s && i < SND_CHANNELS; i++)
        if (!_stricmp(s, s_snd_names[i]))
            return i;
    return -1;
}

/* The switches (settings.ini sets them), read once. */
static void snd_init(void)
{
    int i;
    const char *e;
    if (g_snd_ready)
        return;
    for (i = 0; i < SND_CHANNELS; i++) {
        e = getenv(s_snd_mute_env[i]);
        g_snd_vol[i] = snd_env_pct(s_snd_vol_env[i], 100.0f, 100.0f);
        g_snd_mute[i] = e && *e && *e != '0';
    }
    g_snd_master = snd_env_pct("RECOMP_MASTER_VOLUME", 100.0f, 100.0f);
    e = getenv("RECOMP_MASTER_MUTE");
    g_snd_master_mute = e && *e && *e != '0';
    g_snd_cut = snd_env_pct("RECOMP_CUTSCENE_VOLUME", 100.0f, 200.0f);
    g_snd_solo = snd_channel_named(getenv("RECOMP_AUDIO_SOLO"));
    e = getenv("RECOMP_VOLUME_LOG");
    g_snd_log = e && *e ? atoi(e) : 0;
    g_snd_ready = 1;
    fprintf(stderr, "[VOLUME] master %.0f %%%s, music %.0f %%%s, effects %.0f %%%s, "
            "dialogue %.0f %%%s, other %.0f %%%s, cutscenes %.0f %%%s%s\n",
            g_snd_master, g_snd_master_mute ? " muted" : "",
            g_snd_vol[0], g_snd_mute[0] ? " muted" : "",
            g_snd_vol[1], g_snd_mute[1] ? " muted" : "",
            g_snd_vol[2], g_snd_mute[2] ? " muted" : "",
            g_snd_vol[3], g_snd_mute[3] ? " muted" : "", g_snd_cut,
            g_snd_solo >= 0 ? ", solo " : "",
            g_snd_solo >= 0 ? s_snd_names[g_snd_solo] : "");
}

/* What a channel adds to a voice's volume, in millibels (0 = nothing). */
static int snd_offset_mb(int ch)
{
    float p;
    if (ch < 0 || ch >= SND_CHANNELS)
        return 0;
    if (g_snd_mute[ch] || (g_snd_solo >= 0 && g_snd_solo != ch))
        return -10000;
    p = g_snd_vol[ch] / 100.0f;
    if (p >= 1.0f)
        return 0;
    if (p <= 0.0f)
        return -10000;
    /* Squared: 2000 * log10(p * p). */
    return (int)lrintf(4000.0f * log10f(p));
}

/* A bank file's channel from its name ("audio\m_windy01.xwb"). */
static int snd_channel_of_path(uint32_t path)
{
    char name[48];
    int i, n = 0;
    uint8_t c;
    if (!xact_va_ok(path))
        return -1;
    for (i = 0; i < 96 && (c = MEM8(path + (uint32_t)i)) != 0; i++) {
        if (c == '\\' || c == '/') {
            n = 0;
            continue;
        }
        if (n < (int)sizeof(name) - 1)
            name[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    name[n] = 0;
    if (!strncmp(name, "m_", 2) || !strncmp(name, "multi_", 6))
        return SND_MUSIC;
    if (!strncmp(name, "am_", 3) || !strncmp(name, "sfx_frontend", 12))
        return SND_OTHER;
    if (!strncmp(name, "cut_", 4) || !strncmp(name, "sfx_spe", 7))
        return SND_DIALOGUE;
    return SND_EFFECTS;
}

/* An XACT wave bank's channel, from the game's registered banks; -1 when it
 * is not among them. */
static int snd_channel_of_bank(uint32_t wb)
{
    uint32_t n = MEM32(0x0084B8F0u);
    int guard = 0;
    while (xact_va_ok(n) && guard++ < 256) {
        if (MEM32(n + 0x20) == wb) {
            uint32_t bank = MEM32(n + 0x14);
            return xact_va_ok(bank) ? snd_channel_of_path(MEM32(bank + 4)) : -1;
        }
        n = MEM32(n + 4);
    }
    return -1;
}

/* The voices seen: wrapper -> the sound and track that last set its volume
 * (re-sent when a channel changes) and its channel. */
#define SND_VOICES 512
typedef struct { uint32_t w, snd, trk; int ch; } SndVoice;
static SndVoice s_snd_voices[SND_VOICES];

static SndVoice *snd_voice(uint32_t w, int add)
{
    int i, free_i = -1;
    for (i = 0; i < SND_VOICES; i++) {
        if (s_snd_voices[i].w == w)
            return &s_snd_voices[i];
        if (free_i < 0 && !s_snd_voices[i].w)
            free_i = i;
    }
    if (!add)
        return NULL;
    if (free_i < 0) {
        /* Full: drop the ones whose track has moved on. */
        for (i = 0; i < SND_VOICES; i++) {
            SndVoice *v = &s_snd_voices[i];
            if (!xact_va_ok(v->trk) || MEM32(v->trk + 4) != v->w) {
                v->w = 0;
                if (free_i < 0)
                    free_i = i;
            }
        }
        if (free_i < 0)
            return NULL;
    }
    s_snd_voices[free_i].w = w;
    return &s_snd_voices[free_i];
}

/* 0x0054E484: XACT voice SetVolume (eax = wrapper, [esp+4] = millibels). */
void sub_0054E484_enter(void)
{
    uint32_t w = g_eax, ret = MEM32(g_esp);
    int32_t mb = (int32_t)MEM32(g_esp + 4), was = mb;
    int ch = -1, off;
    SndVoice *v;

    snd_init();
    if (ret == 0x0055172Fu) {
        /* From 0x00551690: its frame holds sound [ebp+8], track [ebp+0xC]. */
        uint32_t snd = MEM32(g_ebp + 8), trk = MEM32(g_ebp + 0xC);
        if (xact_va_ok(trk)) {
            ch = snd_channel_of_bank(MEM32(trk + 0x74));
            g_snd_class_count[ch < 0 ? SND_CHANNELS : ch]++;
            if ((v = snd_voice(w, 1)) != NULL) {
                if (g_snd_log > 0 && (v->snd != snd || v->trk != trk))
                    fprintf(stderr, "[VOLUME] voice %08X: %s (bank %08X)\n", w,
                            ch < 0 ? "bank not found, effects" : s_snd_names[ch],
                            MEM32(trk + 0x74));
                v->snd = snd;
                v->trk = trk;
                v->ch = ch;
            }
        }
    } else if ((v = snd_voice(w, 0)) != NULL) {
        ch = v->ch;
    }
    if (ch < 0)
        ch = SND_EFFECTS;
    off = snd_offset_mb(ch);
    if (off) {
        mb += off;
        if (mb < -10000)
            mb = -10000;
        if (mb > 0)
            mb = 0;
        MEM32(g_esp + 4) = (uint32_t)mb;
    }
    if (g_snd_log > 1)
        fprintf(stderr, "[VOLUME] set %08X %s %d -> %d mB (ret %08X)\n", w,
                s_snd_names[ch], was, mb, ret);
}

/* Every playing sound's voices asked for their volume again (game thread,
 * audio_update), after a channel change. */
static void snd_resend(void)
{
    uint32_t r_eax = g_eax, r_ecx = g_ecx, r_edx = g_edx, r_ebx = g_ebx;
    uint32_t r_esi = g_esi, r_edi = g_edi, r_ebp = g_ebp, r_esp = g_esp;
    int i, sent = 0;

    for (i = 0; i < SND_VOICES; i++) {
        SndVoice *v = &s_snd_voices[i];
        if (!v->w)
            continue;
        if (!xact_va_ok(v->trk) || !xact_va_ok(v->snd) ||
            MEM32(v->trk + 4) != v->w || MEM32(v->trk + 8) != v->snd) {
            v->w = 0;               /* the track has moved on */
            continue;
        }
        if (MEM16(v->snd + 0x30) != 2)   /* only playing sounds */
            continue;
        g_esp -= 4; MEM32(g_esp) = v->trk;
        g_esp -= 4; MEM32(g_esp) = v->snd;
        g_esp -= 4; MEM32(g_esp) = 0;   /* return address, popped by the callee */
        sub_00551690();
        g_esp = r_esp;
        sent++;
    }
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi; g_ebp = r_ebp; g_esp = r_esp;
    if (g_snd_log > 0)
        fprintf(stderr, "[VOLUME] re-sent %d voice(s); voices by bank so far: "
                "music %u, effects %u, dialogue %u, other %u, not found %u\n",
                sent, g_snd_class_count[0], g_snd_class_count[1],
                g_snd_class_count[2], g_snd_class_count[3],
                g_snd_class_count[4]);
}

/* Once per audio_update: a channel change re-sent, and the output gain
 * (master x the cutscene volume while a cutscene plays). */
static void snd_update(void)
{
    static long done_gen;
    static int was_cut = -1;
    long gen = g_snd_gen;
    int cut;
    float m, gain;

    snd_init();
    if (gen != done_gen) {
        done_gen = gen;
        snd_resend();
    }
    cut = MEM8(0x0076F0E0u) == 1;
    if (cut != was_cut && g_snd_log > 0)
        fprintf(stderr, "[VOLUME] cutscene %s (level %02X)\n", cut ? "on" : "off",
                MEM32(0x007FA1F8u));
    was_cut = cut;
    m = g_snd_master_mute ? 0.0f : g_snd_master / 100.0f;
    gain = m * m;
    if (cut)
        gain *= (g_snd_cut / 100.0f) * (g_snd_cut / 100.0f);
    xbox_ApuSetOutputGain(gain);
}

/* For the F2 menu (overlay.cpp). A channel: volume in percent (< 0 = keep)
 * and mute (< 0 = keep); solo: a channel or -1; master in percent; the
 * cutscene volume in percent. They apply from the next audio_update. */
void conker_SetSoundChannel(int ch, float pct, int mute)
{
    if (ch < 0 || ch >= SND_CHANNELS)
        return;
    snd_init();
    if (pct >= 0.0f)
        g_snd_vol[ch] = pct > 100.0f ? 100.0f : pct;
    if (mute >= 0)
        g_snd_mute[ch] = mute != 0;
    _InterlockedIncrement(&g_snd_gen);
}

void conker_SetSoundSolo(int ch)
{
    snd_init();
    g_snd_solo = ch >= 0 && ch < SND_CHANNELS ? ch : -1;
    _InterlockedIncrement(&g_snd_gen);
    fprintf(stderr, "[VOLUME] solo %s\n", g_snd_solo >= 0 ? s_snd_names[g_snd_solo] : "off");
}

int conker_SoundSolo(void)
{
    snd_init();
    return g_snd_solo;
}

void conker_SetMasterVolume(float pct)
{
    snd_init();
    g_snd_master = pct < 0.0f ? 0.0f : pct > 100.0f ? 100.0f : pct;
}

void conker_SetMasterMute(int on)
{
    snd_init();
    g_snd_master_mute = on != 0;
}

void conker_SetCutsceneVolume(float pct)
{
    snd_init();
    g_snd_cut = pct < 0.0f ? 0.0f : pct > 200.0f ? 200.0f : pct;
}

/* At start-up (main.c): the master volume from the first sound on (the
 * opening movies play before the game's audio manager runs audio_update). */
void conker_SoundInit(void)
{
    float m;
    snd_init();
    m = g_snd_master_mute ? 0.0f : g_snd_master / 100.0f;
    if (m < 1.0f)
        xbox_ApuSetOutputGain(m * m);
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

/* D3D_BlockOnTime (0x0053C190, stdcall (time, flags)): the game's wait for
 * the GPU, from D3DDevice_Swap every frame (the fence of the frame before
 * last) and from resource waits. It ends in a loop that spins on the
 * GPU-time word ([dev+0x30]) until the GPU has passed `time`: the game thread
 * sat at 100% of a core, 83-92% of it in this loop, at 30 and 60 fps alike
 * (2026-11-01, GitHub issue 11: "single core maxed out"). xemu spins the
 * same.
 *
 * Here the thread sleeps until the executor's fence mirror has moved the word
 * past `time` (it wakes the sleepers when it does); the original then finds
 * the GPU done and returns at its first test. Only for a fence already
 * submitted (`time` != the next fence time [dev+0x2C], else the original
 * submits it first) and without a block callback ([dev+0x19FC], which the
 * original calls before waiting). After 100 ms the original takes over, so a
 * fence that needs the original's own push to arrive costs that, not a hang.
 * RECOMP_GPU_WAIT_SLEEP=0 = the original spin. */
extern uint32_t xbox_Nv2aFenceGeneration(void);
extern void xbox_Nv2aWaitFence(uint32_t seen, DWORD ms);

void sub_0053C190_enter(void)
{
    static int on = -1;
    static unsigned timeouts;
    uint32_t dev, time, next, word;
    ULONGLONG t0 = 0;

    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_WAIT_SLEEP");
        on = !(e && *e == '0');
    }
    dev = MEM32(0x005499E8u);
    if (!on || !dev || MEM32(dev + 0x19FC))
        return;
    time = MEM32(g_esp + 4);
    next = MEM32(dev + 0x2C);
    word = MEM32(dev + 0x30);
    if (time == next || !word)
        return;
    for (;;) {
        uint32_t gen = xbox_Nv2aFenceGeneration();
        if (next - time >= next - MEM32(word))   /* the original's "passed" */
            return;
        if (!t0)
            t0 = GetTickCount64();
        else if (GetTickCount64() - t0 >= 100) {
            if (timeouts++ < 8) {
                fprintf(stderr, "[GPUWAIT] fence %08X not reached in 100 ms"
                        " (GPU at %08X, next %08X); the original waits\n",
                        time, MEM32(word), next);
                fflush(stderr);
            }
            return;
        }
        xbox_Nv2aWaitFence(gen, 2);
    }
}

/* 0x0053C630 d3d_pfb_flush: sets 0x100410 bit 16 and spins until the "GPU"
 * clears it. Here the executor (ack thread) answers it at the top of its next
 * pass, after the segment it is running, so this waits as long as the
 * executor is busy (the barrier D3D's fence waits rely on, architecture
 * gotchas). Same flush, but the thread sleeps until it is answered, and the
 * request wakes the executor, which now sleeps when idle (toolkit
 * xbox_Nv2aWaitHandshake, RECOMP_EXEC_SLEEP; =0 is the original spin).
 * CDevice_KickOff (0x0053C450) and the kick-and-wait at 0x0053C500 write
 * DMA_PUT right after: kick_follows keeps the executor awake for it. Same
 * registers out as the original: eax = the register base, ecx = old | bit. */
extern void xbox_Nv2aWaitHandshake(volatile uint32_t *reg, uint32_t mask,
                                   int kick_follows);

void sub_0053C630(void)
{
    uint32_t ret = MEM32(g_esp);
    uint32_t regs = MEM32(MEM32(0x005499E8u) + 0x1C28u);
    volatile uint32_t *flush = (volatile uint32_t *)XBOX_PTR(regs + 0x100410u);
    uint32_t v = *flush | 0x10000u;

    *flush = v;
    xbox_Nv2aWaitHandshake(flush, 0x10000u,
                           ret >= 0x0053C450u && ret < 0x0053C630u);
    g_eax = regs;
    g_ecx = v;
    g_esp += 4;                     /* ret */
}

/* ── Conker: the loading screen (to-do 26) ────────────────────────────────
 *
 * A level load (level_load_run 0x002AADC0) loops over its loader's steps
 * and after each one calls loadscreen_step 0x002AD3B0 (bl = draw even when
 * the screen's animation is over, loader +0x40): animate the loading screen
 * (time-based: 0x002AD240 reads the clock, not a step count), draw it into
 * the FRONT buffer (0x002ACA70, 0x002ACB20), then D3D_SetFence +
 * D3D_BlockOnTime. On the Xbox the display scans the front buffer out as
 * it is drawn, so the screen shows; here a present happened only at a
 * Swap, so it was drawn and never shown (conker-notes 2026-10-30). The
 * picture is already in the front buffer when the loop starts (drawn before
 * it, so the last game frame is gone by then).
 *
 * A load's length is not the drawing: a field load from the main menu is
 * 3.6 s with every step drawn, with one every 33 ms and with none, because
 * the loader (state 2, 0x002AB870) waits until 0x00018C00 finds all 76
 * sound banks free, and the menu tune's cue reports its stop ~3.05 s after
 * StopTune (the sound's own release in XACT; conker-notes 2026-11-07).
 *
 * RECOMP_LOADSCREEN (= settings loading_screen):
 *   on (default): every step as the game does, and what it drew shown (at
 *     most once every LS_PRESENT_MS);
 *   off: no loading screen: black at once (xbox_Nv2aPresentNow black: the
 *     last frame is already overwritten), no drawing, the next level fades
 *     in over LS_FADE_IN_MS (conker_LoadCover for the overlay); skipped
 *     steps yield the thread (RECOMP_LOAD_YIELD=0 none, 2 = Sleep(1));
 *   hidden (debug): every step drawn, never shown = before this change.
 * One [LOAD] line per load. */
#define LS_PRESENT_MS  14.0
#define LS_FADE_IN_MS 300.0
enum { LS_ON, LS_OFF, LS_HIDDEN };
extern void xbox_Nv2aPresentNow(int black);
extern void sub_002AD3B0_gen(void);
extern void sub_002AADC0_gen(void);

static double skip_ms(void);

static struct {
    double t0, last_present, step_ms;
    unsigned steps, drawn, presents;
    int active, mode;                   /* mode: this load's */
} s_ld;
static volatile double g_lc_t0, g_lc_t1;    /* off: load start / end, 0 = none */
static volatile int g_ls_mode = -1;

static int loadscreen_mode(void)
{
    if (g_ls_mode < 0) {
        const char *e = getenv("RECOMP_LOADSCREEN");
        g_ls_mode = e && (!_stricmp(e, "off") || !strcmp(e, "0")) ? LS_OFF
                  : e && !_stricmp(e, "hidden") ? LS_HIDDEN : LS_ON;
    }
    return g_ls_mode;
}

/* F2 (applies from the next load): 1 = the loading screen, 0 = none. */
void conker_SetLoadingScreen(int on)
{
    g_ls_mode = on ? LS_ON : LS_OFF;
}

static void load_yield(void)
{
    static int y = -1;
    if (y < 0) {
        const char *e = getenv("RECOMP_LOAD_YIELD");
        y = e && *e ? atoi(e) : 1;
    }
    if (y == 1)
        SwitchToThread();
    else if (y >= 2)
        Sleep(1);
}

/* For the overlay: how black the load cover is, 0..1 (mode off): black
 * through the load, then fading out as the next level draws. */
float conker_LoadCover(void)
{
    double t0 = g_lc_t0, t1 = g_lc_t1, a;
    if (t0 <= 0.0)
        return 0.0f;
    if (t1 <= 0.0)
        return 1.0f;
    a = 1.0 - (skip_ms() - t1) / LS_FADE_IN_MS;
    if (a <= 0.0) {
        g_lc_t0 = g_lc_t1 = 0.0;
        return 0.0f;
    }
    return (float)(a > 1.0 ? 1.0 : a);
}

/* ── Conker: fast loads (to-do 118) and what holds a load (to-do 110) ────
 *
 * A level load waits until the old level's sound banks are freed, and a
 * bank is freed only when no cue plays on it any more. A music stop is not
 * immediate: CMusicTune_StopTune -> CAudioCue_Stop -> audioSoundBankStopCue
 * 0x00400B80 with ecx = 0 -> the XACT stop without its immediate flag 2, so
 * XACT plays the sound's release, and the cue reports
 * AUDIO_NOTIFICATION_STOP only when that is over: the menu tune 3.0 s, the
 * Windy exit's birdytoon 8.0 s (run_20261106_180620). That set a field load
 * from the main menu at 3.6 s and Windy -> throne room at 8 s (in the tips
 * screen wait 0x002A3C20, Sleep(16) until 0x00018870 frees every bank);
 * the release's 63 s+ hang there (to-do 110) was such a stop never ending.
 * The tunes are stopped by level_unload 0x002AA140 (game_main_loop, right
 * before level_change), its last call, the audio "NewGame" 0x000187C0 ->
 * 0x0001E570 (every tune; XACT log callers, run_20261107_113938).
 *
 * RECOMP_FAST_LOADS (= settings fast_loads, default off, user 2026-11-07;
 * 1 = on): inside level_unload, level_load_run 0x002AADC0 and the tips
 * wait every stop is the immediate kind (ecx = 1 at 0x00400B80), the path
 * the game itself takes for a stop without a fade. Not the stop that
 * audioSoundBankFreeCue 0x00400C00 makes of a cue freed while playing
 * (return 0x00400D02): that cue is half-way through being freed. The music
 * then cuts at the load's start instead of fading under the loading
 * screen. Off = the original. No loading screen with it: Loading Screen
 * off (user 2026-11-07: no separate choice under Fast Loads).
 *
 * Never issue a second, immediate stop for a sound already releasing (the
 * first build did, clearing bit 2 of the sound bank cue's +0x38, its "stop
 * issued" guard): XACT's stop update (0x00552EFF) then counts the tracks
 * done without stopping their voices (track flag 0x40 is set by then), the
 * voice still plays its release, goes back to the pool, and DirectSound
 * later walked a broken voice list (crash in 0x004CB2D1 +0x113, every soak
 * run at It's War's MachineGunNest, run_20261107_112727 / _113122).
 *
 * Either way a load or tips wait that runs past 5 s prints what still holds
 * it ([LOAD] line: sound banks still registered, their users) and again
 * every 10 s, so a hang like to-do 110's names its cause. */
static int cutevent_log_on(void);
static void cutevent_where(const char *what);
static int s_fl_window;                 /* > 0: inside an unload or load */
static double s_fl_t0, s_fl_report, s_fl_unload_ms;
static unsigned s_fl_forced;            /* since the last [LOAD] line */
static volatile int g_fl_mode = -1;
extern void sub_002A3C20_gen(void);
extern void sub_002AA140_gen(void);

static int fast_loads_on(void)
{
    if (g_fl_mode < 0) {
        const char *e = getenv("RECOMP_FAST_LOADS");
        g_fl_mode = e && *e && strcmp(e, "0") && _stricmp(e, "off");
    }
    return g_fl_mode;
}

/* F2 (applies from the next load). */
void conker_SetFastLoads(int on)
{
    g_fl_mode = on ? 1 : 0;
}

/* audioSoundBankStopCue (eax = sound bank cue, ecx != 0 = immediate; it
 * does nothing when +0x38 bit 2, a stop issued, is set; bit 4 makes it
 * immediate itself). */
void sub_00400B80_enter(void)
{
    uint32_t sb = g_eax;

    if (!s_fl_window || g_ecx || !xact_va_ok(sb) || (MEM32(sb + 0x38) & 6u) ||
        MEM32(g_esp) == 0x00400D02u || !fast_loads_on())
        return;
    g_ecx = 1;
    s_fl_forced++;
}

static void fl_open(void)
{
    if (s_fl_window++)
        return;
    s_fl_t0 = skip_ms();
    s_fl_report = s_fl_t0 + 5000.0;
}

static void fl_close(void)
{
    if (s_fl_window > 0)
        s_fl_window--;
}

/* Past 5 s in a load: the sound banks still registered (what the waits
 * wait for), every 10 s. From the loading steps and audio_update. */
static void fl_wait_report(void)
{
    uint32_t mgr = MEM32(0x00847024u), banks, b, i;
    double now;
    char nm[64];
    int n = 0;

    if (!s_fl_window || (now = skip_ms()) < s_fl_report)
        return;
    s_fl_report = now + 10000.0;
    fprintf(stderr, "[LOAD] still waiting after %.1f s (fast loads %s); sound banks"
            " still registered:", (now - s_fl_t0) / 1000.0,
            fast_loads_on() ? "on" : "off");
    banks = xact_va_ok(mgr) ? MEM32(mgr + 4) : 0;
    for (i = 0; xact_va_ok(banks) && i < 76; i++) {
        b = MEM32(banks + 4 * i);
        if (!xact_va_ok(b) || !MEM8(b + 0x15) || MEM8(b + 0x22))
            continue;
        fprintf(stderr, " %s (users %u%s)", audio_log_str(MEM32(b + 4), nm, sizeof nm),
                MEM32(b + 0x18), MEM8(b + 0x14) ? ", locked" : "");
        n++;
    }
    fprintf(stderr, "%s\n", n ? "" : " none");
    fflush(stderr);
}

/* level_unload (game_main_loop, before level_change): frees the level and
 * stops its tunes last. */
void sub_002AA140(void)
{
    double t0 = skip_ms();
    if (cutevent_log_on())
        cutevent_where("level unload");
    fl_open();
    sub_002AA140_gen();
    fl_close();
    s_fl_unload_ms = skip_ms() - t0;
}

/* The tips screen wait (some loads): stops music and effects, then waits
 * until every sound bank is freed. */
void sub_002A3C20(void)
{
    double t0 = skip_ms();
    fl_open();
    sub_002A3C20_gen();
    fl_close();
    fprintf(stderr, "[LOAD] tips screen wait: %.0f ms (fast loads %s; %u stops made"
            " immediate)\n", skip_ms() - t0, fast_loads_on() ? "on" : "off",
            s_fl_forced);
    fflush(stderr);
    s_fl_forced = 0;
}

void sub_002AD3B0(void)
{
    int mode = s_ld.active ? s_ld.mode : loadscreen_mode();
    double now, t;
    uint32_t screen;
    int force = (g_ebx & 0xFFu) != 0;

    fl_wait_report();
    if (!s_ld.active || mode == LS_OFF) {
        if (!s_ld.active) {             /* not seen so far */
            static unsigned n;
            if (n++ < 16) {
                fprintf(stderr, "[LOAD] loading screen step outside a load"
                        " (bl %d, caller %08X)\n", force, MEM32(g_esp));
                fflush(stderr);
            }
        }
        if (mode == LS_OFF) {
            s_ld.steps++;
            load_yield();
            g_esp += 4;                 /* ret */
            return;
        }
        sub_002AD3B0_gen();
        return;
    }
    s_ld.steps++;
    now = skip_ms();
    sub_002AD3B0_gen();
    t = skip_ms();
    s_ld.step_ms += t - now;
    screen = MEM32(0x008758C4u);
    if (force || (screen && !MEM8(screen + 0x84))) {
        s_ld.drawn++;
        if (mode == LS_ON && t - s_ld.last_present >= LS_PRESENT_MS) {
            s_ld.last_present = t;
            s_ld.presents++;
            xbox_Nv2aPresentNow(0);
        }
    }
}

void sub_002AADC0(void)
{
    int mode = loadscreen_mode();
    uint32_t to = MEM32(0x007FA1F4u);   /* the level being loaded */
    double t1;

    memset(&s_ld, 0, sizeof s_ld);
    s_ld.t0 = skip_ms();
    s_ld.mode = mode;
    s_ld.active = 1;
    if (mode == LS_OFF) {
        g_lc_t1 = 0.0;
        g_lc_t0 = s_ld.t0;
        s_ld.presents++;
        xbox_Nv2aPresentNow(1);         /* black, not the loading picture */
    }
    fl_open();
    sub_002AADC0_gen();
    fl_close();
    t1 = skip_ms();
    s_ld.active = 0;
    if (mode == LS_OFF)
        g_lc_t1 = t1;
    fprintf(stderr, "[LOAD] level 0x%02X: %.0f ms (loading screen %s; %u"
            " steps, %u drawn, %.0f ms in them, %u shown; unload before it"
            " %.0f ms; fast loads %s: %u stops made immediate)\n", to,
            t1 - s_ld.t0, mode == LS_ON ? "on" : mode == LS_OFF ? "off"
            : "hidden", s_ld.steps, s_ld.drawn, s_ld.step_ms, s_ld.presents,
            s_fl_unload_ms, fast_loads_on() ? "on" : "off", s_fl_forced);
    s_fl_forced = 0;
    s_fl_unload_ms = 0.0;
    fflush(stderr);
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

/* ── Conker: pause screen animations at real time above 30 fps (to-do 83) ──
 *
 * The UI page draw 0x0024D170 (eax = page; from 0x0024EBB0) moves its
 * clock [0x00849C40] and every element's animation (0x002478A0) by dt
 * [0x007FA21C], or by a fixed 1/30 s when dt is 0. dt is 0 while paused
 * [0x007FA275], so the pause screen's animations (the Quit? box's Yes / No
 * flash) ran one 30 fps frame per frame: 3.9x real time at 120 (UI clock
 * +11.7 s in 3 s). While paused, dt is this frame's step [0x007FA24C] / 30
 * for the call (1/30 at 30 fps, as before), then 0 again. Nothing else in
 * the UI code reads dt. RECOMP_PAUSE_UI_FIX=0 = as before. */
extern void sub_0024D170_gen(void);
void sub_0024D170(void)
{
    static int on = -1;
    float step = MEMF(0x007FA24Cu);
    if (on < 0) {
        const char *e = getenv("RECOMP_PAUSE_UI_FIX");
        on = !(e && *e == '0');
    }
    if (!on || !MEM8(0x007FA275u) || MEMF(0x007FA21Cu) != 0.0f || !(step > 0.0f)) {
        sub_0024D170_gen();
        return;
    }
    MEMF(0x007FA21Cu) = step / 30.0f;
    sub_0024D170_gen();
    MEMF(0x007FA21Cu) = 0.0f;
}

/* ── Conker: D-pad left / right in sideways menu lists (to-do 90) ─────────
 *
 * A UI list's input 0x00249ED0 (eax = list; from 0x00249D00) turns the
 * pad's state into four answers: up and down move the cursor [list+0x24],
 * left and right set the item's toggle. A sideways list (byte [list+0x29],
 * the pause screen's Quit? Yes / No) swaps the stick's axes, so stick left
 * goes to Yes and right to No, but not the D-pad's bits (the input record
 * [[0x008470FC + pad*4] + 8], 1 up, 2 down, 4 left, 8 right; pad
 * [0x00776558], record 0x0075A0B4 for 2 and up): the D-pad, and the arrow
 * keys with it, moved it only with down (Yes) and up (No) (user
 * 2026-11-05; the original's own). In such a list the D-pad's bits are
 * swapped for the call as the stick's axes are: left counts as down, right
 * as up, up as right and down as left (the stick's up / down there: the
 * toggle and its sound, no move; user 2026-11-05). RECOMP_MENU_DPAD_FIX=0
 * = as before. */
extern void sub_00249ED0_gen(void);
void sub_00249ED0(void)
{
    static int on = -1;
    uint32_t rec, pad = MEM8(0x00776558u);
    uint8_t was;
    if (on < 0) {
        const char *e = getenv("RECOMP_MENU_DPAD_FIX");
        on = !(e && *e == '0');
    }
    rec = pad < 2u ? MEM32(MEM32(0x008470FCu + pad * 4u) + 8u) : 0x0075A0B4u;
    if (!on || !MEM8(g_eax + 0x29u) || MEM8(0x00862D08u) || !rec) {
        sub_00249ED0_gen();
        return;
    }
    was = MEM8(rec + 8u);
    MEM8(rec + 8u) = (uint8_t)((was & ~0x0Fu) | (was & 1u ? 8u : 0u) | (was & 2u ? 4u : 0u)
                               | (was & 4u ? 2u : 0u) | (was & 8u ? 1u : 0u));
    sub_00249ED0_gen();
    MEM8(rec + 8u) = was;
}

static void audio_update_logs(void)
{
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

/* audio_update (0x00017CF0, fastcall, ecx = the audio manager): its body
 * runs only when GetTickCount has moved more than 15 ms since its last run
 * ([ecx+0x18]), and the sound lifetimes in it (looped / timed emitters,
 * 0x00030730 / 0x00030BC0) count down by this frame's 60 Hz ticks
 * [0x007FA208]. At 30 fps it runs every frame; at 120 about every third
 * and the ticks of the frames in between were lost: the money pickup's
 * slot-machine sound (eyeroll, 149 ticks = 2.5 s) played 7.4 s (to-do 85).
 * Here a run sees every tick since the last run (both copies, 0x007FA208
 * and 0x007FA20C), then the frame's own come back. Repeat calls in one
 * frame (the load wait 0x00018870) after a run get this frame's ticks, as
 * before. RECOMP_AUDIO_TICK_FIX=0 = original; [AUDIOTICK] with
 * RECOMP_AUDIO_LOG=1 (per 10 s: runs, frames, ticks given).
 * The cue and emitter timers in it also count seconds by the frame time
 * [0x007FA218] (a cue's start delay, cue +0xE8 in CAudioCue_Update
 * 0x0001A510; music fades, emitter lifetimes) and 30 fps frames by
 * [0x007FA250], one frame's worth a run. At 120 a run saw a third of the
 * time, and the four cannons of Harbour's battery (one cue each, start
 * delays ~20 ms apart) began 41-109 ms apart, a metallic smear; at 30 the
 * delays move in 30 fps frames and they start in the same run or one frame
 * apart. Here a run gets the whole 30 fps frames since the last run (from
 * the step [0x007FA24C]) in [0x007FA250] and, once at least one has passed,
 * all the frame time since the last such run in [0x007FA218] (else 0):
 * at 30 fps each run gets its own frame's values, as before; exact 1/30 s
 * steps instead put the battery's 4th cannon a frame late every volley,
 * 3 in 9 originally (to-do 5). RECOMP_AUDIO_TIME_FIX=0 = without this
 * part.
 *
 * Photo mode (to-do 9): the freeze skips the game's own sound pause (cue
 * pause, music pause; photo_mode.c); only the APU was held. In the Great
 * Mighty Poo song a 10 s freeze left the song's timeline 10 s ahead of the
 * held stream: its first marker came 49.0 s after the song started, as
 * without a freeze, and ~5 s after the freeze a second set of song streams
 * started over the held one, which was dropped with the rest of the vocals
 * (user 2026-11-06, run_20261106_215411). XACT's track events run on
 * KeQuerySystemTime: its sound pause (0x00551246) notes the time and on
 * resume moves every pending event on by the time paused, so only the
 * game's own pause keeps a song in step (photo_game_sound_pause above).
 * The game's sound logic in this update also runs on the frame time and
 * ticks (cue start delays and stop timeouts, emitter lifetimes; the pause
 * menu leaves those running), so while photo mode holds the sound
 * (pause_silence on) every call also gets no time: ticks, frame time and
 * 30 fps frames 0 (0 frame time and 0 frames are what a run between 30 fps
 * frames already gets above 30 fps), and the frozen frames' time is not
 * saved up for afterwards. Nothing in the sound code reads the step
 * [0x007FA24C]. With pause_silence off the sound plays on, and so does its
 * time. A frame step moves the world one frame and not the sound.
 * RECOMP_PHOTO_SOUND_FIX=0 = as before, =2 zero time only (not enough on
 * its own: run_20261106_220814); "[PHOTO] game's sound pause / resume" and
 * "[PHOTO] sound held" lines per freeze. */
static int photo_sound_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_PHOTO_SOUND_FIX");
        mode = e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1;
    }
    return mode;
}

static int photo_sound_held(void)
{
    return photo_sound_mode() && photo_frozen()
        && strcmp(settings_get("pause_silence"), "0") != 0;
}

/* The game's own sound pause for photo mode (to-do 9): game_frame's pause
 * compare (0x002A4A5C on, 0x002A4B1E off) without the pause menu's music
 * duck (0x0001E750 tune 0x23) and its stop. Zero time alone was not enough:
 * the song's marker still came 49.0 s after the song started, in the middle
 * of a 10 s freeze, with the APU held (run_20261106_220814): XACT's track
 * events run on KeQuerySystemTime, which the APU hold does not stop. The
 * game's pause pauses every cue through XACT (0x0001AAE0 -> 0x00400D30 ->
 * 0x00551246, which moves the events on by the time paused), as for the
 * pause menu. With it the marker came 10.0 s later and the cutscene ended
 * with its song (run_20261106_221959). Unless the audio manager's +0x85 is
 * set: effects manager
 * [mgr+0x40] (paused flag +0x14) 0x0002F2C0(on) with ebx = its +4 (the
 * cue table), music [mgr+0x3C] (paused flag its byte 0) 0x0001E4B0(on)
 * with ebx = it, [[mgr+0x44]+0x1D] = on; then the speech state 0x0001B140
 * (eax 0 = no speech, else [0x00847050] = 2 paused / 1 going). Both calls
 * are ret 4. Only what this pause set is undone. */
extern void sub_0002F2C0(void);
extern void sub_0001E4B0(void);
extern void sub_0001B140(void);

static void photo_game_sound_pause(int on)
{
    static int paused_fx, paused_music, paused_any;
    uint32_t mgr = MEM32(0x00847024u), fx, music, r_eax, r_ecx, r_edx, r_ebx;
    uint32_t r_esi, r_edi, r_ebp, r_esp;
    long t0 = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    int speech, did_fx = 0, did_music = 0;

    if (!on && !paused_any)
        return;
    if (!xact_va_ok(mgr))
        return;
    r_eax = g_eax; r_ecx = g_ecx; r_edx = g_edx; r_ebx = g_ebx;
    r_esi = g_esi; r_edi = g_edi; r_ebp = g_ebp; r_esp = g_esp;
    if (!MEM8(mgr + 0x85u)) {
        fx = MEM32(mgr + 0x40u);
        music = MEM32(mgr + 0x3Cu);
        MEM8(0x00847046u) = 0;
        if (xact_va_ok(fx) && (on ? !MEM8(fx + 0x14u) : paused_fx && MEM8(fx + 0x14u))) {
            g_ebx = MEM32(fx + 4u);
            g_esp -= 4; MEM32(g_esp) = (uint32_t)on;
            g_esp -= 4; MEM32(g_esp) = 0;   /* return address, popped by the callee */
            sub_0002F2C0();
            g_esp = r_esp;
            MEM8(fx + 0x14u) = (uint8_t)on;
            paused_fx = on;
            did_fx = 1;
        }
        if (xact_va_ok(music) && (on ? !MEM8(music) : paused_music && MEM8(music))) {
            g_ebx = music;
            g_esp -= 4; MEM32(g_esp) = (uint32_t)on;
            g_esp -= 4; MEM32(g_esp) = 0;
            sub_0001E4B0();
            g_esp = r_esp;
            MEM8(music) = (uint8_t)on;
            paused_music = on;
            did_music = 1;
        }
        if (xact_va_ok(MEM32(mgr + 0x44u)))
            MEM8(MEM32(mgr + 0x44u) + 0x1Du) = (uint8_t)on;
    }
    g_esp -= 4; MEM32(g_esp) = 0;
    sub_0001B140();
    g_esp = r_esp;
    speech = (int)g_eax;
    if (speech)
        MEM32(0x00847050u) = on ? 2u : 1u;
    g_eax = r_eax; g_ecx = r_ecx; g_edx = r_edx; g_ebx = r_ebx;
    g_esi = r_esi; g_edi = r_edi; g_ebp = r_ebp; g_esp = r_esp;
    paused_any = on;
    fprintf(stderr, "[PHOTO] game's sound %s: effects %s, music %s, speech state %d (%ld ms)\n",
            on ? "pause" : "resume", did_fx ? "yes" : "no", did_music ? "yes" : "no",
            speech, (long)(clock() * 1000 / CLOCKS_PER_SEC) - t0);
}

extern void sub_00017CF0_gen(void);
void sub_00017CF0(void)
{
    static int on = -1, time_on = -1;
    static uint32_t last_fc, acc, given, runs, frames, given30;
    static float step_acc, time_acc;
    static int ran_this_frame;
    static long log_ms;
    static int was_held;
    static uint32_t held_frames, held_ticks;
    static float held_time;
    extern void sample_profiler_frame(void);
    long now = (long)(clock() * 1000 / CLOCKS_PER_SEC);
    uint32_t mgr = g_ecx, fc = MEM32(0x007F9F60u), stamp, keep8, keepc;
    uint32_t n30 = 0, keep50 = 0;
    float keep18 = 0.0f;
    int held;

    /* RECOMP_HITCH_MS: one call per game frame (src/sample_profiler.c). */
    sample_profiler_frame();
    snd_update();
    level_music_wait_for_banks();
    audio_update_logs();
    fl_wait_report();
    if (on < 0) {
        const char *e = getenv("RECOMP_AUDIO_TICK_FIX");
        on = !(e && *e == '0');
        e = getenv("RECOMP_AUDIO_TIME_FIX");
        time_on = !(e && *e == '0');
    }
    held = photo_sound_held();
    if (fc != last_fc) {
        last_fc = fc;
        if (held) {
            held_frames++;
            held_ticks += MEM32(0x007FA208u);
            held_time += MEMF(0x007FA218u);
        } else {
            acc += MEM32(0x007FA208u);
            step_acc += MEMF(0x007FA24Cu);
            time_acc += MEMF(0x007FA218u);
        }
        ran_this_frame = 0;
        frames++;
    }
    if (held) {
        if (!was_held && photo_sound_mode() == 1)
            photo_game_sound_pause(1);
        was_held = 1;
        keep8 = MEM32(0x007FA208u);
        keepc = MEM32(0x007FA20Cu);
        keep18 = MEMF(0x007FA218u);
        keep50 = MEM8(0x007FA250u);
        MEM32(0x007FA208u) = 0;
        MEM32(0x007FA20Cu) = 0;
        MEMF(0x007FA218u) = 0.0f;
        MEM8(0x007FA250u) = 0;
        sub_00017CF0_gen();
        MEM32(0x007FA208u) = keep8;
        MEM32(0x007FA20Cu) = keepc;
        MEMF(0x007FA218u) = keep18;
        MEM8(0x007FA250u) = (uint8_t)keep50;
        return;
    }
    if (was_held) {
        photo_game_sound_pause(0);      /* undoes only what it did */
        fprintf(stderr, "[PHOTO] sound held %u frames: %.1f s of frame time,"
                " %u ticks not given\n", held_frames, held_time, held_ticks);
        was_held = 0;
        held_frames = held_ticks = 0;
        held_time = 0.0f;
    }
    if (!on || ran_this_frame || !xact_va_ok(mgr)) {
        sub_00017CF0_gen();
        return;
    }
    stamp = MEM32(mgr + 0x18u);
    keep8 = MEM32(0x007FA208u);
    keepc = MEM32(0x007FA20Cu);
    MEM32(0x007FA208u) = acc;
    MEM32(0x007FA20Cu) = acc;
    if (time_on) {
        /* At most 1 s, as frame_timer_update clamps the frame time. */
        n30 = step_acc >= 30.0f ? 30u : (uint32_t)step_acc;
        keep18 = MEMF(0x007FA218u);
        keep50 = MEM8(0x007FA250u);
        MEMF(0x007FA218u) = n30 ? (time_acc > 1.0f ? 1.0f : time_acc) : 0.0f;
        MEM8(0x007FA250u) = (uint8_t)n30;
    }
    sub_00017CF0_gen();
    MEM32(0x007FA208u) = keep8;
    MEM32(0x007FA20Cu) = keepc;
    if (time_on) {
        MEMF(0x007FA218u) = keep18;
        MEM8(0x007FA250u) = (uint8_t)keep50;
    }
    if (MEM32(mgr + 0x18u) != stamp) { /* the body ran */
        given += acc;
        runs++;
        acc = 0;
        ran_this_frame = 1;
        if (time_on) {
            step_acc = step_acc >= 30.0f ? 0.0f : step_acc - (float)n30;
            if (n30)
                time_acc = 0.0f;
            given30 += n30;
        }
    }
    if (audio_log_on() && now - log_ms >= 10000) {
        fprintf(stderr, "[AUDIOTICK] t=%ld runs %u frames %u ticks given %u"
                " 30fps frames given %u\n", now, runs, frames, given, given30);
        runs = frames = given = given30 = 0;
        log_ms = now;
    }
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
    /* RECOMP_AUDIO_CALLERS=<cue>[,<cue>...]: who asked for these sounds
     * (InitCue runs inside the game's play call; to-do 100, the slingshot). */
    {
        const char *want = getenv("RECOMP_AUDIO_CALLERS");
        const char *p = strstr(text, "Initialiser for ");
        if (want && *want && p) {
            char name[96], list[512], *tok, *ctx = NULL;
            size_t k = 0;
            p += 16;
            while (p[k] && p[k] != ' ' && k + 1 < sizeof name) {
                name[k] = p[k];
                k++;
            }
            name[k] = 0;
            snprintf(list, sizeof list, "%s", want);
            for (tok = strtok_s(list, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx))
                if (!strcmp(tok, name)) {
                    /* The sound code's frames are big: look 4 KB up. */
                    fprintf(stderr, "[AUDIO] frame %u %s", MEM32(0x007F9F60), name);
                    guest_callers_line("", 1024, 24);
                    break;
                }
        }
    }
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
 * than 1, or it moves more than 20 in one frame. =2 adds [CTRAJ]: every
 * frame for 3 s after a character's animation changes, its position (a
 * death's flight at any frame rate). */
static int char_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CHAR_LOG");
        on = e && *e && *e != '0' ? (atoi(e) > 1 ? atoi(e) : 1) : 0;
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

/* Cutscene guns that fire without a flash (to-do 81). In a cutscene a
 * soldier's shots are script events, not part of his animation: event
 * 0x5F / 2 in the cutscene event switch 0x000599C0 (return 0x0005B783) calls
 * gun_flash_start 0x001A7F50 (cdecl: character, 0.08 s, attachment type
 * 0x43, flash 0) for a combat squirrel (kind 0x58). It shows the gun
 * attachment's _PART_FLASH_1 (shape at attachment +0x2E) and makes a type
 * 0x4E effect (0x001A80B0) that hides it again after the time, on game time
 * [0x007FA21C]. The It's War opening plays the same firing animations
 * (crouchaim_fire, squirrel_dive_shoot) on soldiers the script gives no
 * shots: id 35 diving behind Conker and then at the right of the next cut,
 * and 1A, 31 and 32 crouching earlier on. Their guns kick with no flash; the
 * real game does the same (user, gameplay videos). The script's first shot
 * comes 0-340 ms into a firing animation, then about every 0.2 s.
 * A firing animation (name "fire" or "shoot") that the cutscene starts
 * (char_set_animation 0x002F1790 called from 0x0005637D) on a combat squirrel
 * is tracked; while it plays and no shot of the game's has come for that
 * character, his gun flashes as a script shot would: 0.12 s in, then every
 * 0.17 / 0.23 s by turns. Any gun_flash_start of the game's for him ends it
 * (the script shoots this one). RECOMP_CUTSCENE_FLASH=0 = original;
 * RECOMP_CUTSCENE_FLASH_LOG=1: [CFLASH] lines (tracked, flash, ended).
 * The flash part is a cone along the barrel: with the gun pointing at the
 * camera (35's close-up) it is edge-on and shows nothing. An added flash
 * also gets a muzzle light: the gun's _LOCATOR_BARREL in the world from
 * 0x001A81B0 (cdecl: locator 0, attachment type, 0; eax = character, ebx =
 * the vec3 it writes; returns with it unwritten when the gun has no
 * locator; a gameplay shot 0x000E8CD0 does the same), then a light there
 * from 0x0011E790 (ecx = &position, dl = 0; stack: &{u8 3, u8 fade, u16
 * ticks, u16, u16}, f32 size, f32 r, g, b, 0x7F, 0, 0xFF; the caller pops
 * them), the maker of the game's shot lights. Lights only brighten what is
 * near, they draw nothing themselves: the gameplay muzzle light 0x0019A290
 * (colour 4.0, size 50000, fade 0x18, 4-6 ticks) washed the whole close-up
 * white for most of a second; the combat Tediz' 0x000E72F0 (2.0, size 6-7,
 * fade 0xFF, 3-5) showed nothing. Ours: size 5000, fade 0xFF, 3 ticks, a
 * two-frame pop per shot in the close-up (user: looks good, 2026-11-05);
 * colour 2.0 turned Conker practically white (user: half as bright); the
 * lit surfaces clip, so 1.0 was still near white and 0.5 keeps his colours
 * (RECOMP_CUTSCENE_FLASH_GLOW_COLOUR=<colour> sets it).
 * Only for a gun at the lens (user: the glow just there, and no farther than
 * needed): the muzzle within CUTSCENE_FLASH_GLOW_R (30) world units of the
 * render camera ([0x0084B868] +0x80, as char_cam_inside). It's War opening:
 * 35's close-up 20-22, 35 behind Conker 122, 1A 230, the rest 400-1400.
 * RECOMP_CUTSCENE_FLASH_GLOW=0 = the flash part only,
 * RECOMP_CUTSCENE_FLASH_GLOW_R=<units> the reach. */
static int cutscene_flash_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CUTSCENE_FLASH");
        on = !(e && *e == '0');
    }
    return on;
}

static int cutscene_flash_log(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CUTSCENE_FLASH_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

static int cutscene_flash_glow(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CUTSCENE_FLASH_GLOW");
        on = !(e && *e == '0');
    }
    return on;
}

static float cutscene_flash_glow_r(void)
{
    static float r = -1.0f;
    if (r < 0.0f) {
        const char *e = getenv("RECOMP_CUTSCENE_FLASH_GLOW_R");
        r = e && *e ? (float)atof(e) : 30.0f;
    }
    return r;
}

static float cutscene_flash_glow_colour(void)
{
    static float c = -1.0f;
    if (c < 0.0f) {
        const char *e = getenv("RECOMP_CUTSCENE_FLASH_GLOW_COLOUR");
        c = e && *e ? (float)atof(e) : 0.5f;
    }
    return c;
}

extern void sub_001A81B0(void);
extern void sub_0011E790(void);

/* A muzzle light at character c's gun (attachment type 0x43). */
static void cutscene_flash_light(uint32_t c)
{
    uint32_t eax = g_eax, ebx = g_ebx, ecx = g_ecx, edx = g_edx, esp = g_esp;
    uint32_t pos = (g_esp - 0x40u) & ~0xFu, moved, st = pos + 0x10u, rc;
    float dist = 1e30f;
    MEM32(pos) = MEM32(c + 0x78u);              /* unwritten = still here */
    MEM32(pos + 4u) = MEM32(c + 0x7Cu);
    MEM32(pos + 8u) = MEM32(c + 0x80u);
    g_esp = pos;
    g_esp -= 4; MEM32(g_esp) = 0u;
    g_esp -= 4; MEM32(g_esp) = 0x43u;           /* gun attachment type */
    g_esp -= 4; MEM32(g_esp) = 0u;              /* _LOCATOR_BARREL */
    g_esp -= 4; MEM32(g_esp) = 0u;  /* return address, popped by the callee */
    g_eax = c; g_ebx = pos;
    sub_001A81B0();
    moved = MEM32(pos) != MEM32(c + 0x78u) || MEM32(pos + 4u) != MEM32(c + 0x7Cu)
            || MEM32(pos + 8u) != MEM32(c + 0x80u);
    rc = MEM32(0x0084B868u);
    if (moved && xact_va_ok(rc)) {
        float dx = MEMF(pos) - MEMF(rc + 0x80u), dy = MEMF(pos + 4u) - MEMF(rc + 0x84u),
              dz = MEMF(pos + 8u) - MEMF(rc + 0x88u);
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (moved && (g_eax & 0xFFu) && dist <= cutscene_flash_glow_r()) {
        MEM8(st) = 3u;
        MEM8(st + 1u) = 0xFFu;                      /* fade */
        MEM16(st + 2u) = 3u;                        /* ticks */
        MEM16(st + 4u) = 3u;
        MEM16(st + 6u) = 0u;
        g_esp = pos;
        g_esp -= 4; MEM32(g_esp) = 0xFFu;
        g_esp -= 4; MEM32(g_esp) = 0u;
        g_esp -= 4; MEM32(g_esp) = 0x7Fu;
        g_esp -= 4; MEMF(g_esp) = cutscene_flash_glow_colour();    /* b */
        g_esp -= 4; MEMF(g_esp) = cutscene_flash_glow_colour();    /* g */
        g_esp -= 4; MEMF(g_esp) = cutscene_flash_glow_colour();    /* r */
        g_esp -= 4; MEMF(g_esp) = 5000.0f;          /* size */
        g_esp -= 4; MEM32(g_esp) = st;
        g_esp -= 4; MEM32(g_esp) = 0u;  /* return address, popped by the callee */
        g_ecx = pos; g_edx = 0u;
        sub_0011E790();
    }
    if (cutscene_flash_log())
        fprintf(stderr, "[CFLASH] t=%ld ms id %02X light %s: muzzle %.1f from the camera\n",
                attach_ms(), MEM8(c + 0x10Bu), !moved ? "skipped (no barrel)"
                : dist <= cutscene_flash_glow_r() ? "made" : "not made (far)", dist);
    g_eax = eax; g_ebx = ebx; g_ecx = ecx; g_edx = edx; g_esp = esp;
}

static struct { uint32_t c, anim; float t, next; int n; } s_cflash[8];

static void cutscene_flash_end(int i, const char *why)
{
    if (cutscene_flash_log())
        fprintf(stderr, "[CFLASH] t=%ld ms id %02X anim %u: %s after %d added\n",
                attach_ms(), MEM8(s_cflash[i].c + 0x10Bu), s_cflash[i].anim, why,
                s_cflash[i].n);
    s_cflash[i].c = 0;
}

/* From char_set_animation (sub_002F1790_enter): character c starts animation
 * anim (name at name_va), asked for from `from`. */
static void cutscene_flash_anim(uint32_t c, uint32_t anim, uint32_t name_va,
                                uint32_t from)
{
    char name[96];
    const char *leaf;
    int i, slot = -1;
    for (i = 0; i < 8; i++)
        if (s_cflash[i].c == c)
            cutscene_flash_end(i, "next animation");
    if (from != 0x0005637Du || MEM32(c + 0x68) != 0x58u)
        return;
    char_guest_str(name_va, name, sizeof name);
    _strlwr(name);
    leaf = strrchr(name, '\\');
    leaf = leaf ? leaf + 1 : name;
    if (!strstr(leaf, "fire") && !strstr(leaf, "shoot"))
        return;
    for (i = 0; i < 8 && slot < 0; i++)
        if (!s_cflash[i].c)
            slot = i;
    if (slot < 0)
        return;
    s_cflash[slot].c = c;
    s_cflash[slot].anim = anim;
    s_cflash[slot].t = 0.0f;
    s_cflash[slot].next = 0.12f;
    s_cflash[slot].n = 0;
    if (cutscene_flash_log())
        fprintf(stderr, "[CFLASH] t=%ld ms id %02X anim %u \"%s\": tracked\n",
                attach_ms(), MEM8(c + 0x10Bu), anim, leaf);
}

extern void sub_001A7F50_gen(void);

/* Per frame (sub_00076F10_enter): the flashes due. */
static void cutscene_flash_frame(void)
{
    float dt = MEMF(0x007FA21Cu);
    int i;
    for (i = 0; i < 8; i++) {
        uint32_t c = s_cflash[i].c, eax, ecx, edx, esp;
        if (!c)
            continue;
        if (!MEM32(c + 0x64) || MEM32(c + 0xA4) != s_cflash[i].anim) {
            cutscene_flash_end(i, "animation over");
            continue;
        }
        s_cflash[i].t += dt;
        if (s_cflash[i].t < s_cflash[i].next)
            continue;
        s_cflash[i].n++;
        s_cflash[i].next += (s_cflash[i].n & 1) ? 0.17f : 0.23f;
        if (s_cflash[i].next < s_cflash[i].t)   /* a long frame: one flash */
            s_cflash[i].next = s_cflash[i].t + 0.17f;
        if (cutscene_flash_log())
            fprintf(stderr, "[CFLASH] t=%ld ms id %02X anim %u: flash %d at %.2f s\n",
                    attach_ms(), MEM8(c + 0x10Bu), s_cflash[i].anim, s_cflash[i].n,
                    s_cflash[i].t);
        eax = g_eax; ecx = g_ecx; edx = g_edx; esp = g_esp;
        g_esp -= 4; MEM32(g_esp) = 0u;              /* flash index */
        g_esp -= 4; MEM32(g_esp) = 0x43u;           /* gun attachment type */
        g_esp -= 4; MEM32(g_esp) = 0x3DA3D70Au;     /* 0.08 s */
        g_esp -= 4; MEM32(g_esp) = c;
        g_esp -= 4; MEM32(g_esp) = 0u;  /* return address, popped by the callee */
        sub_001A7F50_gen();
        g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp;
        if (cutscene_flash_glow())
            cutscene_flash_light(c);
    }
}

/* gun_flash_start: a flash of the game's own for a tracked character ends
 * the added ones (the script shoots him). */
void sub_001A7F50(void)
{
    uint32_t owner = MEM32(g_esp + 4u);
    int i;
    for (i = 0; i < 8; i++)
        if (owner && s_cflash[i].c == owner)
            cutscene_flash_end(i, "the game's own shot");
    sub_001A7F50_gen();
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
    if (!xact_va_ok(c) || MEM32(c + 0xA4) == MEM32(g_esp + 8))
        return;                         /* most calls re-ask every frame */
    if (cutscene_flash_on())
        cutscene_flash_anim(c, MEM32(g_esp + 8), MEM32(g_esp + 4),
                            MEM32(g_esp) == 0x002F16F7u ? MEM32(g_esp + 0x30) : 0);
    if (!char_log_on())
        return;
    char_guest_str(MEM32(g_esp + 4), name, sizeof name);
    fprintf(stderr, "[ANIMREQ] t=%ld ms f%u char %08X id %02X kind %X \"%s\" index %u -> %u "
            "a3 %08X a5 %08X a6 %.2f a8 %u pos %.1f %.1f %.1f (ret %08X from %08X)\n", attach_ms(),
            s_char_frame, c, MEM8(c + 0x10B), MEM32(c + 0x68), name, MEM32(c + 0xA4),
            MEM32(g_esp + 8), MEM32(g_esp + 0xC), MEM32(g_esp + 0x14), char_f(g_esp + 0x18),
            MEM32(g_esp + 0x20), char_f(c + 0x78), char_f(c + 0x7C), char_f(c + 0x80),
            MEM32(g_esp), MEM32(g_esp) == 0x002F16F7u ? MEM32(g_esp + 0x30) : 0);
}

/* Gait above 30 fps (to-do 75). 0x00312F80 (from the character update
 * 0x002D6570) sets the player's movement speed [char+0x568]+0x390 =
 * min(2 x the distance moved this frame (pos +0x78 vs last frame's +0x84),
 * top speed char+0x648), and the move direction +0x394/+0x398 from it. Its
 * readers test it against fixed values made for 30 fps frames: 0x003116E0
 * picks the run over the walk above 20 (Conker with the bat: one-handed run
 * vs two-handed walk) and sets the animation rate from it, 0x002FCC30 tests
 * it against 14. Above 30 fps a frame covers less ground, so the run was never
 * picked at full stick. The hook on its normalise call (0x00428323, return
 * 0x0031307C, the first point after the speed is written and before anything
 * uses it) scales the speed to a 30 fps frame: / step (0.5 at 60, 0.25 at 120),
 * capped at the top speed as the game does. Only below step 1, so 30 fps (and
 * its dips, step 2) stay the original. RECOMP_GAIT_FIX=0 = original. */
static int gait_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GAIT_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

void sub_00428323_enter(void)
{
    uint32_t c = g_esi, ctl;
    float step, v, top;
    if (MEM32(g_esp) != 0x0031307Cu || !gait_fix_on() || !xact_va_ok(c)
        || !xact_va_ok(ctl = MEM32(c + 0x568)))
        return;
    step = MEMF(0x007FA24Cu);
    if (!(step > 0.0f && step < 1.0f))
        return;
    v = MEMF(ctl + 0x390) / step;
    top = MEMF(c + 0x648);
    if (v > top)
        v = top;
    if (char_log_on() && MEM8(c + 0x10B) == 1 && !(s_char_frame & 15))
        fprintf(stderr, "[GAIT] t=%ld ms f%u speed %.2f -> %.2f (top %.2f, step %.3f)\n",
                attach_ms(), s_char_frame, MEMF(ctl + 0x390), v, top, step);
    MEMF(ctl + 0x390) = v;
}

/* Ground hazard at 30 Hz (to-do 5, the barn roof). The character update
 * 0x002D6570 calls 0x002D9FD0 (edi = character) once a frame: it moves the
 * hazard value [char+0x1D4] (int16) toward the target [char+0x1D6] from the
 * ground's material by a twelfth of the gap and one more; 0x002E34D0 right
 * after kills the character at -94 or lower. On BarnOutside's thatched roof
 * the target drops far below that and climbs back: eased 30 times a second
 * the value never gets there, eased every frame at 120 it does at once
 * (the user's log: health gone 41 ms after the first straw step). The
 * wrapper lets a character's ease through only on frames where the steps
 * add up to one 30 fps frame, as the console does; at step 1 and above
 * (30 fps, its dips) every call passes, and so does a second call for a
 * character in the same frame (frame count 0x007F9F60 unchanged), as the
 * original would ease twice. A skipped call pops the return address itself
 * (g_esp += 4): without that the guest stack crept 4 bytes a skipped frame
 * and the game froze within a second of leaving the title screen.
 * RECOMP_HAZARD_FIX=0 = original; RECOMP_HAZARD_LOG=1: [HAZARD] when the
 * player's target changes (at most every 10 frames), the lowest value
 * reached since the line before, and the same-frame calls so far. Lead from
 * giacomozama's commit message (addresses only; GPL, no code taken). */
static struct {
    uint32_t fc;
    float acc;
} s_hzc[256];
static struct {
    int on, log, seen;
    uint32_t log_fc, repeats;
    int16_t last_target, low;
} s_hz = { -1, -1, 0, 0, 0, 0, 0 };

extern void sub_002D9FD0_gen(void);
void sub_002D9FD0(void)
{
    uint32_t c = g_edi, fc = MEM32(0x007F9F60u);
    float step = MEMF(0x007FA24Cu);
    int id = MEM8(c + 0x10B), ease = 1;

    if (s_hz.on < 0) {
        const char *e = getenv("RECOMP_HAZARD_FIX");
        s_hz.on = !(e && *e == '0');
        e = getenv("RECOMP_HAZARD_LOG");
        s_hz.log = e && *e == '1';
    }
    if (!s_hz.on || !(step > 0.0f && step < 0.999f)) {
        s_hzc[id].acc = 0.0f;
    } else if (fc == s_hzc[id].fc) {
        s_hz.repeats++;
    } else {
        s_hzc[id].acc += step;
        ease = s_hzc[id].acc >= 0.999f;
        if (ease) {
            s_hzc[id].acc -= 1.0f;
            if (s_hzc[id].acc < 0.0f)
                s_hzc[id].acc = 0.0f;
        }
    }
    s_hzc[id].fc = fc;
    if (ease)
        sub_002D9FD0_gen();
    else
        g_esp += 4;                     /* ret */
    if (id == 1 && !s_hz.seen) {
        s_hz.seen = 1;
        fprintf(stderr, "[HAZARD] ground hazard eased on 30 Hz ticks: %s (player target %d, value %d)\n",
                s_hz.on ? "on" : "off (RECOMP_HAZARD_FIX=0)", (int16_t)MEM16(c + 0x1D6),
                (int16_t)MEM16(c + 0x1D4));
    }
    if (s_hz.log && id == 1) {
        int16_t t = (int16_t)MEM16(c + 0x1D6), v = (int16_t)MEM16(c + 0x1D4);
        if (t != s_hz.last_target && (fc - s_hz.log_fc >= 10 || t == 0)) {
            s_hz.log_fc = fc;
            fprintf(stderr, "[HAZARD] t=%ld ms f%u target %d -> %d, value %d (lowest since last %d, "
                    "step %.3f, same-frame calls %u)\n", attach_ms(), fc, s_hz.last_target, t, v,
                    s_hz.low, step, s_hz.repeats);
            s_hz.last_target = t;
            s_hz.low = v;
        }
        if (v < s_hz.low)
            s_hz.low = v;
    }
}

/* Jumps and falls above 60 fps (2026-11-05). The air update 0x002EBAD0
 * (esi = character; from the character update and 7 other callers) moves a
 * character that is off the ground in 0.1 sub-steps: n = (int)(step
 * [0x007FDB8C] x 10), each one speed [char+0xD0] -= gravity [char+0xD4] x
 * 0.1 and y [char+0x7C] += speed x 0.05. 10 at 30 fps and 5 at 60, but 2.5
 * rounds down to 2 at 120 (3.33 to 3 at 90), so a fifth of every frame's
 * fall was lost: jumps hung ~20 % longer at 120 and a running jump went
 * ~20 % farther (Mrs Bee spot, tap A 876 against 738 ms in the air, a
 * running jump 443 against 364 units; per frame 1.24 off the speed for
 * 6.2 / 4 = 1.55). The wrapper carries the lost fraction per character
 * (id +0x10B): for the call the step reads (n + 0.5) / 10, n the sub-steps
 * due with the carry, then the real step goes back. Only on the air path
 * (the on-ground test the function starts with is false and the kind is not
 * 0xC / 0x22: that path uses the step as a float) and below step 1.
 * RECOMP_AIR_FIX=0 = original. */
static struct {
    int on, seen;
    float carry[256];
    uint32_t fc[256];
} s_air = { -1, 0 };

extern void sub_002EBAD0_gen(void);
void sub_002EBAD0(void)
{
    uint32_t c = g_esi, kind;
    float s = MEMF(0x007FDB8Cu), x;
    int id, n;

    if (s_air.on < 0) {
        const char *e = getenv("RECOMP_AIR_FIX");
        s_air.on = !(e && *e == '0');
    }
    if (!s_air.on || !xact_va_ok(c) || !(MEMF(0x007FA24Cu) < 0.999f) || !(s > 0.0f)) {
        sub_002EBAD0_gen();
        return;
    }
    id = MEM8(c + 0x10Bu);
    kind = MEM32(c + 0x68u);
    if ((MEMF(c + 0xDCu) <= 0.1f && MEMF(c + 0xD0u) <= 0.0f) || kind == 0xCu || kind == 0x22u) {
        s_air.carry[id] = 0.0f;          /* ground path: the step as is */
        sub_002EBAD0_gen();
        return;
    }
    if (MEM32(0x007F9F60u) - s_air.fc[id] > 1u)
        s_air.carry[id] = 0.0f;          /* a new jump or fall */
    s_air.fc[id] = MEM32(0x007F9F60u);
    x = s * 10.0f + s_air.carry[id];
    n = (int)(x + 1e-4f);
    s_air.carry[id] = x - (float)n;
    if (s_air.carry[id] < 0.0f)
        s_air.carry[id] = 0.0f;
    MEMF(0x007FDB8Cu) = ((float)n + 0.5f) / 10.0f;
    sub_002EBAD0_gen();
    MEMF(0x007FDB8Cu) = s;
    if (!s_air.seen && id == 1) {
        s_air.seen = 1;
        fprintf(stderr, "[AIR] fall sub-steps carried between frames: on (step %.3f, %d this frame)\n",
                MEMF(0x007FA24Cu), n);
    }
}

/* Swimming height above 30 fps (2026-11-05). The swim update 0x002D45D0
 * (cdecl, [esp+4] = character, ebp inside) ends, on every path that has
 * water, with the pull to the swimming height (0x002D50D9-0x002D5127): y
 * [char+0x7C] += the vertical speed [ctl+0x28] x the frame's step
 * [0x007FDB8C]; above the target T = water line [char+0x228] - 60 + 14, the
 * height over it is cut to 0.35 (x 0.65 at 0x002D5116, no step) and the
 * speed set to 0; nothing after it moves y. So the cut ran 4 times per 30
 * fps frame at 120: Conker floated 3 units lower (-1736.2 against -1732.9
 * in Mrs Catfish's pond, the step's share of each frame's rise) and a
 * bank's lift was cut away 4x as fast (to -1730.1 against -1721.6 at the
 * same bank), so a shore he climbs out onto at 30 can be a wall at 120
 * (giacomozama found it at 60). The wrapper leaves q = 0.35 / (0.65 x step
 * + 0.35) of the height instead of 0.35 when the cut ran (y over T and the
 * speed 0 after the call): a rise or lift that grows with time then settles
 * where it does at 30 fps, at every rate. Not when there is no water (the
 * line at [0x00648E60], -10000: early return), nor at step 1 and above.
 * RECOMP_SWIM_FIX=0 = original; RECOMP_SWIM_LOG=1: [SWIM] per second for
 * the player (cuts, height over T before / after). */
static struct {
    int on, log, seen;
    unsigned cuts;
    float over_in, over_out;
    long last_ms;
} s_swim = { -1, -1, 0 };

extern void sub_002D45D0_gen(void);
void sub_002D45D0(void)
{
    uint32_t c = MEM32(g_esp + 4u), ctl;
    float step = MEMF(0x007FA24Cu), water, t, over;

    if (s_swim.on < 0) {
        const char *e = getenv("RECOMP_SWIM_FIX");
        s_swim.on = !(e && *e == '0');
        e = getenv("RECOMP_SWIM_LOG");
        s_swim.log = e && *e == '1';
    }
    sub_002D45D0_gen();
    if (!s_swim.on || !(step > 0.0f && step < 0.999f) || !xact_va_ok(c))
        return;
    ctl = MEM32(c + 0x568u);
    water = MEMF(c + 0x228u);
    if (!xact_va_ok(ctl) || water == MEMF(0x00648E60u) || MEMF(ctl + 0x28u) != 0.0f)
        return;
    t = water - MEMF(0x00648E38u) + MEMF(0x00649240u);
    over = MEMF(c + 0x7Cu) - t;
    if (!(over > 0.0f))
        return;
    MEMF(c + 0x7Cu) = t + over / (0.65f * step + 0.35f);
    if (MEM8(c + 0x10Bu) != 1)
        return;
    if (!s_swim.seen) {
        s_swim.seen = 1;
        fprintf(stderr, "[SWIM] swimming height cut per 30 fps frame: on (step %.3f)\n", step);
    }
    if (s_swim.log) {
        long now = attach_ms();
        s_swim.cuts++;
        s_swim.over_in += over;
        s_swim.over_out += over / (0.65f * step + 0.35f);
        if (now - s_swim.last_ms >= 1000) {
            fprintf(stderr, "[SWIM] t=%ld ms %u cuts, height over the target %.2f -> %.2f (mean), "
                    "y %.2f water %.2f step %.3f\n", now, s_swim.cuts, s_swim.over_in / s_swim.cuts,
                    s_swim.over_out / s_swim.cuts, MEMF(c + 0x7Cu), water, step);
            s_swim.last_ms = now;
            s_swim.cuts = 0;
            s_swim.over_in = s_swim.over_out = 0.0f;
        }
    }
}

/* Hit counter at 30 Hz (to-do 5, the Vault's alien at 120). A character's
 * byte [char+0x214] is set to 60 when its attack lands (0x002EDDD0,
 * 0x002EF1D0) and counts down by one a call in 0x002D5540 (from the
 * character update, edi = character) and 0x002E9550 (eax = character): the
 * timers beside it there ([char+0x180], [char+0x21D]) already count down by
 * the frame's 30 Hz frames [0x007FA250], this one by 1 a frame. The
 * alien's script (0x00739E82, op 0x21 at 0x002C6A10) asks it after its tail
 * sweep: still set = "hit him", wait 35 frames (1.17 s) while Conker gets
 * up; 0 = "missed", attack again at once. 60 frames are 2 s at 30 fps but
 * 0.5 s at 120, so at 120 it always looked like a miss and the alien swept
 * Conker again and again before he was up (runs run_20261105_113850 at 30,
 * _114154 at 120; samples of his record every frame: conker-notes
 * 2026-11-05). Here a call's decrement goes through only on frames where
 * the steps add up to one 30 fps frame, per character (as the hazard ease
 * above; [0x007FA250] itself came to 36 a second at 120, so the counter
 * lasted 1.63 s, not 2); every call of a counted frame passes, as the
 * original's; at step 1 and above (30 fps, its dips) as before.
 * RECOMP_HIT30_FIX=0 = original; RECOMP_HIT30_LOG=1: [HIT30] when a counter
 * is set and when it reaches 0. */
static struct {
    int on, log, seen;
    long set_ms[256];
} s_hit30 = { -1, -1, 0 };

/* A character's 30 fps frames, per character id [char+0x10B]: the frames'
 * steps add up, and a frame where they reach one 30 fps frame is a tick for
 * every call in it; at step 1 and above (30 fps, its dips) every frame is.
 * Shared by the hit counter above and the projectile timers (to-do 100). */
static struct {
    uint32_t fc[256];
    float acc[256];
    uint8_t tick[256];
} s_c30;

static int char30_tick(uint8_t id)
{
    uint32_t fc = MEM32(0x007F9F60u);
    float step = MEMF(0x007FA24Cu);

    if (!(step > 0.0f && step < 0.999f)) {
        s_c30.acc[id] = 0.0f;
        s_c30.tick[id] = 1;
    } else if (fc != s_c30.fc[id]) {
        s_c30.acc[id] += step;
        s_c30.tick[id] = s_c30.acc[id] >= 0.999f;
        if (s_c30.tick[id]) {
            s_c30.acc[id] -= 1.0f;
            if (s_c30.acc[id] < 0.0f)
                s_c30.acc[id] = 0.0f;
        }
    }
    s_c30.fc[id] = fc;
    return s_c30.tick[id];
}

static void hit30_after(uint32_t c, uint8_t before)
{
    uint8_t after = MEM8(c + 0x214), id = MEM8(c + 0x10B);
    float step = MEMF(0x007FA24Cu);

    if (s_hit30.on && !char30_tick(id) && before && after == (uint8_t)(before - 1)) {
        MEM8(c + 0x214) = before;       /* not a 30 fps frame for this one */
        after = before;
    }
    if (!s_hit30.seen) {
        s_hit30.seen = 1;
        fprintf(stderr, "[HIT30] hit counters count 30 Hz frames: %s\n",
                s_hit30.on ? "on" : "off (RECOMP_HIT30_FIX=0)");
    }
    if (s_hit30.log && after != before && (after > before || after == 0)) {
        if (after > before)
            s_hit30.set_ms[id] = attach_ms();
        fprintf(stderr, "[HIT30] t=%ld ms f%u char %08X id %02X kind %X %u -> %u%s (%.2f s since set, "
                "step %.3f)\n", attach_ms(), MEM32(0x007F9F60u), c, id, MEM32(c + 0x68), before,
                after, after > before ? " set" : " out", (attach_ms() - s_hit30.set_ms[id]) / 1000.0,
                step);
    }
}

static void hit30_init(void)
{
    if (s_hit30.on < 0) {
        const char *e = getenv("RECOMP_HIT30_FIX");
        s_hit30.on = !(e && *e == '0');
        e = getenv("RECOMP_HIT30_LOG");
        s_hit30.log = e && *e == '1';
    }
}

/* The same function counts three more of its timers by one a call instead
 * of the 30 Hz frames (2026-11-05): [char+0x1B6] (12 at a launch from
 * 0x002F3B70 at 0x002F3DDA: steering at 0.2 in 0x002EB830 while set),
 * [ctl+0x6D] (4 when the character leaves the ground, 0x002EC0F0 at
 * 0x002EC3D1: 0x002F3B70 still lets the jump 0x002F30A0 start while set, a
 * late press at a ledge's edge) and [ctl+0x6A] (halved a call; read by
 * 0x002DF610). At 120 those windows lasted a quarter as long. They keep
 * the hit counter's 30 fps frames: a call's change is put back on the
 * others. Under RECOMP_HIT30_FIX. */
extern void sub_002D5540_gen(void);
void sub_002D5540(void)
{
    uint32_t c = g_edi, ctl;
    uint8_t before, b1b6, b6d, b6a;

    hit30_init();
    if (!xact_va_ok(c) || !xact_va_ok(ctl = MEM32(c + 0x568u))) {
        sub_002D5540_gen();
        return;
    }
    before = MEM8(c + 0x214);
    b1b6 = MEM8(c + 0x1B6u);
    b6d = MEM8(ctl + 0x6Du);
    b6a = MEM8(ctl + 0x6Au);
    sub_002D5540_gen();
    hit30_after(c, before);
    if (!s_hit30.on || char30_tick(MEM8(c + 0x10B)))
        return;
    if (b1b6 && MEM8(c + 0x1B6u) == (uint8_t)(b1b6 - 1))
        MEM8(c + 0x1B6u) = b1b6;
    if (b6d && MEM8(ctl + 0x6Du) == (uint8_t)(b6d - 1))
        MEM8(ctl + 0x6Du) = b6d;
    if (MEM8(ctl + 0x6Au) == (uint8_t)(b6a >> 1))
        MEM8(ctl + 0x6Au) = b6a;
}

extern void sub_002E9550_gen(void);
void sub_002E9550(void)
{
    uint32_t c = g_eax;
    uint8_t before;

    hit30_init();
    if (!xact_va_ok(c)) {
        sub_002E9550_gen();
        return;
    }
    before = MEM8(c + 0x214);
    sub_002E9550_gen();
    hit30_after(c, before);
}

/* Projectile timers at 30 Hz (to-do 100, #4: the Windy slingshot beetles at
 * 120). A projectile is a character in the character array (spawned by
 * 0x003030C0; the slingshot's from its release animation's event 11,
 * 0x00304340) with a lifetime [char+0x17D] = 100. The projectile update
 * 0x002DB350 ([esp+4] = projectile, kind [char+0x476]) takes one off a call
 * (0x002DD04C) and removes it at 0, unless the kind has a frame limit
 * ([char+0xFC]). 100 frames are 3.33 s at 30 fps but 0.83 s at 120, so the
 * pellet was gone before the far beetles (shot to hit 0.37 / 0.77 / 1.07 /
 * 1.43 s: run_20261106_012519 at 30, _013119 at ~90-107: three beetles,
 * _014414 at 120: two). Its other per-call counters: at rest the lifetime
 * goes every other call (toggle [char+0x1B8] gives one back, 0x002DBD98) and
 * [char+0x17B] counts up to 10; a homing one's turn countdown [char+0x17C]
 * (-1 a call above 8, 0x002DC491); and [char+0x179] counts up while its
 * health [char+0xDC] is 0, removed above 30. On frames that are not a 30 fps
 * frame for the projectile their change is put back (a lifetime set in the
 * call, 100 again, 6 after a hit or 1, is kept). RECOMP_SHOT30_FIX=0 =
 * original; RECOMP_SHOT30_LOG=1: [SHOT30] when a lifetime runs out. */
static struct {
    int on, log, seen;
    uint16_t serial[256];
    long born_ms[256];
} s_shot30 = { -1, -1, 0 };

extern void sub_002DB350_gen(void);
void sub_002DB350(void)
{
    uint32_t c = MEM32(g_esp + 4);
    uint8_t id, b17d, b17c, b17b, b179;
    uint16_t b1b8;

    if (s_shot30.on < 0) {
        const char *e = getenv("RECOMP_SHOT30_FIX");
        s_shot30.on = !(e && *e == '0');
        e = getenv("RECOMP_SHOT30_LOG");
        s_shot30.log = e && *e == '1';
    }
    if (!xact_va_ok(c)) {
        sub_002DB350_gen();
        return;
    }
    id = MEM8(c + 0x10B);
    if (MEM16(c + 0xCC) != s_shot30.serial[id]) {   /* a new projectile here */
        s_shot30.serial[id] = MEM16(c + 0xCC);
        s_shot30.born_ms[id] = attach_ms();
    }
    b17d = MEM8(c + 0x17D);
    b17c = MEM8(c + 0x17C);
    b17b = MEM8(c + 0x17B);
    b179 = MEM8(c + 0x179);
    b1b8 = MEM16(c + 0x1B8);
    sub_002DB350_gen();
    if (!s_shot30.seen) {
        s_shot30.seen = 1;
        fprintf(stderr, "[SHOT30] projectile timers count 30 Hz frames: %s\n",
                s_shot30.on ? "on" : "off (RECOMP_SHOT30_FIX=0)");
    }
    if (s_shot30.on && !char30_tick(id)) {
        if (b17d && MEM8(c + 0x17D) == (uint8_t)(b17d - 1))
            MEM8(c + 0x17D) = b17d;
        if (MEM16(c + 0x1B8) == (uint16_t)((b1b8 - 1) & 1) && MEM16(c + 0x1B8) != b1b8)
            MEM16(c + 0x1B8) = b1b8;
        if (b17c > 8 && MEM8(c + 0x17C) == (uint8_t)(b17c - 1))
            MEM8(c + 0x17C) = b17c;
        if (b17b < 10 && MEM8(c + 0x17B) == (uint8_t)(b17b + 1))
            MEM8(c + 0x17B) = b17b;
        if (MEM8(c + 0x179) == (uint8_t)(b179 + 1))
            MEM8(c + 0x179) = b179;
    }
    if (s_shot30.log && b17d && !MEM8(c + 0x17D))
        fprintf(stderr, "[SHOT30] t=%ld ms f%u projectile %08X id %02X kind %u: lifetime out after "
                "%.2f s (step %.3f)\n", attach_ms(), MEM32(0x007F9F60u), c, id, MEM16(c + 0x476),
                (attach_ms() - s_shot30.born_ms[id]) / 1000.0, MEMF(0x007FA24Cu));
}

/* Gun fire rate above 30 fps (2026-11-05, to-do 94, user: fix before
 * 0.2.0). The ammo check 0x00331A80 (fastcall; ~35 callers: Conker's guns,
 * the multiplayer guns, vehicles) holds the fire-rate timers when its last
 * argument is set: [weapon+0xB8] (and +0xC0) gain the frame's dt
 * [0x007FA21C] up to the shot interval [weapon+0xBC] (+0xC4), clamped,
 * and a shot is allowed only once it is reached; after a shot the timer
 * starts again at 0, so what a frame went past the interval is lost. At
 * 30 fps that loss is up to a frame: the SMG (Chemical Warfare) fired 14.5
 * shots a second at 30, 16 at 60, 18 at 120; the multiplayer Sturm21 on
 * auto 15 at 30, 17-18 at 120 (a 50-round clip empty in a 3 s hold at 120,
 * 5 left at 30; user's session). For the call dt is 1/30 on frames where
 * the steps add up to a 30 fps frame and 0 on the others (0 while paused),
 * so the timers move on the console's 30 fps grid and shots come as often
 * as there; the callee 0x003319F0 reads no dt. Counted here, as the hit
 * counter above: the game's own 30 Hz frames [0x007FA250] came to 36 a
 * second at 120 (run _183709: 17.8 shots / s with it). At step 1 and
 * above as before. RECOMP_FIRE30_FIX=0 = original. */
static int s_fire30_on = -1;
static uint32_t s_fire30_fc;
static float s_fire30_acc;
static int s_fire30_tick;

/* The dt the fire timers get this frame, or -1 = leave the frame's dt. */
static float fire30_dt(void)
{
    float step = MEMF(0x007FA24Cu);
    uint32_t fc = MEM32(0x007F9F60u);

    if (s_fire30_on < 0) {
        const char *e = getenv("RECOMP_FIRE30_FIX");
        s_fire30_on = !(e && *e == '0');
        fprintf(stderr, "[FIRE30] gun fire timers on 30 fps frames: %s\n",
                s_fire30_on ? "on" : "off (RECOMP_FIRE30_FIX=0)");
    }
    if (!s_fire30_on || !(step > 0.0f && step < 0.999f)) {
        s_fire30_acc = 0.0f;
        return -1.0f;
    }
    if (MEMF(0x007FA21Cu) == 0.0f)
        return 0.0f;
    if (fc != s_fire30_fc) {
        s_fire30_fc = fc;
        s_fire30_acc += step;
        s_fire30_tick = s_fire30_acc >= 0.999f;
        if (s_fire30_tick) {
            s_fire30_acc -= 1.0f;
            if (s_fire30_acc < 0.0f)
                s_fire30_acc = 0.0f;
        }
    }
    return s_fire30_tick ? 1.0f / 30.0f : 0.0f;
}

extern void sub_00331A80_gen(void);
void sub_00331A80(void)
{
    float d30 = fire30_dt(), dt;

    if (d30 < 0.0f) {
        sub_00331A80_gen();
        return;
    }
    dt = MEMF(0x007FA21Cu);
    MEMF(0x007FA21Cu) = d30;
    sub_00331A80_gen();
    MEMF(0x007FA21Cu) = dt;
}

/* The same for the inventory's per-frame weapon timers 0x00331D90 (cdecl,
 * per weapon slot of 0xD8 from +0x7C): the fire cooldown [slot+0x84] (set
 * to the interval [slot+0x88] by every shot, 0x00331BF0, and waited on by
 * the ammo check above) and the reload, burst and other timers
 * (+0x8C, +0x98, +0xA8, +0xB8) lose dt and are clamped at 0, which is what
 * slowed the SMG at 30 (the timers above alone changed nothing: 100 shots
 * at 120 against 71 at 30 in two 3 s holds); 0x00332940 gets the same dt
 * as its argument. Under RECOMP_FIRE30_FIX. */
extern void sub_00331D90_gen(void);
void sub_00331D90(void)
{
    float d30 = fire30_dt(), dt;

    if (d30 < 0.0f) {
        sub_00331D90_gen();
        return;
    }
    dt = MEMF(0x007FA21Cu);
    MEMF(0x007FA21Cu) = d30;
    sub_00331D90_gen();
    MEMF(0x007FA21Cu) = dt;
}

/* Tail spin above 30 fps (2026-11-05, user: the first part of the spin far
 * faster at 120, the change to the wind-down sharper). The player movement
 * 0x002F3B70 (ecx = character, from the character update) times the spin
 * by the hover counter [ctl+0x45] (+ the 30 Hz frames, 0x002F4731) but
 * moves the animation speed [char+0x120] once a call: below 17 + 0.25
 * (0x002F4769), then 10 % of the way to its target (0x002F477B); both
 * paths then set [ctl+0x54] = 6, written nowhere else in it. Measured
 * (Mrs Bee spot, jump + A held): at 30 1.0 -> 3.75 in 0.4 s, then x0.92 a
 * frame to ~0.65; at 120 the same 0.4 s reached 12.25 and the wind-down
 * ran 4x as fast. The wrapper scales that call's change to the step: the
 * ramp by step, the ease by (1 - 0.9^step) / 0.1, when the call went
 * through that block (speed changed, animation the same, [ctl+0x54] = 6,
 * the counter moved by the 30 Hz frames). RECOMP_SPIN_FIX=0 = original. */
static int s_spin_on = -1;

extern void sub_002F3B70_gen(void);
void sub_002F3B70(void)
{
    uint32_t c = g_ecx, ctl;
    float step = MEMF(0x007FA24Cu), r0, r1;
    uint32_t a0;
    uint8_t p0;

    if (s_spin_on < 0) {
        const char *e = getenv("RECOMP_SPIN_FIX");
        s_spin_on = !(e && *e == '0');
    }
    if (!s_spin_on || !(step > 0.0f && step < 0.999f) || !xact_va_ok(c)
        || !xact_va_ok(ctl = MEM32(c + 0x568u))) {
        sub_002F3B70_gen();
        return;
    }
    r0 = MEMF(c + 0x120u);
    a0 = MEM32(c + 0xA4u);
    p0 = MEM8(ctl + 0x45u);
    sub_002F3B70_gen();
    r1 = MEMF(c + 0x120u);
    if (r1 == r0 || MEM32(c + 0xA4u) != a0 || MEM8(ctl + 0x54u) != 6
        || MEM8(ctl + 0x45u) != (uint8_t)(p0 + MEM8(0x007FA250u)))
        return;
    if (MEM8(ctl + 0x45u) < 0x11u) {
        if (fabsf(r1 - r0 - MEMF(0x00648E14u)) < 1e-4f)
            MEMF(c + 0x120u) = r0 + (r1 - r0) * step;
    } else {
        MEMF(c + 0x120u) = r0 + (r1 - r0) * (1.0f - powf(0.9f, step)) / 0.1f;
    }
}

/* Tails, ragdolls and cloth at 30 Hz (to-do 78). One particle system
 * (dc_squirreltail.cpp: particles 0x28 bytes {+0 last pos, +0xC push,
 * +0x18 mass, +0x1C pos}, solver 0x00098380: pins to bones, distance
 * limits, collision; Verlet step 0x000931E0: velocity = pos - last pos,
 * x 0.98 a call) moves three things, all from the character update's
 * 0x00087BC0, once a frame: 0x0008F2B0 (from 0x00087AA0, cdecl [esp+4] =
 * character index) = the tail chain (vtable 0x005F0DCC; pushes 0x00091D20,
 * three solver rounds 0x00091C20) or, while the controller's +0x3F4 object
 * is type 2, a RAGDOLL_ one (vtable 0x005F0DF0: gravity -500 x dt^2 +
 * Verlet 0x00093C60), both writing the character's skin matrices; and
 * 0x00090FB0 (eax = index, state 0x34) = a cloth grid (0x00092C30,
 * matrices 0x00092CF0). Only the gravity uses dt: the velocity, its 0.98,
 * the pushes and the solver rounds are per call, so above 30 fps all of it
 * ran 2x / 4x as fast a second. A second system moves whole bodies (the
 * Tediz shot dead, phys_Ragdoll.cpp; the user: they fly up at 30, only
 * tip over at 120): 0x000964D0 from 0x002C5500, ragdoll [char+0x3D8] made
 * by 0x00094440 at rest (gravity -2000), stepped by 0x00413FA0 (Verlet
 * 0x00414440 with dt, four solver rounds a call, a per-round callback
 * 0x000974A0); it sets the character's position (+0x78, +0x30) from its
 * root body, blended here too (kind 2). The wrappers run them at 30 Hz like the
 * original: a call goes through on frames where the steps add up to one 30
 * fps frame, with that tick's summed dt, step and 60 Hz ticks in place
 * (their wag timers and countdowns read them); the push's "after a skipped
 * frame" test (solver +0xC0 + 1 != frame count 0x007F9F60, full weight) is
 * kept consecutive (sub_00091D20_enter). The joints a call changed are
 * kept relative to their nearest ancestor it did not change (world space
 * when none), and every frame they are put back between the ticks
 * (p30_curve), on the ancestor's matrix of this frame, so a
 * tail moves with its body and a ragdoll moves smoothly. Before a tick,
 * joints nobody rewrote since are given back the sim's own last output, so
 * it sees what it saw at 30. At step 1 and above (30 fps, its dips) the
 * calls pass straight through. RECOMP_PHYS30=0 = original;
 * RECOMP_PHYS30_LOG=1: [PHYS30] per sim start/change and the player's
 * changed joints' motion per 2 s (works with the fix off too). */
#define P30_CHARS   0x81
#define P30_KINDS   3                  /* 0 tail / ragdoll, 1 cloth, 2 body ragdoll */
#define P30_MAXJ    256
typedef struct {
    uint32_t model, bones, gen;
    int n, have, nw, scans;
    uint32_t last_call;                /* frame count of the last call */
    uint8_t w[P30_MAXJ];               /* changed by the last tick */
    int16_t ref[P30_MAXJ];             /* its ancestor, -1 = world */
    float prev[P30_MAXJ][16], last[P30_MAXJ][16];  /* relative to ref */
    float old[P30_MAXJ][16], older[P30_MAXJ][16];  /* the two ticks before (smooth) */
    float lastw[P30_MAXJ][16];         /* the sim's own last output */
    float mine[P30_MAXJ][16];          /* what we wrote last */
    double motion;                     /* log: summed joint movement */
    float seen[P30_MAXJ][3];
    float pprev[2][3], plast[2][3];    /* kind 2: character +0x78, +0x30 */
    float pold[2][3], polder[2][3];
} p30_slot;
static p30_slot *s_p30_slots[P30_KINDS][P30_CHARS];
static struct {
    uint32_t fc, gen, tick_fc, prev_tick_fc;
    int active, tick, in_call;
    float acc, r, dt_sum, step_sum;
    uint32_t ticks_sum;
    float t_dt, t_step;                /* this tick's sums */
    uint32_t t_ticks;
    long log_ms;
    unsigned calls, skipped;
} s_p30;

static volatile int g_phys30 = -1;     /* F2 physics_30hz; < 0 = not read */

void conker_SetPhys30(int on)
{
    g_phys30 = on != 0;
}

static int phys30_on(void)
{
    if (g_phys30 < 0) {
        const char *e = getenv("RECOMP_PHYS30");
        g_phys30 = !(e && *e == '0');
    }
    return g_phys30;
}

/* Between ticks (to-do 84): 1 = a Catmull-Rom curve through the last four
 * ticks, shown two ticks late (no corner every tick: a running tail swings
 * ~4 times a second and looked jittery with straight lines); 0 = the
 * straight line between the last two, one tick late. RECOMP_PHYS30_SMOOTH. */
static int phys30_smooth(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PHYS30_SMOOTH");
        on = !(e && *e == '0');
    }
    return on;
}

/* One value between ticks: o = older .. l = last, r in [0, 1). */
static float p30_curve(float o, float d, float p, float l, float r)
{
    if (!phys30_smooth())
        return p + (l - p) * r;
    return d + 0.5f * r * ((p - o) + r * ((2.0f * o - 5.0f * d + 4.0f * p - l)
                                          + r * (3.0f * d - o - 3.0f * p + l)));
}

static int phys30_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PHYS30_LOG");
        on = e ? atoi(e) : 0;
    }
    return on;
}

/* Once a frame (the first sim call of a frame): advance the 30 Hz clock. */
static void p30_frame(void)
{
    uint32_t fc = MEM32(0x007F9F60u);
    float step;

    if (fc == s_p30.fc)
        return;
    s_p30.fc = fc;
    step = MEMF(0x007FA24Cu);
    if (!phys30_on() || !(step > 0.0f && step < 0.999f)) {
        if (s_p30.active)
            s_p30.gen++;               /* snapshots stale from here */
        s_p30.active = 0;
        s_p30.tick = 1;
        return;
    }
    if (!s_p30.active) {
        s_p30.active = 1;
        s_p30.gen++;
        s_p30.acc = 1.0f - step;       /* tick at once (this frame's time) */
        s_p30.dt_sum = s_p30.step_sum = 0.0f;
        s_p30.ticks_sum = 0;
    }
    s_p30.acc += step;
    s_p30.step_sum += step;
    s_p30.dt_sum += MEMF(0x007FA21Cu);
    s_p30.ticks_sum += MEM32(0x007FA20Cu);
    s_p30.tick = s_p30.acc >= 0.999f;
    if (s_p30.tick) {
        s_p30.t_dt = s_p30.dt_sum;
        s_p30.t_step = s_p30.step_sum;
        s_p30.t_ticks = s_p30.ticks_sum;
        s_p30.dt_sum = s_p30.step_sum = 0.0f;
        s_p30.ticks_sum = 0;
        s_p30.acc -= 1.0f;
        if (s_p30.acc >= 0.999f || s_p30.acc < 0.0f)
            s_p30.acc = 0.0f;
        s_p30.prev_tick_fc = s_p30.tick_fc;
        s_p30.tick_fc = fc;
    }
    s_p30.r = s_p30.acc;
}

static void m4_mul(float *o, const float *a, const float *b)
{
    float t[16];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j]
                         + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}

/* General 4x4 inverse (cofactors); 0 when singular. */
static int m4_inv(float *o, const float *m)
{
    double inv[16], det;
    int i;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-20))
        return 0;
    for (i = 0; i < 16; i++)
        o[i] = (float)(inv[i] / det);
    return 1;
}

/* The character's skin matrices and joint count (u16 [char+0x3C4]+0x10;
 * joint records [[char+0x4EC + lod*4]+0x34], 0x24 each, u16 parent at
 * +0x18). 0 when it has none. */
static int p30_char(uint32_t c, uint32_t *bones, uint32_t *model, uint32_t *joints)
{
    uint32_t def = MEM32(c + 0x3C4u), lodm;
    int n;
    *bones = MEM32(c + 0x3C8u);
    if (!xact_va_ok(*bones) || !xact_va_ok(def))
        return 0;
    n = MEM16(def + 0x10u);
    lodm = MEM32(c + 0x4ECu + 4u * MEM8(c + 0x3A8u));
    *joints = xact_va_ok(lodm) ? MEM32(lodm + 0x34u) : 0;
    *model = def;
    return n > 0 && n <= P30_MAXJ ? n : 0;
}

static float *p30_bone(uint32_t bones, int j)
{
    return (float *)XBOX_PTR(bones + (uint32_t)j * 64u);
}

/* The joint shown this frame: between the ticks by r (p30_curve), on the
 * ancestor's matrix now. */
static void p30_show(p30_slot *s, uint32_t bones, int j)
{
    float m[16];
    int k;
    for (k = 0; k < 16; k++)
        m[k] = p30_curve(s->older[j][k], s->old[j][k], s->prev[j][k], s->last[j][k],
                         s_p30.r);
    if (s->ref[j] >= 0)
        m4_mul(m, m, p30_bone(bones, s->ref[j]));
    memcpy(p30_bone(bones, j), m, 64);
    memcpy(s->mine[j], m, 64);
}

/* After the sim ran: which joints it changed (against `before`), their
 * ancestors, and the new tick kept relative to them. */
static void p30_take(p30_slot *s, uint32_t c, uint32_t bones, uint32_t joints,
                     const float (*before)[16], int fresh)
{
    float inv[16];
    int j, a, guard, nw = 0;
    for (j = 0; j < s->n; j++) {        /* bit 0 now, bit 1 the last tick */
        s->w[j] = (uint8_t)(((s->w[j] & 1) && !fresh) << 1)
                | (memcmp(p30_bone(bones, j), before[j], 64) != 0);
        nw += s->w[j] & 1;
    }
    for (j = 0; j < s->n; j++) {
        if (!(s->w[j] & 1))
            continue;
        a = joints ? (int)MEM16(joints + (uint32_t)j * 0x24u + 0x18u) : 0xFFFF;
        for (guard = 0; a < s->n && (s->w[a] & 1) && guard < P30_MAXJ; guard++)
            a = (int)MEM16(joints + (uint32_t)a * 0x24u + 0x18u);
        s->ref[j] = (int16_t)(a < s->n && a != j ? a : -1);
        memcpy(s->lastw[j], p30_bone(bones, j), 64);
        if (s->w[j] & 2) {
            memcpy(s->older[j], s->old[j], 64);
            memcpy(s->old[j], s->prev[j], 64);
            memcpy(s->prev[j], s->last[j], 64);
        }
        if (s->ref[j] >= 0 && m4_inv(inv, p30_bone(bones, s->ref[j])))
            m4_mul(s->last[j], p30_bone(bones, j), inv);
        else {
            s->ref[j] = -1;
            memcpy(s->last[j], p30_bone(bones, j), 64);
        }
        if (!(s->w[j] & 2)) {          /* new to the sim: no tick before */
            memcpy(s->prev[j], s->last[j], 64);
            memcpy(s->old[j], s->last[j], 64);
            memcpy(s->older[j], s->last[j], 64);
        }
    }
    for (j = 0; j < s->n; j++)
        s->w[j] &= 1;
    if (phys30_log_on() && nw != s->nw)
        fprintf(stderr, "[PHYS30] t=%ld char %08X id %02X kind %X: sim joints %d -> %d"
                " (of %d)\n", attach_ms(), c, MEM8(c + 0x10Bu), MEM32(c + 0x68u), s->nw,
                nw, s->n);
    s->nw = nw;
}

/* Log: the player's sim joints' movement (on their ancestors) per 2 s. */
static void p30_log_motion(p30_slot *s, uint32_t c, uint32_t bones)
{
    float inv[16], rel[16];
    int j, k, tip = -1;
    long now;
    if (MEM8(c + 0x10Bu) != 1)
        return;
    for (j = 0; j < s->n; j++) {
        if (!s->w[j])
            continue;
        memcpy(rel, p30_bone(bones, j), 64);
        if (s->ref[j] >= 0 && m4_inv(inv, p30_bone(bones, s->ref[j])))
            m4_mul(rel, rel, inv);
        for (k = 0; k < 3; k++) {
            s->motion += fabs(rel[12 + k] - s->seen[j][k]);
            s->seen[j][k] = rel[12 + k];
        }
        tip = j;
    }
    if (phys30_log_on() >= 2 && tip >= 0)  /* the last sim joint, every call */
        fprintf(stderr, "[P30TIP] t=%ld f%u tip %d: %.2f %.2f %.2f pos %.1f %.1f %.1f\n",
                attach_ms(), s_p30.fc, tip, s->seen[tip][0], s->seen[tip][1],
                s->seen[tip][2], MEMF(c + 0x78u), MEMF(c + 0x7Cu), MEMF(c + 0x80u));
    now = attach_ms();
    if (now - s_p30.log_ms >= 2000) {
        fprintf(stderr, "[PHYS30] t=%ld player sim joints %d: movement %.1f units/s,"
                " calls %u (%u shown from the last tick), fix %s\n", now, s->nw,
                s->motion * 1000.0 / (double)(now - s_p30.log_ms + 1), s_p30.calls,
                s_p30.skipped, s_p30.active ? "on" : "off");
        s->motion = 0.0;
        s_p30.calls = s_p30.skipped = 0;
        s_p30.log_ms = now;
    }
}

/* Log: a hash of every live character's skin matrices, to find whose bones
 * a sim that changed none of its own wrote. */
static void p30_hash_all(uint32_t *h)
{
    uint32_t base = MEM32(0x0084A5F8u), bones, model, joints;
    int i, n;
    for (i = 0; i < P30_CHARS; i++) {
        const uint8_t *p;
        size_t k, len;
        uint32_t x = 2166136261u;
        h[i] = 0;
        if (!MEM32(base + (uint32_t)i * 0x6D0u + 0x64u))
            continue;
        n = p30_char(base + (uint32_t)i * 0x6D0u, &bones, &model, &joints);
        if (!n)
            continue;
        p = (const uint8_t *)XBOX_PTR(bones);
        len = (size_t)n * 64u;
        for (k = 0; k < len; k++)
            x = (x ^ p[k]) * 16777619u;
        h[i] = x | 1u;
    }
}

/* Kind 2 (the body ragdoll) also sets the character's position from its
 * root body: +0x78 and the copy at +0x30, kept and blended like the joints
 * (world space). take = after a tick. */
static void p30_pos(p30_slot *s, uint32_t c, int take, int fresh)
{
    static const uint32_t at[2] = { 0x78u, 0x30u };
    int i, k;
    for (i = 0; i < 2; i++)
        for (k = 0; k < 3; k++) {
            uint32_t va = c + at[i] + 4u * (uint32_t)k;
            if (take) {
                s->polder[i][k] = fresh ? MEMF(va) : s->pold[i][k];
                s->pold[i][k] = fresh ? MEMF(va) : s->pprev[i][k];
                s->pprev[i][k] = fresh ? MEMF(va) : s->plast[i][k];
                s->plast[i][k] = MEMF(va);
            }
            MEMF(va) = p30_curve(s->polder[i][k], s->pold[i][k], s->pprev[i][k],
                                 s->plast[i][k], s_p30.r);
        }
}

/* One sim call for character `idx`; `gen` the original, `skip` its return
 * (stack bytes to drop). */
static void p30_call(int kind, uint32_t idx, void (*gen)(void), uint32_t skip)
{
    static float before[P30_MAXJ][16];
    static uint32_t h0[P30_CHARS], h1[P30_CHARS];
    uint32_t c, bones, model, joints, keep_ticks, keep_ticks2;
    float keep_dt, keep_step;
    p30_slot *s;
    int n, j, fresh, scan;

    p30_frame();
    if (idx >= P30_CHARS || (!s_p30.active && !phys30_log_on())) {
        gen();
        return;
    }
    c = MEM32(0x0084A5F8u) + idx * 0x6D0u;
    n = p30_char(c, &bones, &model, &joints);
    if (!n) {
        gen();
        return;
    }
    s = s_p30_slots[kind][idx];
    if (!s) {
        s = s_p30_slots[kind][idx] = (p30_slot *)calloc(1, sizeof *s);
        if (!s) {
            gen();
            return;
        }
        if (phys30_log_on())
            fprintf(stderr, "[PHYS30] t=%ld new sim (kind %d) for char %u id %02X kind %X,"
                    " %d joints\n", attach_ms(), kind, idx, MEM8(c + 0x10Bu), MEM32(c + 0x68u), n);
    }
    fresh = !s->have || s->gen != s_p30.gen || s->model != model || s->bones != bones
            || s->n != n || s->last_call + 8u < s_p30.fc;
    s->last_call = s_p30.fc;
    s_p30.calls++;
    if (fresh) {
        memset(s->w, 0, sizeof s->w);
        s->model = model;
        s->bones = bones;
        s->n = n;
        s->gen = s_p30.gen;
        s->nw = 0;
    }
    if (!s_p30.active) {               /* log only */
        memcpy(before, p30_bone(bones, 0), (size_t)n * 64u);
        gen();
        p30_take(s, c, bones, joints, (const float (*)[16])before, 1);
        s->have = 1;
        p30_log_motion(s, c, bones);
        return;
    }
    /* A camera cut ([0x00762E40], or the character's +0x546) resets a tail
     * for that one frame: step now, as a new start. A sim that changed none
     * of this character's joints (output elsewhere) is left as it was. */
    if (!s_p30.tick && (MEM8(0x00762E40u) || MEM8(c + 0x546u) || (!s->nw && kind != 2)))
        fresh = 1;
    if (kind == 2 && !s_p30.tick && !s->have) {
        g_esp += skip;                 /* a new body ragdoll waits for a tick: */
        return;                        /* its launch is one 30 Hz step's */
    }
    if (!s_p30.tick && !fresh) {       /* between ticks: show, no step */
        for (j = 0; j < n; j++)
            if (s->w[j])
                p30_show(s, bones, j);
        if (kind == 2)
            p30_pos(s, c, 0, 0);
        s_p30.skipped++;
        g_esp += skip;
        if (phys30_log_on())
            p30_log_motion(s, c, bones);
        return;
    }
    if (!fresh)                        /* nobody rewrote it: its own last output */
        for (j = 0; j < n; j++)
            if (s->w[j] && !memcmp(p30_bone(bones, j), s->mine[j], 64))
                memcpy(p30_bone(bones, j), s->lastw[j], 64);
    memcpy(before, p30_bone(bones, 0), (size_t)n * 64u);
    scan = phys30_log_on() && s->have && !s->nw && s->scans < 3;
    if (scan)
        p30_hash_all(h0);
    keep_dt = MEMF(0x007FA21Cu);
    keep_step = MEMF(0x007FA24Cu);
    keep_ticks = MEM32(0x007FA20Cu);
    keep_ticks2 = MEM32(0x007FA208u);
    if (s_p30.tick) {
        MEMF(0x007FA21Cu) = s_p30.t_dt;
        MEMF(0x007FA24Cu) = s_p30.t_step;
        MEM32(0x007FA20Cu) = s_p30.t_ticks;
        MEM32(0x007FA208u) = s_p30.t_ticks;
    }
    s_p30.in_call = s_p30.tick;
    gen();
    s_p30.in_call = 0;
    MEMF(0x007FA21Cu) = keep_dt;
    MEMF(0x007FA24Cu) = keep_step;
    MEM32(0x007FA20Cu) = keep_ticks;
    MEM32(0x007FA208u) = keep_ticks2;
    if (scan) {
        char who[256];
        int len = 0;
        p30_hash_all(h1);
        who[0] = 0;
        for (j = 0; j < P30_CHARS && len < 200; j++)
            if (h0[j] != h1[j])
                len += sprintf(who + len, " %d (id %02X kind %X)", j,
                               MEM8(MEM32(0x0084A5F8u) + (uint32_t)j * 0x6D0u + 0x10Bu),
                               MEM32(MEM32(0x0084A5F8u) + (uint32_t)j * 0x6D0u + 0x68u));
        s->scans++;
        fprintf(stderr, "[PHYS30] t=%ld char %u id %02X kind %X (sim kind %d) changed none of"
                " its %d joints; bones changed:%s\n", attach_ms(), idx, MEM8(c + 0x10Bu),
                MEM32(c + 0x68u), kind, n, who[0] ? who : " none");
    }
    p30_take(s, c, bones, joints, (const float (*)[16])before, fresh);
    s->have = 1;
    for (j = 0; j < n; j++)
        if (s->w[j])
            p30_show(s, bones, j);
    if (kind == 2)
        p30_pos(s, c, 1, fresh);
    if (phys30_log_on())
        p30_log_motion(s, c, bones);
}

/* The tail chain's push (thiscall, ecx = the tail sim, [ecx+4] = solver):
 * at 30 its solver's frame stamp +0xC0 is last frame's on the first round;
 * here the last tick's, so it is made last frame's. */
void sub_00091D20_enter(void)
{
    uint32_t solver;
    if (!s_p30.in_call || !xact_va_ok(g_ecx) || !xact_va_ok(solver = MEM32(g_ecx + 4u)))
        return;
    if (MEM32(solver + 0xC0u) == s_p30.prev_tick_fc && s_p30.prev_tick_fc)
        MEM32(solver + 0xC0u) = s_p30.fc - 1u;
}

extern void sub_0008F2B0_gen(void);
void sub_0008F2B0(void)
{
    p30_call(0, MEM32(g_esp + 4u), sub_0008F2B0_gen, 4u);   /* cdecl ret */
}

extern void sub_00090FB0_gen(void);
void sub_00090FB0(void)
{
    p30_call(1, g_eax, sub_00090FB0_gen, 4u);
}

/* The body ragdoll's start (0x00094440, 5 stack arguments, ret 0x14, from
 * 0x002C5500): its bodies start at rest, then 0x00096A10 launches them,
 * pos = last pos + velocity x dt [0x007FA21C], a displacement per step. At
 * 30 Hz steps that has to be one 30 fps frame's (1/30 s), not this frame's:
 * with 1/120 the Tediz left with a quarter of their speed. */
extern void sub_00094440_gen(void);
void sub_00094440(void)
{
    float keep = MEMF(0x007FA21Cu);
    int over;
    p30_frame();
    over = s_p30.active && keep > 0.0f;
    if (over)
        MEMF(0x007FA21Cu) = 1.0f / 30.0f;
    sub_00094440_gen();
    if (over)
        MEMF(0x007FA21Cu) = keep;
}

/* The body ragdoll's step (phys_Ragdoll.cpp; eax = the ragdoll [char+0x3D8],
 * [esp+4] = the character, [esp+8] = its position +0x78; ret 8), from the
 * character update's 0x002C5500. */
extern void sub_000964D0_gen(void);
void sub_000964D0(void)
{
    uint32_t c = MEM32(g_esp + 4u), base = MEM32(0x0084A5F8u);
    uint32_t idx = c >= base && (c - base) % 0x6D0u == 0 ? (c - base) / 0x6D0u : P30_CHARS;
    p30_call(2, idx, sub_000964D0_gen, 12u);
}

/* Main Menu Speed (to-do 76). The main menu's camera 0x001FD0F0 (level
 * 0x1D) turns from one of its 6 boards to the next with a progress
 * [0x00776218] (0..1) moved each frame by ticks x speed [0x0077621C]
 * (+-0.05; -0.025 for its own automatic turns) x 0.5, where ticks =
 * [0x00776214], the front end's copy of the 60 Hz ticks clamped to 1..4
 * (0x001FC390). Above 60 a frame has under one tick (1 0 1 0 at 120) but
 * counts as one, so the turn ran 2x as fast at 120 (40 frames, 375 ms
 * against 20 frames, 667 ms at 30). Held left/right repeats once a turn
 * ends, so the scrolling ran fast too. The wrapper scales the speed by
 * this frame's real share of a tick (step x 2) over the clamped count for
 * the call, only when that is below 1, and puts it back unless the call
 * ended the turn (0). The zoom into / out of a board after A / B (to-do
 * 102, user 2026-11-06: under the same option) is the same: zoom
 * [0x00776220] (0..1) += ticks x speed [0x00776224] x 0.75, the speed set
 * on the press and zeroed at either end; scaled the same way and put back
 * unless the call set a new one. main_menu_speed = 0 / RECOMP_MENU_SPEED=0
 * = as before; RECOMP_MENU_LOG=1 adds a [MENUSPEED] line per turn / zoom. */
static volatile int g_menu_speed = -1;

void conker_SetMainMenuSpeed(int on)
{
    g_menu_speed = on != 0;
}

/* For the overlay's quit question (to-do 28): 1 while the main menu shows
 * its boards with none zoomed into and none turning (level 0x1D, front-end
 * state [0x00776574] 3, zoom [0x00776220] and its speed [0x00776224] 0,
 * turn speed [0x0077621C] 0), no scene playing ([0x0076F0E0] != 1) and the
 * Live box closed ([0x00862D08]). There frontend_menu_update 0x001FA7D0
 * runs only the idle timer, so B and Esc mean nothing to the game. Any
 * thread. */
int conker_MainMenuBoard(void)
{
    return MEM32(0x007FA1F8u) == 0x1D && MEM32(0x00776574u) == 3
        && MEMF(0x00776220u) == 0.0f && MEMF(0x00776224u) == 0.0f
        && MEMF(0x0077621Cu) == 0.0f && MEM8(0x0076F0E0u) != 1
        && MEM8(0x00862D08u) == 0;
}
extern void sub_001FD0F0_gen(void);
void sub_001FD0F0(void)
{
    static int log = -1;
    float v = MEMF(0x0077621Cu), z = MEMF(0x00776224u), k = 1.0f, vk, zk;
    int ticks = (int)MEM32(0x00776214u);
    if (g_menu_speed < 0) {
        const char *e = getenv("RECOMP_MENU_SPEED");
        g_menu_speed = !(e && *e == '0');
    }
    if (g_menu_speed && (v != 0.0f || z != 0.0f) && MEM32(0x007FA1F8u) == 0x1D && ticks > 0)
        k = MEMF(0x007FA24Cu) * 2.0f / (float)ticks;
    if (!(k > 0.0f && k < 1.0f)) {
        sub_001FD0F0_gen();
        return;
    }
    if (log < 0) {
        const char *e = getenv("RECOMP_MENU_LOG");
        log = e && *e && *e != '0';
    }
    if (log && v != 0.0f && (MEMF(0x00776218u) == 1.0f || MEMF(0x00776218u) == 0.0f))
        fprintf(stderr, "[MENUSPEED] turn speed %.3f x %.3f (ticks %d, step %.3f)\n", v, k,
                ticks, MEMF(0x007FA24Cu));
    if (log && z != 0.0f && (MEMF(0x00776220u) == 1.0f || MEMF(0x00776220u) == 0.0f))
        fprintf(stderr, "[MENUSPEED] zoom speed %.3f x %.3f (ticks %d, step %.3f)\n", z, k,
                ticks, MEMF(0x007FA24Cu));
    vk = v * k;
    zk = z * k;
    MEMF(0x0077621Cu) = vk;
    MEMF(0x00776224u) = zk;
    sub_001FD0F0_gen();
    if (v != 0.0f && MEMF(0x0077621Cu) != 0.0f)
        MEMF(0x0077621Cu) = v;
    if (z != 0.0f && MEMF(0x00776224u) == zk)
        MEMF(0x00776224u) = z;
}

/* Sub-menu Speed (to-do 102, user 2026-11-06: on by default, its own
 * option). The front end's other users of the ticks [0x00776214] (clamped
 * to 1..4 by 0x001FC390, so above 60 a frame with no 60 Hz tick counts as
 * one) are fades: the chapter name's cross-fade on left / right
 * [0x0077655F] (+-32 a tick, the chapter list page 0x001FAF70) and the
 * screen fade [0x006BCFE8] (- rate [0x006BCFE9] a tick: the menu update
 * 0x001FA7D0 and the states 0x001FBC10, 0x001FE980, 0x001FE9F0). All of
 * them multiply, so for these calls the ticks are the real count
 * [0x007FA20C] when that is 0, and put back after (the main menu camera,
 * called from the level, keeps its own scaling above). 30 and 60 fps always
 * have a tick a frame, so they are untouched. The scene list's held-stick
 * repeat (GetTickCount, 0x00249FD0) and the UI pages' animations (dt) were
 * already real time (2026-11-06 probes). submenu_speed = 0 /
 * RECOMP_SUBMENU_SPEED=0 = as before. */
static volatile int g_submenu_speed = -1;

void conker_SetSubmenuSpeed(int on)
{
    g_submenu_speed = on != 0;
}

static void submenu_call(void (*gen)(void))
{
    uint32_t keep = MEM32(0x00776214u);
    if (g_submenu_speed < 0) {
        const char *e = getenv("RECOMP_SUBMENU_SPEED");
        g_submenu_speed = !(e && *e == '0');
    }
    if (!g_submenu_speed || keep != 1u || (int)MEM32(0x007FA20Cu) > 0) {
        gen();
        return;
    }
    MEM32(0x00776214u) = 0;
    gen();
    MEM32(0x00776214u) = keep;
}

extern void sub_001FA7D0_gen(void);
void sub_001FA7D0(void) { submenu_call(sub_001FA7D0_gen); }
extern void sub_001FAF70_gen(void);
void sub_001FAF70(void) { submenu_call(sub_001FAF70_gen); }
extern void sub_001FBC10_gen(void);
void sub_001FBC10(void) { submenu_call(sub_001FBC10_gen); }
extern void sub_001FE980_gen(void);
void sub_001FE980(void) { submenu_call(sub_001FE980_gen); }
extern void sub_001FE9F0_gen(void);
void sub_001FE9F0(void) { submenu_call(sub_001FE9F0_gen); }

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
    static struct { uint32_t live, anim; float x, y, z; long changed; } last[0x81];
    uint32_t base = MEM32(0x0084A5F8u), c;
    int i;
    long now = attach_ms();
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
        if (last[i].live && anim != last[i].anim && MEM8(c + 0x10B) != 1) {
            /* RECOMP_WATCH_DEATH=<anim>: the first time a character (not the
             * player) starts that animation, RECOMP_WATCH on its height
             * (+0x7C), to find what moves it (a death's flight). */
            static int watch_anim = -2;
            if (watch_anim == -2) {
                const char *e = getenv("RECOMP_WATCH_DEATH");
                watch_anim = e && *e ? atoi(e) : -1;
            }
            if (watch_anim >= 0 && anim == (uint32_t)watch_anim) {
                char env[96];
                extern void xbox_WatchInit(void);
                snprintf(env, sizeof env, "RECOMP_WATCH=0x%08X", c + 0x7Cu);
                _putenv(env);
                fprintf(stderr, "[CHAR] t=%ld char #%02X anim %u: arming %s\n", now, i, anim,
                        env);
                xbox_WatchInit();
                watch_anim = -1;
            }
            last[i].changed = now;
        }
        if (char_log_on() >= 2 && last[i].changed && now - last[i].changed < 3000)
            fprintf(stderr, "[CTRAJ] t=%ld f%u #%02X id %02X kind %X anim %u state %X pos"
                    " %.2f %.2f %.2f step %.3f\n", now, s_char_frame, i, MEM8(c + 0x10B),
                    MEM32(c + 0x68), anim, MEM32(c + 0x64), x, y, z, MEMF(0x007FA24Cu));
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
    if (cutscene_flash_on())
        cutscene_flash_frame();
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

/* ── Fire imp's back (to-do 87; RECOMP_IMP_FIX, on) ──
 * The FireImp model (main menu fireplace) has no triangles over a kite of
 * four vertices in the middle of its back, so the fire behind shows through;
 * the original's data (the Xbox One version shows it too). The model is one
 * recorded pushbuffer (size 0x60F8); its first body piece is a strip of 1084
 * indices from vertex 332, and the kite's corners 121, 44, 251 (top) and 38
 * (bottom) are all in it (conker-notes 2026-11-05). Fix: the BEGIN_END(0)
 * closing that strip becomes a JUMP to a block of ours that closes it, draws
 * the two triangles (same vertex arrays, same bone constants, wound like
 * their neighbours) and jumps back past the two words. Found by the strip's
 * indices, so a buffer is patched again whenever the model is loaded anew. */
#define IMP_PB_SIZE    0x60F8u
#define IMP_STRIP_N    1084u
#define IMP_STRIP_HASH 0xE7E194F6u          /* FNV-1a of the strip's indices */
#define IMP_SLOTS      16u
#define IMP_SLOT_BYTES 64u

extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);

static uint32_t s_imp_block;               /* IMP_SLOTS patch blocks (contiguous) */
static uint32_t s_imp_slot_end[IMP_SLOTS]; /* the strip end each slot returns to */
static uint32_t s_imp_next_slot;

static int imp_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_IMP_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

static int imp_block_va(uint32_t va)
{
    return s_imp_block && va >= s_imp_block
        && va < s_imp_block + IMP_SLOTS * IMP_SLOT_BYTES;
}

/* The BEGIN_END(0) word closing the body strip, or 0 when this is not the
 * FireImp's buffer (or it is patched already). */
static uint32_t imp_find_strip_end(uint32_t data, uint32_t size)
{
    uint32_t end = data + size - 4, va = data, w, n = 0, h = 0, first = 0;
    int in_strip = 0;

    while (va < end) {
        uint32_t m, cnt, i;
        w = MEM32(va);
        if ((w & 3u) == 1u) {                   /* a jump inside the buffer */
            uint32_t t = (w & 0x0FFFFFFCu) | (data & 0xF0000000u);
            if (t <= va || t > end)
                return 0;
            va = t;
            continue;
        }
        if ((w & 0x00030003u) != 0u)
            return 0;
        m = w & 0x1FFFu;
        cnt = (w >> 18) & 0x7FFu;
        if (m == 0x17FCu && cnt == 1u) {        /* BEGIN_END */
            uint32_t prim = MEM32(va + 4);
            if (prim == 6u) {
                in_strip = 1;
                n = 0;
                h = 2166136261u;
            } else if (prim == 0u && in_strip) {
                if (n == IMP_STRIP_N && first == 332u && h == IMP_STRIP_HASH)
                    return va;
                in_strip = 0;
            }
        } else if (in_strip && (m == 0x1800u || m == 0x1808u)) {
            for (i = 0; i < cnt; i++) {
                uint32_t d = MEM32(va + 4 + i * 4u);
                uint32_t v[2] = { d & 0xFFFFu, d >> 16 };
                uint32_t k, per = m == 0x1800u ? 2u : 1u;
                if (m == 0x1808u)
                    v[0] = d;
                for (k = 0; k < per; k++) {
                    if (n == 0)
                        first = v[k];
                    h = (h ^ v[k]) * 16777619u;
                    n++;
                }
            }
        }
        va += 4 + cnt * 4u;
    }
    return 0;
}

static void imp_fix(uint32_t pb)
{
    uint32_t data, e, slot, b, i;
    static const uint16_t tri[6] = { 121, 44, 38, 44, 251, 38 };

    if (!imp_fix_on() || !xact_va_ok(pb) || MEM32(pb + 12) != IMP_PB_SIZE)
        return;
    data = MEM32(pb + 4);
    if (data < 0x80000000u || !xact_va_ok(data))
        return;
    e = imp_find_strip_end(data, IMP_PB_SIZE);
    if (!e)
        return;                                 /* not the imp, or done */
    if (!s_imp_block) {
        s_imp_block = xbox_ContiguousAlloc(IMP_SLOTS * IMP_SLOT_BYTES, 4096);
        if (!s_imp_block)
            return;
    }
    for (slot = 0; slot < IMP_SLOTS && s_imp_slot_end[slot] != e; slot++)
        ;
    if (slot == IMP_SLOTS)
        slot = s_imp_next_slot++ % IMP_SLOTS;
    b = s_imp_block + slot * IMP_SLOT_BYTES;
    MEM32(b + 0) = 0x000417FCu;                 /* end the strip */
    MEM32(b + 4) = 0;
    MEM32(b + 8) = 0x000417FCu;                 /* triangles */
    MEM32(b + 12) = 5;
    MEM32(b + 16) = 0x400C1800u;                /* ARRAY_ELEMENT16 x3, 6 indices */
    for (i = 0; i < 3; i++)
        MEM32(b + 20 + i * 4u) = tri[2 * i] | (uint32_t)tri[2 * i + 1] << 16;
    MEM32(b + 32) = 0x000417FCu;
    MEM32(b + 36) = 0;
    MEM32(b + 40) = ((e + 8u) & 0x0FFFFFFFu) | 1u;   /* back past the two words */
    s_imp_slot_end[slot] = e;
    _WriteBarrier();                            /* x86 keeps stores in order */
    MEM32(e) = (b & 0x0FFFFFFFu) | 1u;          /* the executor may be reading */
    fprintf(stderr, "[IMPFIX] fire imp's back filled: pb %08X data %08X strip end %08X"
            " -> %08X\n", pb, data, e, b);
}

/* D3DDevice_RunPushBuffer(pb, fixups): pb +4 data, +12 size */
void sub_0053AE30_enter(void)
{
    static unsigned n;
    uint32_t pb = MEM32(g_esp + 4), data, size, va, end, bad = 0, w;
    imp_fix(pb);
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
            if (imp_block_va(t)) { va += 8; continue; }   /* imp_fix's splice */
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

static void dome_tint_factor(void);
static void grass_fog_factor(void);

/* 0x00537360: D3DDevice_SetRenderState_TextureFactor (stdcall: value).
 * From the glare add (returns to 0x001DA610): white scaled to grey. From
 * the underwater dome (0x00155A93): brown in the poo (dome_tint_factor).
 * From a waving grass patch (0x001A9942): the fog (grass_fog_factor). */
void sub_00537360_enter(void)
{
    float k;
    uint32_t g;
    if (MEM32(g_esp) == 0x00155A93u) {
        dome_tint_factor();
        return;
    }
    if (MEM32(g_esp) == 0x001A9942u) {
        grass_fog_factor();
        return;
    }
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
 * original; conker_SetGrassFull from the F2 menu.
 * Each part of the fix (grass, characters, fur, ambient particles below)
 * also has its own switch under that one (user 2026-10-31):
 * RECOMP_POPIN_GRASS, _CHARACTERS, _FUR = 0 (popin_* in settings.ini)
 * leaves that part as the original; the particles part is experimental
 * and off unless RECOMP_POPIN_PARTICLES=1 (user 2026-10-31);
 * conker_SetPopinPart from the F2 menu. */
enum { POPIN_GRASS, POPIN_CHARACTERS, POPIN_FUR, POPIN_PARTICLES,
       POPIN_PARTS };
static volatile int g_grass_full = -1;
static volatile int g_popin_part[POPIN_PARTS] = { -1, -1, -1, -1 };
static const char *const k_popin_env[POPIN_PARTS] = {
    "RECOMP_POPIN_GRASS", "RECOMP_POPIN_CHARACTERS", "RECOMP_POPIN_FUR",
    "RECOMP_POPIN_PARTICLES"
};
static uint32_t s_grass_patch, s_grass_pos[3];
static uint32_t s_grass_fog;            /* TFACTOR for this patch, 0 = none */
static int s_grass_fog_draw;            /* its TFACTOR went in: blend at the draw */
static void grass_fog_measure(uint32_t patch, int player);

static int env_on(const char *name, int def)
{
    const char *e = getenv(name);
    return e && *e ? *e != '0' : def;
}

/* The pop-in fix's part (POPIN_*) is on: the main switch and its own. */
static int popin_on(int part)
{
    if (g_grass_full < 0)
        g_grass_full = env_on("RECOMP_GRASS_FULL", 1);
    if (g_popin_part[part] < 0)
        g_popin_part[part] = env_on(k_popin_env[part],
                                    part != POPIN_PARTICLES);
    return g_grass_full && g_popin_part[part];
}

/* For the F2 menu (overlay.cpp): applies from the next frame. */
void conker_SetGrassFull(int on)
{
    g_grass_full = on != 0;
}

/* part: 0 grass, 1 characters, 2 fur, 3 particles (POPIN_*). */
void conker_SetPopinPart(int part, int on)
{
    if (part >= 0 && part < POPIN_PARTS)
        g_popin_part[part] = on != 0;
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
    s_grass_fog = 0;
    s_grass_fog_draw = 0;
    if (!popin_on(POPIN_GRASS) || !xact_va_ok(patch) || !xact_va_ok(players)
        || player < 0 || player > 3)
        return;
    cam = MEM32(players + (uint32_t)player * 0x770u + 0x3E0u);
    if (!xact_va_ok(cam))
        return;
    grass_fog_measure(patch, player);   /* before the centre moves */
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

    if (!popin_on(POPIN_CHARACTERS) || !xact_va_ok(c)) {
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
    if (!fur || !popin_on(POPIN_FUR))
        return;
    full = g_xmm3.f[0] > 0.0f ? g_xmm3.f[0] * 0.4f : 400.0f;
    if (g_xmm0.f[0] > full)
        g_xmm0.f[0] = full;
}

/* Ambient particles too (user 2026-10-31: the field's falling leaves and
 * dandelion fluff follow the camera, seen flying away in photo mode).
 * 0x001A6850 draws one particle emitter (cdecl: emitter, short player) as
 * a point list. Kinds 2 and 4 (+0x2C) are a fixed set of points kept in a
 * box of +0x68 x +0x6C x +0x68 units (1000 x 500 x 1000 in the field)
 * around that player's camera (player record +0x330): the vertex program
 * wraps each point into the box, so the cloud moves with the camera and
 * ends half a box away. With the fix on, the emitter is drawn again with
 * the box moved by whole box sizes, which the wrap turns into the
 * neighbouring part of one endless cloud: tiles out to
 * RECOMP_PARTICLE_TILES boxes each way across (default 3) and one box above
 * and below, the far ones first (the points blend over each other). Each
 * added box draws only part of the points (+0x4C, the count drawn):
 * particle_density percent (RECOMP_PARTICLE_DENSITY, default 15) in the
 * first ring of boxes, divided by r^2 in ring r, as boxes behind boxes
 * add up (all of them at full count was a blizzard, user 2026-10-31);
 * 50 was cluttered and showed particles clipping into walls; the user
 * picked 15. conker_SetParticleDensity from the F2 slider. */
static volatile int g_particle_density = -1;

/* For the F2 menu (overlay.cpp): applies from the next frame. */
void conker_SetParticleDensity(float percent)
{
    g_particle_density = percent < 0.0f ? 0
                       : percent > 100.0f ? 100 : (int)(percent + 0.5f);
}

/* RECOMP_PARTICLE_LOG=1: a [PARTICLES] line when an effect starts being
 * drawn (not seen in the last 3 s), e.g. per map in a soak run: what (an
 * emitter of 0x001A6850 with its kind, box and count, or the other camera
 * box effect, 0x001BFE30's streaks). */
static void particle_log(const char *what, uint32_t em)
{
    static int on = -1, n;
    static struct { uint32_t em, kind, box; clock_t seen; } s[64];
    uint32_t kind = em ? MEM32(em + 0x2Cu) : 0, box = em ? MEM32(em + 0x68u) : 0;
    uint32_t count = em ? MEM32(em + 0x4Cu) : 0;
    clock_t now = clock();
    int i;

    if (on < 0) {
        const char *e = getenv("RECOMP_PARTICLE_LOG");
        on = e && *e == '1';
    }
    if (!on)
        return;
    for (i = 0; i < n; i++)             /* the count grows as it fades in */
        if (s[i].em == em && s[i].kind == kind && s[i].box == box)
            break;
    if (i == n) {
        if (n == 64)
            return;
        n++;
        s[i].em = em; s[i].kind = kind; s[i].box = box;
        s[i].seen = now - 10 * CLOCKS_PER_SEC;
    }
    if (now - s[i].seen > 3 * CLOCKS_PER_SEC) {
        if (em)
            fprintf(stderr, "[PARTICLES] %s %08X kind %u box %.0f x %.0f count %u"
                    " tex %d/%08X\n", what, em, kind, MEMF(em + 0x68u),
                    MEMF(em + 0x6Cu), count, (int32_t)MEM32(em + 0x58u),
                    MEM32(em + 0x5Cu));
        else
            fprintf(stderr, "[PARTICLES] %s\n", what);
    }
    s[i].seen = now;
}

/* It's War's streaks and any other user of this camera box effect, logged
 * only (RECOMP_PARTICLE_LOG). */
extern void sub_001BFE30_gen(void);
void sub_001BFE30(void)
{
    particle_log("camera streaks 0x001BFE30", 0);
    sub_001BFE30_gen();
}

extern void sub_001A6850_gen(void);
void sub_001A6850(void)
{
    enum { MAXT = 8, MAXN = (2 * MAXT + 1) * (2 * MAXT + 1) * 3 };
    static int tiles = -1;
    int density;
    uint32_t sp = g_esp, ret = MEM32(sp), em = MEM32(sp + 4), count;
    uint32_t a2 = MEM32(sp + 8), players = MEM32(0x008470DCu), rec;
    int16_t player = (int16_t)a2;
    int32_t kind;
    float bx, by, c[3];
    struct { float d; int8_t x, y, z; } t[MAXN], tmp;
    int n = 0, i, j, k;

    if (tiles < 0) {
        const char *e = getenv("RECOMP_PARTICLE_TILES");
        tiles = e ? atoi(e) : 3;
        if (tiles < 0) tiles = 0;
        if (tiles > MAXT) tiles = MAXT;
    }
    if (xact_va_ok(em))
        particle_log("emitter", em);
    if (!popin_on(POPIN_PARTICLES) || !tiles || !xact_va_ok(em)
        || !xact_va_ok(players) || player < 0 || player > 3) {
        sub_001A6850_gen();
        return;
    }
    if (g_particle_density < 0) {
        const char *e = getenv("RECOMP_PARTICLE_DENSITY");
        conker_SetParticleDensity(e ? (float)atof(e) : 15.0f);
    }
    density = g_particle_density;
    kind = (int32_t)MEM32(em + 0x2Cu);
    count = MEM32(em + 0x4Cu);
    bx = MEMF(em + 0x68u);
    by = MEMF(em + 0x6Cu);
    if ((kind != 2 && kind != 4) || !(bx > 0.0f) || !(by > 0.0f)) {
        sub_001A6850_gen();
        return;
    }
    rec = players + (uint32_t)player * 0x770u + 0x330u;
    c[0] = MEMF(rec);
    c[1] = MEMF(rec + 4);
    c[2] = MEMF(rec + 8);
    for (i = -tiles; i <= tiles; i++)
        for (j = -1; j <= 1; j++)
            for (k = -tiles; k <= tiles; k++) {
                float dx = i * bx, dy = j * by, dz = k * bx;
                t[n].d = dx * dx + dy * dy + dz * dz;
                t[n].x = (int8_t)i; t[n].y = (int8_t)j; t[n].z = (int8_t)k;
                n++;
            }
    for (i = 1; i < n; i++)             /* farthest first */
        for (j = i; j > 0 && t[j - 1].d < t[j].d; j--) {
            tmp = t[j]; t[j] = t[j - 1]; t[j - 1] = tmp;
        }
    for (i = 0; i < n; i++) {
        uint32_t keep = count;
        int ring = abs(t[i].x) > abs(t[i].z) ? abs(t[i].x) : abs(t[i].z);
        if (abs(t[i].y) > ring)
            ring = abs(t[i].y);
        if (ring)                       /* ring r: density / r^2 */
            keep = (uint32_t)((uint64_t)count * (uint32_t)density / 100u
                              / (uint32_t)(ring * ring));
        if (!keep)
            continue;
        MEM32(em + 0x4Cu) = keep;
        MEMF(rec) = c[0] + t[i].x * bx;
        MEMF(rec + 4) = c[1] + t[i].y * by;
        MEMF(rec + 8) = c[2] + t[i].z * bx;
        g_esp = sp;
        MEM32(sp) = ret;
        MEM32(sp + 4) = em;
        MEM32(sp + 8) = a2;
        sub_001A6850_gen();
    }
    MEM32(em + 0x4Cu) = count;
    MEMF(rec) = c[0];
    MEMF(rec + 4) = c[1];
    MEMF(rec + 8) = c[2];
    g_esp = sp + 4;                     /* as after one call */
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

/* Grass "camera push" off. The grass waving program bends each blade tip by
 * c[101] x tip height^2 / distance^2 from c[100], both set last in
 * grass_draw_begin (0x001A8DC0): c[100] = camera +0x84, c[101] = (camera
 * +0x78 - +0x84) x 20000, i.e. blades are pushed away from the camera. But
 * c[100] is a world position and the blade a patch-local one, so the push
 * lands on whichever blades' local coordinates sit near the camera's world
 * position: only in maps built round the origin (the Bull fight paddock),
 * where those blades are flung across the screen (GitHub issue 8). With
 * the Pop-in Fix every patch uses the waving program, so it shows far more
 * often than in the original. The last call (0x00536B00, SetVertexShader
 * Constant: ecx = register, edx = 4 floats; return 0x001A9531) gets zeros
 * for c[101] (w too: it is added to the position's w). RECOMP_GRASS_PUSH=1
 * = the original. */
void sub_00536B00_enter(void)
{
    static int push = -1;
    if (MEM32(g_esp) != 0x001A9531u || g_ecx != 0x65u)
        return;
    if (push < 0)
        push = env_on("RECOMP_GRASS_PUSH", 0);
    if (push || !xact_va_ok(g_edx))
        return;
    MEM32(g_edx) = 0;
    MEM32(g_edx + 4) = 0;
    MEM32(g_edx + 8) = 0;
    MEM32(g_edx + 12) = 0;
}

/* Grass fog (part of the Pop-in Fix's grass). The level's haze is each
 * world vertex program's own: fog = min((depth - near) / (far - near), max)
 * into oFog (c[103] = 1/(far-near), near/(far-near), max, set by
 * 0x003ED850), mixed in by the world's pixel shaders. Effects are drawn
 * with D3D fog off, and neither grass program writes oFog: grass never
 * gets the haze. The original draws it only within 800 (7 % fog at most
 * in the field) and faded out to 2500, so that never showed; with every
 * patch drawn, the far ones (the farm plot seen from the bridge, 35-48 %
 * fog there) stand out bright green against the hazed ground, and pop out
 * of and back into the pause screen's hazy picture. Fix: one fog amount
 * per patch, from its centre's view depth (the viewport record's view
 * matrix, [0x0084A13C] + player x 0x2B0 + 0x40, as the patch draw itself
 * uses) and the level's fog (0x00771CD8..: B, G, R as ints, near, far;
 * far 0 = none; max [0x00750300]; 0x0010A2B0 copies them on), given as
 * TFACTOR (fog colour, alpha = amount) in place of the waving path's
 * (returns to 0x001A9942; its stages never read TFACTOR) and blended over
 * by texture stage 1 at the draw (DrawVertices 0x0053A930, returns to
 * 0x001A9A3C): BLENDFACTORALPHA (TFACTOR, CURRENT), alpha CURRENT. Stage 1
 * is off for grass, so it is free; the game's own state cache (0x007717xx)
 * gets the same values so its next "set if changed" puts them back. Only
 * while the grass part is on; RECOMP_GRASS_FOG=0 = without (A/B),
 * RECOMP_GRASS_FOG_LOG=1 = a [GRASSFOG] line per patch. */
#define XB_TOP_SELECTARG1        2u
#define XB_TOP_BLENDFACTORALPHA 15u
#define XB_TA_CURRENT            1u
#define XB_TA_TFACTOR            3u

static int grass_fog_log(void)
{
    static int on = -1;
    if (on < 0)
        on = env_on("RECOMP_GRASS_FOG_LOG", 0);
    return on;
}

static uint32_t fog_byte(uint32_t va)
{
    int32_t c = (int32_t)MEM32(va);
    return c < 0 ? 0u : c > 255 ? 255u : (uint32_t)c;
}

/* Fog toggle (to-do 120, GitHub issue 17; F2 Picture "Fog", fog = 0 in
 * settings.ini, RECOMP_FOG=0). The level's distance fog has one source:
 * 0x003ED850 turns the level's values into c[103] = 1/(far-near),
 * near/(far-near), max, uploaded through SetVertexShaderConstant1
 * (0x00536AA0, fastcall ecx = register, edx = 0x0081BE00, returns to
 * 0x003ED9D6; register 103, or the newer programs' base + 0x60), and every
 * world vertex program writes oFog = min(depth term, max): the fog amount
 * the pixel shaders mix towards the fog colour. Fog off = max 0 in that
 * upload, so every amount is 0 and nothing else changes: FogEnable, the
 * program variants and the combiners stay as the level set them (FogEnable
 * 0 would read as full fog through the world's own final combiner, which
 * mixes by amount, not D3D's visibility). Ghidra shows no direct writer
 * of the fog table mode [0x007502F8] (0, linear), and the fog colour alpha
 * [0x007502EB] is written only by 0x0010A2B0 (0xFF), so the second fog
 * constant (c[189], set when that alpha is below 0xFF) is not in play.
 * A second renderer, 0x00408D70 (its programs from the declaration at
 * 0x007557CC), keeps its fog in the vertex colour instead: c[107] =
 * 1/(far-near), far/(far-near) (from the stack, returns to 0x00408E11),
 * oD0.rgb = view z x c[107].x + c[107].y = visibility (1 near, 0 far, in
 * some variants times a second range's term, c[112], left alone): fog off =
 * c[107] = 0, 1. Grass gets no fog either (grass_fog_measure). Read every
 * upload: live. One [FOG] line per path, the first time it runs. */
static volatile int g_fog = -1, g_uw_fog = -1;

void conker_SetFog(int on)
{
    g_fog = on != 0;
}

/* Under water the level's fog record gives a second set (record + 0x10C,
 * 0x0012B2B0, when camera 0's [+4] + 0x8C2 is set): the murky water colour.
 * F2 Picture "Underwater Fog" (underwater_fog, RECOMP_UNDERWATER_FOG=0)
 * switches that one on its own (user 2026-11-07). */
void conker_SetUnderwaterFog(int on)
{
    g_uw_fog = on != 0;
}

/* Camera 0's water object ([0x008470DC] + 4); +0x8C2 = under water. */
static uint32_t fog_cam_water(void)
{
    uint32_t cam = MEM32(0x008470DCu), sub;

    if (!xact_va_ok(cam))
        return 0;
    sub = MEM32(cam + 4u);
    return xact_va_ok(sub) ? sub : 0;
}

static int fog_underwater(void)
{
    uint32_t sub = fog_cam_water();

    return sub && MEM8(sub + 0x8C2u) != 0;
}

static int uw_fog_on(void)
{
    if (g_uw_fog < 0)
        g_uw_fog = env_on("RECOMP_UNDERWATER_FOG", 1);
    return g_uw_fog;
}

/* Conker stays green under water with Underwater Fog off: not the fog
 * amount, and not the light map set 0x0012B2B0 picks (a test giving it the
 * above-water sets left him green; conker-notes 2026-11-07). Kept (user
 * 2026-11-07: "looks more natural in normal gameplay"). */
static int fog_on(void)
{
    static int was_under = -1;
    int under = fog_underwater();

    if (g_fog < 0)
        g_fog = env_on("RECOMP_FOG", 1);
    if (under != was_under) {
        if (was_under >= 0)
            fprintf(stderr, "[FOG] camera %s water\n", under ? "under" : "out of");
        was_under = under;
    }
    return under ? uw_fog_on() : g_fog;
}

/* 0x00536AA0: D3DDevice_SetVertexShaderConstant1 (fastcall). */
void sub_00536AA0_enter(void)
{
    static int seen_world, seen_second;
    uint32_t ret = MEM32(g_esp);

    if (ret == 0x003ED9D6u && g_edx == 0x0081BE00u) {
        if (!seen_world) {
            seen_world = 1;
            fprintf(stderr, "[FOG] world fog: c[%u] = %g %g max %g (fog %s)\n",
                    g_ecx, MEMF(0x0081BE00u), MEMF(0x0081BE04u),
                    MEMF(0x0081BE08u), fog_on() ? "on" : "off");
        }
        if (!fog_on())
            MEMF(0x0081BE08u) = 0.0f;   /* c[103].z: the fog amount's cap */
    } else if (ret == 0x00408E11u) {
        if (!seen_second) {
            seen_second = 1;
            fprintf(stderr, "[FOG] second renderer fog: c[%u] = %g %g (fog %s)\n",
                    g_ecx, MEMF(g_edx), MEMF(g_edx + 4u), fog_on() ? "on" : "off");
        }
        if (!fog_on()) {
            MEMF(g_edx) = 0.0f;         /* c[107]: visibility 1 everywhere */
            MEMF(g_edx + 4u) = 1.0f;
        }
    }
}

static void grass_fog_measure(uint32_t patch, int player)
{
    static int on = -1;
    uint32_t rec = MEM32(0x0084A13Cu), v, a8;
    float fnear, ffar, fmax, depth, a;

    if (on < 0)
        on = env_on("RECOMP_GRASS_FOG", 1);
    if (!on || !fog_on() || !xact_va_ok(rec))
        return;
    ffar = MEMF(0x00771CE4u);
    fnear = MEMF(0x00771CE8u);
    if (ffar == 0.0f || !(ffar > fnear))
        return;                         /* no fog in this level */
    fmax = MEMF(0x00750300u);
    v = rec + (uint32_t)player * 0x2B0u + 0x40u;
    depth = -(MEMF(patch + 0x2Cu) * MEMF(v + 0x08u)
              + MEMF(patch + 0x30u) * MEMF(v + 0x18u)
              + MEMF(patch + 0x34u) * MEMF(v + 0x28u) + MEMF(v + 0x38u));
    a = (depth - fnear) / (ffar - fnear);
    if (a > fmax)
        a = fmax;
    if (a > 1.0f)
        a = 1.0f;
    a8 = a > 0.0f ? (uint32_t)(a * 255.0f + 0.5f) : 0u;
    if (grass_fog_log())
        fprintf(stderr, "[GRASSFOG] patch %08X depth %.0f fog %.3f (near %g far %g "
                "max %g) colour %02X%02X%02X\n", patch, depth, a, fnear, ffar,
                fmax, fog_byte(0x00771CE0u), fog_byte(0x00771CDCu),
                fog_byte(0x00771CD8u));
    if (!a8)
        return;
    s_grass_fog = a8 << 24 | fog_byte(0x00771CE0u) << 16
                  | fog_byte(0x00771CDCu) << 8 | fog_byte(0x00771CD8u);
}

static void grass_fog_factor(void)
{
    if (!s_grass_fog)
        return;
    MEM32(g_esp + 4) = s_grass_fog;
    s_grass_fog_draw = 1;
}

/* 0x0053A930: D3DDevice_DrawVertices. */
void sub_0053A930_enter(void)
{
    if (MEM32(g_esp) != 0x001A9A3Cu || !s_grass_fog_draw)
        return;
    s_grass_fog_draw = 0;
    MEM32(0x005473E0u) = MEM32(0x007717E8u) = XB_TOP_BLENDFACTORALPHA;
    MEM32(0x005473E8u) = MEM32(0x007717C8u) = XB_TA_TFACTOR;
    MEM32(0x005473ECu) = MEM32(0x007717B8u) = XB_TA_CURRENT;
    MEM32(0x005473F0u) = MEM32(0x007717A8u) = XB_TOP_SELECTARG1;
    MEM32(0x005473F8u) = MEM32(0x00771788u) = XB_TA_CURRENT;
    MEM32(0x005499F0u) |= 0x800u;       /* texture stage states changed */
}

/* ── Conker: D3DX matrix multiply natively (speed; RECOMP_D3DX_NATIVE) ────
 *
 * 0x0042841D is D3DXMatrixMultiply's SSE version (stdcall out, a, b; out =
 * a * b, returned in EAX): ~60 SSE instructions, which the translation does
 * as four volatile 32-bit loads per MOVAPS and lane loops on thread-local
 * registers. In It's War's opening cutscene it was 5-13 % of the game thread
 * (most calls from 0x003EF650; conker-notes 2026-11-04). This is the same
 * arithmetic with SSE intrinsics in the same order -- per row ((x*b0 + y*b1)
 * + z*b2) + w*b3, all of a and b read before out is written (out may be a or
 * b) -- so out is bit for bit the generated one's, and xmm0-xmm5, EAX and
 * ECX end as they did. RECOMP_D3DX_NATIVE=0 runs the generated body; =2 runs
 * it and checks the native result against it on every call ([D3DX] lines). */
extern void sub_0042841D_gen(void);
static int s_d3dx_native = -1;
static unsigned long long s_d3dx_calls, s_d3dx_diff;

/* out = a * b as 0x0042841D computes it; z, w = its last row's xmm0, xmm1. */
static void d3dx_mat_mul(float *out, const float *a, const float *b,
                         __m128 *z, __m128 *w)
{
    __m128 b0 = _mm_loadu_ps(b), b1 = _mm_loadu_ps(b + 4);
    __m128 b2 = _mm_loadu_ps(b + 8), b3 = _mm_loadu_ps(b + 12);
    __m128 r[4];
    int i;

    for (i = 0; i < 4; i++) {
        __m128 row = _mm_loadu_ps(a + 4 * i);
        __m128 x = _mm_mul_ps(_mm_shuffle_ps(row, row, 0x00), b0);
        __m128 y = _mm_mul_ps(_mm_shuffle_ps(row, row, 0x55), b1);
        *z = _mm_mul_ps(_mm_shuffle_ps(row, row, 0xAA), b2);
        x = _mm_add_ps(x, y);
        *w = _mm_mul_ps(_mm_shuffle_ps(row, row, 0xFF), b3);
        x = _mm_add_ps(x, *z);
        r[i] = _mm_add_ps(x, *w);
    }
    for (i = 0; i < 4; i++)
        _mm_storeu_ps(out + 4 * i, r[i]);
}

void sub_0042841D(void)
{
    uint32_t out = MEM32(g_esp + 4), a = MEM32(g_esp + 8), b = MEM32(g_esp + 12);
    float *o = (float *)XBOX_PTR(out);
    __m128 z, w;

    if (s_d3dx_native < 0) {
        const char *e = getenv("RECOMP_D3DX_NATIVE");
        s_d3dx_native = e && *e ? atoi(e) : 1;
    }
    if (s_d3dx_native == 0) {
        sub_0042841D_gen();
        return;
    }
    if (s_d3dx_native == 2) {
        float ca[16], cb[16], res[16];
        memcpy(ca, (const void *)XBOX_PTR(a), sizeof ca);
        memcpy(cb, (const void *)XBOX_PTR(b), sizeof cb);
        sub_0042841D_gen();
        d3dx_mat_mul(res, ca, cb, &z, &w);
        s_d3dx_calls++;
        if (memcmp(res, o, sizeof res) || memcmp(&z, &g_xmm0, 16)
            || memcmp(&w, &g_xmm1, 16) || memcmp(o, &g_xmm2, 16)
            || memcmp(o + 12, &g_xmm5, 16)) {
            if (s_d3dx_diff++ < 8)
                fprintf(stderr, "[D3DX] matrix multiply call %llu differs: out"
                        " %08X a %08X b %08X\n", s_d3dx_calls, out, a, b);
        }
        if (s_d3dx_calls % 1000000u == 0)
            fprintf(stderr, "[D3DX] matrix multiply check: %llu calls, %llu"
                    " differ\n", s_d3dx_calls, s_d3dx_diff);
        return;
    }
    d3dx_mat_mul(o, (const float *)XBOX_PTR(a), (const float *)XBOX_PTR(b),
                 &z, &w);
    _mm_storeu_ps(g_xmm0.f, z);
    _mm_storeu_ps(g_xmm1.f, w);
    memcpy(&g_xmm2, o, 16);
    memcpy(&g_xmm3, o + 4, 16);
    memcpy(&g_xmm4, o + 8, 16);
    memcpy(&g_xmm5, o + 12, 16);
    g_ecx = g_eax = out;
    g_esp += 16;                        /* ret 12 */
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

/* ── Conker: cutscene skip (F2 Cutscenes; to-do 16, SPEC 10.2) ───────────
 *
 * The game's skip: 0x00050020 (every frame while a scene plays) asks
 * 0x0004FF00 (cutscene_can_skip; slot in edi, answer in eax) and, on a yes
 * with more than 1.5 s left before the scene's skip point [0x0076F0E8 +
 * slot*4] (time now [0x0076F0F8 + slot*4], ticks of [0x007F9F5C] a second),
 * fades out for 0.5 s and ends the scene (0x0004FDC0 -> 0x000507A0). The end
 * starts the next part: in another level that part loads first, so one skip
 * goes to the next load. can_skip says no when:
 *   - ThroneRoom (level 0x18) scene 1, always;
 *   - the skip button was not pressed this frame ([0x00763260 + pad x 0x1B0
 *     + 0xA0], the pad record's LT edge: pad 0 when [0x006BCFEB] > 0, else
 *     pad 20; copied from the input record [[0x008470FC + pad x 4] + 8]
 *     (0x0075A0B4 for pad 20), whose +4 is held, +8 pressed this frame);
 *   - the scene script marks it unskippable ([0x007655F7], event 0x0E);
 *   - not seen on this save: bit scene [0x0076F0DC + slot] of level
 *     [0x007FA1F8]'s 64 at 0x00765608 + level x 8, unless [0x007655E9] or
 *     [0x00801A33] (the chapter list);
 *   - less than 0.5 s before the skip point (unless [0x007655F9]).
 * The front end (0x1D) and FrontendTable (0x21) have rules of their own.
 *
 * skippable_cutscenes (RECOMP_CUTSCENE_SKIP, default on) lifts the first,
 * third and fourth in story levels. With hold_to_skip (RECOMP_SKIP_HOLD,
 * default on) such a scene skips after the button is held SKIP_HOLD_MS
 * (real time, so every frame rate is the same) while the overlay draws a
 * ring (conker_SkipRing); off, a press skips it as a seen one. A scene the
 * game lets you skip still skips on a press, and so does any scene too short
 * for the hold and the 1.5 s (skip_short_now). The hold starts over when a new
 * scene or part starts or the game stops asking (pause, the fade): held into
 * one, the button must be let go first, so holding through a load skips
 * one part only. RECOMP_SKIP_LOG=1 prints [SKIP] lines. */
#define SKIP_HOLD_MS 3000.0
/* LT held through a skip went on into play: Classic's head-cam (user
 * 2026-11-04, field save), Modern's aim. Set at a skip with LT held,
 * pad_filter then hides LT from the game until it is let go. */
static volatile LONG s_skip_lt_block;

static volatile int g_skip_on = -1, g_skip_hold = -1;   /* < 0 = not read yet */
static volatile double g_skip_t0;     /* hold start (skip_ms), 0 = none */
static volatile double g_skip_asked;  /* when can_skip last ran */

void conker_SetCutsceneSkip(int on)
{
    g_skip_on = on != 0;
}

/* ── Conker: cutscene event log (RECOMP_CUTEVENT_LOG=1, debug) ──────────
 *
 * Issue 16 (2026-11-06): the throne room's story scene (level 0x18) went
 * black in the Professor's lab shot above 30 fps, and its lab ambience
 * never started. The level holds three sets (Throneroom, _OutsideCastle,
 * _Lab); the per-frame event dispatcher (0x00056050) loads the one camera 0's
 * +0x334 falls in at the end of its run (0x0012B260 unload, 0x0012B1D0 load,
 * name in esi). Events aimed at a character or object (track type 2 / 3)
 * are dropped by 0x0005E240 when that target is not found, before the
 * executor 0x000569A0 runs. These lines show which events were tried, which
 * ran, and when a set loaded, to tell a dropped event from a late one:
 *   [CUTEVT] try  slot, cutscene time now/last, event time, record bytes, track type/id
 *   [CUTEVT] run  the same record reached the executor
 *   [CUTEVT] set  a set load, with camera 0's +0x334 */
static int cutevent_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CUTEVENT_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

void sub_0005E240_enter(void)
{
    uint32_t slot = g_esi, rec = MEM32(g_esp + 4), track = MEM32(g_esp + 8) & 0xFFu;
    uint32_t tracks, tt = 0xFFFF, tid = 0xFF;
    if (!cutevent_log_on())
        return;
    tracks = slot < 8 ? MEM32(0x0076EF2Cu + slot * 4) : 0;
    if (xact_va_ok(tracks)) {
        tt = MEM16(tracks + track * 8);
        tid = MEM8(tracks + track * 8 + 2);
    }
    fprintf(stderr, "[CUTEVT] t=%ld ms try level %02X slot %u time %d/%d ev %d "
            "rec %08X [%02X %02X %04X %02X %02X %04X] track %u type %u id %02X\n",
            attach_ms(), MEM32(0x007FA1F8u), slot,
            slot < 2 ? (int)MEM32(0x0076F0F8u + slot * 4) : -1,
            slot < 2 ? (int)MEM32(0x0076F0F0u + slot * 4) : -1,
            (int)MEM32(g_esp + 0x10), rec,
            xact_va_ok(rec) ? MEM8(rec) : 0, xact_va_ok(rec) ? MEM8(rec + 1) : 0,
            xact_va_ok(rec) ? MEM16(rec + 2) : 0, xact_va_ok(rec) ? MEM8(rec + 4) : 0,
            xact_va_ok(rec) ? MEM8(rec + 5) : 0, xact_va_ok(rec) ? MEM16(rec + 6) : 0,
            track, tt, tid);
}

void sub_000569A0_enter(void)
{
    if (cutevent_log_on())
        fprintf(stderr, "[CUTEVT] t=%ld ms run rec %08X (ret %08X)\n",
                attach_ms(), g_eax, MEM32(g_esp));
}

void sub_0012B1D0_enter(void)
{
    uint32_t cam = MEM32(0x008470DCu);
    char name[40] = "?";
    int i;
    if (!cutevent_log_on())
        return;
    if (xact_va_ok(g_esi))
        for (i = 0; i < (int)sizeof name - 1 && MEM8(g_esi + i); i++) {
            name[i] = (char)MEM8(g_esi + i);
            name[i + 1] = '\0';
        }
    fprintf(stderr, "[CUTEVT] t=%ld ms set load \"%s\" (chosen %d, loaded %08X, "
            "camera +0x334 %.0f, ret %08X)\n", attach_ms(), name,
            (int)MEM32(0x0068C490u), MEM32(0x008493BCu),
            xact_va_ok(cam) ? MEMF(cam + 0x334u) : 0.0f, MEM32(g_esp));
}

/* The same log follows levels and scene parts around a skip (issue 16: a skip
 * in the throne room scene's first part crashed the barn load in the level
 * unload, on -1 values, at any frame rate):
 *   [CUTEVT] level change  0x002AA0C0: requested [0x007FA1F4], current [0x007FA1F8]
 *   [CUTEVT] level unload  0x002AA140
 *   [CUTEVT] sections push 0x0034EEE0 (list in ebx) / pop 0x0034EF90; depth [0x0084B108],
 *            stack [0x008013A0 + depth*4]
 *   [CUTEVT] scene start 0x0004EC10 (stack: level, scene, slot), end 0x000507A0 (esi =
 *            slot; next part: level [0x0076F0B7], scene [0x0076F0B8] - 1), skip_now 0x0004FDC0 */
static void cutevent_where(const char *what)
{
    uint32_t d = MEM32(0x0084B108u);
    fprintf(stderr, "[CUTEVT] t=%ld ms %s: level %02X (requested %02X) sections depth %d "
            "top %08X (ret %08X)\n", attach_ms(), what, MEM32(0x007FA1F8u),
            MEM32(0x007FA1F4u), (int)d, d && d < 64 ? MEM32(0x0080139Cu + d * 4) : 0,
            MEM32(g_esp));
}

void sub_002AA0C0_enter(void)
{
    if (cutevent_log_on())
        cutevent_where("level change");
}

/* level_unload: the wrapper sub_002AA140 (fast loads, above). */

void sub_0034EEE0_enter(void)
{
    char first[24] = "?";
    uint32_t s;
    int i;
    if (!cutevent_log_on())
        return;
    if (xact_va_ok(g_ebx) && xact_va_ok(s = MEM32(g_ebx)))
        for (i = 0; i < (int)sizeof first - 1 && MEM8(s + i); i++) {
            first[i] = (char)MEM8(s + i);
            first[i + 1] = '\0';
        }
    cutevent_where("sections push");
    fprintf(stderr, "[CUTEVT]   list %08X first \"%s\"\n", g_ebx, first);
}

void sub_0034EF90_enter(void)
{
    if (cutevent_log_on())
        cutevent_where("sections pop");
}

void sub_0004EC10_enter(void)
{
    if (cutevent_log_on())
        fprintf(stderr, "[CUTEVT] t=%ld ms scene start: level %02X scene %u slot %u "
                "(current level %02X, ret %08X)\n", attach_ms(), MEM32(g_esp + 4),
                MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(0x007FA1F8u), MEM32(g_esp));
}

void sub_000507A0_enter(void)
{
    uint32_t s = g_esi;
    if (cutevent_log_on())
        fprintf(stderr, "[CUTEVT] t=%ld ms scene end: slot %u level %02X scene %u time %d "
                "next level %02X scene %d skipping %u (ret %08X)\n", attach_ms(), s,
                MEM32(0x007FA1F8u), s < 4 ? MEM8(0x0076F0DCu + s) : 0,
                s < 2 ? (int)MEM32(0x0076F0F8u + s * 4) : -1, MEM8(0x0076F0B7u),
                (int)MEM8(0x0076F0B8u) - 1, MEM8(0x00765601u), MEM32(g_esp));
}

void sub_0004FDC0_enter(void)
{
    if (cutevent_log_on())
        fprintf(stderr, "[CUTEVT] t=%ld ms skip_now (ret %08X)\n", attach_ms(), MEM32(g_esp));
}

/* Level request 0x002A94A0 (kind, byte, a, b; level in esi): kinds 1 / 2 set
 * the outer loop's state [0x007819D6] to 3 (init level esi), 4 / 6 to 2
 * (change level), 5 to 1 (front end); ignored while [0x0076EC82] is set. */
void sub_002A94A0_enter(void)
{
    if (cutevent_log_on())
        fprintf(stderr, "[CUTEVT] t=%ld ms level request: kind %u level %02X arg %u "
                "(state %u, ignore flag %u/%u, current %02X, ret %08X, then %08X %08X)\n",
                attach_ms(), MEM32(g_esp + 4), g_esi, MEM8(g_esp + 8),
                MEM8(0x007819D6u), MEM8(0x0076EC82u), MEM8(0x00771130u),
                MEM32(0x007FA1F8u), MEM32(g_esp), MEM32(g_esp + 0x14), MEM32(g_esp + 0x18));
}

/* ── Conker: a cutscene that starts mid-timeline above 30 fps ─────────────
 *
 * Issue 16 (2026-11-06; RECOMP_CUTSCENE_START_FIX, on; =0 the original).
 * A cutscene can start at a time other than 0: its start event (type 5,
 * 0x77) sets the start [0x007655F4] (word), and 0x0004F8A0 puts the clock
 * there (slot s: now [0x0076F0F8 + s*4], last [0x0076F0F0 + s*4]; the reset
 * 0x0004E9E0 made last -100 and cleared flag [0x0076F0D8 + s]). Its opening
 * events (times 0..start: who stands where, the set's ambience, the first
 * view) are meant to fire together: the clock step 0x000501F0 sets last to
 * -1 the first time now leaves the start, so the dispatcher 0x00056050 fires
 * everything up to now -- but only while the flag is 0, and every dispatcher
 * run (mode 0) sets it, even one whose window last..now was empty.
 *
 * At 30 fps every step moves the clock 2 ticks, so it always leaves the
 * start before the dispatcher's first run. Above 60 fps a step is under a
 * tick (now = (int)(60 * seconds): 100.5 -> 100), and at 60 the float sum
 * can land just under (100.99998 -> 100): the dispatcher runs on a clock
 * still at the start, finds nothing, sets the flag, and the opening events
 * are never fired. The throne room story scene's second part (the
 * Professor's lab, start 100) then drew nothing but subtitles and a shadow
 * under the game's own black pass, its lab ambience never started, and a
 * skip into it crashed (conker-notes 2026-11-06). A start of 0 is safe: the
 * step forces now 0 -> 1.
 *
 * Fix: the dispatcher's entry notes when its flag-setting run sees an empty
 * window ("owed"); the clock step's entry then clears the flag again while
 * the clock still sits on the start, so the game's own catch-up runs when
 * the clock moves. A first run that did fire the opening (a scene started
 * between the step and the dispatcher: window -100..start) is never owed,
 * so nothing fires twice. RECOMP_CUTEVENT_LOG=1 prints [CUTEVT] start fix. */
static int s_cut_owed[2];

static int cutscene_start_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CUTSCENE_START_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

void sub_00056050_enter(void)
{
    uint32_t mode = MEM32(g_esp + 4), s = MEM32(g_esp + 8);
    if (mode != 0 || s > 1 || !cutscene_start_fix_on() || MEM8(0x0076F0D8u + s))
        return;
    s_cut_owed[s] = MEM32(0x0076F0F0u + s * 4) == MEM32(0x0076F0F8u + s * 4);
}

void sub_000501F0_enter(void)
{
    uint32_t s = g_esi, start, now;
    if (s > 1 || !s_cut_owed[s] || !cutscene_start_fix_on())
        return;
    start = MEM16(0x007655F4u);
    now = MEM32(0x0076F0F8u + s * 4);
    if (now != start) {
        s_cut_owed[s] = 0;              /* left the start: the catch-up ran */
        return;
    }
    if (MEM8(0x0076F0E0u + s) == 1 && MEM8(0x0076F0D8u + s)
        && MEM32(0x0076F0F0u + s * 4) == start) {
        MEM8(0x0076F0D8u + s) = 0;
        if (cutevent_log_on())
            fprintf(stderr, "[CUTEVT] t=%ld ms start fix: slot %u still on its start "
                    "%u, opening events kept for the catch-up\n", attach_ms(), s, start);
    }
}

/* ── IsBadReadPtr without SEH (issue 16) ──────────────────────────────────
 *
 * XAPI's IsBadReadPtr (0x0042C843, stdcall lp, ucb; IsBadCodePtr 0x0042C911
 * calls it with 1) reads the first byte and one byte per page inside an SEH
 * frame (__SEH_prolog 0x00470A34) and answers TRUE when a read faults. The
 * translation has no SEH, so that read of a bad pointer was a real access
 * violation. The game's "free it if it is valid" helpers (0x000630C0;
 * XMemFree 0x002AF6CF; ~100 IsBadCodePtr callers) pass such pointers on
 * purpose: skipping the throne room story scene in its first part left -1
 * in one, and the barnyard load crashed freeing it (at 30 fps too;
 * conker-notes 2026-11-06). This answers the same from the host pages
 * (committed and readable = good) without reading them. The first 16 TRUE
 * answers print [BADPTR] with the caller; RECOMP_BADPTR_FIX=0 = the
 * generated body (crashes on a bad pointer, as before). */
extern void sub_0042C843_gen(void);

static int guest_range_readable(uint32_t lp, uint32_t last)
{
    uintptr_t h, end;
    /* Guest RAM (64 MB) is one mapping, always committed and readable. */
    if (lp >= 0x00010000u && last < 0x04000000u)
        return 1;
    h = XBOX_PTR(lp) & ~(uintptr_t)0xFFF;
    end = XBOX_PTR(last);
    while (h <= end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;
        DWORD p;
        if (!VirtualQuery((void *)h, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
            return 0;
        p = mbi.Protect;
        if ((p & (PAGE_NOACCESS | PAGE_GUARD))
            || !(p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ
                      | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= h)
            return 0;
        h = next;
    }
    return 1;
}

void sub_0042C843(void)
{
    static int on = -1, shown;
    uint32_t lp = MEM32(g_esp + 4), ucb = MEM32(g_esp + 8), bad;
    if (on < 0) {
        const char *e = getenv("RECOMP_BADPTR_FIX");
        on = !(e && *e == '0');
    }
    if (!on) {
        sub_0042C843_gen();
        return;
    }
    if (!ucb)
        bad = 0;
    else if (!lp || lp + ucb - 1 < lp)
        bad = 1;
    else
        bad = !guest_range_readable(lp, lp + ucb - 1);
    if (bad && lp && shown < 16) {      /* NULL is the everyday "nothing to free" */
        shown++;
        fprintf(stderr, "[BADPTR] IsBadReadPtr(%08X, %u) = TRUE (ret %08X, its caller's "
                "ret %08X)\n", lp, ucb, MEM32(g_esp),
                MEM32(g_esp) == 0x0042C91Cu ? MEM32(g_esp + 0xC) : 0);
    }
    g_eax = bad;
    g_esp += 12;                        /* ret 8 */
}

void conker_SetSkipHold(int on)
{
    g_skip_hold = on != 0;
}

static int skip_switch(volatile int *v, const char *env)
{
    if (*v < 0) {
        const char *e = getenv(env);
        *v = !(e && *e == '0');
    }
    return *v;
}

static double skip_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* Black over the picture from one of our skips until what follows draws
 * (user 2026-11-04): the skip jumps the scene to its end and starts the next
 * part, which in another level loads first ([0x0076F0B6] = its scene + 1
 * until it starts), and the last frame stays up while it loads: the
 * scene's end (Conker on the throne) and things drawn over the game's fade
 * (New Game's title cards, which run on their own clock) showed. The cover
 * goes black faster than the game's fade (conker_SkipCover), stays until
 * what follows has drawn a while (skip_cover_poll) and fades out in 0.3 s;
 * never longer than 30 s. RECOMP_SKIP_LOG=1 prints [SKIP] cover lines. */
#define SKIP_COVER_MIN_MS 600.0
#define SKIP_COVER_SETTLE 8             /* frames of what follows first */
static volatile double g_cover_t0, g_cover_t1;  /* start / release, 0 = none */
static uint32_t s_cover_key;                    /* level << 8 | scene skipped */
static int s_cover_load;                        /* a load was seen */
static int s_cover_ready;                       /* frames the release held */
static int s_skip_log = -1;

static void skip_cover_start(uint32_t key)
{
    g_cover_t1 = 0.0;
    g_cover_t0 = skip_ms();
    s_cover_key = key;
    /* The next part (cutscene_end 0x000507A0 starts scene [0x0076F0B8] - 1
     * of level [0x0076F0B7]): in another level, a load follows. */
    s_cover_load = MEM8(0x0076F0B8u) && MEM8(0x0076F0B7u) != (key >> 8);
    s_cover_ready = 0;
}

/* Every frame (controls_poll). The skipped scene ends ([0x0076F0E0] 0
 * none, 1 playing, 2 the next part starting). With a load ([0x0076F0B6] or
 * state 2 seen), let go once another level plays a scene, or has had none
 * for 2.5 s (a load into play; the level number may change as a load
 * starts); without, once the scene has ended or another plays. Either way
 * only after that has held SKIP_COVER_SETTLE frames in a row: no frame runs
 * while the skip and a load happen (a load is rarely seen here), so a
 * frame of state 0 between them can't let go mid-load (user's 120 fps
 * glimpse of the throne and the cards, 2026-11-04). */
static void skip_cover_poll(void)
{
    double now, t0 = g_cover_t0;
    uint8_t run = MEM8(0x0076F0E0u), pend = MEM8(0x0076F0B6u);
    uint32_t lv = MEM32(0x007FA1F8u), key = lv << 8 | MEM8(0x0076F0DCu);
    int done;
    if (t0 <= 0.0 || g_cover_t1 > 0.0)
        return;
    now = skip_ms();
    if (pend || run == 2)
        s_cover_load = 1;
    if (s_cover_load) {
        static double lv_seen;          /* when another level was first seen */
        if (lv == s_cover_key >> 8)
            lv_seen = 0.0;
        else if (lv_seen <= 0.0)
            lv_seen = now;
        done = !pend && run != 2 && lv_seen > 0.0
               && (run == 1 || now - lv_seen >= 2500.0);
    } else
        done = run == 0 || (run == 1 && key != s_cover_key);
    if (!done || now - t0 < SKIP_COVER_MIN_MS) {
        s_cover_ready = 0;
        if (now - t0 < 30000.0)
            return;
    } else if (++s_cover_ready < SKIP_COVER_SETTLE)
        return;
    g_cover_t1 = now;
    if (s_skip_log > 0)
        fprintf(stderr, "[SKIP] cover off after %.0f ms (load %d, level 0x%02X, scene %u, "
                "state %u)\n", now - t0, s_cover_load, lv, key & 0xFFu, run);
}

/* For the overlay: how black the cover is, 0..1. Black from the skip's own
 * frame: the skip restarts the game's fade-out from clear (0x00050020),
 * and where that fade had held the picture black (the throne behind New
 * Game's title cards) the scene showed for a split second (user, 120 fps);
 * any fade-in of ours let it through, and the scene's end (the title
 * cards as brighter boxes over it) after. */
float conker_SkipCover(void)
{
    double t0 = g_cover_t0, t1 = g_cover_t1, now = skip_ms(), a = 1.0;
    if (t0 <= 0.0)
        return 0.0f;
    if (t1 > 0.0) {
        double out = 1.0 - (now - t1) / 300.0;
        if (out <= 0.0) {
            g_cover_t0 = g_cover_t1 = 0.0;
            return 0.0f;
        }
        if (out < a)
            a = out;
    }
    return (float)a;
}

/* For the overlay's ring: the hold's share of SKIP_HOLD_MS, 0 when none. */
float conker_SkipRing(void)
{
    double t0 = g_skip_t0, now = skip_ms();
    if (t0 <= 0.0 || now - g_skip_asked > 150.0)
        return 0.0f;
    return now - t0 >= SKIP_HOLD_MS ? 1.0f : (float)((now - t0) / SKIP_HOLD_MS);
}

/* Why the game said no, when it is a reason the option lifts; NULL else. */
static const char *skip_lifted(uint32_t lv, uint32_t scene)
{
    uint32_t bits;
    if (lv == 0x18u && scene == 1u)
        return "throne room scene 1";
    /* The front end lets only its scene 5 through to the rules below; the
     * story plays more there (New Game: Berri's room, scene 3). */
    if (lv == 0x1Du && scene != 5u)
        return "front end scene";
    if (MEM8(0x007655F7u))
        return "marked unskippable";
    if (MEM8(0x00801A33u) || MEM8(0x007655E9u) || scene >= 64u)
        return NULL;
    bits = MEM32(0x00765608u + lv * 8u + (scene >= 32u ? 4u : 0u));
    return bits & (1u << (scene & 31u)) ? NULL : "not seen yet";
}

/* A short scene's skip, at once (to-do 89): what 0x00050020 does on a yes
 * (fade out, stop the cutscene music 0x0001F6D0 and speech 0x0001B140 /
 * 0x0001B180) and then, without its 0.5 s fade wait, its skip_now call.
 * Run from inside can_skip, so the caller must then be told no. */
extern void sub_0001F6D0(void);
extern void sub_0001B140(void);
extern void sub_0001B180(void);
extern void sub_0004FDC0(void);
static void skip_short_now(int fade)
{
    MEM8(0x008497F9u) = 0;
    if (MEM32(0x00771C50u) != 1u || (int32_t)MEM32(0x00771C60u) <= (int32_t)MEM32(0x00771C5Cu)
        || MEM8(0x00771C64u)) {
        MEM32(0x00771C50u) = 1u;
        MEM32(0x00771C54u) = 0u;
        MEM8(0x00771C58u) = MEM8(0x00771C59u) = MEM8(0x00771C5Au) = 0;
        MEM32(0x00771C5Cu) = (uint32_t)fade;
        MEM32(0x00771C60u) = 0u;
    }
    /* Every call from here pushes a return address: a translated function's
     * ret pops one (esp += 4), so a bare call moved the guest stack 4 bytes
     * up. Before 2026-11-06 the last three calls had none: a short scene's
     * skip left esp 8 bytes off, the functions unwinding after it restored
     * ebx & co. from the wrong slots, and after the throne room scene's
     * part one was skipped the outer loop wrote ebx's 0x45 as the level
     * state and unloaded the barnyard twice (crash; issue 16, conker-notes
     * 2026-11-06 "throne room"). 0x0001F6D0 is ret 4 (its argument too). */
    g_esp -= 4; MEM32(g_esp) = MEM32(MEM32(0x00847024u) + 0x3Cu);
    g_esp -= 4; MEM32(g_esp) = 0;       /* return address, popped by the callee */
    sub_0001F6D0();
    g_esp -= 4; MEM32(g_esp) = 0;
    sub_0001B140();
    if (g_eax == 1u || g_eax == 2u) {
        g_esp -= 4; MEM32(g_esp) = 0;
        sub_0001B180();
    }
    MEM8(0x0076F0B4u) = 0;
    MEM8(0x00765600u) = 0;
    MEM8(0x00765601u) = 2;
    g_esp -= 4; MEM32(g_esp) = 0;
    sub_0004FDC0();
    MEM8(0x00765601u) = 0;
}

extern void sub_0004FF00_gen(void);
void sub_0004FF00(void)
{
    static int log = -1, wait_up;
    static uint32_t last_scene = 0xFFFFFFFFu;
    uint32_t slot = g_edi, lv = MEM32(0x007FA1F8u), scene, pad, rec, key;
    int held, edge, left, fps, fade, gate;
    const char *why;
    double now;

    sub_0004FF00_gen();
    if (log < 0) {
        const char *e = getenv("RECOMP_SKIP_LOG");
        log = s_skip_log = e ? atoi(e) : 0;
    }
    if (slot > 3u)
        return;
    now = skip_ms();
    scene = MEM8(0x0076F0DCu + slot);
    pad = (int8_t)MEM8(0x006BCFEBu) > 0 ? 0u : 20u;
    edge = MEM32(0x00763300u + pad * 0x1B0u) != 0;
    rec = pad < 2u ? MEM32(MEM32(0x008470FCu + pad * 4u) + 8u) : 0x0075A0B4u;
    held = !MEM8(0x008471A0u) && (rec ? (MEM32(rec + 4u) & 0x4000u) != 0 : edge);
    if (log > 1) {
        static uint32_t was[6];
        uint32_t is[6] = { pad, rec, rec ? MEM32(rec) : 0u, rec ? MEM32(rec + 4u) : 0u,
                           rec ? MEM32(rec + 8u) : 0u, (uint32_t)edge };
        if (memcmp(is, was, sizeof(is))) {
            memcpy(was, is, sizeof(is));
            fprintf(stderr, "[SKIP] asked: slot %u level 0x%02X scene %u pad %u rec %08X "
                    "+0 %08X +4 %08X +8 %08X edge %u eax %u\n", slot, lv, scene, pad,
                    rec, is[2], is[3], is[4], is[5], g_eax);
        }
    }
    key = lv << 8 | scene;
    if (key != last_scene || now - g_skip_asked > 250.0) {
        last_scene = key;
        g_skip_t0 = 0.0;
        wait_up = held;
    }
    g_skip_asked = now;
    /* 0x00050020 starts the skip only with more than gate ticks left, then
     * fades for fade ticks. A scene shorter than the hold plus that (It's
     * War's 2 s Tediz reloading, level 0x04 scene 6, user 2026-11-05) could
     * never be skipped: there a press skips at once, up to its last tick. */
    fps = (int)MEMF(0x007F9F5Cu);
    fade = ((fps * 0x1E00) / 0x3C) >> 8;
    gate = fps + fade;
    left = (int)MEM32(0x0076F0E8u + slot * 4u) - (int)MEM32(0x0076F0F8u + slot * 4u);
    if (edge && held && left > 0
        && (int)MEM32(0x0076F0E8u + slot * 4u) < (int)(SKIP_HOLD_MS / 1000.0 * fps) + gate
        && skip_switch(&g_skip_on, "RECOMP_CUTSCENE_SKIP")
        && lv != 0x21u && lv != 0x46u && lv != 0x47u && lv <= 0x4Au
        && MEM32(0x0069CF90u + lv * 4u) != 0) {
        why = skip_lifted(lv, scene);
        if (log)
            fprintf(stderr, "[SKIP] level 0x%02X scene %u (%s, short: %d ticks): skipped on a "
                    "press, %d left\n", lv, scene, why ? why : "skippable",
                    (int)MEM32(0x0076F0E8u + slot * 4u), left);
        g_skip_t0 = 0.0;
        skip_cover_start(key);
        InterlockedExchange(&s_skip_lt_block, 1);
        skip_short_now(fade);
        g_eax = 0;
        return;
    }
    if (g_eax) {
        if (log)
            fprintf(stderr, "[SKIP] level 0x%02X scene %u: the game's own skip\n", lv, scene);
        g_skip_t0 = 0.0;
        if (held)
            InterlockedExchange(&s_skip_lt_block, 1);
        return;
    }
    if (!held) {
        if (g_skip_t0 > 0.0 && log)
            fprintf(stderr, "[SKIP] let go after %.0f ms\n", now - g_skip_t0);
        g_skip_t0 = 0.0;
        wait_up = 0;
        return;
    }
    if (!skip_switch(&g_skip_on, "RECOMP_CUTSCENE_SKIP") || wait_up
        || lv == 0x21u || lv == 0x46u || lv == 0x47u || lv > 0x4Au
        || MEM32(0x0069CF90u + lv * 4u) == 0
        || (why = skip_lifted(lv, scene)) == NULL)
        return;
    if (left <= gate) {
        if (edge && log)
            fprintf(stderr, "[SKIP] level 0x%02X scene %u (%s): no skip point ahead (%d ticks)\n",
                    lv, scene, why, left);
        return;
    }
    if (!skip_switch(&g_skip_hold, "RECOMP_SKIP_HOLD")) {
        if (edge) {
            if (log)
                fprintf(stderr, "[SKIP] level 0x%02X scene %u (%s): skipped on a press\n",
                        lv, scene, why);
            skip_cover_start(key);
            InterlockedExchange(&s_skip_lt_block, 1);
            g_eax = 1;
        }
        return;
    }
    if (g_skip_t0 <= 0.0) {
        g_skip_t0 = now;
        if (log)
            fprintf(stderr, "[SKIP] level 0x%02X scene %u (%s): hold started\n",
                    lv, scene, why);
        return;
    }
    if (now - g_skip_t0 >= SKIP_HOLD_MS) {
        if (log)
            fprintf(stderr, "[SKIP] level 0x%02X scene %u (%s): skipped after %.0f ms\n",
                    lv, scene, why, now - g_skip_t0);
        g_skip_t0 = 0.0;
        wait_up = 1;
        skip_cover_start(key);
        InterlockedExchange(&s_skip_lt_block, 1);
        g_eax = 1;
    }
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
static volatile int g_fps = -1;        /* frame rate; < 0 = not read yet */
static volatile int g_dip_fix = -1;    /* dip fix at 30; < 0 = not read yet */
static uint32_t g_asked_interval;      /* the game's own, from the reset */

/* Above 60 (to-do 20): where the game asks for interval 2 it gets 1, as at
 * 60, and the toolkit's vblank clock runs at the frame rate instead of 60
 * (xbox_SetVblankRate; VSync then locks it to a monitor at that rate). The
 * game counts those faster vblanks as 60 Hz ones, so frame_timer_update's
 * results are set right before its last call (sub_00378620_enter):
 * step = vblanks x 30 / rate (0.25 at 120), and the whole 60 Hz ticks
 * [0x007FA20C] that ~200 readers add up (timers, countdowns, texture
 * animation) come out of an accumulator, 1 0 1 0 ... at 120, so they
 * still add up to 60 a second. The game's own frame divider [0x007FA204]
 * (a debug slow motion) does the same: step below one tick, 0-tick frames.
 * Everything frame_timer_update derives from the two is redone from the
 * values it started with (kept in sub_002A7B60_enter). */
#define FT_TICKS    0x007FA20Cu   /* 60 Hz ticks this frame (int) */
#define FT_TICKS2   0x007FA208u   /* copy of it */
#define FT_STEP     0x007FA24Cu   /* float, 1.0 = a 30 fps frame */
#define FT_HALF     0x007F9F34u   /* u16 tick counter (30 Hz in bit 1+) */
#define FT_HALF_OUT 0x007FA250u   /* u8: 30 Hz frames this frame */
#define FT_TOTAL    0x007FA244u   /* ticks since start; % 30 in 0x007FA240 */
#define FT_HIST     0x007F9EF8u   /* 10 floats: the last steps */
#define FT_HIST_I   0x007F9F21u   /* u8 next history slot */
#define FT_HIST_N   0x007F9F20u   /* u8 history filled, up to 10 */
static int g_vblank_rate = 60;         /* what the toolkit clock runs at */
static struct {
    int armed;                         /* this frame's results to redo */
    uint16_t half;
    uint32_t total;
    uint8_t hist_i, hist_n;
    unsigned acc;                      /* ticks x rate, carried */
} s_ft;

#define FRAME_UNCAPPED 1000           /* "uncapped": the clock's top rate */
extern void xbox_SetVblankRate(int hz);
extern int xbox_VsyncCapHz(void);

static int framestep_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("RECOMP_FRAMESTEP_LOG");
        on = s ? (atoi(s) > 1 ? atoi(s) : 1) : 0;
    }
    return on;
}

/* 30 (original), 60, or above (90-1000; "uncapped" = 1000). */
static int frame_rate(void)
{
    if (g_fps < 0) {
        const char *e = getenv("RECOMP_FRAME_RATE");
        int r = !e ? 30 : !_stricmp(e, "uncapped") ? FRAME_UNCAPPED : atoi(e);
        g_fps = r < 60 ? 30 : r > FRAME_UNCAPPED ? FRAME_UNCAPPED : r;
    }
    return g_fps;
}

/* The vblank clock for the game's interval: 60, or the frame rate above
 * 60, capped at the monitor's refresh with VSync on and under it with vrr
 * (xbox_VsyncCapHz). */
static int clock_rate(void)
{
    int r = frame_rate(), cap = xbox_VsyncCapHz();

    if (g_asked_interval != 2u || r <= 60)
        return 60;
    if (cap && r > cap)
        r = cap < 60 ? 60 : cap;
    return r;
}

static int fps60_on(void)
{
    return frame_rate() >= 60;
}

static int dip_fix_on(void)
{
    if (g_dip_fix < 0) {
        const char *e = getenv("RECOMP_DIP_FIX");
        g_dip_fix = !e || atoi(e) != 0;
    }
    return fps60_on() || g_dip_fix;
}

/* From the F2 menu (overlay.cpp): 30, 60 or above (0 = uncapped);
 * applies from the next frame. */
void conker_SetFrameRate(int fps)
{
    if (fps == 0)
        fps = FRAME_UNCAPPED;
    g_fps = fps < 60 ? 30 : fps > FRAME_UNCAPPED ? FRAME_UNCAPPED : fps;
}

void conker_SetDipFix(int on)
{
    g_dip_fix = on != 0;
}

/* The interval the game should run at: its own, or 1 for its 2 at 60 and
 * above. */
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
    /* The last frame's clocks: ticks, 30 Hz frames, step, dt. */
    hdr[0] = MEM32(0x007FA20Cu);
    hdr[1] = MEM8(0x007FA250u);
    hdr[2] = MEM32(0x007FA24Cu);
    hdr[3] = MEM32(0x007FA21Cu);
    fwrite(hdr, 16, 1, f);
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
    /* LT held through a cutscene skip: hidden until let go. */
    if (s_skip_lt_block) {
        if (a[XBOX_BUTTON_LTRIGGER] > XBOX_ANALOG_BUTTON_THRESHOLD)
            a[XBOX_BUTTON_LTRIGGER] = 0;
        else
            InterlockedExchange(&s_skip_lt_block, 0);
    }
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
    skip_cover_poll();
    mp = lv == 0x46u || lv > 0x4Au || MEM32(0x0069CF90u + lv * 4u) == 0;
    /* The front end also plays story scenes (New Game: Berri's room, the
     * bar): cutscene keys there while one runs (no list on screen). */
    if (lv == 0x1Du && MEM8(0x0076F0E0u) == 1 && !MEM8(0x007FA275u)
        && !(s_ctl_list_frame && s_ctl_frame - s_ctl_list_frame <= 2))
        set = CONTROLS_CUTSCENE;
    else if (lv == 0x1Du || lv == 0x46u)
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
    controls_set_mode(set);           /* every frame: latched skip keys */
    if (set == last)
        return;
    last = set;
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
    precompile_poll();                  /* --precompile-shaders only */
    controls_poll();
    photo_frame_begin();
    if (g_asked_interval) {
        int rate = clock_rate();
        want = frame_interval(g_asked_interval);
        if (MEM8(0x006C0268u) != want || rate != g_vblank_rate) {
            MEM8(0x006C0268u) = (uint8_t)want;
            MEM32(0x00549BF4u) = (MEM32(0x00549BF4u) & 0x80000000u) | want;
            if (rate != g_vblank_rate) {
                g_vblank_rate = rate;
                s_ft.acc = 0;
                xbox_SetVblankRate(rate);
            }
            fprintf(stderr, "[FSTEP] frame rate: interval %u, vblank clock %d Hz"
                    " (%d fps)\n", want, rate,
                    want == 1u ? rate : want == 2u ? 30 : 0);
        }
    }
    /* Above 60: keep what frame_timer_update starts from, to redo its
     * results (sub_00378620_enter). Not in its reset frames (the step is
     * fixed at 1.0 there) or with the game's own divider. */
    s_ft.armed = g_vblank_rate > 60 && !MEM8(0x007FA23Cu) && !MEM32(0x008758BCu)
              && !MEM8(0x007FA204u);
    if (s_ft.armed) {
        s_ft.half = MEM16(FT_HALF);
        s_ft.total = MEM32(FT_TOTAL);
        s_ft.hist_i = MEM8(FT_HIST_I);
        s_ft.hist_n = MEM8(FT_HIST_N);
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

/* 0x00378620 (texture animation counters, reads the ticks): frame_timer_
 * update's last call, on every path. Above 60 its results are redone here
 * (see g_vblank_rate): the same arithmetic as 0x002A7E89-0x002A7F7F with
 * this rate's step and ticks. Constants read from where the game's code
 * reads them: 0x00648E38 (60.0), 0x00649880 (the 50 Hz factor), 0x00648D80
 * (0.1). */
void sub_00378620_enter(void)
{
    uint32_t vb, ticks;
    uint16_t half;
    float step, inv, avg;
    int i;

    if (!s_ft.armed)
        return;
    s_ft.armed = 0;
    vb = MEM32(FT_TICKS);              /* vblanks at g_vblank_rate */
    s_ft.acc += vb * 60u;
    ticks = s_ft.acc / (unsigned)g_vblank_rate;
    s_ft.acc -= ticks * (unsigned)g_vblank_rate;
    step = (float)vb * 30.0f / (float)g_vblank_rate;
    MEM32(FT_TICKS) = ticks;
    MEM32(FT_TICKS2) = ticks;
    if (MEMF(0x007F9F58u) == MEMF(0x00648E38u)) {
        half = (uint16_t)((s_ft.half & 1u) + ticks);
        MEM16(FT_HALF) = half;
        MEM8(FT_HALF_OUT) = (uint8_t)(MEM32(FT_HALF) >> 1);
    } else {
        half = (uint16_t)(ticks * 0x266u + (s_ft.half & 0x3FFu));
        MEM16(FT_HALF) = half;
        MEM8(FT_HALF_OUT) = (uint8_t)(half >> 10);
    }
    inv = 0.0f;
    if (step != 0.0f) {
        inv = 1.0f / step;
        if (MEMF(0x007F9F5Cu) != MEMF(0x00648E38u))
            inv *= MEMF(0x00649880u);
    }
    MEMF(FT_STEP) = step;
    if (MEM8(0x007819D5u)) {
        /* Multiplayer ([0x007819D5], 2026-11-05): frame_timer_update takes
         * dt from the vblanks, vblanks x 1/60 s, instead of the rdtsc time,
         * so on our faster vblank clock dt came to 2 game seconds a wall
         * second at 120 (the Sturm21 fired 27-30 shots a second against
         * 15 at 30, everything timed in seconds ran double). Same formula
         * with this clock's vblank: frame time [0x007FA218] = vblanks /
         * rate, dt [0x007FA21C] = that unless paused, and what is derived
         * from them (ms, us, 1 / dt, the two running sums). */
        float ft = (float)vb / (float)g_vblank_rate, dt;
        float old_ft = MEMF(0x007FA218u), old_dt = MEMF(0x007FA21Cu);
        int32_t us;
        if (ft > 1.0f)
            ft = 1.0f;
        dt = MEM8(0x007FA275u) ? 0.0f : ft;
        MEMF(0x007FA218u) = ft;
        MEMF(0x007FA21Cu) = dt;
        MEM32(0x007FA220u) = (uint32_t)(int32_t)(dt * 1000.0f);
        us = (int32_t)(dt * 1e6f);
        MEM32(0x007FA228u) = (uint32_t)us;
        MEM32(0x007FA22Cu) = (uint32_t)(us >> 31);
        MEMF(0x007F9F4Cu) += dt - old_dt;
        MEMF(0x007F9F48u) += ft - old_ft;
        MEMF(0x007FA214u) = MEM8(0x007FA275u) ? 999999.0f : 1.0f / dt;
    }
    MEM32(FT_TOTAL) = s_ft.total + ticks;
    MEM32(0x007FA240u) = (uint32_t)((int32_t)MEM32(FT_TOTAL) % 30);
    MEMF(0x007FA248u) = inv;
    MEMF(FT_HIST + 4u * s_ft.hist_i) = step;
    if (s_ft.hist_n < 10) {
        MEMF(0x007FA238u) = step;
        MEMF(0x007FA234u) = inv;
    } else {
        avg = 0.0f;
        for (i = 0; i < 10; i++)
            avg += MEMF(FT_HIST + 4u * (uint32_t)i);
        avg *= MEMF(0x00648D80u);
        MEMF(0x007FA238u) = avg;
        MEMF(0x007FA234u) = avg != 0.0f ? 1.0f / avg : 0.0f;
    }
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
/* Characters drawn two-sided in their opaque passes (to-do 80): a lost arm
 * leaves the jacket open at the shoulder, and the stump pieces
 * (r_armstumpShape, l_armstumpShape; gore parts "|_PART_STUMP", HOLE, GASH,
 * HEADTOP...) show only their outside, so the inside of the sleeve was culled
 * and the tail behind showed through (It's War opening cutscene, ~135 s; the
 * real game does the same, user's gameplay video). While s_two_sided is set,
 * D3DDevice_SetRenderState_CullMode (0x00537070, stdcall, [esp+4] mode; the
 * device keeps it at 0x00549C44) gets D3DCULL_NONE and the mode the game
 * asked for is kept for afterwards. RECOMP_CHAR_TWO_SIDED=0 turns it off. */
static int s_two_sided;
static uint32_t s_cull_wanted;

static int two_sided_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CHAR_TWO_SIDED");
        on = !(e && *e == '0');
    }
    return on;
}

/* Not for a character the camera is inside (to-do 97): leaving the head-cam
 * the camera starts in Conker's head and backs out over a few frames, and
 * two-sided it showed the inside of the head and his eyes from behind (the
 * game draws no Conker in head-cam itself: his skin matrices are all 0).
 * Inside = the render camera's position ([0x0084B868] inverse view +0x50,
 * row 3) within CHAR_INSIDE_R model units of one of the character's joints
 * (bind position +0xC x its skin matrix, scaled by that matrix's row 0).
 * Conker leaving head-cam: 9, 13, 28 world units from an ear joint on the
 * first three frames, from joint 15 (top of the head). RECOMP_CHAR_INSIDE_R
 * sets the radius (0 = off), RECOMP_CHAR_INSIDE_LOG=1 [INSIDE] lines. */
#define JREC_BIND(jt, j) ((jt) + (uint32_t)(j) * 0x24u + 0xCu)

static int char_cam_inside(uint32_t idx)
{
    static float r = -1.0f;
    static int log = -1;
    uint32_t base = MEM32(0x0084A5F8u), c, bones, def, joints, rc;
    float ex, ey, ez, best = 1e30f, s = 0.0f;
    int n, j, bj = -1;

    if (r < 0.0f) {
        const char *e = getenv("RECOMP_CHAR_INSIDE_R");
        r = e && *e ? (float)atof(e) : 40.0f;
        e = getenv("RECOMP_CHAR_INSIDE_LOG");
        log = e && *e == '1';
    }
    rc = MEM32(0x0084B868u);
    if (r <= 0.0f || idx >= 0x80 || !xact_va_ok(base) || !xact_va_ok(rc))
        return 0;
    c = base + idx * 0x6D0u;
    n = p30_char(c, &bones, &def, &joints);
    if (!n || !xact_va_ok(joints))
        return 0;
    ex = MEMF(rc + 0x80u); ey = MEMF(rc + 0x84u); ez = MEMF(rc + 0x88u);
    for (j = 0; j < n; j++) {
        const float *m = p30_bone(bones, j);
        float bx = MEMF(JREC_BIND(joints, j)), by = MEMF(JREC_BIND(joints, j) + 4u),
              bz = MEMF(JREC_BIND(joints, j) + 8u);
        float dx = bx * m[0] + by * m[4] + bz * m[8] + m[12] - ex;
        float dy = bx * m[1] + by * m[5] + bz * m[9] + m[13] - ey;
        float dz = bx * m[2] + by * m[6] + bz * m[10] + m[14] - ez;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < best) {
            best = d;
            bj = j;
            s = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        }
    }
    if (bj < 0 || !(s > 0.0f))
        return 0;
    if (log && best < s * r * r * 9.0f)
        fprintf(stderr, "[INSIDE] t=%ld ms char %u id %02X joint %d %.1f world, %.1f model%s\n",
                attach_ms(), idx, MEM8(c + 0x10Bu), bj, sqrtf(best), sqrtf(best / s),
                best < s * r * r ? ": one-sided" : "");
    return best < s * r * r;
}

extern void sub_00537070_gen(void);
void sub_00537070(void)
{
    if (s_two_sided) {
        s_cull_wanted = MEM32(g_esp + 4u);
        MEM32(g_esp + 4u) = 0;
    }
    sub_00537070_gen();
}

static void set_cull_mode(uint32_t mode)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    g_esp -= 4; MEM32(g_esp) = mode;
    g_esp -= 4; MEM32(g_esp) = 0;       /* return address, popped by the callee */
    sub_00537070_gen();
    g_eax = eax; g_ecx = ecx; g_edx = edx;
}

/* Lost arm drawn at rest (to-do 80 part 2): an arm shot off is hidden by
 * its part's visible bit (_PART_RIGHTARM / _PART_LEFTARM; part table
 * [[[char+0x4B0+lod*4]+0x24]+8]+0x14 = {count, records of 0x14: +4 name,
 * +8 bit}, bits at [[char+0x4C4+lod*4]+0xC], MSB first, 0x00081E60), but its
 * triangles are a range of the same skinned meshes as the jacket, and the
 * animation still swings its bones. The jacket's shoulder edge follows the
 * swinging upper arm while the stump (jacket cuff + flesh cap) is an
 * attachment on the clavicle (attachment +4 bone; its matrix is copied from
 * the clavicle's skin matrix by attach_update_all 0x00076F10 into
 * [att+0x60] + [0x007FA230]*64, the frame's half of a double buffer), so a
 * gap opened at the shoulder top (It's War opening cutscene, ~135 s; the
 * real game too). While such a character draws, the lost arm's clavicle and
 * every joint below it get the chest's skin matrix ([char+0x3C8] + joint*64,
 * read through 0x0081BD80 by the palette build), and so does the stump
 * attachment while it draws (stump_att_begin, from sub_000780E0): jacket and
 * stump in their bind fit on the chest (the cuff points out sideways, its
 * rest). Put back after each draw. Tried first (user's close-ups 2026-11-04):
 * the chain on the chest with the stump left on the clavicle, and the joints
 * below the clavicle on the clavicle; both left the gap, because the jacket's
 * edge mostly follows the chest. Live test that closed it: _work\sold80\poke.py. The
 * clavicles are found in the joint records ([[char+0x4EC+lod*4]+0x34], 0x24
 * each: +0xC bind position, +0x18 u16 parent, first child, next sibling):
 * the highest pair of mirrored siblings with three levels below them (not
 * hips or eyes); -x = the right arm (squirrel soldier: chest 3, right
 * clavicle 8, left 9; +x moved the arm he still has, user 2026-11-04).
 * RECOMP_STUMP_FIX=0 turns it off; [STUMP] line once per character and state. */
#define STUMP_MAX 32
static uint32_t s_stump_bones;          /* bone buffer while swapped, 0 = none */
static int s_stump_n;
static uint8_t s_stump_joint[STUMP_MAX];
static uint8_t s_stump_from[STUMP_MAX];
static uint32_t s_stump_saved[STUMP_MAX][16];
static uint8_t s_stump_logged[0x80];
static uint8_t s_stump_idx_by_id[256];  /* character id -> index + 1 while an arm is lost */

static int stump_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_STUMP_FIX");
        on = !(e && *e == '0');
    }
    return on;
}

/* Bit 0 = right arm hidden, bit 1 = left. */
static int stump_lost_arms(uint32_t c, uint32_t lod)
{
    char name[64];
    uint32_t m = MEM32(c + 0x4B0u + lod * 4), inst = MEM32(c + 0x4C4u + lod * 4);
    uint32_t t, bits, n, ents, i, b;
    int lost = 0;

    if (!xact_va_ok(m) || !xact_va_ok(inst) || !xact_va_ok(m = MEM32(m + 0x24u))
        || !xact_va_ok(m = MEM32(m + 8u)) || !xact_va_ok(t = MEM32(m + 0x14u))
        || !xact_va_ok(bits = MEM32(inst + 0xCu)) || !xact_va_ok(ents = MEM32(t + 4u)))
        return 0;
    n = MEM32(t);
    for (i = 0; i < n && i < 64; i++) {
        b = MEM32(ents + i * 0x14u + 8u);
        if (b >= 256 || (MEM8(bits + (b >> 3)) >> (7 - (b & 7))) & 1)
            continue;
        char_guest_str(MEM32(ents + i * 0x14u + 4u), name, sizeof name);
        if (strstr(name, "_PART_RIGHTARM"))
            lost |= 1;
        else if (strstr(name, "_PART_LEFTARM"))
            lost |= 2;
    }
    return lost;
}

#define JREC(jt, j) ((jt) + (uint32_t)(j) * 0x24u)

static int stump_depth(uint32_t jt, uint32_t j, int level)
{
    uint32_t k;
    int d = 0, x, guard = 0;
    if (level > 8)
        return 0;
    for (k = MEM16(JREC(jt, j) + 0x1Au); k < 128 && guard++ < 32;
         k = MEM16(JREC(jt, k) + 0x1Cu)) {
        x = stump_depth(jt, k, level + 1);
        if (x > d)
            d = x;
    }
    return d + 1;
}

/* Add j and every joint below it, each taking the matrix of joint 'from'. */
static void stump_add_chain(uint32_t jt, uint32_t j, uint32_t from, int level)
{
    uint32_t k;
    int guard = 0;
    if (level > 8 || s_stump_n >= STUMP_MAX)
        return;
    s_stump_joint[s_stump_n] = (uint8_t)j;
    s_stump_from[s_stump_n++] = (uint8_t)from;
    for (k = MEM16(JREC(jt, j) + 0x1Au); k < 128 && guard++ < 32;
         k = MEM16(JREC(jt, k) + 0x1Cu))
        stump_add_chain(jt, k, from, level + 1);
}

/* Fills s_stump_joint / s_stump_from for the lost arms; 0 = no shoulder found. */
static int stump_find(uint32_t jt, int lost, int *clav_out)
{
    uint32_t stack[128], p, a, b;
    int sp = 0, seen = 0, best_a = -1, best_b = -1, ga, gb;
    float best_y = -1e30f;

    stack[sp++] = 0;
    while (sp && seen++ < 128) {
        p = stack[--sp];
        ga = 0;
        for (a = MEM16(JREC(jt, p) + 0x1Au); a < 128 && ga++ < 32; a = MEM16(JREC(jt, a) + 0x1Cu)) {
            float ax = MEMF(JREC(jt, a) + 0xCu), ay = MEMF(JREC(jt, a) + 0x10u),
                  az = MEMF(JREC(jt, a) + 0x14u);
            if (sp < 127)
                stack[sp++] = a;
            gb = 0;
            for (b = MEM16(JREC(jt, a) + 0x1Cu); b < 128 && gb++ < 32; b = MEM16(JREC(jt, b) + 0x1Cu)) {
                float bx = MEMF(JREC(jt, b) + 0xCu), by = MEMF(JREC(jt, b) + 0x10u),
                      bz = MEMF(JREC(jt, b) + 0x14u);
                if (fabsf(ax) > 1.0f && fabsf(ax + bx) < 0.5f && fabsf(ay - by) < 0.5f
                    && fabsf(az - bz) < 0.5f && ay > best_y
                    && stump_depth(jt, a, 0) >= 4 && stump_depth(jt, b, 0) >= 4) {
                    best_y = ay;
                    best_a = ax < 0 ? (int)a : (int)b;      /* right */
                    best_b = ax < 0 ? (int)b : (int)a;
                }
            }
        }
    }
    if (best_a < 0)
        return 0;
    s_stump_n = 0;
    clav_out[0] = best_a;
    clav_out[1] = best_b;
    for (ga = 0; ga < 2; ga++) {        /* the clavicle and below, on the chest */
        uint32_t clav = (uint32_t)clav_out[ga];
        if (lost & (1 << ga))
            stump_add_chain(jt, clav, MEM16(JREC(jt, clav) + 0x18u), 0);
    }
    return s_stump_n;
}

static void stump_apply(void)
{
    int i;
    for (i = 0; i < s_stump_n; i++)
        memcpy((void *)XBOX_PTR(s_stump_bones + s_stump_joint[i] * 64u),
               (const void *)XBOX_PTR(s_stump_bones + s_stump_from[i] * 64u), 64);
}

static void stump_restore(void)
{
    int i;
    for (i = s_stump_n - 1; i >= 0; i--)
        memcpy((void *)XBOX_PTR(s_stump_bones + s_stump_joint[i] * 64u), s_stump_saved[i], 64);
}

/* Swaps the matrices in for a character with a lost arm; 1 = swapped. */
static int stump_begin(uint32_t idx)
{
    uint32_t base = MEM32(0x0084A5F8u), c, lod, bones, jt;
    int lost, clav[2], i;

    if (!stump_fix_on() || s_stump_bones || idx >= 0x80 || !xact_va_ok(base))
        return 0;
    c = base + idx * 0x6D0u;
    lod = MEM8(c + 0x3A8u);
    if (lod > 4 || !xact_va_ok(bones = MEM32(c + 0x3C8u)))
        return 0;
    lost = stump_lost_arms(c, lod);
    if (MEM8(c + 0x10Bu))
        s_stump_idx_by_id[MEM8(c + 0x10Bu)] = lost ? (uint8_t)(idx + 1) : 0;
    if (s_stump_logged[idx] != (uint8_t)(lost + 1)) {
        if (lost || s_stump_logged[idx])
            fprintf(stderr, "[STUMP] t=%ld ms char %u id %02X arms lost: %s%s%s\n", attach_ms(),
                    idx, MEM8(c + 0x10Bu), lost & 1 ? "right " : "", lost & 2 ? "left " : "",
                    lost ? "" : "none");
        s_stump_logged[idx] = (uint8_t)(lost + 1);
        if (lost) {
            jt = MEM32(MEM32(c + 0x4ECu + lod * 4) + 0x34u);
            if (!xact_va_ok(jt) || !stump_find(jt, lost, clav))
                fprintf(stderr, "[STUMP]   no shoulder pair found in the skeleton: drawn as is\n");
            else
                fprintf(stderr, "[STUMP]   clavicles right %d left %d; %d joints on the chest %u\n",
                        clav[0], clav[1], s_stump_n, s_stump_n ? s_stump_from[0] : 0);
        }
    }
    if (!lost)
        return 0;
    jt = MEM32(MEM32(c + 0x4ECu + lod * 4) + 0x34u);
    if (!xact_va_ok(jt) || !stump_find(jt, lost, clav))
        return 0;
    s_stump_bones = bones;
    for (i = 0; i < s_stump_n; i++)
        memcpy(s_stump_saved[i], (const void *)XBOX_PTR(bones + s_stump_joint[i] * 64u), 64);
    stump_apply();
    return 1;
}

static void stump_end(void)
{
    stump_restore();
    s_stump_bones = 0;
}

/* The stump is an attachment on the clavicle with its own matrix, copied
 * from the clavicle by attach_update_all into [att+0x60] + [0x007FA230]*64
 * (plain copy: +0x1E != 4, +0x2C = 0, +0xC0 = 0). While it draws, that half
 * gets the matrix the joint has under the swap; 1 = patched (saved). */
static uint32_t s_stump_att_m;
static uint32_t s_stump_att_saved[16];

static int stump_att_begin(uint32_t rec)
{
    uint32_t bone = MEM16(rec + 4u), m = MEM32(rec + 0x60u);
    int i;
    if (!s_stump_bones || !xact_va_ok(m) || MEM8(rec + 0x1Eu) == 4 || MEM16(rec + 0x2Cu)
        || MEM32(rec + 0xC0u) || MEM32(rec + 0xC4u) || MEM16(rec + 0x28u))
        return 0;
    for (i = 0; i < s_stump_n && s_stump_joint[i] != bone; i++)
        ;
    if (i == s_stump_n)
        return 0;
    s_stump_att_m = m + (MEM8(0x007FA230u) & 1) * 64u;
    memcpy(s_stump_att_saved, (const void *)XBOX_PTR(s_stump_att_m), 64);
    memcpy((void *)XBOX_PTR(s_stump_att_m),
           (const void *)XBOX_PTR(s_stump_bones + s_stump_from[i] * 64u), 64);
    return 1;
}

static void stump_att_end(void)
{
    memcpy((void *)XBOX_PTR(s_stump_att_m), s_stump_att_saved, 64);
}

/* 0x0007FB00 char_render_pass (cdecl, [esp+4] character index, [esp+0x14]
 * pass: 1 OPAQUE, 12 OPAQUE_BACKGROUND, conker-notes 2026-10-29). */
extern void sub_0007FB00_gen(void);
void sub_0007FB00(void)
{
    uint32_t pass = MEM32(g_esp + 0x14u);
    int two, stump;

    if (photo_hides_char(MEM32(g_esp + 4u))) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    two = !s_two_sided && (pass == 1 || pass == 12) && two_sided_on()
        && !char_cam_inside(MEM32(g_esp + 4u));
    if (two) {
        s_cull_wanted = MEM32(0x00549C44u);
        set_cull_mode(0);
        s_two_sided = 1;
    }
    stump = stump_begin(MEM32(g_esp + 4u));
    sub_0007FB00_gen();
    if (stump)
        stump_end();
    if (two) {
        s_two_sided = 0;
        set_cull_mode(s_cull_wanted);
    }
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
    if (rec && stump_fix_on() && s_stump_idx_by_id[MEM8(rec)]
        && MEM8(MEM32(0x0084A5F8u) + (s_stump_idx_by_id[MEM8(rec)] - 1u) * 0x6D0u + 0x10Bu)
               == MEM8(rec)) {
        /* lost arm: the stump and anything else on that arm on the chest */
        int own = !s_stump_bones && stump_begin(s_stump_idx_by_id[MEM8(rec)] - 1u);
        int att = stump_att_begin(rec);
        sub_000780E0_gen();
        if (att)
            stump_att_end();
        if (own)
            stump_end();
        return;
    }
    sub_000780E0_gen();
}

/* 0x0015CAF0 (cdecl): the draw of object types 0x7A (layer 3, drawn by
 * 0x000D9950 every frame, paused too): the weapon sight, e.g. the SMG's
 * green cross in Chemical Warfare; and 0x72, the vignette (below). Part
 * of photo mode's HUD. */
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

/* 0x001557D0 (fastcall, ecx = the object): the underwater dome, drawn last
 * while Conker is under water: 12 bands of 16 quads round the camera,
 * 12000 wide and 2000 tall, two scrolling grey-teal textures, colour =
 * tex0 x vertex colour + tex1 x TFACTOR (both 0x808080), no depth test.
 * Its middle bands pile up into a band at eye level, the blue strip under
 * the poo after the Bull fight (GitHub issue 8; conker-notes 2026-11-03;
 * xemu shows it too). RECOMP_UNDERWATER_OVERLAY=0 (underwater_overlay = 0)
 * leaves it out everywhere; RECOMP_POO_FIX (poo_fix, default on) draws it
 * brown in the poo level (0x2C), with both colours set to s_dome_brown: the
 * TFACTOR in sub_00537360_enter, the vertex colours in the pushbuffer
 * itself, where the dome writes each band inline (dome_patch_band).
 * conker_SetUnderwaterOverlay and conker_SetPooFix from the F2 menu. */
#define DOME_LEVEL_POO 0x2Cu
/* Times the textures (about 0.4 grey-teal, greener in places), twice:
 * the poo's own colour round the band (99, 65, 25 in run_20261103_100920),
 * measured against the band in the picture (user: 0x804A1C still showed
 * green). RECOMP_POO_TINT_FILE=<file> (hex RRGGBB in it, read again every
 * 60 domes) tries others live. */
#define DOME_BROWN 0x007C3612u
static uint32_t s_dome_brown = DOME_BROWN;
static int s_dome_count;

static void dome_tint_reload(void)
{
    static const char *path;
    static int checked;
    FILE *f;
    unsigned v;
    if (!checked) {
        path = getenv("RECOMP_POO_TINT_FILE");
        checked = 1;
    }
    if (!path || s_dome_count++ % 60)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    if (fscanf(f, "%x", &v) == 1 && (v & 0x00FFFFFFu) != s_dome_brown) {
        s_dome_brown = v & 0x00FFFFFFu;
        fprintf(stderr, "[DOME] tint now %06X\n", s_dome_brown);
    }
    fclose(f);
}

static volatile int g_uw_overlay = -1, g_poo_fix = -1;
static int s_dome_tint;                 /* the dome now drawing is tinted */

void conker_SetUnderwaterOverlay(int on)
{
    g_uw_overlay = on != 0;
}

void conker_SetPooFix(int on)
{
    g_poo_fix = on != 0;
}

static int env_switch(volatile int *v, const char *name)
{
    if (*v < 0) {
        const char *e = getenv(name);
        *v = !(e && *e == '0');
    }
    return *v;
}

static void dome_tint_factor(void)
{
    if (s_dome_tint)
        MEM32(g_esp + 4) = (MEM32(g_esp + 4) & 0xFF000000u) | s_dome_brown;
}

/* One band as the dome writes it, ending at end: BEGIN quads, an inline
 * array of 64 vertices (x y z, colour, u0 v0, u1 v1), END. Its vertex
 * colours (alpha = the band's) turn brown. Called before the next D3D call
 * that could kick the pushbuffer, so the GPU never read the band yet. */
/* RECOMP_DOME_LOG=1: a [DOME] line per tinted dome (bands patched, calls
 * where none was found and the words there), at most 20. */
static int s_dome_log = -1, s_dome_lines;
static uint32_t s_dome_hits, s_dome_bands, s_dome_miss_at, s_dome_miss_w;

static void dome_patch_band(uint32_t end)
{
    uint32_t p = end - 517u * 4u, i, c;
    s_dome_hits++;
    if (MEM32(p) != 0x000417FCu || MEM32(p + 4) != 8u
        || MEM32(p + 8) != 0x48001818u || MEM32(end - 8) != 0x000417FCu
        || MEM32(end - 4) != 0u) {
        s_dome_miss_at = end;
        s_dome_miss_w = MEM32(p);
        return;
    }
    s_dome_bands++;
    for (i = 0; i < 64; i++) {
        c = MEM32(p + 12u + i * 32u + 12u);
        if ((c & 0x00FFFFFFu) == 0x00808080u)
            MEM32(p + 12u + i * 32u + 12u) = (c & 0xFF000000u) | s_dome_brown;
    }
}

static uint32_t pb_put(void)
{
    return MEM32(MEM32(0x005499E8u));
}

/* 0x00543200 (D3D's SetStateVB), at the top of each band but the first:
 * the band before ends at the put pointer. */
void sub_00543200_enter(void)
{
    if (s_dome_tint && MEM32(g_esp) == 0x00155C19u)
        dome_patch_band(pb_put());
}

/* 0x0053C300 (MakeRequestedSpace) after the last band, if it runs. */
void sub_0053C300_enter(void)
{
    if (s_dome_tint && MEM32(g_esp) == 0x0015603Cu)
        dome_patch_band(pb_put());
}

/* 0x00538690 (SetPixelShader), the dome's last call: the last band ends
 * two words (a pushed method and its value) before the put pointer. */
void sub_00538690_enter(void)
{
    if (s_dome_tint && MEM32(g_esp) == 0x00156063u)
        dome_patch_band(pb_put() - 8u);
}

extern void sub_001557D0_gen(void);
void sub_001557D0(void)
{
    if (!env_switch(&g_uw_overlay, "RECOMP_UNDERWATER_OVERLAY")) {
        g_esp += 4;                     /* ret */
        return;
    }
    s_dome_tint = env_switch(&g_poo_fix, "RECOMP_POO_FIX")
                  && MEM32(0x007FA1F8u) == DOME_LEVEL_POO;
    if (s_dome_tint)
        dome_tint_reload();
    s_dome_hits = s_dome_bands = s_dome_miss_at = s_dome_miss_w = 0;
    sub_001557D0_gen();
    if (s_dome_tint) {
        if (s_dome_log < 0) {
            const char *e = getenv("RECOMP_DOME_LOG");
            s_dome_log = e && *e == '1';
        }
        if (s_dome_log && s_dome_lines < 20) {
            s_dome_lines++;
            fprintf(stderr, "[DOME] tinted: %u calls, %u bands patched;"
                    " last miss at %08X (word %08X)\n", s_dome_hits,
                    s_dome_bands, s_dome_miss_at, s_dome_miss_w);
        }
    }
    s_dome_tint = 0;
}

/* 0x001B0B70 (cdecl): the draw of object type 0x70 (layer 2), a sprite
 * that bends the picture behind it (bump-env). With bit 0 of +0x30 set it
 * is drawn in screen space by 0x001B0730: the water and blood drops on the
 * screen and the streaks they leave (2026-10-31; bit 2 = tinted with the
 * colour at +0x100). The rest are in the world and stay.
 * RECOMP_SCREEN_DROPS=0 (or screen_drops = 0) leaves the screen ones out;
 * conker_SetScreenDrops from the F2 menu. */
static volatile int g_screen_drops = -1;

void conker_SetScreenDrops(int on)
{
    g_screen_drops = on != 0;
}

extern void sub_001B0B70_gen(void);
void sub_001B0B70(void)
{
    if (g_screen_drops < 0) {
        const char *e = getenv("RECOMP_SCREEN_DROPS");
        g_screen_drops = !(e && *e == '0');
    }
    if (!g_screen_drops && (MEM8(MEM32(g_esp + 4) + 0x30) & 1)) {
        g_esp += 4;                     /* ret (cdecl) */
        return;
    }
    sub_001B0B70_gen();
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

/* The same draw also does type 0x72 (layer 2): a full-screen vignette,
 * dark corners round a circle at 320,240 of radius 300 (e.g. Chemical
 * Warfare; 2026-10-31). RECOMP_VIGNETTE=0 (or vignette = 0) leaves it
 * out; conker_SetVignette from the F2 menu (GitHub issue 5). */
static volatile int g_vignette = -1;

void conker_SetVignette(int on)
{
    g_vignette = on != 0;
}

extern void sub_0015CAF0_gen(void);
void sub_0015CAF0(void)
{
    if (g_vignette < 0) {
        const char *e = getenv("RECOMP_VIGNETTE");
        g_vignette = !(e && *e == '0');
    }
    if (photo_hud_hidden() ||
        (!g_vignette && MEM8(MEM32(g_esp + 4)) == 0x72)) {
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
            MEM32(g_esp), want != g_asked_interval ? " (60 fps up: 1)" : "");
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
