/**
 * Controller chords (to-do 39, SPEC 10.1; the user's plan 2026-11-17): two
 * buttons of slot 1's controller held together drive our own extras (the
 * menus, photo mode, cheats). One set for every controller, apart from the
 * controls' button remapping. F3 > Slots > Chords sets them (overlay.cpp);
 * settings.ini keeps them ([chords]).
 *
 * Buttons are XInput's bits (CHORD_A ...), whichever kind of controller.
 * A chord fires once both its buttons are down, whichever went down first,
 * and both are then "used" until each is let go: no other chord takes them,
 * and the reader keeps them from what it drives (the game, photo mode).
 */
#ifndef CONKER_CHORDS_H
#define CONKER_CHORDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* XInput's button bits (xinput.h), so a reader without it can build a
 * chord's buttons (the game's own pad state, recomp_manual.c). */
#define CHORD_UP     0x0001u
#define CHORD_DOWN   0x0002u
#define CHORD_LEFT   0x0004u
#define CHORD_RIGHT  0x0008u
#define CHORD_START  0x0010u
#define CHORD_BACK   0x0020u
#define CHORD_LS     0x0040u
#define CHORD_RS     0x0080u
#define CHORD_LB     0x0100u
#define CHORD_RB     0x0200u
#define CHORD_A      0x1000u
#define CHORD_B      0x2000u
#define CHORD_X      0x4000u
#define CHORD_Y      0x8000u

/* What a chord can drive, in the Chords window's order. Universal: anywhere,
 * a chord no other function may have. Photo Mode: in photo mode only.
 * Cheats / Debug: in gameplay only. Photo Mode and Cheats may share one. */
enum {
    CHORD_MENU,          /* the Recomp Menu (Back + Start's list); required */
    CHORD_SETTINGS,      /* F2 */
    CHORD_PHOTO,         /* photo mode on / off (also leaves freecam) */
    CHORD_PHOTO_PAUSE,   /* photo mode: the world paused / running */
    CHORD_FREECAM_SWAP,  /* freecam: the pad to the camera or Conker (--debug) */
    CHORD_NOCLIP,        /* --debug */
    CHORD_BULLET,        /* the Bullet time cheat, once unlocked */
    CHORD_FREECAM,       /* freecam on from gameplay (--debug) */
    CHORD_FNS
};
enum { CHORD_GROUP_UNIVERSAL, CHORD_GROUP_PHOTO, CHORD_GROUP_CHEATS,
       CHORD_GROUPS };
#define CHORD_FN_BIT(fn) (1u << (fn))

/* settings.ini's allowed values (OPT_CHOICE): "none" and every chord's name,
 * in the table's order (chords.c checks the two agree at start). */
#define CHORDS_CHOICES \
    "none back+a back+b back+x back+y back+rb back+lb back+start " \
    "back+dpad_up back+dpad_down back+dpad_left back+dpad_right " \
    "rb+a rb+b rb+x rb+y rb+lb rb+start " \
    "rb+dpad_up rb+dpad_down rb+dpad_left rb+dpad_right " \
    "dpad_up+a dpad_up+b dpad_up+x dpad_up+y dpad_up+lb dpad_up+start " \
    "dpad_down+a dpad_down+b dpad_down+x dpad_down+y dpad_down+lb dpad_down+start " \
    "dpad_left+a dpad_left+b dpad_left+x dpad_left+y dpad_left+lb dpad_left+start " \
    "dpad_right+a dpad_right+b dpad_right+x dpad_right+y dpad_right+lb dpad_right+start " \
    "ls+rs"

/* The chords one can pick (46): settings.ini name ("back+start"), menu label
 * ("Back + Start"), buttons. -1 / NULL / 0 past the end. find: a name to its
 * index, -1 for "none" or a name that is not one. */
int chords_count(void);
const char *chords_name(int c);
const char *chords_label(int c);
unsigned chords_buttons(int c);
int chords_find(const char *name);
/* The chord these buttons make (exactly two, or both sticks), -1 if none. */
int chords_of_buttons(unsigned buttons);
/* One button's name as the menus show it ("D-pad Up", "Back"). */
const char *chords_button_label(unsigned bit);

/* A function: its settings.ini key ("chord_menu"), label, tooltip, group;
 * required = it can't be unbound; shown = its row shows now (--debug, the
 * cheat unlocked); available = it works now (--debug, the cheat on). */
const char *chords_fn_key(int fn);
const char *chords_fn_label(int fn);
const char *chords_fn_tip(int fn);
int chords_fn_group(int fn);
int chords_fn_required(int fn);
int chords_fn_shown(int fn);
int chords_fn_available(int fn);
const char *chords_group_name(int g);
const char *chords_group_tip(int g);

/* The chord a function has now (-1 none), its buttons (0 none) and its
 * label ("Back + RB", or "none"). Copies made by chords_apply: any thread. */
int chords_fn_chord(int fn);
unsigned chords_mask(int fn);
const char *chords_fn_chord_label(int fn);
/* Can fn take chord c: -1 yes, else the shown function that has it and may
 * not share it. */
int chords_taken_by(int fn, int c);
/* settings.ini to the copies, at start and after each change: a value that
 * is not a chord, or one another function has and may not share, leaves the
 * function unbound (the required one: its default), with a [CHORDS] line. */
void chords_apply(void);

/* One reader of slot 1's controller (the menus, photo mode, the game's pad):
 * each keeps its own state. ctx: which functions count (the Universal ones
 * always, and the Photo Mode or the Cheats ones, those available). Returns
 * the functions whose chords fired at this call (CHORD_FN_BIT), and leaves
 * the buttons in use in s->used until let go. A chord with RB fires not
 * while the cheat code's RB hold is on (cheats.c). */
#define CHORD_CTX_GAME  0
#define CHORD_CTX_PHOTO 1
typedef struct {
    unsigned used;
} ChordState;
unsigned chords_step(ChordState *s, unsigned buttons, int ctx);

#ifdef __cplusplus
}
#endif

#endif
