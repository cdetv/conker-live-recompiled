/**
 * Controller binds (to-do 39's remapping; the user's design 2026-11-17):
 * single player's actions on slot 1's controller, one set per scheme
 * (Classic, Modern), as the keyboard's Controls tab has them. Each action
 * presses the game's own button for it, so only which button the player
 * presses changes. Multiplayer gets its own set later.
 *
 * Buttons are XInput's bits (chords.h CHORD_*) plus the two triggers
 * (PADB_LT, PADB_RT), for the physical controller and the game's pad alike.
 * settings.ini keeps the binds ([controller_story] pad_classic_* /
 * pad_modern_*); F3's Controls tab and the launcher's set them.
 */
#ifndef CONKER_PAD_BINDS_H
#define CONKER_PAD_BINDS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PADB_LT 0x0400u
#define PADB_RT 0x0800u

/* The actions, in the Controls tab's order. A section's actions (swimming,
 * the bat) work only there and take over their buttons there. */
enum {
    PADB_JUMP, PADB_CROUCH, PADB_CONTEXT, PADB_AIM, PADB_ATTACK, PADB_RELOAD,
    PADB_HEADCAM_HOLD, PADB_HEADCAM_TOGGLE, PADB_ZOOM, PADB_PAUSE,
    PADB_DIVE, PADB_SWIM_UP, PADB_SWIM_DOWN,
    PADB_BAT_LAUNCH, PADB_BAT_UP, PADB_BAT_DOWN, PADB_BAT_POOP,
    PADB_BAT_AIM_TOGGLE, PADB_BAT_AIM_HOLD,
    PADB_ACTIONS
};

/* The buttons one can pick (11: A, B, X, Y, LB, RB, LT, RT, Start, the stick
 * clicks; Back and the D-pad stay themselves), in the menus' order. */
int pad_binds_choices(void);
unsigned pad_binds_choice(int i);
/* settings.ini's name ("a", "rt", "none"); the menus' label ("A", "Right
 * trigger", "-"). */
const char *pad_binds_button_name(unsigned bit);
const char *pad_binds_button_label(unsigned bit);
/* A settings.ini name to its bit: 0 for "none", ~0u for one that is not. */
unsigned pad_binds_parse(const char *name);

/* An action's settings.ini key in a scheme ("pad_classic_jump"); NULL when
 * the scheme has no such action (Aim in Classic). */
const char *pad_binds_key(int action, int modern);
/* What the menus call it ("Jump"), in that scheme. */
const char *pad_binds_label(int action, int modern);
/* Its name in the keys ("jump") and where it works: 0 the main list, else
 * a section's place + 1 (PADB_SURFACE + 1 ...). */
const char *pad_binds_name(int action);
int pad_binds_place(int action);
/* Its button now (settings.ini, as last applied) and its default. */
unsigned pad_binds_get(int action, int modern);
unsigned pad_binds_default(int action, int modern);
/* Pause can't be left without a button. */
int pad_binds_required(int action);
/* The actions that would clash with action on button in that scheme (they
 * act at the same moment): out gets up to max of them, the count returned.
 * Crouch and Attack never clash (a weapon out says which), nor a swimming /
 * bat action with the main list or with one of another place. */
int pad_binds_clashes(int action, int modern, unsigned button, int *out,
                      int max);

/* settings.ini to the live tables (both schemes); at start and after each
 * change (controls_apply calls it). */
void pad_binds_apply(void);

/* One reading of slot 1's controller in single player gameplay (the pad
 * filter, the controller's thread). in: the physical buttons, the triggers
 * 0-255. where: */
enum {
    PADB_ON_FOOT,      /* the main list (land, vehicles, the beehive gun, The Vault) */
    PADB_SURFACE,      /* swimming at the surface */
    PADB_UNDER,        /* underwater */
    PADB_BAT_GROUND,
    PADB_BAT_AIR,
};
typedef struct {
    unsigned buttons;          /* physical, CHORD_* | PADB_LT / PADB_RT */
    unsigned char lt, rt;      /* physical triggers */
    int place;                 /* PADB_ON_FOOT ... */
    int modern;
    int modern_foot;           /* Modern's own on-foot logic runs (modern_poll) */
    int foot;                  /* on foot (no vehicle; story set) */
    int weapon_out;            /* Classic: Attack's button, not Crouch's, is RT */
    int suit;                  /* The Vault's suit: Modern's Aim blocks (LT) */
} PadBindsIn;
typedef struct {
    unsigned buttons;          /* the game's: CHORD_* bits (LT / RT below) */
    unsigned char lt, rt;      /* the game's triggers 0-255 */
    /* Modern on foot: what modern_poll reads (B and RT are its answer). */
    int ctx, aim, attack, crouch;
    /* The bat's aim (bat_poll taps the left trigger). */
    int bat_toggle, bat_hold;
} PadBindsOut;
void pad_binds_step(const PadBindsIn *in, PadBindsOut *out);
/* Out of gameplay: the head-cam toggle lets go, unless keep (the pause
 * screen, photo mode). */
void pad_binds_idle(int keep);

#ifdef __cplusplus
}
#endif

#endif
