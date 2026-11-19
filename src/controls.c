/**
 * Keyboard and mouse controls (SPEC 10.1): the actions of single player and
 * multiplayer, each bound to up to three keys in settings.ini (to-do 38), pressing the button
 * the original controller has for it. The toolkit turns held keys into a
 * pad (src/input/kbm_pad.c in the fork); this file only says which key is
 * which button, for both sets, and which set is in use.
 *
 * The defaults are settings.c's (story_* and mp_* options); the order here
 * is the F3 menu's. Mouse movement is always the right stick (looking), at
 * each set's own sensitivity.
 *
 * Stick ranges are Conker's own (pad_poll_all 0x003D1B9A and the axis
 * callbacks it installs, 0x003D1E10; conker-notes 2026-10-25): each axis is
 * scaled to 0-110 (left) or 0-99 (right), loses its first 15 (dead zone)
 * and is full at 72 + 15 or 59 + 15. Keys and the mouse ask for tilt in
 * that range, so the first bit of mouse movement already turns.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_kbm.h"

#include "controls.h"
#include "settings.h"
#include "chords.h"
#include "pad_binds.h"
#include "ui_text.h"   /* N_(): the menus' text (to-do 229) */

/* ---- key names ------------------------------------------------------------ */

typedef struct {
    int input;
    const char *name;     /* settings.ini */
    const char *label;    /* the menu */
} KeyName;

static const KeyName s_keys[] = {
    { VK_LBUTTON, "mouse1", "Mouse 1 (left)" },
    { VK_RBUTTON, "mouse2", "Mouse 2 (right)" },
    { VK_MBUTTON, "mouse3", "Mouse 3 (middle)" },
    { VK_XBUTTON1, "mouse4", "Mouse 4" },
    { VK_XBUTTON2, "mouse5", "Mouse 5" },
    { XBOX_KBM_WHEEL_UP, "wheelup", "Wheel up" },
    { XBOX_KBM_WHEEL_DOWN, "wheeldown", "Wheel down" },
    { VK_SPACE, "space", "Space" },
    { VK_RETURN, "enter", "Enter" },
    { VK_BACK, "backspace", "Backspace" },
    { VK_TAB, "tab", "Tab" },
    { VK_ESCAPE, "esc", "Esc" },
    { VK_SHIFT, "shift", "Shift" },
    { VK_CONTROL, "ctrl", "Ctrl" },
    { VK_MENU, "alt", "Alt" },
    { VK_CAPITAL, "capslock", "Caps Lock" },
    { VK_UP, "up", "Up arrow" },
    { VK_DOWN, "down", "Down arrow" },
    { VK_LEFT, "left", "Left arrow" },
    { VK_RIGHT, "right", "Right arrow" },
    { VK_INSERT, "insert", "Insert" },
    { VK_DELETE, "delete", "Delete" },
    { VK_HOME, "home", "Home" },
    { VK_END, "end", "End" },
    { VK_PRIOR, "pageup", "Page Up" },
    { VK_NEXT, "pagedown", "Page Down" },
    { VK_OEM_1, "semicolon", ";" },
    { VK_OEM_PLUS, "equals", "=" },
    { VK_OEM_COMMA, "comma", "," },
    { VK_OEM_MINUS, "minus", "-" },
    { VK_OEM_PERIOD, "period", "." },
    { VK_OEM_2, "slash", "/" },
    { VK_OEM_3, "backquote", "`" },
    { VK_OEM_4, "lbracket", "[" },
    { VK_OEM_5, "backslash", "\\" },
    { VK_OEM_6, "rbracket", "]" },
    { VK_OEM_7, "quote", "'" },
    { VK_MULTIPLY, "nummul", "Num *" },
    { VK_ADD, "numadd", "Num +" },
    { VK_SUBTRACT, "numsub", "Num -" },
    { VK_DECIMAL, "numdot", "Num ." },
    { VK_DIVIDE, "numdiv", "Num /" },
    /* F1-F3, F9 and F11 are the port's own keys; F4 is photo mode's by
     * default, but any key may be. */
    { VK_F4, "f4", "F4" },
    { VK_F5, "f5", "F5" }, { VK_F6, "f6", "F6" }, { VK_F7, "f7", "F7" },
    { VK_F8, "f8", "F8" }, { VK_F10, "f10", "F10" }, { VK_F12, "f12", "F12" },
};
#define NUM_KEYS ((int)(sizeof(s_keys) / sizeof(s_keys[0])))

/* Letters, digits and the number pad's digits by rule, the rest by table. */
static char s_made[3][16];

int controls_parse_key(const char *name)
{
    int i;
    if (!name || !*name || !_stricmp(name, "none"))
        return 0;
    if (!name[1] && ((name[0] >= 'a' && name[0] <= 'z')
                     || (name[0] >= 'A' && name[0] <= 'Z')))
        return name[0] & ~0x20;
    if (!name[1] && name[0] >= '0' && name[0] <= '9')
        return name[0];
    if (!_strnicmp(name, "num", 3) && name[3] >= '0' && name[3] <= '9'
        && !name[4])
        return VK_NUMPAD0 + (name[3] - '0');
    for (i = 0; i < NUM_KEYS; i++)
        if (!_stricmp(s_keys[i].name, name))
            return s_keys[i].input;
    return -1;
}

static const char *key_text(int input, int label)
{
    static int slot;
    char *buf;
    int i;

    if (input == 0)
        return label ? "-" : "none";
    if ((input >= 'A' && input <= 'Z') || (input >= '0' && input <= '9')) {
        buf = s_made[slot++ % 3];
        buf[0] = (char)(label || input <= '9' ? input : input | 0x20);
        buf[1] = '\0';
        return buf;
    }
    if (input >= VK_NUMPAD0 && input <= VK_NUMPAD9) {
        buf = s_made[slot++ % 3];
        snprintf(buf, sizeof(s_made[0]), label ? "Num %d" : "num%d",
                 input - VK_NUMPAD0);
        return buf;
    }
    for (i = 0; i < NUM_KEYS; i++)
        if (s_keys[i].input == input)
            return label ? s_keys[i].label : s_keys[i].name;
    return "";
}

const char *controls_key_name(int input)
{
    return key_text(input, 0);
}

const char *controls_key_label(int input)
{
    return key_text(input, 1);
}

/* ---- an action's keys (to-do 38) ------------------------------------------ */

#define KEY_BITS  20
#define KEY_MASK  0xFFFFFu

static int key_bind(ControlKeys k, int j)
{
    return (int)(((unsigned long long)k >> (KEY_BITS * j)) & KEY_MASK);
}

int controls_keys_count(ControlKeys k)
{
    int j = 0;
    while (j < CONTROLS_MAX_KEYS && key_bind(k, j))
        j++;
    return j;
}

int controls_keys_get(ControlKeys k, int j)
{
    return j >= 0 && j < CONTROLS_MAX_KEYS ? key_bind(k, j) : 0;
}

/* The same key, held or not (a key added again is taken off whole). */
static int same_bind(int a, int b)
{
    return (a & ~CONTROLS_KEEP) == (b & ~CONTROLS_KEEP);
}

int controls_keys_has(ControlKeys k, int bind)
{
    int j;
    for (j = 0; bind > 0 && j < CONTROLS_MAX_KEYS; j++)
        if (key_bind(k, j) && same_bind(key_bind(k, j), bind))
            return 1;
    return 0;
}

ControlKeys controls_keys_add(ControlKeys k, int bind)
{
    int n = controls_keys_count(k);
    if (CONTROLS_INPUT(bind) <= 0 || (unsigned)bind > KEY_MASK
        || n >= CONTROLS_MAX_KEYS || controls_keys_has(k, bind))
        return k;
    return k | ((ControlKeys)bind << (KEY_BITS * n));
}

ControlKeys controls_keys_remove(ControlKeys k, int bind)
{
    ControlKeys out = 0;
    int j, b;
    for (j = 0; j < CONTROLS_MAX_KEYS && (b = key_bind(k, j)) != 0; j++)
        if (!same_bind(b, bind))
            out = controls_keys_add(out, b);
    return out;
}

/* A combo the window or Windows keeps for itself (Alt+Tab, Alt+F4). */
static int combo_reserved(int hold, int input)
{
    return hold == VK_MENU && (input == VK_TAB || input == VK_F4);
}

int controls_bind_ok(int bind)
{
    int in = CONTROLS_INPUT(bind), hold = CONTROLS_HOLD(bind);
    if (in <= 0)
        return 0;
    if (!hold)
        return !(bind & CONTROLS_KEEP);
    return hold != in && hold < XBOX_KBM_WHEEL_UP && !combo_reserved(hold, in);
}

