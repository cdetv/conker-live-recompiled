/**
 * Keyboard and mouse controls (SPEC 10.1): the actions of single player and
 * multiplayer, each bound to one key in settings.ini, pressing the button
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

/* ---- the actions ---------------------------------------------------------- */

/* where: the only place a swimming or bat action works (SWIM_SURFACE,
 * SWIM_UNDER, BAT_GROUND, BAT_AIR); 0 for the rest. scheme: a single player
 * action of one scheme only (CLASSIC, MODERN); 0 for both. */
typedef struct {
    ControlAction a;
    unsigned short out[2];
    unsigned char where;
    unsigned char scheme;
} Action;

#define SWIM_SURFACE 1
#define SWIM_UNDER   2
#define BAT_GROUND   3
#define BAT_AIR      4
#define SWIMMING     "Swimming"
#define BAT          "Bat (Count Batula)"
#define CLASSIC      1
#define MODERN       2

#define LST(d) XBOX_KBM_LSTICK_##d
#define RST(d) XBOX_KBM_RSTICK_##d

/* Single player. Classic: the controller's buttons one to one. Modern
 * (user 2026-10-25): the context key presses B only where B does a context
 * action; holding the aim key takes the weapon out (B) and letting go puts
 * it away; Attack fires only while aiming. Those three follow the game's
 * state, so recomp_manual.c drives them (controls_modern), not the
 * profile. The controller follows Modern too (user 2026-10-30;
 * recomp_manual.c pad_filter), so a row's button is the Modern one there:
 * LT aims, LB is the head-cam, RT and LT also swim underwater. */
static const Action s_story[] = {
    { { "story_move_forward", "Move forward", "Left stick up" }, { LST(UP) } },
    { { "story_move_back", "Move back", "Left stick down" }, { LST(DOWN) } },
    { { "story_move_left", "Move left", "Left stick left" }, { LST(LEFT) } },
    { { "story_move_right", "Move right", "Left stick right" }, { LST(RIGHT) } },
    { { "story_walk", "Walk (hold)", "Left stick half way" }, { XBOX_KBM_WALK } },
    { { "story_jump", "Jump", "A" }, { XBOX_KBM_A } },
    { { "story_crouch", "Crouch", "Right trigger" }, { XBOX_KBM_RT } },
    { { "story_weapon", "Weapon out / away, context action", "B" },
      { XBOX_KBM_B }, 0, CLASSIC },
    { { "story_context", "Context action (on a B pad)", "B" },
      { XBOX_KBM_B }, 0, MODERN },
    { { "story_aim", "Aim: weapon out while held", "Left trigger" },
      { XBOX_KBM_B }, 0, MODERN },
    { { "story_attack", "Attack", "Right trigger" }, { XBOX_KBM_RT } },
    { { "story_reload", "Reload", "X" }, { XBOX_KBM_X } },
    { { "story_head_cam", "Head-cam (tap on, tap off)", "Left trigger" },
      { XBOX_KBM_LT | XBOX_KBM_TOGGLE }, 0, CLASSIC },
    { { "story_head_cam", "Head-cam (tap on, tap off)", "LB" },
      { XBOX_KBM_LT | XBOX_KBM_TOGGLE }, 0, MODERN },
    { { "story_zoom", "Zoom", "Right stick click" }, { XBOX_KBM_RTHUMB } },
    { { "story_pause", "Pause", "Start" }, { XBOX_KBM_START } },
    { { "story_back_button", "Back button", "Back" }, { XBOX_KBM_BACK } },
    { { "story_dpad_up", "D-pad up", "D-pad up" }, { XBOX_KBM_DPAD_UP } },
    { { "story_dpad_down", "D-pad down", "D-pad down" }, { XBOX_KBM_DPAD_DOWN } },
    { { "story_dpad_left", "D-pad left", "D-pad left" }, { XBOX_KBM_DPAD_LEFT } },
    { { "story_dpad_right", "D-pad right", "D-pad right" },
      { XBOX_KBM_DPAD_RIGHT } },
    { { "story_look_up", "Look up (keys)", "Right stick up" }, { RST(UP) } },
    { { "story_look_down", "Look down (keys)", "Right stick down" }, { RST(DOWN) } },
    { { "story_look_left", "Look left (keys)", "Right stick left" }, { RST(LEFT) } },
    { { "story_look_right", "Look right (keys)", "Right stick right" },
      { RST(RIGHT) } },
    /* Swimming (user 2026-10-26): Space up and Ctrl down underwater, as
     * jump and crouch on land; apply_set says how they take over. */
    { { "story_dive", "Dive Under", "B", SWIMMING },
      { XBOX_KBM_B }, SWIM_SURFACE },
    { { "story_swim_up", "Swim up", "B", SWIMMING },
      { XBOX_KBM_B }, SWIM_UNDER, CLASSIC },
    { { "story_swim_up", "Swim up", "B / Right trigger", SWIMMING },
      { XBOX_KBM_B }, SWIM_UNDER, MODERN },
    { { "story_swim_down", "Swim down", "A", SWIMMING },
      { XBOX_KBM_A }, SWIM_UNDER, CLASSIC },
    { { "story_swim_down", "Swim down", "A / Left trigger", SWIMMING },
      { XBOX_KBM_A }, SWIM_UNDER, MODERN },
    /* The bat (Count Batula, user 2026-10-28): B launches from the ground;
     * in the air B flies up, A down, the right trigger poops and the left
     * trigger toggles the aiming view. Aim is driven by recomp_manual.c
     * (controls_bat_aim): a tap of the left trigger, never a hold, since
     * the flight code stops the bat while it is held. */
    { { "story_bat_launch", "Launch (on the ground)", "B", BAT },
      { XBOX_KBM_B }, BAT_GROUND },
    { { "story_bat_up", "Fly up", "B", BAT }, { XBOX_KBM_B }, BAT_AIR },
    { { "story_bat_down", "Fly down", "A", BAT }, { XBOX_KBM_A }, BAT_AIR },
    { { "story_bat_poop", "Poop", "Right trigger", BAT },
      { XBOX_KBM_RT }, BAT_AIR },
    { { "story_bat_aim", "Aim (tap on, tap off)", "Left trigger", BAT },
      { XBOX_KBM_LT }, BAT_AIR, CLASSIC },
    { { "story_bat_aim", "Aim (hold)", "Left trigger", BAT },
      { XBOX_KBM_LT }, BAT_AIR, MODERN },
};

