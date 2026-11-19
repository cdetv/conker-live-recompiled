/**
 * Controller chords (chords.h; to-do 39, the user's plan 2026-11-17).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "chords.h"
#include "settings.h"
#include "cheats.h"
#include "ui_text.h"   /* N_(): the menus' text (to-do 229) */

/* ---- the chords one can pick -------------------------------------------- */

/* The user's list, each pair once: Back with any other button, RB with the
 * rest, a D-pad direction with a face button, LB or Start, both sticks. */
#define MAX_CHORDS 48

static const struct {
    unsigned bit;
    const char *name, *label;
} s_buttons[] = {
    { CHORD_A, "a", "A" },
    { CHORD_B, "b", "B" },
    { CHORD_X, "x", "X" },
    { CHORD_Y, "y", "Y" },
    { CHORD_RB, "rb", "RB" },
    { CHORD_LB, "lb", "LB" },
    { CHORD_START, "start", "Start" },
    { CHORD_BACK, "back", "Back" },
    { CHORD_UP, "dpad_up", N_("D-pad Up") },
    { CHORD_DOWN, "dpad_down", N_("D-pad Down") },
    { CHORD_LEFT, "dpad_left", N_("D-pad Left") },
    { CHORD_RIGHT, "dpad_right", N_("D-pad Right") },
    { CHORD_LS, "ls", "LS" },
    { CHORD_RS, "rs", "RS" },
};
#define NUM_BUTTONS ((int)(sizeof(s_buttons) / sizeof(s_buttons[0])))

static struct {
    char name[32], label[40];
    unsigned mask;
} s_chords[MAX_CHORDS];
static int s_count;

static int button_index(unsigned bit)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
        if (s_buttons[i].bit == bit)
            return i;
    return -1;
}

static void add_chord(unsigned first, unsigned second)
{
    int a = button_index(first), b = button_index(second);

    if (s_count >= MAX_CHORDS || a < 0 || b < 0)
        return;
    snprintf(s_chords[s_count].name, sizeof(s_chords[0].name), "%s+%s",
             s_buttons[a].name, s_buttons[b].name);
    snprintf(s_chords[s_count].label, sizeof(s_chords[0].label), "%s + %s",
             s_buttons[a].label, s_buttons[b].label);
    s_chords[s_count].mask = first | second;
    s_count++;
}

/* Built once, before anything reads the table (settings_load's
 * chords_apply runs on the main thread first). */
static void build_table(void)
{
    static const unsigned back_subs[] = {
        CHORD_A, CHORD_B, CHORD_X, CHORD_Y, CHORD_RB, CHORD_LB, CHORD_START,
        CHORD_UP, CHORD_DOWN, CHORD_LEFT, CHORD_RIGHT,
    };
    static const unsigned rb_subs[] = {
        CHORD_A, CHORD_B, CHORD_X, CHORD_Y, CHORD_LB, CHORD_START,
        CHORD_UP, CHORD_DOWN, CHORD_LEFT, CHORD_RIGHT,
    };
    static const unsigned dpad_subs[] = {
        CHORD_A, CHORD_B, CHORD_X, CHORD_Y, CHORD_LB, CHORD_START,
    };
    static const unsigned dpad[] = { CHORD_UP, CHORD_DOWN, CHORD_LEFT,
                                     CHORD_RIGHT };
    static volatile LONG built;
    char check[1024];
    size_t len;

    if (built)
        return;
    s_count = 0;
    for (size_t i = 0; i < sizeof(back_subs) / sizeof(back_subs[0]); i++)
        add_chord(CHORD_BACK, back_subs[i]);
    for (size_t i = 0; i < sizeof(rb_subs) / sizeof(rb_subs[0]); i++)
        add_chord(CHORD_RB, rb_subs[i]);
    for (size_t d = 0; d < 4; d++)
        for (size_t i = 0; i < sizeof(dpad_subs) / sizeof(dpad_subs[0]); i++)
            add_chord(dpad[d], dpad_subs[i]);
    /* Both sticks clicked: "Click both sticks" in the user's list. */
    add_chord(CHORD_LS, CHORD_RS);
    snprintf(s_chords[s_count - 1].label, sizeof(s_chords[0].label),
             N_("Both sticks clicked"));

    /* settings.c's allowed values come from CHORDS_CHOICES: the same list. */
    strcpy_s(check, sizeof(check), "none");
    len = strlen(check);
    for (int c = 0; c < s_count && len < sizeof(check); c++) {
        snprintf(check + len, sizeof(check) - len, " %s", s_chords[c].name);
        len = strlen(check);
    }
    if (strcmp(check, CHORDS_CHOICES))
        fprintf(stderr, "[CHORDS] BUG: CHORDS_CHOICES is not the chord table\n");
    InterlockedExchange(&built, 1);
}

