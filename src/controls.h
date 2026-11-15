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

/* The photo table's rows. The first four work everywhere (the window's
 * message hook acts on them; PHOTO_REPORT opens Report Bug, to-do 12;
 * PHOTO_MENU the Debug menu with --debug, to-do 14, and later the cheat menu,
 * SPEC 10.3: shown in F3 only with --debug for now); the rest only in photo
 * mode. Every row up to PHOTO_ANYWHERE_LAST works everywhere (PHOTO_NOCLIP
 * and PHOTO_MARK with --debug; PHOTO_BULLET while its cheat is on). Hide HUD had a key here
 * too (hud_key, to-do 152) until to-do 188 left it to F2's checkbox. */
enum {
    PHOTO_TOGGLE, PHOTO_PAUSE, PHOTO_REPORT, PHOTO_MENU, PHOTO_NOCLIP,
    PHOTO_BULLET, PHOTO_MARK,
    PHOTO_FORWARD, PHOTO_BACK, PHOTO_LEFT, PHOTO_RIGHT, PHOTO_UP, PHOTO_DOWN,
    PHOTO_ROLL_LEFT, PHOTO_ROLL_RIGHT, PHOTO_SLOW, PHOTO_FAST,
    PHOTO_ZOOM_IN, PHOTO_ZOOM_OUT, PHOTO_TAKE, PHOTO_LEAVE, PHOTO_RESET,
    PHOTO_HUD, PHOTO_CONKER, PHOTO_STEP, PHOTO_RESUME, PHOTO_PANEL,
    PHOTO_CUT_BLUR, PHOTO_FREECAM_SWAP, PHOTO_ROWS
};
#define PHOTO_ANYWHERE_LAST PHOTO_MARK

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
/* Action i's first key now (an input as below, 0 = none): the fixed sets'
 * one key; a player set's row may have more (controls_keys). */
int controls_key(int set, int i);

/* An action's keys (to-do 38, SPEC 10.1): up to CONTROLS_MAX_KEYS, packed
 * into one value any thread may copy: 20 bits a key, first to last with no
 * gaps; 0 = none. One key (a "bind"): its input in the low 9 bits, a
 * combo's hold key (down first: Shift in Shift + E) in the next 9, and
 * CONTROLS_KEEP for a held combo (once on, the input alone keeps it on). */
#define CONTROLS_MAX_KEYS 3
#define CONTROLS_INPUT(b)         ((b) & 0x1FF)
#define CONTROLS_HOLD(b)          (((b) >> 9) & 0x1FF)
#define CONTROLS_KEEP             (1 << 18)
#define CONTROLS_COMBO(hold, in)  (((hold) << 9) | (in))
typedef long long ControlKeys;
ControlKeys controls_keys(int set, int i);
int controls_keys_count(ControlKeys k);
/* Key j whole (0 past the last). */
int controls_keys_get(ControlKeys k, int j);
/* Is this key one of them (a combo the same combo; held or not)? */
int controls_keys_has(ControlKeys k, int bind);
/* Is any of them held now, by the combo rules (a combo: its hold key down
 * first; a plain key: not taken by a combo of set's table that is on)? */
int controls_keys_held(int set, ControlKeys k);
/* Does a key-down of input press one of them (the window's hotkeys): a
 * plain key not taken by a combo of set's table, or a combo whose hold
 * key is down? */
int controls_keys_pressed(int set, ControlKeys k, int input);
/* Is one of them Alt + input? With Alt down Windows sends WM_SYSKEYDOWN,
 * which the hotkeys otherwise leave alone (Alt+F4 closes the window). */
int controls_keys_alt_pressed(ControlKeys k, int input);
/* bind added last (unchanged when already there or full) / taken out. */
ControlKeys controls_keys_add(ControlKeys k, int bind);
ControlKeys controls_keys_remove(ControlKeys k, int bind);
/* Can this be a key: not two of the same, not the wheel as a hold key, not
 * Alt+Tab or Alt+F4 (Windows'), keep only on a combo. */
int controls_bind_ok(int bind);
/* A settings.ini value ("space, mouse4, shift+e"; one name; "none" or "" =
 * none) to keys: 0 when every one is a key (repeats dropped, past the
 * third ignored), -1 when one is not. A held combo: "shift+c keep". */
int controls_parse_keys(const char *value, ControlKeys *out);
/* Keys as settings.ini writes them ("space, shift+e", "none") and as the
 * menus show them ("Space / Shift + E", "-"); one key as the menus show it.
 * A few static buffers in turn. */
const char *controls_keys_name(ControlKeys k);
const char *controls_keys_label(ControlKeys k);
const char *controls_bind_label(int bind);
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

/* Each port's rumble and stick dead zone (to-do 29): port -1 = all from
 * settings.ini (controls_apply does that); else one port, a live slider
 * value (>= 0) in place of the saved one. */
void controls_apply_pad_feel(int port, float strength, float dead_zone);

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

/* The keys of the photo table's rows that work everywhere (PHOTO_TOGGLE to
 * PHOTO_ANYWHERE_LAST: photo mode, pause, Report Bug, the Debug / cheat menu,
 * noclip, bullet time, mark), for the window's message hook: copies made by
 * controls_apply, so any thread may read them; 0 for another row. */
ControlKeys controls_hotkeys(int row);
/* Is this Windows key one the set in use presses Start with (the game's
 * pause: Esc, story_pause / mp_pause)? For the window's message hook, which
 * keeps it from the game while a menu of ours is up (to-do 181). */
int controls_start_key(int vk);

/* Which set the game is in (recomp_manual.c, from the level), and which it
 * last said. */
void controls_set_mode(int set);
/* A player set's mouse sensitivity (1 = 100 %) and invert, for mouse look
 * done by the game's camera (recomp_manual.c). */
void controls_mouse(int set, float *sx, float *sy, int *invert);
int controls_mode(void);
/* Single player's Modern scheme (story_scheme): 1 when chosen, with its
 * context, aim and attack keys; the game thread drives those from the
 * game's state (recomp_manual.c). Any pointer may be NULL. */
int controls_modern(ControlKeys *context, ControlKeys *aim, ControlKeys *attack);
/* Modern's slingshot (story_slingshot_aim = toggle): 1 when the context key
 * turns a slingshot pad's slingshot on and off instead of the aim key. */
int controls_sling_toggle(void);
/* Multiplayer's zoom held (mp_zoom_mode = hold): 1 when chosen, with the
 * zoom keys; the game thread zooms by the game's state (recomp_manual.c). */
int controls_zoom_hold(ControlKeys *zoom);
/* The bat's aim keys (story_bat_aim) in *aim; returns 1 for Modern (aim
 * while held), 0 for Classic (each press toggles). The game thread taps the
 * left trigger by the game's aim state (recomp_manual.c). */
int controls_bat_aim(ControlKeys *aim);
/* Unlocked Camera's zoom keys (story_cam_zoom_in / _out); returns 1 while
 * single player's on-foot keys are on. For the window thread (overlay.cpp). */
int controls_cam_zoom(ControlKeys *in, ControlKeys *out);

#ifdef __cplusplus
}
#endif

#endif
