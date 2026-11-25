/**
 * Extras, the Recomp Addons of 0.6.0 (F2 tab name: user 2026-11-23; addons.h, SPEC 10.4,
 * docs/research/r4-0.6.0-addons-plan.md).
 *
 * Weapon (to-do 44): Bat mode itself is the wrapper on player_has_bat
 * 0x002F6850 in recomp_manual.c; here the setting, its log and the Frying
 * Pan's swaps (below).
 *
 * No Key Goblings (to-do 46, "Key Goblin" in the plans): the training cave's key sequence is one task
 * script, TrainingFryingPan (task 5, a state machine in its BSS; r3-gargoyle).
 * Two 4-byte patches in the task's heap copy:
 *   A +0x44A: JMP 0x057A over "CREATE three Clangs + play scene 3", to the
 *     script's own "key pops out" step (state 4), so the key comes out
 *     after the tutorial bubble (and the original's 2-s wait), no goblins.
 *   B +0x642: JMP 0x0660 over the TELL 7 that puts the key at Conker's x/z:
 *     the CREATE already puts it at its level-table spot, where it bounced
 *     (user 2026-11-14); the TELL 166 hop stays (user 2026-11-16).
 * Both only while the state is below 3 (the goblins not out yet), and only
 * when the bytes around them are the ones checked (a different game copy
 * is left alone, one log line). A reload copies the task fresh
 * (task_copy_to_heap 0x0035D370): the next frame patches it again. Nothing
 * goes into a save.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "addons.h"

#define LEVEL         0x007FA1F8u
#define TASK_LIST     0x00801904u   /* first task; next at +0x34 */
#define TASK_NEXT     0x34u
#define TASK_BSSOFF   0x14u         /* the BSS's offset in the copy */
#define LVL_FIELD     0x29u         /* TrainingLevel */
#define LVL_CAVE      0x2Eu         /* TrainingCave */
#define KG_TASK_ID    5u            /* TrainingFryingPan */
#define KG_BSSOFF     0x8F0u
#define KG_STATE      8u            /* BSS +8 */
#define KG_GOBLINS    3u            /* state 3 = the goblins are out */

static int va_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x84000000u;
}

/* ── settings ─────────────────────────────────────────────────────────── */

static volatile int g_weapon = -1, g_key_goblin = -1;   /* < 0 = not read yet */

static const char *const s_weapon_names[] = { "both", "bat", "pan" };

static int weapon_from(const char *v)
{
    if (v && !_stricmp(v, "bat"))
        return ADDON_WEAPON_BAT;
    if (v && !_stricmp(v, "pan"))
        return ADDON_WEAPON_PAN;
    return ADDON_WEAPON_BOTH;
}

int addons_weapon(void)
{
    if (g_weapon < 0)
        g_weapon = weapon_from(getenv("RECOMP_WEAPON"));
    return g_weapon;
}

void addons_set_weapon(const char *value)
{
    int w = weapon_from(value);
    if (w != g_weapon)
        fprintf(stderr, "[WEAPON] weapon %s%s\n", s_weapon_names[w],
                (w == ADDON_WEAPON_PAN) != (addons_pan_on() > 0)
                    ? " (the frying pan comes or goes once he stands with no weapon out)" : "");
    g_weapon = w;
}

int addons_key_goblin(void)
{
    if (g_key_goblin < 0) {
        const char *e = getenv("RECOMP_NO_KEY_GOBLINGS");
        g_key_goblin = e && *e && *e != '0';
    }
    return g_key_goblin;
}

void addons_set_key_goblin(int on)
{
    g_key_goblin = on != 0;
}

static volatile int g_update_cut = -1;

int addons_update_cutscenes(void)
{
    if (g_update_cut < 0) {
        const char *e = getenv("RECOMP_UPDATE_CUTSCENES");
        g_update_cut = e && *e && *e != '0';
    }
    return g_update_cut;
}