int chords_count(void)
{
    build_table();
    return s_count;
}

const char *chords_name(int c)
{
    build_table();
    return c >= 0 && c < s_count ? s_chords[c].name : NULL;
}

const char *chords_label(int c)
{
    build_table();
    return c >= 0 && c < s_count ? s_chords[c].label : NULL;
}

unsigned chords_buttons(int c)
{
    build_table();
    return c >= 0 && c < s_count ? s_chords[c].mask : 0;
}

int chords_find(const char *name)
{
    build_table();
    if (!name)
        return -1;
    for (int c = 0; c < s_count; c++)
        if (!_stricmp(name, s_chords[c].name))
            return c;
    return -1;
}

int chords_of_buttons(unsigned buttons)
{
    build_table();
    for (int c = 0; c < s_count; c++)
        if (buttons == s_chords[c].mask)
            return c;
    return -1;
}

const char *chords_button_label(unsigned bit)
{
    int i = button_index(bit);
    return i >= 0 ? s_buttons[i].label : "";
}

/* ---- the functions ------------------------------------------------------- */

static const struct {
    const char *key, *label, *tip;
    int group, required;
} s_fns[CHORD_FNS] = {
    { "chord_menu", N_("Recomp Menu"),
      N_("The list of menus: Settings, Controls, Controller Options, Photo Mode, "
         "Report Bug. Every controller needs a way to it, so it can't be unbound"),
      CHORD_GROUP_UNIVERSAL, 1 },
    { "chord_settings", N_("Settings Menu"),
      N_("F2's settings. Also on the Recomp Menu"),
      CHORD_GROUP_UNIVERSAL, 0 },
    { "chord_photo", N_("Photo Mode"),
      N_("Photo mode on and off (in photo mode or freecam: leave). Also on the "
         "Recomp Menu"),
      CHORD_GROUP_UNIVERSAL, 0 },
    { "chord_photo_pause", N_("Pause / Resume"),
      N_("In photo mode: the world frozen or running"),
      CHORD_GROUP_PHOTO, 0 },
    { "chord_freecam_swap", N_("Freecam: camera or Conker"),
      N_("In freecam: the controller flies the camera or plays Conker"),
      CHORD_GROUP_PHOTO, 0 },
    { "chord_noclip", N_("No-clip"),
      N_("Conker flies through walls (debug mode)"),
      CHORD_GROUP_CHEATS, 0 },
    { "chord_bullet", N_("Bullet Time"),
      N_("Bullet time on and off, while its cheat is on"),
      CHORD_GROUP_CHEATS, 0 },
    { "chord_freecam", N_("Free Camera"),
      N_("Freecam on from gameplay (debug mode): photo mode's camera with the "
         "world running. The Photo Mode chord leaves it"),
      CHORD_GROUP_CHEATS, 0 },
};

static const char *const s_group_names[CHORD_GROUPS] = {
    N_("Universal"), N_("Photo Mode"), N_("Cheats / Debug"),
};
static const char *const s_group_tips[CHORD_GROUPS] = {
    N_("Anywhere. A chord used here can't be used for anything else"),
    N_("In photo mode only. May share a chord with a Cheats / Debug function, "
       "not with a Universal one"),
    N_("In gameplay only. May share a chord with a Photo Mode function, not "
       "with a Universal one"),
};

const char *chords_fn_key(int fn)
{
    return fn >= 0 && fn < CHORD_FNS ? s_fns[fn].key : NULL;
}

const char *chords_fn_label(int fn)
{
    return fn >= 0 && fn < CHORD_FNS ? s_fns[fn].label : "";
}

const char *chords_fn_tip(int fn)
{
    return fn >= 0 && fn < CHORD_FNS ? s_fns[fn].tip : "";
}

int chords_fn_group(int fn)
{
    return fn >= 0 && fn < CHORD_FNS ? s_fns[fn].group : -1;
}

int chords_fn_required(int fn)
{
    return fn >= 0 && fn < CHORD_FNS && s_fns[fn].required;
}

int chords_fn_shown(int fn)
{
    switch (fn) {
    case CHORD_FREECAM_SWAP:
    case CHORD_NOCLIP:
    case CHORD_FREECAM:
        return settings_debug();
    case CHORD_BULLET:
        return cheat_unlocked(CHEAT_BULLET);
    default:
        return fn >= 0 && fn < CHORD_FNS;
    }
}

int chords_fn_available(int fn)
{
    if (fn == CHORD_BULLET)
        return (cheats_active() & CHEAT_BIT(CHEAT_BULLET)) != 0;
    return chords_fn_shown(fn);
}

const char *chords_group_name(int g)
{
    return g >= 0 && g < CHORD_GROUPS ? s_group_names[g] : "";
}