/* One key of a list: "e", "shift+e", "shift+e keep". -1 when it isn't one. */
static int parse_bind(char *s)
{
    char *plus, *sp;
    int keep = 0, hold = 0, in, bind;

    sp = strchr(s, ' ');
    if (sp) {
        *sp++ = '\0';
        while (*sp == ' ')
            sp++;
        if (_stricmp(sp, "keep"))
            return -1;
        keep = CONTROLS_KEEP;
    }
    plus = strchr(s, '+');
    if (plus) {
        *plus++ = '\0';
        hold = controls_parse_key(s);
        if (hold <= 0)
            return -1;
        s = plus;
    }
    in = controls_parse_key(s);
    if (in < 0 || (in == 0 && (hold || keep)))
        return -1;
    if (in == 0)
        return 0;                       /* none */
    bind = CONTROLS_COMBO(hold, in) | keep;
    return controls_bind_ok(bind) ? bind : -1;
}

int controls_parse_keys(const char *value, ControlKeys *out)
{
    ControlKeys k = 0;
    const char *p = value ? value : "";

    for (;;) {
        char name[48];
        size_t n = 0;
        int bind;
        while (*p == ' ' || *p == '\t')
            p++;
        while (*p && *p != ',') {
            if (n + 1 < sizeof(name))
                name[n++] = *p;
            p++;
        }
        while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t'))
            n--;
        name[n] = '\0';
        bind = parse_bind(name);
        if (bind < 0)
            return -1;
        k = controls_keys_add(k, bind);   /* none, a repeat or a 4th: no change */
        if (*p != ',')
            break;
        p++;
    }
    *out = k;
    return 0;
}

static char s_keys_text[4][160];

static void bind_text(char *buf, size_t size, int bind, int label)
{
    if (CONTROLS_HOLD(bind)) {
        strcat_s(buf, size, key_text(CONTROLS_HOLD(bind), label));
        strcat_s(buf, size, label ? " + " : "+");
    }
    strcat_s(buf, size, key_text(CONTROLS_INPUT(bind), label));
    if (bind & CONTROLS_KEEP)
        strcat_s(buf, size, label ? " (keep)" : " keep");
}

static const char *keys_text(ControlKeys k, int label)
{
    static int slot;
    char *buf = s_keys_text[slot++ % 4];
    int j, b;

    if (!k)
        return label ? "-" : "none";
    buf[0] = '\0';
    for (j = 0; j < CONTROLS_MAX_KEYS && (b = key_bind(k, j)) != 0; j++) {
        if (j)
            strcat_s(buf, sizeof(s_keys_text[0]), label ? " / " : ", ");
        bind_text(buf, sizeof(s_keys_text[0]), b, label);
    }
    return buf;
}

const char *controls_keys_name(ControlKeys k)
{
    return keys_text(k, 0);
}

const char *controls_keys_label(ControlKeys k)
{
    return keys_text(k, 1);
}

const char *controls_bind_label(int bind)
{
    return keys_text(controls_keys_add(0, bind), 1);
}

/* ---- combos as a key reader sees them -------------------------------------
 *
 * The toolkit applies the combo rules to the profile's bindings; keys read
 * here (photo mode, Modern's three, zoom-hold, bat aim, the window's
 * hotkeys) follow the same ones: a combo is on when its hold key went down
 * first; while one is on its input's plain keys don't count. Each table's
 * combos are listed when applied (single player with its swimming / bat
 * rows, multiplayer, photo mode). */

#define COMBO_TABLES 3
#define COMBO_MAX    64
static volatile LONG s_combo[COMBO_TABLES][COMBO_MAX];   /* hold << 9 | input */
static volatile LONG s_ncombo[COMBO_TABLES];

static int combo_table(int set)
{
    return set == CONTROLS_PHOTO ? 2
         : set == CONTROLS_MP || set == CONTROLS_MULE_GUN ? 1 : 0;
}

/* Is the hold key down? xbox_KbmHeld, or Windows' own key state for the
 * window's hotkeys while the keyboard and mouse are off. */
static int hold_down(int hold)
{
    return xbox_KbmHeld((unsigned)hold) || (GetKeyState(hold) & 0x8000);
}

/* Is a combo of this table with this input on? at_press: the window hook
 * asks at the input's key-down, so a hold key down now was first; else both
 * held, the hold key first (Mouse 2 held to aim, then Shift: no combo). */
static int input_shadowed(int set, int input, int at_press)
{
    int t = combo_table(set), i, n = (int)s_ncombo[t];
    for (i = 0; i < n && i < COMBO_MAX; i++) {
        LONG c = s_combo[t][i];
        int hold = (int)(c >> 9);
        if ((c & 0x1FF) != input)
            continue;
        if (at_press ? hold_down(hold)
                     : xbox_KbmComboHeld((unsigned)hold, (unsigned)input))
            return 1;
    }
    return 0;
}

int controls_keys_held(int set, ControlKeys k)
{
    int j, b;
    for (j = 0; j < CONTROLS_MAX_KEYS && (b = key_bind(k, j)) != 0; j++) {
        int in = CONTROLS_INPUT(b), hold = CONTROLS_HOLD(b);
        if (hold ? xbox_KbmComboHeld((unsigned)hold, (unsigned)in)
                 : xbox_KbmHeld((unsigned)in) && !input_shadowed(set, in, 0))
            return 1;
    }
    return 0;
}

int controls_keys_pressed(int set, ControlKeys k, int input)
{
    int j, b;
    for (j = 0; input > 0 && j < CONTROLS_MAX_KEYS && (b = key_bind(k, j)) != 0; j++) {
        int hold = CONTROLS_HOLD(b);
        if (CONTROLS_INPUT(b) != input)
            continue;
        if (hold ? hold_down(hold) : !input_shadowed(set, input, 1))
            return 1;
    }
    return 0;
}

int controls_keys_alt_pressed(ControlKeys k, int input)
{
    int j, b;
    for (j = 0; input > 0 && j < CONTROLS_MAX_KEYS && (b = key_bind(k, j)) != 0; j++)
        if (CONTROLS_HOLD(b) == VK_MENU && CONTROLS_INPUT(b) == input)
            return 1;
    return 0;
}

static void list_combos(int set)
{
    int t = combo_table(set), n = controls_count(set), i, j, b, k = 0;
    for (i = 0; i < n; i++) {
        ControlKeys ks = controls_keys(set, i);
        for (j = 0; j < CONTROLS_MAX_KEYS && (b = key_bind(ks, j)) != 0; j++)
            if (CONTROLS_HOLD(b) && k < COMBO_MAX)
                InterlockedExchange(&s_combo[t][k++],
                                    (LONG)(CONTROLS_HOLD(b) << 9 | CONTROLS_INPUT(b)));
    }
    InterlockedExchange(&s_ncombo[t], k);
}

/* ---- the actions ---------------------------------------------------------- */

/* where: the only place a swimming or bat action works (SWIM_SURFACE,
 * SWIM_UNDER, BAT_GROUND, BAT_AIR); 0 for the rest. scheme: a single player
 * action of one scheme only (CLASSIC, MODERN); 0 for both. pad: its
 * controller bind (pad_binds.h PADB_* + 1; to-do 39), 0 = none (the sticks,
 * Back, the D-pad, the Unlocked Camera's zoom: the controller's own). */
typedef struct {
    ControlAction a;
    unsigned short out[2];
    unsigned char where;
    unsigned char scheme;
    unsigned char pad;
} Action;
#define PAD(x) (PADB_##x + 1)

#define SWIM_SURFACE 1
#define SWIM_UNDER   2
#define BAT_GROUND   3
#define BAT_AIR      4
#define SWIMMING     N_("Swimming")
#define BAT          N_("Bat (Count Batula)")
#define CLASSIC      1
#define MODERN       2

#define LST(d) XBOX_KBM_LSTICK_##d
#define RST(d) XBOX_KBM_RSTICK_##d

/* Single player. Classic: the controller's buttons one to one. Modern
 * (user 2026-10-25): the context key presses B only where B does a context
 * action; holding the aim key takes the weapon out (B) and letting go puts
 * it away; Attack fires only while aiming. Those three follow the game's
 * state, so recomp_manual.c drives them (controls_modern), not the
 * profile. The controller has its own binds per scheme (pad: to-do 39,
 * pad_binds.c; the user's defaults 2026-11-17), shown in the menus in place
 * of a row's fixed button. */