void addons_set_update_cutscenes(int on)
{
    if ((on != 0) != (g_update_cut > 0))
        fprintf(stderr, "[CUTEDIT] update cutscenes %s (from the next scene)\n", on ? "on" : "off");
    g_update_cut = on != 0;
}

/* ── Bat mode's log ───────────────────────────────────────────────────── */

static uint32_t s_bat_level = ~0u;

void addons_note_bat(void)
{
    uint32_t lvl = MEM32(LEVEL);
    if (lvl != s_bat_level) {
        s_bat_level = lvl;
        fprintf(stderr, "[WEAPON] bat mode: B takes the bat (level %02X)\n", lvl);
    }
}

/* ── Frying Pan ───────────────────────────────────────────────────────── */

/* Pan mode (to-do 44; follows conker_weapon.c of Txukie's xboxrecomp-conker,
 * commit d0edebc, MIT, "Copyright (c) 2026 sp00nz": its name-table and
 * sound-entry swap). The bat item (0x75) makes held object 0x25, whose model
 * is named by entry 0x280 of the held-object name table 0x006A6108; that entry
 * pointed at the pan's name, "FryingPanBound" (entry 0x281's string), makes
 * every bat the frying pan: from the training level's package where it is
 * loaded, else the loose file carved from the player's TrainingLevel package
 * into the asset cache (caff_cache.c recipe "pan"; a name with no file to load
 * hangs a level: Txukie, so never without the file). The bat's body
 * animations, camera and class stay (as Txukie's); the pan of the training
 * (item 0x76, its own object 0x26) is the game's own.
 *
 * Sounds (the sound table 0x0065E060, 0x30 bytes an entry: +0 bank, +4 name,
 * +8 cue, +0xC flags, +0x10 the entry's own number; all of these in bank 25):
 * the bat's hit becomes the pan's, in play and in the gargoyle scene, and
 * the gargoyle scenes' bat out / in become the weapon-rack scene's pan unhook
 * / put-away (user 2026-11-16: the pan's sounds in play and in cutscenes).
 * In play the take-out and put-away stay: the training's own pan plays the
 * bat's (baseballbat_getout / _putaway, seen 2026-11-23), pan_putaway is the
 * same wave as the bat's put-away (sfx 1063), and pan_unhook's wave is in
 * the training levels' own bank (sfx_lv_hungover): swapped in, InsideSafe's
 * take-out failed ("audioSoundBankGetCueEx has failed", no sound). Entry
 * fields +0, +8, +0xC copied, as Txukie's.
 *
 * Applied at the start of each level load (0x002AA0C0), and mid-level at once
 * (user 2026-11-23: no level load for a switch; addons_pan_now) while Conker
 * stands with no weapon out outside cutscenes: the held weapon object keeps
 * its model while its id stays the same (the bat's object stays after a
 * put-away), so player_has_bat's wrapper then drops a held bat and the next
 * draw makes it with the new name. Single player only: a multiplayer level
 * gets the original names and sounds. */
#define NAME_BAT       0x006A6B08u   /* held-object name entry 0x280 */
#define STR_PAN        0x005EA534u   /* "FryingPanBound" (entry 0x281's) */
#define STR_BAT        0x005EA544u   /* "BaseballBat" */
#define SND_TABLE      0x0065E060u
#define SND_SIZE       0x30u
#define MP_FLAG        0x007819D5u   /* != 0: multiplayer */
#define LEVEL_ASKED    0x007FA1F4u   /* the level being loaded */
#define PAN_FILE       "bfdmodel\\attach\\FryingPanBound\\default.rbm"   /* below aid\ */

extern int caff_assets_has(const char *rel);   /* caff_redirect.c */

typedef struct {
    uint16_t entry, from;
    const char *name, *from_name;
} PanSound;