/* Multiplayer, the game's "Emily" layout. Interact and reload are both A
 * in the game; two keys let the player think of them apart. Holding the
 * weapon key opens the game's weapon wheel, steered with the left stick:
 * the mouse steers it while the key is held; the same for the abilities
 * wheel. */
static const Action s_mp[] = {
    { { "mp_move_forward", "Move forward", "Left stick up" }, { LST(UP) } },
    { { "mp_move_back", "Move back", "Left stick down" }, { LST(DOWN) } },
    { { "mp_move_left", "Move left", "Left stick left" }, { LST(LEFT) } },
    { { "mp_move_right", "Move right", "Left stick right" }, { LST(RIGHT) } },
    { { "mp_walk", "Walk (hold)", "Left stick half way" }, { XBOX_KBM_WALK } },
    { { "mp_jump", "Jump", "Left stick click" }, { XBOX_KBM_LTHUMB } },
    { { "mp_fire", "Fire", "Right trigger" }, { XBOX_KBM_RT } },
    { { "mp_zoom", "Zoom", "Right stick click" }, { XBOX_KBM_RTHUMB } },
    { { "mp_reload", "Reload", "A" }, { XBOX_KBM_A } },
    { { "mp_interact", "Interact (terminals)", "A" }, { XBOX_KBM_A } },
    { { "mp_weapon_mode", "Weapon mode", "Left trigger" }, { XBOX_KBM_LT } },
    { { "mp_ability", "Abilities (tap) / abilities wheel (hold, aim with "
        "the mouse)", "X" }, { XBOX_KBM_X, XBOX_KBM_MOUSE_LSTICK } },
    { { "mp_weapon", "Next weapon (tap) / weapon wheel (hold, aim with "
        "the mouse)", "Y" }, { XBOX_KBM_Y, XBOX_KBM_MOUSE_LSTICK } },
    { { "mp_crouch", "Crouch / prone", "B" }, { XBOX_KBM_B } },
    { { "mp_vote", "Vote", "Black" }, { XBOX_KBM_BLACK } },
    { { "mp_radio", "Radio mode", "White" }, { XBOX_KBM_WHITE } },
    { { "mp_pause", "Pause / options", "Start" }, { XBOX_KBM_START } },
    { { "mp_back_button", "Back button", "Back" }, { XBOX_KBM_BACK } },
    { { "mp_dpad_up", "D-pad up", "D-pad up" }, { XBOX_KBM_DPAD_UP } },
    { { "mp_dpad_down", "D-pad down", "D-pad down" }, { XBOX_KBM_DPAD_DOWN } },
    { { "mp_dpad_left", "D-pad left", "D-pad left" }, { XBOX_KBM_DPAD_LEFT } },
    { { "mp_dpad_right", "D-pad right", "D-pad right" }, { XBOX_KBM_DPAD_RIGHT } },
    { { "mp_look_up", "Look up (keys)", "Right stick up" }, { RST(UP) } },
    { { "mp_look_down", "Look down (keys)", "Right stick down" }, { RST(DOWN) } },
    { { "mp_look_left", "Look left (keys)", "Right stick left" }, { RST(LEFT) } },
    { { "mp_look_right", "Look right (keys)", "Right stick right" },
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
#define MP_MENU "Multiplayer menu"
static const Action s_menu[] = {
    { { "", "Navigate", "Left stick up" }, { LST(UP) } },
    { { "", "Navigate", "Left stick down" }, { LST(DOWN) } },
    { { "", "Navigate", "Left stick left" }, { LST(LEFT) } },
    { { "", "Navigate", "Left stick right" }, { LST(RIGHT) } },
    { { "", "Navigate", "D-pad up" }, { XBOX_KBM_DPAD_UP } },
    { { "", "Navigate", "D-pad down" }, { XBOX_KBM_DPAD_DOWN } },
    { { "", "Navigate", "D-pad left" }, { XBOX_KBM_DPAD_LEFT } },
    { { "", "Navigate", "D-pad right" }, { XBOX_KBM_DPAD_RIGHT } },
    { { "", "Choose", "A" }, { XBOX_KBM_A } },
    { { "", "Choose", "A" }, { XBOX_KBM_A } },
    { { "", "Choose", "A" }, { XBOX_KBM_A } },
    { { "", "Go back", "B" }, { XBOX_KBM_B } },
    { { "", "Go back", "B" }, { XBOX_KBM_B } },
    { { "", "Go back", "B" }, { XBOX_KBM_B } },
    { { "", "", "Y" }, { XBOX_KBM_Y } },
    { { "", "", "Y" }, { XBOX_KBM_Y } },
    { { "", "Map options", "X", MP_MENU }, { XBOX_KBM_X } },
};
static const Action s_menu_game[] = {
    { { "", "Navigate", "Left stick up" }, { LST(UP) } },
    { { "", "Navigate", "Left stick down" }, { LST(DOWN) } },
    { { "", "Navigate", "Left stick left" }, { LST(LEFT) } },
    { { "", "Navigate", "Left stick right" }, { LST(RIGHT) } },
    { { "", "Navigate", "D-pad up" }, { XBOX_KBM_DPAD_UP } },
    { { "", "Navigate", "D-pad down" }, { XBOX_KBM_DPAD_DOWN } },
    { { "", "Navigate", "D-pad left" }, { XBOX_KBM_DPAD_LEFT } },
    { { "", "Navigate", "D-pad right" }, { XBOX_KBM_DPAD_RIGHT } },
    { { "", "Choose", "A" }, { XBOX_KBM_A } },
    { { "", "Choose", "A" }, { XBOX_KBM_A } },
    { { "", "Choose", "A" }, { XBOX_KBM_A } },
    { { "", "Go back", "B" }, { XBOX_KBM_B } },
    { { "", "Close the menu", "Start" }, { XBOX_KBM_START } },
    { { "", "Go back", "B" }, { XBOX_KBM_B } },
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
    { { "", "Resume", "Start" }, { XBOX_KBM_START } },
    { { "", "Resume", "Start" }, { XBOX_KBM_START } },
    { { "", "Quit", "A" }, { XBOX_KBM_A } },
    { { "", "Confirm quit", "A" }, { XBOX_KBM_A } },
    { { "", "Confirm quit", "A" }, { XBOX_KBM_A } },
    { { "", "Confirm quit", "A" }, { XBOX_KBM_A } },
    { { "", "Open / close options", "Y" }, { XBOX_KBM_Y } },
    { { "", "Open / close options", "Y" }, { XBOX_KBM_Y } },
    { { "", "Navigate", "Left stick" }, { LST(UP) } },
    { { "", "Navigate", "Left stick" }, { LST(DOWN) } },
    { { "", "Navigate", "Left stick" }, { LST(LEFT) } },
    { { "", "Navigate", "Left stick" }, { LST(RIGHT) } },
    { { "", "Navigate", "D-pad" }, { XBOX_KBM_DPAD_UP } },
    { { "", "Navigate", "D-pad" }, { XBOX_KBM_DPAD_DOWN } },
    { { "", "Navigate", "D-pad" }, { XBOX_KBM_DPAD_LEFT } },
    { { "", "Navigate", "D-pad" }, { XBOX_KBM_DPAD_RIGHT } },
};
static const int s_pause_keys[] = {
    VK_ESCAPE, VK_BACK, 'Q', 'E', VK_SPACE, VK_RETURN, 'Y', 'O',
    'W', 'S', 'A', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
};

/* Cutscenes (user 2026-10-25): Space skips one seen before (the left
 * trigger, as on the controller), Esc pauses. Nothing else, so the
 * head-cam key cannot start a skip and stay held after it. E and Mouse 1
 * skip too (user 2026-11-04, to-do 16). */
static const Action s_cutscene[] = {
    { { "", "Skip", "Left trigger" }, { XBOX_KBM_LT } },
    { { "", "Skip", "Left trigger" }, { XBOX_KBM_LT } },
    { { "", "Skip", "Left trigger" }, { XBOX_KBM_LT } },
    { { "", "Pause", "Start" }, { XBOX_KBM_START } },
};
static const int s_cutscene_keys[] = { VK_SPACE, 'E', VK_LBUTTON, VK_ESCAPE };

/* Photo mode (user 2026-10-29, SPEC 10.1): no output, the keys are read
 * by photo_mode.c; rows in controls.h's PHOTO_ order. The button is the
 * pad's own, fixed. */
#define PHOTO_ANYWHERE "Toggle"
#define PHOTO_IN       "In photo mode"
static const Action s_photo[] = {
    { { "photo_toggle", "Photo mode on / off", "Back + RB", PHOTO_ANYWHERE } },
    { { "pause_key", "Pause (hold the picture)", "none", PHOTO_ANYWHERE } },
    { { "photo_forward", "Fly forward", "Left stick up", PHOTO_IN } },
    { { "photo_back", "Fly back", "Left stick down", PHOTO_IN } },
    { { "photo_left", "Fly left", "Left stick left", PHOTO_IN } },
    { { "photo_right", "Fly right", "Left stick right", PHOTO_IN } },
    { { "photo_up", "Fly up", "Right trigger", PHOTO_IN } },
    { { "photo_down", "Fly down", "Left trigger", PHOTO_IN } },
    { { "photo_roll_left", "Roll left (anticlockwise)", "LB", PHOTO_IN } },
    { { "photo_roll_right", "Roll right (clockwise)", "RB", PHOTO_IN } },
    { { "photo_slow", "Fly slower (hold)", "Right stick click", PHOTO_IN } },
    { { "photo_fast", "Fly faster (hold)", "Left stick click", PHOTO_IN } },
    { { "photo_zoom_in", "Zoom in", "D-pad up", PHOTO_IN } },
    { { "photo_zoom_out", "Zoom out", "D-pad down", PHOTO_IN } },
    { { "photo_take", "Take a photo", "A", PHOTO_IN } },
    { { "photo_leave", "Leave photo mode", "Back + RB", PHOTO_IN } },
    { { "photo_reset", "Camera back to the start", "Back", PHOTO_IN } },
    { { "photo_hud", "Show / hide the HUD and speech bubbles", "Y",
        PHOTO_IN } },
    { { "photo_conker", "Show / hide Conker", "X", PHOTO_IN } },
    { { "photo_step", "Frame step (the world moves one frame)", "D-pad right",
        PHOTO_IN } },
    { { "photo_panel", "Photo settings", "Start", PHOTO_IN } },
    /* B was "leave" until 2026-10-29 (user): Back + RB leaves too. */
    { { "photo_cutscene_blur", "Cutscene blur on / off", "B", PHOTO_IN } },
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

const ControlAction *controls_table_row(int set, int i, int *scheme)
{
    int n;
    const Action *t = set_table(set, &n);
    if (i < 0 || i >= n)
        return NULL;
    *scheme = t[i].scheme;
    return &t[i].a;
}

/* An action's key: settings.ini's for the two player sets, the fixed one
 * for menus, the pause screen and cutscenes. */
int controls_key(int set, int i)
{
    if (set == CONTROLS_MENU || set == CONTROLS_MENU_GAME)
        return s_menu_keys[i];
    if (set == CONTROLS_CUTSCENE)
        return s_cutscene_keys[i];
    if (set == CONTROLS_PAUSE)
        return s_pause_keys[i];
    return controls_parse_key(settings_get(controls_action(set, i)->key));
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

/* Is input bound to a swimming or bat action that works here? */
static volatile LONG s_photo_toggle_key, s_pause_key;

int controls_photo_toggle_key(void) { return (int)s_photo_toggle_key; }
int controls_pause_key(void) { return (int)s_pause_key; }

/* Photo mode's profile: nothing bound and the mouse not looking, so the
 * game gets a pad at rest; photo_mode.c reads the keys itself. */
static void apply_photo(void)
{
    xbox_KbmBinding none[1] = { { 0, 0 } };
    xbox_KbmMouse m;

    memset(&m, 0, sizeof(m));
    m.left.full = m.right.full = 1.0f;
    xbox_KbmSetProfile(CONTROLS_PHOTO, none, 0, &m);
    InterlockedExchange(&s_photo_toggle_key,
                        controls_key(CONTROLS_PHOTO, PHOTO_TOGGLE));
    InterlockedExchange(&s_pause_key, controls_key(CONTROLS_PHOTO, PHOTO_PAUSE));
}

static int place_key(const Action *t, const int *map, int n, int set,
                     int where, int input)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[map[i]].where == where && controls_key(set, i) == input)
            return 1;
    return 0;
}

static int zoom_hold_set(void)
{
    return !strcmp(settings_get("mp_zoom_mode"), "hold");
}

/* Single player's scheme as last applied, and Modern's three keys, for the
 * game thread (controls_modern); the same for multiplayer's zoom held. */
static volatile LONG s_modern, s_modern_keys[3];
static volatile LONG s_zoom_hold, s_zoom_key;
static volatile LONG s_bat_aim_key;
static volatile LONG s_sling_toggle;

int controls_sling_toggle(void)
{
    return (int)s_sling_toggle;
}

int controls_bat_aim(int *aim)
{
    *aim = (int)s_bat_aim_key;
    return (int)s_modern;
}

int controls_zoom_hold(int *zoom)
{
    *zoom = (int)s_zoom_key;
    return (int)s_zoom_hold;
}

int controls_modern(int *context, int *aim, int *attack)
{
    *context = (int)s_modern_keys[0];
    *aim = (int)s_modern_keys[1];
    *attack = (int)s_modern_keys[2];
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

static void apply_set(int set)
{
    static const char *const prefix[] = { "story", "mp" };
    xbox_KbmBinding b[128];
    xbox_KbmMouse m;
    char key[64];
    int n, r, j, k = 0, map[MAX_ROWS];
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

    for (r = 0; r < n; r++) {
        const Action *a = &t[map[r]];
        int input = controls_key(set, r);
        unsigned short out = a->out[0] & ~XBOX_KBM_TOGGLE;
        if (input <= 0)
            continue;
        if (set == s_mode && key_latched(input))
            continue;
        if (a->where != 0 && a->where != where)
            continue;
        if (!strcmp(a->a.key, "story_bat_aim"))
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
                b[k].output = XBOX_KBM_LT;
                k++;
            }
            continue;
        }
        if (input == VK_RBUTTON)
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
            if (place_key(t, map, n, set, where, input))
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
                && input != controls_parse_key(settings_get("story_attack")))
                o = XBOX_KBM_LT;
            /* Holding the alien's tail the left stick goes round to spin
             * him (user 2026-10-28): Move right spins clockwise, Move left
             * anticlockwise, as a thumb going round. */
            if (set == CONTROLS_VAULT_TAIL && o == XBOX_KBM_LSTICK_RIGHT)
                o = XBOX_KBM_LSTICK_SPIN_CW;
            else if (set == CONTROLS_VAULT_TAIL && o == XBOX_KBM_LSTICK_LEFT)
                o = XBOX_KBM_LSTICK_SPIN_CCW;
            b[k].input = (unsigned short)input;
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
        InterlockedExchange(&s_modern_keys[0],
                            controls_parse_key(settings_get("story_context")));
        InterlockedExchange(&s_modern_keys[1],
                            controls_parse_key(settings_get("story_aim")));
        InterlockedExchange(&s_modern_keys[2],
                            controls_parse_key(settings_get("story_attack")));
        InterlockedExchange(&s_bat_aim_key,
                            controls_parse_key(settings_get("story_bat_aim")));
        InterlockedExchange(&s_sling_toggle,
                            !strcmp(settings_get("story_slingshot_aim"),
                                    "toggle"));
        InterlockedExchange(&s_modern, modern);
    }
    if (set == CONTROLS_MP) {
        InterlockedExchange(&s_zoom_key,
                            controls_parse_key(settings_get("mp_zoom")));
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

/* "1"-"4": Windows' (XInput) controller 1-4 = host pad 0-3; "ps1"-"ps4":
 * PlayStation controller 1-4 = host pad 4-7. */
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
}

void controls_apply(void)
{
    apply_ports();
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