const char *chords_group_tip(int g)
{
    return g >= 0 && g < CHORD_GROUPS ? s_group_tips[g] : "";
}

/* ---- what each has ------------------------------------------------------- */

static volatile LONG s_fn_chord[CHORD_FNS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

int chords_fn_chord(int fn)
{
    return fn >= 0 && fn < CHORD_FNS ? (int)s_fn_chord[fn] : -1;
}

unsigned chords_mask(int fn)
{
    return chords_buttons(chords_fn_chord(fn));
}

const char *chords_fn_chord_label(int fn)
{
    int c = chords_fn_chord(fn);
    return c >= 0 ? chords_label(c) : "none";
}

/* Two functions may not share a chord: either is Universal, or both are in
 * one group. */
static int may_share(int fa, int fb)
{
    int ga = s_fns[fa].group, gb = s_fns[fb].group;
    return ga != CHORD_GROUP_UNIVERSAL && gb != CHORD_GROUP_UNIVERSAL
           && ga != gb;
}

int chords_taken_by(int fn, int c)
{
    if (c < 0)
        return -1;
    for (int o = 0; o < CHORD_FNS; o++)
        if (o != fn && chords_fn_shown(o) && s_fn_chord[o] == c
            && !may_share(fn, o))
            return o;
    return -1;
}

void chords_apply(void)
{
    int got[CHORD_FNS];

    build_table();
    for (int fn = 0; fn < CHORD_FNS; fn++)
        got[fn] = -2;                   /* not decided yet */
    /* The rows that show first (a hidden cheat's chord, kept in the file,
     * gives way to one that shows), each in the window's order. */
    for (int pass = 0; pass < 2; pass++)
        for (int fn = 0; fn < CHORD_FNS; fn++) {
            if ((chords_fn_shown(fn) != 0) != (pass == 0))
                continue;
            const char *v = settings_get(s_fns[fn].key);
            int c = chords_find(v);
            if (c < 0 && (!v || _stricmp(v, "none"))) {
                fprintf(stderr, "[CHORDS] %s = %s is not a chord: %s\n",
                        s_fns[fn].key, v ? v : N_("(none)"),
                        s_fns[fn].required ? "the default" : "unbound");
                c = s_fns[fn].required ? chords_find(settings_default(s_fns[fn].key))
                                       : -1;
            } else if (c < 0 && s_fns[fn].required) {
                fprintf(stderr, "[CHORDS] %s can't be unbound: the default\n",
                        s_fns[fn].key);
                c = chords_find(settings_default(s_fns[fn].key));
            }
            for (int o = 0; o < CHORD_FNS && c >= 0; o++)
                if (o != fn && got[o] == c && !may_share(fn, o)) {
                    fprintf(stderr, "[CHORDS] %s = %s is %s's already: unbound\n",
                            s_fns[fn].key, s_chords[c].name, s_fns[o].key);
                    c = -1;
                }
            got[fn] = c;
        }
    for (int fn = 0; fn < CHORD_FNS; fn++)
        InterlockedExchange(&s_fn_chord[fn], got[fn]);
    {
        char line[512];
        size_t len = 0;
        line[0] = '\0';
        for (int fn = 0; fn < CHORD_FNS; fn++) {
            snprintf(line + len, sizeof(line) - len, "%s%s %s", fn ? ", " : "",
                     s_fns[fn].key + 6, got[fn] >= 0 ? s_chords[got[fn]].name
                                                     : "none");
            len = strlen(line);
        }
        fprintf(stderr, "[CHORDS] %s\n", line);
    }
}

/* ---- the check ----------------------------------------------------------- */

static int in_ctx(int fn, int ctx)
{
    switch (s_fns[fn].group) {
    case CHORD_GROUP_UNIVERSAL:
        return 1;
    case CHORD_GROUP_PHOTO:
        return ctx == CHORD_CTX_PHOTO && chords_fn_available(fn);
    default:
        return ctx == CHORD_CTX_GAME && chords_fn_available(fn);
    }
}

unsigned chords_step(ChordState *s, unsigned buttons, int ctx)
{
    unsigned fired = 0;

    s->used &= buttons;                 /* let go: free again */
    for (int fn = 0; fn < CHORD_FNS; fn++) {
        unsigned m = chords_mask(fn);
        if (!m || (buttons & m) != m || (s->used & m) || !in_ctx(fn, ctx))
            continue;
        /* RB held on the Options screen or the pause menu is the cheat
         * code's (cheats.c): its B and A and D-pad are the code's. */
        if ((m & CHORD_RB) && cheats_pad_holding())
            continue;
        fired |= CHORD_FN_BIT(fn);
        s->used |= m;
    }
    return fired;
}