static const PanSound s_pan_snd[] = {
    { 0xC98, 0xDC4, "baseballbat_impact",  "fryingpan_impact" },
    { 0xCCA, 0xDC4, "hung_cut_bat_hit",    "fryingpan_impact" },
    { 0xCCC, 0xCD8, "hung_cut_bat_out",    "hung_cut_pan_unhook" },
    { 0xCCB, 0xCD7, "hung_cut_bat_in",     "hung_cut_pan_putaway" },
};
#define PAN_SOUNDS ((int)(sizeof(s_pan_snd) / sizeof(s_pan_snd[0])))
static const uint32_t s_snd_fields[3] = { 0x0u, 0x8u, 0xCu };

static uint32_t s_snd_orig[PAN_SOUNDS][3];
static int s_pan_state = -1;     /* -1 tables not checked yet, -2 not as expected, 0 off, 1 on */

static int str_is(uint32_t va, const char *s)
{
    if (!va_ok(va))
        return 0;
    for (; *s; s++, va++)
        if (MEM8(va) != (uint8_t)*s)
            return 0;
    return MEM8(va) == 0;
}

static uint32_t snd_entry(uint16_t n)
{
    return SND_TABLE + (uint32_t)n * SND_SIZE;
}

/* The tables as this game copy has them, read before anything is changed. */
static int pan_tables_ok(void)
{
    if (!str_is(STR_PAN, "FryingPanBound") || !str_is(STR_BAT, "BaseballBat")
        || MEM32(NAME_BAT) != STR_BAT)
        return 0;
    for (int i = 0; i < PAN_SOUNDS; i++) {
        const PanSound *s = &s_pan_snd[i];
        uint32_t e = snd_entry(s->entry), f = snd_entry(s->from);
        if (MEM32(e + 0x10u) != s->entry || !str_is(MEM32(e + 4u), s->name)
            || MEM32(f + 0x10u) != s->from || !str_is(MEM32(f + 4u), s->from_name))
            return 0;
        for (int k = 0; k < 3; k++)
            s_snd_orig[i][k] = MEM32(e + s_snd_fields[k]);
    }
    return 1;
}

int addons_pan_available(void)
{
    return caff_assets_has(PAN_FILE);
}

int addons_pan_on(void)
{
    return s_pan_state;
}

static void pan_apply(int on)
{
    for (int i = 0; i < PAN_SOUNDS; i++) {
        uint32_t e = snd_entry(s_pan_snd[i].entry), f = snd_entry(s_pan_snd[i].from);
        for (int k = 0; k < 3; k++)
            MEM32(e + s_snd_fields[k]) = on ? MEM32(f + s_snd_fields[k]) : s_snd_orig[i][k];
    }
    MEM32(NAME_BAT) = on ? STR_PAN : STR_BAT;
    s_pan_state = on;
}

/* Mid-level: the Frying Pan on or off now, when the Weapon setting says so and
 * Conker is idle (player_has_bat's wrapper, recomp_manual.c: single player, no
 * cutscene, no weapon out). 1 = changed just now: the caller drops a held bat. */
int addons_pan_now(void)
{
    static int missing_for = -1;     /* the setting a missing file was found for */
    int w = addons_weapon(), want = w == ADDON_WEAPON_PAN;

    if (s_pan_state < 0 || want == s_pan_state)
        return 0;
    if (want && (missing_for == w || !addons_pan_available())) {
        if (missing_for != w)
            fprintf(stderr, "[WEAPON] no frying pan in the asset cache (assets\\aid\\%s): "
                    "the bat stays\n", PAN_FILE);
        missing_for = w;
        return 0;
    }
    missing_for = -1;
    pan_apply(want);
    fprintf(stderr, "[WEAPON] frying pan %s now (level %02X)%s\n", want ? "on" : "off",
            MEM32(LEVEL), want ? ": every bat is the frying pan, with its sounds" : "");
    return 1;
}

static void milk_level_load(uint32_t lvl);