static const Action s_story[] = {
    { { "story_move_forward", N_("Move forward"), N_("Left stick up") }, { LST(UP) } },
    { { "story_move_back", N_("Move back"), N_("Left stick down") }, { LST(DOWN) } },
    { { "story_move_left", N_("Move left"), N_("Left stick left") }, { LST(LEFT) } },
    { { "story_move_right", N_("Move right"), N_("Left stick right") }, { LST(RIGHT) } },
    { { "story_walk", N_("Walk (hold)"), N_("Left stick half way") }, { XBOX_KBM_WALK } },
    { { "story_jump", N_("Jump"), "A" }, { XBOX_KBM_A }, 0, 0, PAD(JUMP) },
    { { "story_crouch", N_("Crouch (hold)"), N_("Right trigger") }, { XBOX_KBM_RT }, 0, 0,
      PAD(CROUCH) },
    { { "story_weapon", N_("Weapon out / away, context action"), "B" },
      { XBOX_KBM_B }, 0, CLASSIC, PAD(CONTEXT) },
    { { "story_context", N_("Context action (on a B pad)"), "B" },
      { XBOX_KBM_B }, 0, MODERN, PAD(CONTEXT) },
    { { "story_aim", N_("Aim weapon (hold)"), N_("Left trigger") },
      { XBOX_KBM_B }, 0, MODERN, PAD(AIM) },
    { { "story_attack", N_("Attack (while aiming)"), N_("Right trigger") }, { XBOX_KBM_RT },
      0, 0, PAD(ATTACK) },
    { { "story_reload", N_("Reload"), "X" }, { XBOX_KBM_X }, 0, 0, PAD(RELOAD) },
    /* The head-cam held or toggled (user 2026-11-17: both, on the keyboard
     * and the controller; the game's own left trigger is held). */
    { { "story_head_cam_hold", N_("Head-cam (hold)"), N_("Left trigger") },
      { XBOX_KBM_LT }, 0, 0, PAD(HEADCAM_HOLD) },
    { { "story_head_cam", N_("Head-cam (toggle)"), "LB" },
      { XBOX_KBM_LT | XBOX_KBM_TOGGLE }, 0, 0, PAD(HEADCAM_TOGGLE) },
    { { "story_zoom", N_("Zoom (Unlocked Camera: reset camera)"), N_("Right stick click") },
      { XBOX_KBM_RTHUMB }, 0, 0, PAD(ZOOM) },
    /* Unlocked Camera's zoom (to-do 41, user 2026-11-15): no button of the
     * game's; the window hook passes its presses to the camera
     * (controls_cam_zoom, conker_CameraZoomNotch). */
    { { "story_cam_zoom_in", N_("Camera zoom in (Unlocked Camera)"),
        N_("D-pad up + right stick up") }, { 0 } },
    { { "story_cam_zoom_out", N_("Camera zoom out (Unlocked Camera)"),
        N_("D-pad up + right stick down") }, { 0 } },
    { { "story_pause", N_("Pause"), "Start" }, { XBOX_KBM_START }, 0, 0, PAD(PAUSE) },
    { { "story_back_button", N_("Back button"), "Back" }, { XBOX_KBM_BACK } },
    { { "story_dpad_up", N_("D-pad up"), N_("D-pad up") }, { XBOX_KBM_DPAD_UP } },
    { { "story_dpad_down", N_("D-pad down"), N_("D-pad down") }, { XBOX_KBM_DPAD_DOWN } },
    { { "story_dpad_left", N_("D-pad left"), N_("D-pad left") }, { XBOX_KBM_DPAD_LEFT } },
    { { "story_dpad_right", N_("D-pad right"), N_("D-pad right") },
      { XBOX_KBM_DPAD_RIGHT } },
    { { "story_look_up", N_("Look up (keys)"), N_("Right stick up") }, { RST(UP) } },
    { { "story_look_down", N_("Look down (keys)"), N_("Right stick down") }, { RST(DOWN) } },
    { { "story_look_left", N_("Look left (keys)"), N_("Right stick left") }, { RST(LEFT) } },
    { { "story_look_right", N_("Look right (keys)"), N_("Right stick right") },
      { RST(RIGHT) } },
    /* Swimming (user 2026-10-26): Space up and Ctrl down underwater, as
     * jump and crouch on land; apply_set says how they take over. The game's
     * own buttons: B dives, B swims up, A down. */
    { { "story_dive", N_("Dive Under"), "B", SWIMMING },
      { XBOX_KBM_B }, SWIM_SURFACE, 0, PAD(DIVE) },
    { { "story_swim_up", N_("Swim up"), "B", SWIMMING },
      { XBOX_KBM_B }, SWIM_UNDER, 0, PAD(SWIM_UP) },
    { { "story_swim_down", N_("Swim down"), "A", SWIMMING },
      { XBOX_KBM_A }, SWIM_UNDER, 0, PAD(SWIM_DOWN) },
    /* The bat (Count Batula, user 2026-10-28): B launches from the ground;
     * in the air B flies up, A down, the right trigger poops and the left
     * trigger toggles the aiming view. Aim is driven by recomp_manual.c
     * (controls_bat_aim): a tap of the left trigger, never a hold, since
     * the flight code stops the bat while it is held. Two aim rows (user
     * 2026-11-17): a toggle and a hold; story_bat_aim is the scheme's own
     * (Classic toggles, Modern holds), story_bat_aim_alt the other. */
    { { "story_bat_launch", N_("Launch (on the ground)"), "B", BAT },
      { XBOX_KBM_B }, BAT_GROUND, 0, PAD(BAT_LAUNCH) },
    { { "story_bat_up", N_("Fly up"), "B", BAT }, { XBOX_KBM_B }, BAT_AIR, 0,
      PAD(BAT_UP) },
    { { "story_bat_down", N_("Fly down"), "A", BAT }, { XBOX_KBM_A }, BAT_AIR, 0,
      PAD(BAT_DOWN) },
    { { "story_bat_poop", N_("Poop"), N_("Right trigger"), BAT },
      { XBOX_KBM_RT }, BAT_AIR, 0, PAD(BAT_POOP) },
    { { "story_bat_aim", N_("Aim (toggle)"), N_("Left trigger"), BAT },
      { XBOX_KBM_LT }, BAT_AIR, CLASSIC, PAD(BAT_AIM_TOGGLE) },
    { { "story_bat_aim_alt", N_("Aim (hold)"), N_("Left trigger held"), BAT },
      { XBOX_KBM_LT }, BAT_AIR, CLASSIC, PAD(BAT_AIM_HOLD) },
    { { "story_bat_aim_alt", N_("Aim (toggle)"), N_("Left trigger"), BAT },
      { XBOX_KBM_LT }, BAT_AIR, MODERN, PAD(BAT_AIM_TOGGLE) },
    { { "story_bat_aim", N_("Aim (hold)"), N_("Left trigger held"), BAT },
      { XBOX_KBM_LT }, BAT_AIR, MODERN, PAD(BAT_AIM_HOLD) },
};

/* Multiplayer, the game's "Emily" layout. Interact and reload are both A
 * in the game; two keys let the player think of them apart. Holding the
 * weapon key opens the game's weapon wheel, steered with the left stick:
 * the mouse steers it while the key is held; the same for the abilities
 * wheel. */
