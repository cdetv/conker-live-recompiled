/**
 * Keyboard and mouse controls (controls.c): what each key does in single
 * player and in multiplayer, for the F3 menu (overlay.cpp) and settings.ini
 * (settings.c), handed to the toolkit's keyboard-and-mouse pad.
 */
#ifndef CONKER_CONTROLS_H
#define CONKER_CONTROLS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The sets, also the toolkit's profile slots: the two the player sets up,
 * then the fixed keys for the front ends' menus, the single player pause
 * screen, menus inside a level (the multiplayer pause menu) and cutscenes,
 * then single player's keys swimming at the surface and underwater (the
 * single player set with the swimming keys in charge), then single player's
 * keys on the beehive gun (forward and back swapped), then multiplayer's
 * keys in the Mule 52's gunner seat (forward and back swapped too), then
 * single player's keys in The Vault's suit (Crouch and Mouse 2 block), then
 * the same while holding the alien's tail (Move left / right spin the left
 * stick round; the mouse is the right stick), then single player's keys as
 * the bat in Count Batula, on the ground and flying (the bat keys in
 * charge, like swimming). */
#define CONTROLS_STORY        0
#define CONTROLS_MP           1
#define CONTROLS_MENU         2
#define CONTROLS_PAUSE        3
#define CONTROLS_MENU_GAME    4
#define CONTROLS_CUTSCENE     5
#define CONTROLS_SWIM_SURFACE 6
#define CONTROLS_SWIM_UNDER   7
#define CONTROLS_BEE_GUN      8
#define CONTROLS_MULE_GUN     9
#define CONTROLS_VAULT_SUIT   10
#define CONTROLS_VAULT_TAIL   11
#define CONTROLS_BAT_GROUND   12
#define CONTROLS_BAT_AIR      13
/* Photo mode (photo_mode.c): an empty profile, so no key reaches the game
 * while the photo camera flies; its keys are read with xbox_KbmHeld. Also
 * the F3 "Photo mode" table, rows in the PHOTO_ order below. */
#define CONTROLS_PHOTO        14

/* The photo table's rows. The first two work everywhere (the window's
 * message hook acts on them); the rest only in photo mode. */
enum {
    PHOTO_TOGGLE, PHOTO_PAUSE,
    PHOTO_FORWARD, PHOTO_BACK, PHOTO_LEFT, PHOTO_RIGHT, PHOTO_UP, PHOTO_DOWN,
    PHOTO_ROLL_LEFT, PHOTO_ROLL_RIGHT, PHOTO_SLOW, PHOTO_FAST,
    PHOTO_ZOOM_IN, PHOTO_ZOOM_OUT, PHOTO_TAKE, PHOTO_LEAVE, PHOTO_RESET,
    PHOTO_HUD, PHOTO_CONKER, PHOTO_STEP, PHOTO_PANEL, PHOTO_CUT_BLUR,
    PHOTO_ROWS
};

/* One action: its settings.ini key ("story_jump"), what the menu calls it,
 * and the Xbox button it presses, for the menu ("A", "Right trigger").
 * section: the F3 menu's heading for a group of actions (NULL: the set's
 * main list); a key may be in two sections without a clash. */
typedef struct {
    const char *key;
    const char *label;
    const char *button;
    const char *section;
} ControlAction;

int controls_count(int set);
const ControlAction *controls_action(int set, int i);
/* Action i's key now (an input as below, 0 = none). */
int controls_key(int set, int i);
/* Row i of a set's whole table, single player's rows of both schemes (for
 * --dump-settings and resetting a set); NULL past the end. *scheme: 0 for
 * both schemes, else the one it belongs to (CONTROLS_CLASSIC, _MODERN). */
#define CONTROLS_CLASSIC 1
#define CONTROLS_MODERN  2
const ControlAction *controls_table_row(int set, int i, int *scheme);

/* Key names. An input is a Windows virtual-key code (mouse buttons too) or
 * the wheel (256 up, 257 down), 0 = none. parse: a settings.ini name to an
 * input, -1 for a name that is not one. name: the settings.ini name.
 * label: what the menu shows. Both "" for an input that has none. */
int controls_parse_key(const char *name);
const char *controls_key_name(int input);
const char *controls_key_label(int input);

/* settings.ini to the toolkit, both sets; at start and after each change in
 * the F3 menu. */
void controls_apply(void);

/* Controller ports 0-3 (the game's players 1-4), from settings.ini: which
 * controller drives one (a host pad: 0-3 = Windows' (XInput) controller
 * 1-4, 4-7 = PlayStation controller 1-4; -2 = any, -1 = none), the port
 * the keyboard and mouse drive, whether a host pad is connected, and which
 * one drives a port now (-1 none). Changed by saving controller_slot<n> /
 * keyboard_slot and calling controls_apply. */
#define CONTROLS_XINPUT_PADS 4
#define CONTROLS_HOST_PADS   8
int controls_port_source(int port);
int controls_keyboard_port(void);
int controls_pad_connected(int pad);
int controls_pad_of_port(int port);
/* A connected controller's name as Windows lists it (USB game
 * controllers; SDL's name for a PlayStation pad), UTF-8; 0 and "" when it
 * has none. */
int controls_pad_name(int pad, char *out, int size);

/* The photo mode key (PHOTO_TOGGLE) and the pause key (PHOTO_PAUSE), for
 * the window's message hook: copies made by controls_apply, so any thread
 * may read them. */
int controls_photo_toggle_key(void);
int controls_pause_key(void);

/* Which set the game is in (recomp_manual.c, from the level), and which it
 * last said. */
void controls_set_mode(int set);
/* A player set's mouse sensitivity (1 = 100 %) and invert, for mouse look
 * done by the game's camera (recomp_manual.c). */
void controls_mouse(int set, float *sx, float *sy, int *invert);
int controls_mode(void);
/* Single player's Modern scheme (story_scheme): 1 when chosen, with its
 * context, aim and attack keys (inputs as above); the game thread drives
 * those from the game's state (recomp_manual.c). */
int controls_modern(int *context, int *aim, int *attack);
/* Multiplayer's zoom held (mp_zoom_mode = hold): 1 when chosen, with the
 * zoom key; the game thread zooms by the game's state (recomp_manual.c). */
int controls_zoom_hold(int *zoom);
/* The bat's aim key (story_bat_aim) in *aim; returns 1 for Modern (aim
 * while held), 0 for Classic (each press toggles). The game thread taps the
 * left trigger by the game's aim state (recomp_manual.c). */
int controls_bat_aim(int *aim);

#ifdef __cplusplus
}
#endif

#endif