void addons_level_load(void)
{
    static int said_missing;
    uint32_t lvl = MEM32(LEVEL_ASKED);
    int want = addons_weapon() == ADDON_WEAPON_PAN && !MEM8(MP_FLAG);

    milk_level_load(lvl);

    if (s_pan_state == -1)
        s_pan_state = pan_tables_ok() ? 0 : -2;
    if (s_pan_state == -2) {
        if (want && !said_missing++)
            fprintf(stderr, "[WEAPON] the held-object names or sound entries are not as "
                    "expected: no frying pan, the bat stays\n");
        return;
    }
    if (want && !addons_pan_available()) {
        if (!said_missing++)
            fprintf(stderr, "[WEAPON] no frying pan in the asset cache (assets\\aid\\%s): "
                    "the bat stays\n", PAN_FILE);
        want = 0;
    }
    if (want == s_pan_state)
        return;
    pan_apply(want);
    fprintf(stderr, "[WEAPON] frying pan %s from level %02X%s\n", want ? "on" : "off", lvl,
            want ? ": every bat is the frying pan, with its sounds" : "");
}

/* ── Milk Fix ─────────────────────────────────────────────────────────── */

/* The Milk Fix (to-do 45, "Intro Fix" in the plans; the name: user
 * 2026-11-23; after Txukie's conker_intro.c, MIT): the goblet is held object
 * 49, whose model name (table entry 0x006A61CC) is the string
 * "effects_models_conker_milk" at 0x005EDF80, its only reference. With its
 * last letter a '2' the throne room, whose package holds only the goblet,
 * asks for aid\bfdmodel\attach\effects_models_conker_mil2\default.rbm: the
 * glass of milk the asset cache has (caff_cache.c recipe "milk",
 * src/milk_glass.c). Set at each level load (the goblet is made only in
 * throne room scenes), only with that file in the cache. */
#define GOBLET_NAME    0x005EDF80u
#define GOBLET_LETTER  0x005EDF99u   /* the k */
#define MILK_FILE      "bfdmodel\\attach\\effects_models_conker_mil2\\default.rbm"   /* below aid\ */

static volatile int g_milk = -1;
static int s_milk_state = -1;    /* -1 not checked yet, -2 not as expected, 0 off, 1 on */

int addons_milk_fix(void)
{
    if (g_milk < 0) {
        const char *e = getenv("RECOMP_MILK_FIX");
        g_milk = e && *e && *e != '0';
    }
    return g_milk;
}

void addons_set_milk_fix(int on)
{
    if ((on != 0) != (g_milk > 0))
        fprintf(stderr, "[MILK] milk fix %s (from the next level load)\n", on ? "on" : "off");
    g_milk = on != 0;
}

int addons_milk_available(void)
{
    return caff_assets_has(MILK_FILE);
}

static void milk_level_load(uint32_t lvl)
{
    static int said;
    int want = addons_milk_fix();

    if (s_milk_state == -1)
        s_milk_state = str_is(GOBLET_NAME, "effects_models_conker_milk") ? 0 : -2;
    if (s_milk_state == -2) {
        if (want && !said++)
            fprintf(stderr, "[MILK] the goblet's name is not where it should be: the goblet stays\n");
        return;
    }
    if (want && !addons_milk_available()) {
        if (!said++)
            fprintf(stderr, "[MILK] no glass of milk in the asset cache (assets\\aid\\%s): the "
                    "goblet stays\n", MILK_FILE);
        want = 0;
    }
    if (want == s_milk_state)
        return;
    MEM8(GOBLET_LETTER) = want ? '2' : 'k';
    s_milk_state = want;
    fprintf(stderr, "[MILK] %s from level %02X\n",
            want ? "Conker holds a glass of milk (the goblet's name -> effects_models_conker_mil2)"
                 : "the goblet as the original", lvl);
}

/* ── Key Goblin ───────────────────────────────────────────────────────── */

typedef struct {
    uint32_t off;            /* the 4 bytes changed */
    uint8_t  orig[4], ours[4];
    uint32_t ctx;            /* 12 bytes around them (off = ctx + 4) */
    uint8_t  ctxb[12];
    uint32_t tgt;            /* where the jump lands */
    uint8_t  tgtb[4];
} TaskPatch;