static const Action s_mp[] = {
    { { "mp_move_forward", N_("Move forward"), N_("Left stick up") }, { LST(UP) } },
    { { "mp_move_back", N_("Move back"), N_("Left stick down") }, { LST(DOWN) } },
    { { "mp_move_left", N_("Move left"), N_("Left stick left") }, { LST(LEFT) } },
    { { "mp_move_right", N_("Move right"), N_("Left stick right") }, { LST(RIGHT) } },
    { { "mp_walk", N_("Walk (hold)"), N_("Left stick half way") }, { XBOX_KBM_WALK } },
    { { "mp_jump", N_("Jump"), N_("Left stick click") }, { XBOX_KBM_LTHUMB } },
    { { "mp_fire", N_("Fire"), N_("Right trigger") }, { XBOX_KBM_RT } },
    { { "mp_zoom", N_("Zoom"), N_("Right stick click") }, { XBOX_KBM_RTHUMB } },
    { { "mp_reload", N_("Reload"), "A" }, { XBOX_KBM_A } },
    { { "mp_interact", N_("Interact (terminals)"), "A" }, { XBOX_KBM_A } },
    { { "mp_weapon_mode", N_("Weapon mode"), N_("Left trigger") }, { XBOX_KBM_LT } },
    { { "mp_ability", N_("Abilities (tap) / abilities wheel (hold, aim with "
           "the mouse)"), "X" }, { XBOX_KBM_X, XBOX_KBM_MOUSE_LSTICK } },
    { { "mp_weapon", N_("Next weapon (tap) / weapon wheel (hold, aim with "
           "the mouse)"), "Y" }, { XBOX_KBM_Y, XBOX_KBM_MOUSE_LSTICK } },
    { { "mp_crouch", N_("Crouch / prone"), "B" }, { XBOX_KBM_B } },
    { { "mp_vote", N_("Vote"), "Black" }, { XBOX_KBM_BLACK } },
    { { "mp_radio", N_("Radio mode"), "White" }, { XBOX_KBM_WHITE } },
    { { "mp_pause", N_("Pause / options"), "Start" }, { XBOX_KBM_START } },
    { { "mp_back_button", N_("Back button"), "Back" }, { XBOX_KBM_BACK } },
    { { "mp_dpad_up", N_("D-pad up"), N_("D-pad up") }, { XBOX_KBM_DPAD_UP } },
    { { "mp_dpad_down", N_("D-pad down"), N_("D-pad down") }, { XBOX_KBM_DPAD_DOWN } },
    { { "mp_dpad_left", N_("D-pad left"), N_("D-pad left") }, { XBOX_KBM_DPAD_LEFT } },
    { { "mp_dpad_right", N_("D-pad right"), N_("D-pad right") }, { XBOX_KBM_DPAD_RIGHT } },
    { { "mp_look_up", N_("Look up (keys)"), N_("Right stick up") }, { RST(UP) } },
    { { "mp_look_down", N_("Look down (keys)"), N_("Right stick down") }, { RST(DOWN) } },
    { { "mp_look_left", N_("Look left (keys)"), N_("Right stick left") }, { RST(LEFT) } },
    { { "mp_look_right", N_("Look right (keys)"), N_("Right stick right") },
      { RST(RIGHT) } },
};

/* Menus (user 2026-10-25): the same keys everywhere but Esc. On the front
 * ends (the main menu in the pub, the multiplayer menus) Esc goes back
 * like B; in a level's menu (the multiplayer pause menu) Esc is Start,
 * which closes it. Not changeable. Two keys for one button are two rows;
 * both tables share s_menu_keys.
 *
 * The F3 menu lists the fixed sets one line per label, its keys in row
 * order (user 2026-10-26); a row without a label is not listed: Y does
 * nothing on the main menu, so it stays bound for the menus that may use
 * it but is not shown.
 *
 * Tab is X, which opens the map options on the multiplayer map select
 * (user 2026-10-28); F3 lists it in its own "Multiplayer menu" section. In
 * a level's menu Tab does nothing. */
#define MP_MENU N_("Multiplayer menu")
static const Action s_menu[] = {
    { { "", N_("Navigate"), N_("Left stick up") }, { LST(UP) } },
    { { "", N_("Navigate"), N_("Left stick down") }, { LST(DOWN) } },
    { { "", N_("Navigate"), N_("Left stick left") }, { LST(LEFT) } },
    { { "", N_("Navigate"), N_("Left stick right") }, { LST(RIGHT) } },
    { { "", N_("Navigate"), N_("D-pad up") }, { XBOX_KBM_DPAD_UP } },
    { { "", N_("Navigate"), N_("D-pad down") }, { XBOX_KBM_DPAD_DOWN } },
    { { "", N_("Navigate"), N_("D-pad left") }, { XBOX_KBM_DPAD_LEFT } },
    { { "", N_("Navigate"), N_("D-pad right") }, { XBOX_KBM_DPAD_RIGHT } },
    { { "", N_("Choose"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Choose"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Choose"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Go back"), "B" }, { XBOX_KBM_B } },
    { { "", N_("Go back"), "B" }, { XBOX_KBM_B } },
    { { "", N_("Go back"), "B" }, { XBOX_KBM_B } },
    { { "", "", "Y" }, { XBOX_KBM_Y } },
    { { "", "", "Y" }, { XBOX_KBM_Y } },
    { { "", N_("Map options"), "X", MP_MENU }, { XBOX_KBM_X } },
};
static const Action s_menu_game[] = {
    { { "", N_("Navigate"), N_("Left stick up") }, { LST(UP) } },
    { { "", N_("Navigate"), N_("Left stick down") }, { LST(DOWN) } },
    { { "", N_("Navigate"), N_("Left stick left") }, { LST(LEFT) } },
    { { "", N_("Navigate"), N_("Left stick right") }, { LST(RIGHT) } },
    { { "", N_("Navigate"), N_("D-pad up") }, { XBOX_KBM_DPAD_UP } },
    { { "", N_("Navigate"), N_("D-pad down") }, { XBOX_KBM_DPAD_DOWN } },
    { { "", N_("Navigate"), N_("D-pad left") }, { XBOX_KBM_DPAD_LEFT } },
    { { "", N_("Navigate"), N_("D-pad right") }, { XBOX_KBM_DPAD_RIGHT } },
    { { "", N_("Choose"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Choose"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Choose"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Go back"), "B" }, { XBOX_KBM_B } },
    { { "", N_("Close the menu"), "Start" }, { XBOX_KBM_START } },
    { { "", N_("Go back"), "B" }, { XBOX_KBM_B } },
    { { "", "", "Y" }, { XBOX_KBM_Y } },
    { { "", "", "Y" }, { XBOX_KBM_Y } },
    { { "", "", "" }, { 0 } },
};
static const int s_menu_keys[] = {
    'W', 'S', 'A', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    'E', VK_SPACE, VK_RETURN, 'Q', VK_ESCAPE, VK_BACK, 'Y', 'O', VK_TAB,
};

/* The single player pause screen (A quits, B resumes, Y options). Its
 * Options page is no menu list: the same three buttons stay (Y = back),
 * the stick picks a line and changes it; "Quit?" asks Yes / No, answered
 * with A. So the menu's moving and choosing keys work here too, and Q is
 * A as the user asked (Quit). Backspace resumes, as Esc does. */
static const Action s_pause[] = {
    { { "", N_("Resume"), "Start" }, { XBOX_KBM_START } },
    { { "", N_("Resume"), "Start" }, { XBOX_KBM_START } },
    { { "", N_("Quit"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Confirm quit"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Confirm quit"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Confirm quit"), "A" }, { XBOX_KBM_A } },
    { { "", N_("Open / close options"), "Y" }, { XBOX_KBM_Y } },
    { { "", N_("Open / close options"), "Y" }, { XBOX_KBM_Y } },
    { { "", N_("Open / close options"), "Y" }, { XBOX_KBM_Y } },
    { { "", N_("Navigate"), N_("Left stick") }, { LST(UP) } },
    { { "", N_("Navigate"), N_("Left stick") }, { LST(DOWN) } },
    { { "", N_("Navigate"), N_("Left stick") }, { LST(LEFT) } },
    { { "", N_("Navigate"), N_("Left stick") }, { LST(RIGHT) } },
    { { "", N_("Navigate"), "D-pad" }, { XBOX_KBM_DPAD_UP } },
    { { "", N_("Navigate"), "D-pad" }, { XBOX_KBM_DPAD_DOWN } },
    { { "", N_("Navigate"), "D-pad" }, { XBOX_KBM_DPAD_LEFT } },
    { { "", N_("Navigate"), "D-pad" }, { XBOX_KBM_DPAD_RIGHT } },
    /* To-do 196: X opens the cheat menu from the Options page once it is
     * unlocked; the X key (user 2026-11-11, was Tab; Tab is now Y, Options).
     * No label: F3 doesn't list it. */
    { { "", "", "X" }, { XBOX_KBM_X } },
};
static const int s_pause_keys[] = {
    VK_ESCAPE, VK_BACK, 'Q', 'E', VK_SPACE, VK_RETURN, 'Y', 'O', VK_TAB,
    'W', 'S', 'A', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'X',
};

/* Cutscenes (user 2026-10-25): Space skips one seen before (the left
 * trigger, as on the controller), Esc pauses. Nothing else, so the
 * head-cam key cannot start a skip and stay held after it. E and Mouse 1
 * skip too (user 2026-11-04, to-do 16). */
static const Action s_cutscene[] = {
    { { "", N_("Skip"), N_("Left trigger") }, { XBOX_KBM_LT } },
    { { "", N_("Skip"), N_("Left trigger") }, { XBOX_KBM_LT } },
    { { "", N_("Skip"), N_("Left trigger") }, { XBOX_KBM_LT } },
    { { "", N_("Pause"), "Start" }, { XBOX_KBM_START } },
};
static const int s_cutscene_keys[] = { VK_SPACE, 'E', VK_LBUTTON, VK_ESCAPE };

/* Photo mode (user 2026-10-29, SPEC 10.1): no output, the keys are read
 * by photo_mode.c; rows in controls.h's PHOTO_ order. The button is the
 * pad's own, fixed. */
#define PHOTO_ANYWHERE N_("Toggle")
#define PHOTO_IN       N_("In photo mode")
static const Action s_photo[] = {
    { { "photo_toggle", N_("Photo mode on / off"), "Back + RB", PHOTO_ANYWHERE } },
    { { "pause_key", N_("Pause (hold the picture)"), "none", PHOTO_ANYWHERE } },
    /* To-do 12 (SPEC 9.2): the pad reaches it through F2's button. */
    { { "report_key", N_("Report Bug"), "none", PHOTO_ANYWHERE } },
    /* To-do 14 (SPEC 10.3): the pad reaches it through Back + Start. */
    { { "cheats_key", N_("Debug menu"), "none", PHOTO_ANYWHERE } },
    /* To-do 24 (SPEC 10.3): --debug only; the pad has RB + A. */
    { { "noclip_key", N_("Noclip"), "RB + A", PHOTO_ANYWHERE } },
    /* To-do 161: shown once the cheat is unlocked; the pad has both sticks clicked. */
    { { "bullet_key", N_("Bullet time (cheat)"), N_("LS + RS click"), PHOTO_ANYWHERE } },
    /* To-do 168: --debug only; a [MARK] line and a shot (the console's mark). */
    { { "mark_key", N_("Mark (log line + shot)"), "none", PHOTO_ANYWHERE } },
    { { "photo_forward", N_("Fly forward"), N_("Left stick up"), PHOTO_IN } },
    { { "photo_back", N_("Fly back"), N_("Left stick down"), PHOTO_IN } },
    { { "photo_left", N_("Fly left"), N_("Left stick left"), PHOTO_IN } },
    { { "photo_right", N_("Fly right"), N_("Left stick right"), PHOTO_IN } },
    { { "photo_up", N_("Fly up"), N_("Right trigger"), PHOTO_IN } },
    { { "photo_down", N_("Fly down"), N_("Left trigger"), PHOTO_IN } },
    { { "photo_roll_left", N_("Roll left (anticlockwise)"), "LB", PHOTO_IN } },
    { { "photo_roll_right", N_("Roll right (clockwise)"), "RB", PHOTO_IN } },
    { { "photo_slow", N_("Fly slower (hold)"), N_("Right stick click"), PHOTO_IN } },
    { { "photo_fast", N_("Fly faster (hold)"), N_("Left stick click"), PHOTO_IN } },
    { { "photo_zoom_in", N_("Zoom in"), N_("D-pad up"), PHOTO_IN } },
    { { "photo_zoom_out", N_("Zoom out"), N_("D-pad down"), PHOTO_IN } },
    { { "photo_take", N_("Take a photo"), "A", PHOTO_IN } },
    { { "photo_leave", N_("Leave photo mode"), "Back + RB", PHOTO_IN } },
    { { "photo_reset", N_("Camera back to the start"), "Back", PHOTO_IN } },
    { { "photo_hud", N_("Show / hide the HUD and speech bubbles"), "Y",
        PHOTO_IN } },
    { { "photo_conker", N_("Show / hide Conker"), "X", PHOTO_IN } },
    { { "photo_step", N_("Frame step (one frame; hold: normal speed)"), N_("D-pad right"),
        PHOTO_IN } },
    { { "photo_resume", N_("Pause / resume the world"), "Back + A", PHOTO_IN } },
    { { "photo_panel", N_("Photo settings"), "Start", PHOTO_IN } },
    /* B was "leave" until 2026-10-29 (user): Back + RB leaves too. */
    { { "photo_cutscene_blur", N_("Cutscene blur on / off"), "B", PHOTO_IN } },
    /* To-do 33: --debug only, freecam: the pad and keys go to the camera or Conker. */
    { { "freecam_swap", N_("Freecam: camera or Conker"), "Back + X", PHOTO_IN } },
};

static const Action *set_table(int set, int *n)
{
    if (set == CONTROLS_PHOTO) {
        *n = (int)(sizeof(s_photo) / sizeof(s_photo[0]));
        return s_photo;
    }
    if (set == CONTROLS_MENU) {
        *n = (int)(sizeof(s_menu) / sizeof(s_menu[0]));
        return s_menu;
    }
    if (set == CONTROLS_PAUSE) {
        *n = (int)(sizeof(s_pause) / sizeof(s_pause[0]));
        return s_pause;
    }
    if (set == CONTROLS_MENU_GAME) {
        *n = (int)(sizeof(s_menu_game) / sizeof(s_menu_game[0]));
        return s_menu_game;
    }
    if (set == CONTROLS_CUTSCENE) {
        *n = (int)(sizeof(s_cutscene) / sizeof(s_cutscene[0]));
        return s_cutscene;
    }
    if (set == CONTROLS_MP || set == CONTROLS_MULE_GUN) {
        *n = (int)(sizeof(s_mp) / sizeof(s_mp[0]));
        return s_mp;
    }
    /* Single player, also swimming. */
    *n = (int)(sizeof(s_story) / sizeof(s_story[0]));
    return s_story;
}

static int modern_set(void)
{
    return !strcmp(settings_get("story_scheme"), "modern");
}

/* A set's rows as the menu and the profile see them: single player's
 * without the other scheme's. Returns the table, its count in *n and, in
 * map, each row's index in the table. */
static const Action *set_rows(int set, int *n, int *map)
{
    int all, i, k = 0, skip;
    const Action *t = set_table(set, &all);

    skip = t != s_story ? 0 : modern_set() ? CLASSIC : MODERN;
    for (i = 0; i < all; i++)
        if (!skip || t[i].scheme != skip)
            map[k++] = i;
    *n = k;
    return t;
}

#define MAX_ROWS 64

int controls_count(int set)
{
    int n, map[MAX_ROWS];
    set_rows(set, &n, map);
    return n;
}

static const Action *row(int set, int i)
{
    int n, map[MAX_ROWS];
    const Action *t = set_rows(set, &n, map);
    return i >= 0 && i < n ? &t[map[i]] : NULL;
}

const ControlAction *controls_action(int set, int i)
{
    const Action *r = row(set, i);
    return r ? &r->a : NULL;
}

/* The controller button shown for a row: the photo table's chord rows
 * (photo mode, its leave, noclip, bullet time, pause / resume, the freecam
 * swap) show the chord set in F3 > Slots > Chords (chords.c). */
const char *controls_button(int set, int i)
{
    const ControlAction *a = controls_action(set, i);
    int pa = controls_pad_action(set, i);
    if (pa >= 0)
        return pad_binds_button_label(pad_binds_get(pa, modern_set()));
    if (set == CONTROLS_PHOTO) {
        int fn = i == PHOTO_TOGGLE || i == PHOTO_LEAVE ? CHORD_PHOTO
               : i == PHOTO_NOCLIP ? CHORD_NOCLIP
               : i == PHOTO_BULLET ? CHORD_BULLET
               : i == PHOTO_RESUME ? CHORD_PHOTO_PAUSE
               : i == PHOTO_FREECAM_SWAP ? CHORD_FREECAM_SWAP : -1;
        if (fn >= 0)
            return chords_fn_chord_label(fn);
    }
    return a ? a->button : "";
}

int controls_pad_action(int set, int i)
{
    const Action *r = set == CONTROLS_STORY ? row(set, i) : NULL;
    return r && r->pad ? r->pad - 1 : -1;
}

int controls_table_pad(int set, int i)
{
    int n;
    const Action *t = set_table(set, &n);
    return set == CONTROLS_STORY && i >= 0 && i < n && t[i].pad ? t[i].pad - 1 : -1;
}

const ControlAction *controls_table_row(int set, int i, int *scheme)
{
    int n;
    const Action *t = set_table(set, &n);
    if (i < 0 || i >= n)
        return NULL;
    *scheme = t[i].scheme;
    return &t[i].a;
}

/* A player set option's keys from settings.ini (already checked there). */
static ControlKeys keys_of(const char *option)
{
    ControlKeys k;
    return controls_parse_keys(settings_get(option), &k) < 0 ? 0 : k;
}

/* An action's keys: settings.ini's for the two player sets, the fixed one
 * for menus, the pause screen and cutscenes. */
ControlKeys controls_keys(int set, int i)
{
    if (set == CONTROLS_MENU || set == CONTROLS_MENU_GAME)
        return controls_keys_add(0, s_menu_keys[i]);
    if (set == CONTROLS_CUTSCENE)
        return controls_keys_add(0, s_cutscene_keys[i]);
    if (set == CONTROLS_PAUSE)
        return controls_keys_add(0, s_pause_keys[i]);
    return keys_of(controls_action(set, i)->key);
}

int controls_key(int set, int i)
{
    return CONTROLS_INPUT(controls_keys_get(controls_keys(set, i), 0));
}

/* ---- to the toolkit ------------------------------------------------------- */

static volatile LONG s_mode = -1;

/* The two player sets' mouse settings, for mouse look on the game thread
 * (controls_mouse): copied when applied, not read from settings.ini there. */
static volatile float s_sens[2][2];
static volatile LONG s_inv[2];

void controls_mouse(int set, float *sx, float *sy, int *invert)
{
    int i = set == CONTROLS_MP || set == CONTROLS_MULE_GUN ? 1 : 0;
    *sx = s_sens[i][0];
    *sy = s_sens[i][1];
    *invert = (int)s_inv[i];
}

/* The photo table's rows that work everywhere, for the window's hook. */
static volatile LONG64 s_hot[PHOTO_ANYWHERE_LAST + 1];

ControlKeys controls_hotkeys(int row)
{
    return row >= 0 && row <= PHOTO_ANYWHERE_LAST ? (ControlKeys)s_hot[row] : 0;
}

/* Photo mode's profile: nothing bound and the mouse not looking, so the
 * game gets a pad at rest; photo_mode.c reads the keys itself. */
static void apply_photo(void)
{
    xbox_KbmBinding none[1] = { { 0, 0 } };
    xbox_KbmMouse m;
    int r;

    memset(&m, 0, sizeof(m));
    m.left.full = m.right.full = 1.0f;
    xbox_KbmSetProfile(CONTROLS_PHOTO, none, 0, &m);
    for (r = 0; r <= PHOTO_ANYWHERE_LAST; r++)
        InterlockedExchange64(&s_hot[r], controls_keys(CONTROLS_PHOTO, r));
}

/* Is this key bound to a swimming or bat action that works here? */
static int place_key(const Action *t, const int *map, int n, int set,
                     int where, int bind)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[map[i]].where == where
            && controls_keys_has(controls_keys(set, i), bind))
            return 1;
    return 0;
}

static int zoom_hold_set(void)
{
    return !strcmp(settings_get("mp_zoom_mode"), "hold");
}

/* Single player's scheme as last applied, and Modern's three keys, for the
 * game thread (controls_modern); the same for multiplayer's zoom held. */
static volatile LONG s_modern;
static volatile LONG64 s_modern_keys[3];
static volatile LONG s_zoom_hold;
static volatile LONG64 s_zoom_key;
static volatile LONG64 s_bat_aim_key[2];   /* toggle, hold */
static volatile LONG s_sling_toggle;
static volatile LONG64 s_cam_zoom_keys[2];

/* Unlocked Camera's zoom keys, while single player's on-foot keys are on,
 * or its swimming keys (the same rows; to-do 256, user's report 2026-11-16:
 * the wheel did nothing in the water) (the window thread asks at each key /
 * wheel message). */
int controls_cam_zoom(ControlKeys *in, ControlKeys *out)
{
    LONG set = s_mode;
    *in = (ControlKeys)s_cam_zoom_keys[0];
    *out = (ControlKeys)s_cam_zoom_keys[1];
    return set == CONTROLS_STORY || set == CONTROLS_BEE_GUN
        || set == CONTROLS_VAULT_SUIT || set == CONTROLS_SWIM_SURFACE
        || set == CONTROLS_SWIM_UNDER;
}

int controls_sling_toggle(void)
{
    return (int)s_sling_toggle;
}

int controls_bat_aim(ControlKeys *toggle, ControlKeys *hold)
{
    *toggle = (ControlKeys)s_bat_aim_key[0];
    *hold = (ControlKeys)s_bat_aim_key[1];
    return (int)s_modern;
}

int controls_zoom_hold(ControlKeys *zoom)
{
    *zoom = (ControlKeys)s_zoom_key;
    return (int)s_zoom_hold;
}

int controls_modern(ControlKeys *context, ControlKeys *aim, ControlKeys *attack)
{
    if (context)
        *context = (ControlKeys)s_modern_keys[0];
    if (aim)
        *aim = (ControlKeys)s_modern_keys[1];
    if (attack)
        *attack = (ControlKeys)s_modern_keys[2];
    return (int)s_modern;
}

/* Swimming: the swimming actions press their buttons only where they work
 * and take over there. At the surface only the Dive key presses B (the
 * weapon key's B is left out); underwater only Swim up and Swim down press
 * B and A (jump and the weapon key do nothing: Q only dives, user
 * 2026-10-26). A key bound to a swimming action does only that where it
 * works: Ctrl underwater is Swim down, not crouch.
 *
 * The bat the same way (user 2026-10-28): on the ground the Launch key
 * presses B (Space is Launch, not jump); in the air only the bat's keys
 * press A, B and the triggers (the head-cam key's held left trigger would
 * stop the bat), and Aim is left to recomp_manual.c. */
static volatile LONG s_cut_latched;   /* controls_set_mode */

/* A skip key (s_cutscene's left trigger rows) left out while latched. */
static int key_latched(int input)
{
    int r, n = (int)(sizeof(s_cutscene) / sizeof(s_cutscene[0]));
    for (r = 0; r < n; r++)
        if ((s_cut_latched & (1 << r)) && s_cutscene_keys[r] == input)
            return 1;
    return 0;
}

/* Per set, a bit per Windows key that presses Start (controls_start_key). */
static volatile LONG s_start_bits[CONTROLS_PHOTO][8];

static void apply_set(int set)
{
    static const char *const prefix[] = { "story", "mp" };
    xbox_KbmBinding b[256];
    xbox_KbmMouse m;
    char key[64];
    int n, r, q, j, k = 0, map[MAX_ROWS];
    const Action *t = set_rows(set, &n, map);
    int where = set == CONTROLS_SWIM_SURFACE ? SWIM_SURFACE
              : set == CONTROLS_SWIM_UNDER ? SWIM_UNDER
              : set == CONTROLS_BAT_GROUND ? BAT_GROUND
              : set == CONTROLS_BAT_AIR ? BAT_AIR : 0;
    int p = set == CONTROLS_MP || set == CONTROLS_MULE_GUN ? 1
          : set == CONTROLS_STORY || where || set == CONTROLS_BEE_GUN
            || set == CONTROLS_VAULT_SUIT || set == CONTROLS_VAULT_TAIL ? 0 : -1;
    int suit = set == CONTROLS_VAULT_SUIT || set == CONTROLS_VAULT_TAIL;
    int suit_free_m2 = suit;
    int modern = p == 0 && modern_set();
    ControlKeys attack = keys_of("story_attack");

    /* Each key of each row (to-do 38: up to three a row); a key left out
     * below is left out alone, the row's other keys go on. A combo key is a
     * binding with its hold key (the toolkit's combo rules). */
    memset(b, 0, sizeof(b));
    for (r = 0, q = 0; r < n; q + 1 < CONTROLS_MAX_KEYS ? q++ : (q = 0, r++)) {
        const Action *a = &t[map[r]];
        int bind = controls_keys_get(controls_keys(set, r), q);
        int input = CONTROLS_INPUT(bind);
        unsigned short hold = (unsigned short)CONTROLS_HOLD(bind);
        unsigned short flags = (bind & CONTROLS_KEEP) ? XBOX_KBM_KEEP : 0;
        unsigned short out = a->out[0] & ~XBOX_KBM_TOGGLE;
        if (input <= 0 || k + 3 > (int)(sizeof(b) / sizeof(b[0])))
            continue;
        if (set == s_mode && !hold && key_latched(input))
            continue;
        if (a->where != 0 && a->where != where)
            continue;
        if (!strcmp(a->a.key, "story_bat_aim")
            || !strcmp(a->a.key, "story_bat_aim_alt"))
            continue;
        /* Modern on foot: the context key, aiming and Attack follow the
         * game's state (controls_modern; recomp_manual.c). Elsewhere the
         * context key is plain B, and the aim key blocks in The Vault's
         * suit (below) and does nothing else. */
        if (modern && set == CONTROLS_STORY
            && (!strcmp(a->a.key, "story_context")
                || !strcmp(a->a.key, "story_aim")
                || !strcmp(a->a.key, "story_attack")))
            continue;
        /* Multiplayer zoom held (mp_zoom_mode): the key follows the
         * game's zoom state on foot (controls_zoom_hold; recomp_manual.c),
         * so the plain set leaves it out. The Mule's gunner seat keeps it. */
        if (set == CONTROLS_MP && zoom_hold_set()
            && !strcmp(a->a.key, "mp_zoom"))
            continue;
        if (modern && !strcmp(a->a.key, "story_aim")) {
            if (suit) {
                b[k].input = (unsigned short)input;
                b[k].hold = hold;
                b[k].flags = flags;
                b[k].output = XBOX_KBM_LT;
                k++;
            }
            continue;
        }
        if (bind == VK_RBUTTON)
            suit_free_m2 = 0;
        if (!a->where && where) {
            if ((where == SWIM_SURFACE || where == SWIM_UNDER)
                && out == XBOX_KBM_B)
                continue;
            if (where == SWIM_UNDER && out == XBOX_KBM_A)
                continue;
            if (where == BAT_AIR
                && (out == XBOX_KBM_A || out == XBOX_KBM_B
                    || out == XBOX_KBM_LT || out == XBOX_KBM_RT))
                continue;
            if (place_key(t, map, n, set, where, bind))
                continue;
        }
        for (j = 0; j < 2 && a->out[j]; j++) {
            unsigned short o = a->out[j];
            /* The beehive gun aims down with the stick forward; forward and
             * back keys swap there so W aims up (user 2026-10-28; the
             * controller stays as the game has it). The Mule 52's gunner
             * seat in multiplayer aims the same way (user 2026-10-28). */
            if (set == CONTROLS_BEE_GUN || set == CONTROLS_MULE_GUN) {
                unsigned short s = o & ~XBOX_KBM_TOGGLE;
                if (s == XBOX_KBM_LSTICK_UP)
                    o = (unsigned short)((o & XBOX_KBM_TOGGLE) | XBOX_KBM_LSTICK_DOWN);
                else if (s == XBOX_KBM_LSTICK_DOWN)
                    o = (unsigned short)((o & XBOX_KBM_TOGGLE) | XBOX_KBM_LSTICK_UP);
            }
            /* The Vault's suit (the final boss): the right trigger hits and
             * the left blocks, there is no crouching; the Crouch key blocks
             * there (user 2026-10-28; not in F3, not changeable). A Crouch
             * key that is also the Attack key keeps hitting. */
            if (suit && !strcmp(a->a.key, "story_crouch")
                && !controls_keys_has(attack, bind))
                o = XBOX_KBM_LT;
            /* Holding the alien's tail the left stick goes round to spin
             * him (user 2026-10-28): Move right spins clockwise, Move left
             * anticlockwise, as a thumb going round. */
            if (set == CONTROLS_VAULT_TAIL && o == XBOX_KBM_LSTICK_RIGHT)
                o = XBOX_KBM_LSTICK_SPIN_CW;
            else if (set == CONTROLS_VAULT_TAIL && o == XBOX_KBM_LSTICK_LEFT)
                o = XBOX_KBM_LSTICK_SPIN_CCW;
            b[k].input = (unsigned short)input;
            b[k].hold = hold;
            b[k].flags = flags;
            b[k].output = o;
            k++;
        }
    }
    /* And Mouse 2 blocks there, unless it is bound to a single player
     * action on land (the player's Crouch key blocks then). */
    if (suit_free_m2) {
        b[k].input = VK_RBUTTON;
        b[k].output = XBOX_KBM_LT;
        k++;
    }
    if (set == CONTROLS_STORY) {
        InterlockedExchange64(&s_modern_keys[0], keys_of("story_context"));
        InterlockedExchange64(&s_modern_keys[1], keys_of("story_aim"));
        InterlockedExchange64(&s_modern_keys[2], attack);
        /* Classic: story_bat_aim toggles, _alt holds; Modern the other way. */
        InterlockedExchange64(&s_bat_aim_key[modern ? 1 : 0], keys_of("story_bat_aim"));
        InterlockedExchange64(&s_bat_aim_key[modern ? 0 : 1], keys_of("story_bat_aim_alt"));
        InterlockedExchange64(&s_cam_zoom_keys[0], keys_of("story_cam_zoom_in"));
        InterlockedExchange64(&s_cam_zoom_keys[1], keys_of("story_cam_zoom_out"));
        InterlockedExchange(&s_sling_toggle,
                            !strcmp(settings_get("story_slingshot_aim"),
                                    "toggle"));
        InterlockedExchange(&s_modern, modern);
    }
    if (set == CONTROLS_MP) {
        InterlockedExchange64(&s_zoom_key, keys_of("mp_zoom"));
        InterlockedExchange(&s_zoom_hold, zoom_hold_set());
    }
    if (p >= 0) {
        snprintf(key, sizeof(key), "%s_mouse_x", prefix[p]);
        m.sens_x = (float)atof(settings_get(key)) / 100.0f;
        snprintf(key, sizeof(key), "%s_mouse_y", prefix[p]);
        m.sens_y = (float)atof(settings_get(key)) / 100.0f;
        snprintf(key, sizeof(key), "%s_invert_y", prefix[p]);
        m.invert_y = strcmp(settings_get(key), "0") != 0;
        s_sens[p][0] = m.sens_x;
        s_sens[p][1] = m.sens_y;
        s_inv[p] = m.invert_y;
    } else {
        /* In menus the mouse does nothing (no stray stick). */
        m.sens_x = m.sens_y = 0.0f;
        m.invert_y = 0;
    }
    /* Walking: a bit under half of the game's range (to be tuned in play). */
    m.walk = 0.45f;
    m.left.dead = 15.0f / 110.0f;
    m.left.full = 87.0f / 110.0f;
    m.right.dead = 15.0f / 99.0f;
    m.right.full = 74.0f / 99.0f;
    xbox_KbmSetProfile(set, b, k, &m);
    /* The keys that press Start in this set (to-do 181). */
    {
        LONG bits[8] = { 0 };
        for (j = 0; j < k; j++)
            if ((b[j].output & ~XBOX_KBM_TOGGLE) == XBOX_KBM_START
                && !b[j].hold && b[j].input < 256)
                bits[b[j].input >> 5] |= (LONG)(1u << (b[j].input & 31));
        for (j = 0; j < 8; j++)
            InterlockedExchange(&s_start_bits[set][j], bits[j]);
    }
}

int controls_start_key(int vk)
{
    const int set = controls_mode();
    if (vk <= 0 || vk >= 256 || set < 0 || set >= CONTROLS_PHOTO)
        return 0;
    return (s_start_bits[set][vk >> 5] >> (vk & 31)) & 1;
}

/* ---- controller ports (F3 > Slots) -----------------------------
 *
 * The toolkit's fixed port assignment (xinput_device.c, xinput_xbox.h; not
 * included: its Xbox types clash with windows.h). Ports 1-4 are the game's
 * players; each gets the controller its controller_slot<n> names, and the
 * keyboard and mouse go to keyboard_slot's. */
#define PORT_NONE (-1)    /* XBOX_INPUT_NONE */
#define PORT_ANY  (-2)    /* XBOX_INPUT_ANY */
void xbox_InputAssign(unsigned long port, int source);
int  xbox_InputAssigned(unsigned long port);
int  xbox_InputAssignActive(void);
void xbox_InputSetKeyboardPort(int port);
int  xbox_InputHostPadConnected(unsigned long slot);
int  xbox_InputHostPadOfPort(unsigned long port);
void xbox_SetRumbleStrength(int port, int percent);
void xbox_SetStickDeadZone(int port, float title_left, float title_right,
                           float wanted);
int  xbox_InputGyroRate(unsigned long port, float *yaw, float *pitch);
void xbox_InputGyroCalibrate(unsigned long port);
void xbox_InputGyroSetAuto(int on);

int controls_gyro_rate(int port, float *yaw, float *pitch)
{
    return xbox_InputGyroRate((unsigned long)port, yaw, pitch);
}

void controls_gyro_calibrate(int port)
{
    xbox_InputGyroCalibrate((unsigned long)port);
}

/* "1"-"4": Windows' (XInput) controller 1-4 = host pad 0-3; "ps1"-"ps4":
 * PlayStation / Switch controller 1-4 (SDL) = host pad 4-7. */
static int port_source(const char *v)
{
    if (!strcmp(v, "any"))
        return PORT_ANY;
    if (v[0] >= '1' && v[0] <= '4' && !v[1])
        return v[0] - '1';
    if (!strncmp(v, "ps", 2) && v[2] >= '1' && v[2] <= '4' && !v[3])
        return CONTROLS_XINPUT_PADS + v[2] - '1';
    return PORT_NONE;
}

int controls_port_source(int port)
{
    char key[32];
    snprintf(key, sizeof(key), "controller_slot%d", port + 1);
    return port_source(settings_get(key));
}

/* As last applied: read every frame by mouse look (game thread). */
static volatile LONG s_kb_port;

int controls_keyboard_port(void)
{
    return (int)s_kb_port;
}

int  xbox_InputHostPadName(unsigned long slot, char *out, int size);

int controls_pad_connected(int pad) { return xbox_InputHostPadConnected(pad); }
int controls_pad_name(int pad, char *out, int size)
{
    return xbox_InputHostPadName(pad, out, size);
}
int controls_pad_of_port(int port) { return xbox_InputHostPadOfPort(port); }

/* A controller (or "any") is one port's at a time: a second one wanting it
 * (a hand-edited file) gets none. */
static void apply_ports(void)
{
    unsigned used = 0;
    int p;
    for (p = 0; p < 4; p++) {
        int src = controls_port_source(p);
        unsigned bit = src == PORT_NONE ? 0u : 1u << (src + 2);
        if (used & bit)
            src = PORT_NONE;
        used |= bit;
        if (!xbox_InputAssignActive() || xbox_InputAssigned(p) != src)
            xbox_InputAssign(p, src);
    }
    p = atoi(settings_get("keyboard_slot")) - 1;
    InterlockedExchange(&s_kb_port, p >= 0 && p < 4 ? p : 0);
    xbox_InputSetKeyboardPort((int)s_kb_port);
    controls_apply_pad_feel(-1, -1.0f, -1.0f);
}

/* To-do 29 (per slot, user 2026-11-07): each port's rumble (on and
 * strength) and stick dead zone to the toolkit (xinput_device.c). The
 * game's own dead zone is 15 of 110 on the left stick, 15 of 99 on the
 * right (0x003D2630, conker-notes "Input"); at 14 % (the default) nothing
 * is reshaped. port -1 = all four from settings.ini; else one port, with a
 * slider's live value (>= 0) in place of the saved one. */
void controls_apply_pad_feel(int port, float strength, float dead_zone)
{
    char key[32];
    int p;
    for (p = 0; p < 4; p++) {
        float s, d;
        if (port >= 0 && p != port)
            continue;
        snprintf(key, sizeof(key), "rumble_strength%d", p + 1);
        s = strength >= 0.0f ? strength : (float)atof(settings_get(key));
        snprintf(key, sizeof(key), "rumble%d", p + 1);
        if (!strcmp(settings_get(key), "0"))
            s = 0.0f;
        xbox_SetRumbleStrength(p, (int)(s + 0.5f));
        snprintf(key, sizeof(key), "stick_dead_zone%d", p + 1);
        d = dead_zone >= 0.0f ? dead_zone : (float)atof(settings_get(key));
        xbox_SetStickDeadZone(p, 15.0f / 110.0f, 15.0f / 99.0f,
                              d == 14.0f ? -1.0f : d / 100.0f);
    }
}

/* Gyro aiming per slot (to-do 35, F3 > Slots > Options; user 2026-11-15):
 * copied when applied, read every frame by gyro_look (game thread).
 * [port][0 single player, 1 multiplayer]; speeds as factors (1 = the aim
 * turns as far as the controller does). */
static volatile LONG  s_gyro_on[4][2];
static volatile float s_gyro_speed[4][2];
static volatile LONG  s_gyro_inv[4];
/* The right stick's camera speed per slot, single player (slot 1's
 * story_camera_speed1) and multiplayer (mp_camera_speedN) apart (user
 * 2026-11-15), factors; copied with the gyro's. */
static volatile float s_cam_speed[4][2] = {
    { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f },
};

float controls_camera_speed(int port, int mp)
{
    return port >= 0 && port < 4 ? s_cam_speed[port][mp ? 1 : 0] : 1.0f;
}

int controls_gyro(int port, int mp, float *sx, float *sy, int *invert)
{
    if (port < 0 || port > 3)
        return 0;
    *sx = s_gyro_speed[port][0];
    *sy = s_gyro_speed[port][1];
    *invert = (int)s_gyro_inv[port];
    return (int)s_gyro_on[port][mp ? 1 : 0];
}

void controls_apply_gyro(void)
{
    static const char *const keys[] = {
        "gyro_story", "gyro_mp", "gyro_speed_x", "gyro_speed_y",
        "gyro_invert_y",
    };
    char key[32];
    float v[5];
    int p, i;
    for (p = 0; p < 4; p++) {
        for (i = 0; i < 5; i++) {
            snprintf(key, sizeof(key), "%s%d", keys[i], p + 1);
            v[i] = (float)atof(settings_get(key));
        }
        InterlockedExchange(&s_gyro_on[p][0], v[0] != 0.0f);
        InterlockedExchange(&s_gyro_on[p][1], v[1] != 0.0f);
        s_gyro_speed[p][0] = v[2] / 100.0f;
        s_gyro_speed[p][1] = v[3] / 100.0f;
        InterlockedExchange(&s_gyro_inv[p], v[4] != 0.0f);
        /* Single player is slot 1's alone: story_camera_speed1 only. */
        v[0] = p ? 100.0f : (float)atof(settings_get("story_camera_speed1"));
        s_cam_speed[p][0] = v[0] > 0.0f ? v[0] / 100.0f : 1.0f;
        snprintf(key, sizeof(key), "mp_camera_speed%d", p + 1);
        v[0] = (float)atof(settings_get(key));
        s_cam_speed[p][1] = v[0] > 0.0f ? v[0] / 100.0f : 1.0f;
    }
    xbox_InputGyroSetAuto(strcmp(settings_get("gyro_auto_calibrate"), "0") != 0);
}

void controls_apply(void)
{
    chords_apply();
    pad_binds_apply();
    apply_ports();
    controls_apply_gyro();
    apply_set(CONTROLS_STORY);
    apply_set(CONTROLS_MP);
    apply_set(CONTROLS_MENU);
    apply_set(CONTROLS_PAUSE);
    apply_set(CONTROLS_MENU_GAME);
    apply_set(CONTROLS_CUTSCENE);
    apply_set(CONTROLS_SWIM_SURFACE);
    apply_set(CONTROLS_SWIM_UNDER);
    apply_set(CONTROLS_BEE_GUN);
    apply_set(CONTROLS_MULE_GUN);
    apply_set(CONTROLS_VAULT_SUIT);
    apply_set(CONTROLS_VAULT_TAIL);
    apply_set(CONTROLS_BAT_GROUND);
    apply_set(CONTROLS_BAT_AIR);
    apply_photo();
    list_combos(CONTROLS_STORY);
    list_combos(CONTROLS_MP);
    list_combos(CONTROLS_PHOTO);
    xbox_KbmSetEnabled(strcmp(settings_get("keyboard_mouse"), "0") != 0);
    if (s_mode < 0)
        controls_set_mode(CONTROLS_STORY);
}

/* A skip key held as a cutscene starts (Mouse 1 shooting, E for the
 * context action that started it, Space jumping) is the game's left
 * trigger at once, a skip press; held as it ends (holding to skip), it is
 * the same key's action in play at once (user 2026-11-04). So keys held
 * as the cutscene set comes or goes are left out of the new set until let
 * go (bit r = row r of s_cutscene in s_cut_latched). A set built with keys
 * left out is built again when next used (s_latch_built). */
static LONG s_latch_built;            /* bit = a set built with a latch */

static void rebuild_latched(int set)
{
    if (set < 0 || set >= CONTROLS_PHOTO)
        return;
    apply_set(set);
    if (s_cut_latched)
        s_latch_built |= 1 << set;
    else
        s_latch_built &= ~(1 << set);
    xbox_KbmSelectProfile(set);
}

void controls_set_mode(int set)
{
    int r, n = (int)(sizeof(s_cutscene) / sizeof(s_cutscene[0])), held = 0;
    int old = (int)InterlockedExchange(&s_mode, set);

    if (old != set) {
        int any = set == CONTROLS_CUTSCENE || old == CONTROLS_CUTSCENE;
        for (r = 0; r < n; r++)
            if (s_cutscene[r].out[0] == XBOX_KBM_LT
                && (any || (s_cut_latched & (1 << r)))
                && xbox_KbmHeld((unsigned)s_cutscene_keys[r]))
                held |= 1 << r;
        InterlockedExchange(&s_cut_latched, held);
        if (held || (set >= 0 && set < CONTROLS_PHOTO
                     && (s_latch_built & (1 << set))))
            rebuild_latched(set);
        else
            xbox_KbmSelectProfile(set);
        return;
    }
    if (!s_cut_latched)
        return;
    for (r = 0; r < n; r++)
        if ((s_cut_latched & (1 << r))
            && xbox_KbmHeld((unsigned)s_cutscene_keys[r]))
            held |= 1 << r;
    if (held != s_cut_latched) {
        InterlockedExchange(&s_cut_latched, held);
        rebuild_latched(set);
    }
}

int controls_mode(void)
{
    return s_mode < 0 ? CONTROLS_STORY : (int)s_mode;
}