/* Checked against the live task dump (r4-0.6.0-addons-plan.md section 3). */
static const TaskPatch s_kg[2] = {
    { 0x44Au, { 0x19, 0x80, 0x07, 0x20 }, { 0x10, 0x60, 0x7A, 0x05 },
      0x446u, { 0x20, 0x80, 0xB9, 0x00, 0x19, 0x80, 0x07, 0x20, 0x1A, 0xC0, 0x03, 0x00 },
      0x57Au, { 0x0F, 0x30, 0x08, 0x00 } },
    { 0x642u, { 0x19, 0x00, 0x74, 0xFF }, { 0x10, 0x60, 0x60, 0x06 },
      0x63Eu, { 0x20, 0x80, 0xE7, 0x00, 0x19, 0x00, 0x74, 0xFF, 0x59, 0x80, 0xFF, 0x7F },
      0x660u, { 0x19, 0x80, 0x40, 0x1F } },
};
#define KG_PATCHES ((int)(sizeof(s_kg) / sizeof(s_kg[0])))

static int bytes_are(uint32_t va, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        if (MEM8(va + (uint32_t)i) != b[i])
            return 0;
    return 1;
}

static uint32_t find_task(uint32_t id)
{
    uint32_t t = MEM32(TASK_LIST);
    for (int n = 0; n < 64 && va_ok(t); n++) {
        if ((MEM32(t) & 0x3FFu) == id)
            return t;
        t = MEM32(t + TASK_NEXT);
    }
    return 0;
}

/* -1: not as checked; 1: ours in every spot; 0: the original in every spot;
 * 2: mixed (one ours, one not). */
static int kg_check(uint32_t t)
{
    int ours = 0, orig = 0;
    if (MEM32(t + TASK_BSSOFF) != KG_BSSOFF)
        return -1;
    for (int i = 0; i < KG_PATCHES; i++) {
        const TaskPatch *p = &s_kg[i];
        if (!bytes_are(t + p->ctx, p->ctxb, 4)
            || !bytes_are(t + p->ctx + 8, p->ctxb + 8, 4)
            || !bytes_are(t + p->tgt, p->tgtb, 4))
            return -1;
        if (bytes_are(t + p->off, p->ours, 4))
            ours++;
        else if (bytes_are(t + p->off, p->orig, 4))
            orig++;
        else
            return -1;
    }
    return ours == KG_PATCHES ? 1 : orig == KG_PATCHES ? 0 : 2;
}

static void key_goblin_frame(void)
{
    static uint32_t warned;     /* the task copy reported as different */
    uint32_t lvl = MEM32(LEVEL), t, state;
    int want, now;

    if (lvl != LVL_FIELD && lvl != LVL_CAVE)
        return;
    t = find_task(KG_TASK_ID);
    if (!t)
        return;
    state = MEM32(t + KG_BSSOFF + KG_STATE);
    if (state >= KG_GOBLINS)
        return;                 /* the goblins are out, or the key is */
    want = addons_key_goblin();
    now = kg_check(t);
    if (now < 0) {
        if (warned != t) {
            warned = t;
            fprintf(stderr, "[KEYGOBLIN] task bytes differ: left alone (task %08X)\n", t);
        }
        return;
    }
    if (now == want)
        return;
    for (int i = 0; i < KG_PATCHES; i++) {
        const uint8_t *b = want ? s_kg[i].ours : s_kg[i].orig;
        for (int k = 0; k < 4; k++)
            MEM8(t + s_kg[i].off + (uint32_t)k) = b[k];
    }
    fprintf(stderr, "[KEYGOBLIN] %s (task %08X, level %02X, state %u)\n",
            want ? "patched" : "put back", t, lvl, state);
}

void addons_frame(void)
{
    key_goblin_frame();
}
