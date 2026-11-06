/**
 * settings.ini: the player's settings, in the save folder (SPEC 5, 10.1).
 *
 * A plain INI file the player edits in Notepad or with the F2 overlay
 * (overlay.cpp), which writes the whole file back. Each option maps onto a
 * switch the runtime already reads with getenv, so loading the file is just
 * setting those variables before anything reads them -- no toolkit change,
 * and every existing run command keeps working. Which value wins, lowest
 * first:
 *
 *   built-in default < settings.ini < environment variable < command line
 *
 * A variable already set (run.ps1 -Env, or by hand) is left alone and the
 * file's value is logged as ignored. An option at the runtime's own unset
 * behaviour (Option.unset) sets nothing: render_resolution = 640x480 leaves
 * RECOMP_RENDER_RES unset, which is not the same as setting it to 640x480
 * (that makes the process DPI aware and the window open at a new size).
 * Defaults are not always that: a new file says render_resolution = auto.
 *
 * A missing file is written with every option at its default and a comment
 * each, so the file documents itself. Unknown sections, keys and values are
 * one [SETTINGS] warning each in the log, never fatal. Loading never
 * rewrites an existing file; the overlay's first change does, which also
 * brings in options added since (and drops lines it does not know).
 *
 * Only player-facing options belong here. The bring-up switches the game
 * needs (RECOMP_EXT_VMA and the rest) are built-in defaults in main.c, and
 * the debug switches stay environment-only.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "settings.h"
#include "controls.h"

typedef enum { OPT_BOOL, OPT_CHOICE, OPT_RES, OPT_PATH, OPT_RECT, OPT_NUM,
               OPT_KEY } OptKind;

typedef struct {
    const char *section;
    const char *key;
    OptKind     kind;
    const char *def;       /* value written to a new file */
    const char *unset;     /* what the runtime does with the switch unset:
                            * at this value nothing is set */
    const char *env;       /* runtime switch it maps to; NULL = read by main.c
                            * or the overlay */
    const char *choices;   /* OPT_CHOICE: space-separated allowed values */
    const char *comment;   /* written above the line in the file; NULL =
                            * none, the line follows the one before */
    float       min, max;  /* OPT_NUM: the range taken */
} Option;

/* The picture's shape follows the render resolution (a wide size is
 * widescreen), so there is no separate widescreen option any more; old
 * files' "widescreen" lines are ignored. RECOMP_WIDESCREEN stays as a debug
 * switch (16:9 at 640x480). */
static const Option s_options[] = {
    /* The release defaults the user chose (2026-10-30): full screen,
     * auto resolution, scale 100, fsr, original texture filtering, 60 fps,
     * smaa, sharpen 100. */
    /* Was fullscreen (0/1) until 2026-11-03; an old file's fullscreen = 0
     * is read as windowed (settings_load). User 2026-11-03: full screen
     * at the monitor's own resolution and refresh rate. */
    { "display", "display_mode", OPT_CHOICE, "borderless", "windowed",
      "RECOMP_DISPLAY_MODE", "windowed borderless fullscreen",
      "How the game is shown: windowed (a window), borderless (default; a\n"
      "window without a frame covering the monitor: instant Alt+Tab) or\n"
      "fullscreen (exclusive full screen: the game takes the monitor at its\n"
      "own resolution and refresh rate; Alt+Tab takes a moment). F11 or\n"
      "Alt+Enter switches between it and a window in the game. Applies at\n"
      "once." },
    { "display", "render_resolution", OPT_RES, "auto", "640x480",
      "RECOMP_RENDER_RES", NULL,
      "Size the game is drawn at; the picture takes its shape, so a wide\n"
      "size is widescreen (HUD, menus and movies are then stretched).\n"
      "auto (default) = your main monitor's shape and size, but at most\n"
      "1440 pixels tall (4K would drop below 30 fps); auto-4:3 = the same\n"
      "height at 4:3, the original shape; native = exactly your monitor's\n"
      "size; 640x480 = the original Xbox size; or any WxH such as 1920x1080." },
    { "display", "render_scale", OPT_CHOICE, "100", "100",
      "RECOMP_RENDER_SCALE", "50 67 75 100 125 150 200",
      "Draw the game bigger or smaller than the render resolution, in\n"
      "percent: 100 = the same (default). Above 100 the game is drawn\n"
      "bigger and shrunk to fit (supersampling): smoother edges and finer\n"
      "detail, but slower. Below 100 it is drawn smaller and stretched by\n"
      "the upscaler: faster, but softer. Very big sizes are capped." },
    { "display", "upscaler", OPT_CHOICE, "fsr", "bilinear",
      "RECOMP_UPSCALER", "bilinear fsr",
      "How a render scale below 100 is stretched to fit: bilinear\n"
      "(plain, soft) or fsr (default; AMD FidelityFX Super Resolution 1:\n"
      "keeps edges sharp; the sharpen setting then sets FSR's own\n"
      "sharpening)." },
    { "display", "texture_filter", OPT_CHOICE, "original", "original",
      "RECOMP_TEX_FILTER", "off original aniso2 aniso4 aniso8 aniso16",
      "How textures far away or at an angle are drawn: original (default;\n"
      "smaller copies of each texture far away, as on the Xbox: steady,\n"
      "slightly soft in the distance), off (always the full-size texture:\n"
      "sharpest, but shimmers when moving), aniso2/aniso4/aniso8/aniso16\n"
      "(original plus anisotropic filtering: also sharp at a low angle)." },
    { "display", "grass_full_detail", OPT_BOOL, "1", "1", "RECOMP_GRASS_FULL",
      NULL,
      "Pop-in Fix (grass, flowers, fur and characters): 1 = full detail\n"
      "at any distance\n"
      "(default; grass keeps waving and never thins out, nobody pops in or\n"
      "out with distance, fur stays on), 0 = the original (grass still and\n"
      "thinned out a few steps away, gone further off; characters hidden\n"
      "past their distance limit; fur fading out with distance)." },
    { "display", "popin_grass", OPT_BOOL, "1", "1", "RECOMP_POPIN_GRASS",
      NULL,
      "The Pop-in Fix's parts, each 1 = fixed, 0 = as the original; they\n"
      "apply only while grass_full_detail = 1. popin_grass: grass and\n"
      "flowers (far grass also gets the level's haze, like the ground\n"
      "under it), popin_characters: characters, popin_fur: fur (all three on\n"
      "by default), popin_particles: ambient particles (falling leaves,\n"
      "dust) all round instead of only near the camera (experimental, off\n"
      "by default)." },
    { "display", "popin_characters", OPT_BOOL, "1", "1",
      "RECOMP_POPIN_CHARACTERS", NULL, NULL },
    { "display", "popin_fur", OPT_BOOL, "1", "1", "RECOMP_POPIN_FUR", NULL,
      NULL },
    { "display", "popin_particles", OPT_BOOL, "0", "0",
      "RECOMP_POPIN_PARTICLES", NULL, NULL },
    { "display", "particle_density", OPT_NUM, "15", "15",
      "RECOMP_PARTICLE_DENSITY", NULL,
      "How many ambient particles popin_particles adds beyond the game's\n"
      "own box round the camera, 0 to 100 percent (default 15; fewer the\n"
      "further away; 0 = only the game's own).", 0.0f, 100.0f },
    { "display", "fps_counter", OPT_CHOICE, "0", "0", NULL, "0 1 2",
      "Frame rate in the top left corner: 0 = off (default), 1 = frames\n"
      "per second, 2 = also how long a frame takes (average and longest).\n"
      "F1 steps through them in the game." },
    /* 144 and up (and uncapped) work through RECOMP_FRAME_RATE but are
     * left out until the game's whole-tick code is fixed for them (user
     * 2026-11-03; conker-notes "drunk walk loses its turning"). */
    { "display", "frame_rate", OPT_CHOICE, "60", "30", "RECOMP_FRAME_RATE",
      "30 60 90 120",
      "Frames per second the game runs at: 30 (the original), 60 (default;\n"
      "smoother), 90 or 120 (for fast monitors; best when the PC holds the\n"
      "rate steadily: some animations step unevenly when it dips). With\n"
      "VSync on no rate goes above the monitor's refresh, with vrr 3 under\n"
      "it. The game keeps its speed at every rate. Applies at once." },
    /* Was dip_fix (default 0) for a day; renamed so files that F2 had
     * already written with dip_fix = 0 get the new default. */
    { "display", "slowdown_fix", OPT_BOOL, "1", "1", "RECOMP_DIP_FIX", NULL,
      "At 30 fps, keep the game at its normal speed when the frame rate\n"
      "dips: 1 = on (default), 0 = off (the original, which runs fast in\n"
      "a dip). 60 fps always does this." },
    /* User 2026-11-04 (to-do 76): named by the user, on by default. */
    { "display", "main_menu_speed", OPT_BOOL, "1", "1", "RECOMP_MENU_SPEED",
      NULL,
      "Above 60 fps, the main menu turns from board to board (left / right)\n"
      "at its original speed: 1 = on (default), 0 = off (it turns faster the\n"
      "higher the frame rate, twice as fast at 120). The chapter list and\n"
      "the other pages are not changed. Applies at once." },
    /* To-do 78 (2026-11-04): on by default; F2 only with --debug (user). */
    { "display", "physics_30hz", OPT_BOOL, "1", "1", "RECOMP_PHYS30", NULL,
      "Above 30 fps, tails, ragdolls and cloth move at their original\n"
      "speed: 1 = on (default), 0 = off (they move faster the higher the\n"
      "frame rate, 4x as fast at 120). Applies at once." },
    /* User 2026-11-03: three modes, on by default (SPEC 10.1 item 3).
     * Always set: unset is the toolkit's old present (no wait, no tearing). */
    { "display", "vsync", OPT_CHOICE, "on", "", "RECOMP_VSYNC", "on off vrr",
      "VSync: on (default; no tearing, every frame waits for the monitor's\n"
      "refresh, and on a 60, 120, 180 or 240 Hz monitor the game's clock\n"
      "follows it, so no frame is shown twice or skipped), off (each frame\n"
      "shows the moment it is ready: the lowest delay, but the picture can\n"
      "tear) or vrr (G-Sync / FreeSync: for a monitor with it turned on, in\n"
      "full screen; the monitor waits for the game's frames). Applies at\n"
      "once." },
    /* To-do 16 (user 2026-11-04): both on by default. */
    { "cutscenes", "skippable_cutscenes", OPT_BOOL, "1", "1",
      "RECOMP_CUTSCENE_SKIP", NULL,
      "Every story cutscene can be skipped, also the ones the game does not\n"
      "let you skip (not seen yet on this save, or never): 1 = on (default),\n"
      "0 = off (the original). Scenes the game lets you skip still skip on a\n"
      "press of the skip button (left trigger; Space, E or Mouse 1). A\n"
      "scene made of parts skips one part at a time. Applies at once." },
    { "cutscenes", "hold_to_skip", OPT_BOOL, "1", "1", "RECOMP_SKIP_HOLD",
      NULL,
      "With skippable_cutscenes on: the scenes the game does not let you\n"
      "skip need the skip button held for 3 seconds (a ring fills in the\n"
      "corner): 1 = on (default), 0 = off (a press skips them too). Applies\n"
      "at once." },
    /* User 2026-11-04: the plain ring; the tail look a --debug choice. */
    { "cutscenes", "skip_ring_tail", OPT_BOOL, "0", "0", NULL, NULL,
      "Debug (--debug only): the hold-to-skip ring drawn as Conker's tail,\n"
      "curled up with fur: 1 = on, 0 = off (default; the plain ring)." },
    { "picture", "gamma", OPT_NUM, "1", "1", "RECOMP_GAMMA", NULL,
      "Picture brightness curve, 0.5 to 1.5: 1 = original (default); higher\n"
      "lifts the dark and middle tones, lower darkens them.", 0.5f, 1.5f },
    { "picture", "contrast", OPT_NUM, "1", "1", "RECOMP_CONTRAST", NULL,
      "Picture contrast, 0.5 to 1.5: 1 = original (default).", 0.5f, 1.5f },
    { "picture", "saturation", OPT_NUM, "1", "1", "RECOMP_SATURATION", NULL,
      "Colour strength, 0 to 2: 1 = original (default), 0 = black and\n"
      "white, 2 = twice as colourful.", 0.0f, 2.0f },
    { "picture", "antialiasing", OPT_CHOICE, "smaa", "off", "RECOMP_AA",
      "off fxaa smaa",
      "Smoothing of jagged edges: off (the original), fxaa (NVIDIA FXAA:\n"
      "quick, slightly softens the whole picture) or smaa (default; SMAA:\n"
      "smooths edges more cleanly and keeps text and textures sharper)." },
    { "picture", "sharpen", OPT_NUM, "100", "0", "RECOMP_SHARPEN", NULL,
      "Sharpening (AMD FidelityFX CAS), 0 to 100 percent: 0 = off,\n"
      "100 = strongest (default).", 0.0f, 100.0f },
    { "picture", "crt", OPT_CHOICE, "off", "off", "RECOMP_CRT",
      "off scanlines crt",
      "Old TV look: off (default), scanlines (dark lines between the\n"
      "game's 480 lines) or crt (scanlines plus a slightly curved screen\n"
      "and a soft glow). Scanlines need a picture at least 960 pixels\n"
      "tall to show fully; they fade out below that." },
    { "picture", "crt_strength", OPT_NUM, "50", "50", "RECOMP_CRT_STRENGTH",
      NULL,
      "How strong the crt setting is, 0 to 100 percent (default 50).",
      0.0f, 100.0f },
    { "picture", "glow", OPT_NUM, "100", "100", "RECOMP_GLOW", NULL,
      "The game's own glow (halos round lamps, glowing text, the soft\n"
      "haze in some levels), 0 to 100 percent: 100 = original (default),\n"
      "0 = off.", 0.0f, 100.0f },
    { "picture", "screen_haze", OPT_BOOL, "1", "1", "RECOMP_SCREEN_HAZE", NULL,
      "The game's screen haze in some levels (a mist or colour wash over\n"
      "the whole view, e.g. the green gas in Chemical Warfare): 1 = on as\n"
      "the original (default), 0 = off (a clear view)." },
    { "picture", "vignette", OPT_BOOL, "1", "1", "RECOMP_VIGNETTE", NULL,
      "The dark corners round the picture in some levels (e.g. Chemical\n"
      "Warfare): 1 = on as the original (default), 0 = off." },
    { "picture", "screen_drops", OPT_BOOL, "1", "1", "RECOMP_SCREEN_DROPS",
      NULL,
      "Water and blood drops on the screen (rain, splashes, hits) and\n"
      "the streaks they leave: 1 = on as the original (default), 0 = off." },
    { "picture", "underwater_overlay", OPT_BOOL, "1", "1",
      "RECOMP_UNDERWATER_OVERLAY", NULL,
      "The shimmering haze round Conker under water (it shows as a band\n"
      "at eye level): 1 = on as the original (default), 0 = off." },
    { "picture", "poo_fix", OPT_BOOL, "1", "1", "RECOMP_POO_FIX", NULL,
      "Under the poo after the Bull fight, that haze drawn brown to match\n"
      "the poo instead of the original's blue band: 1 = on (default),\n"
      "0 = the original. Normal water is never changed." },
    { "picture", "cutscene_blur", OPT_CHOICE, "updated", "updated",
      "RECOMP_DOF", "off original updated cinematic",
      "Depth of field in cutscenes (the background or foreground out of\n"
      "focus): updated (default; the game's own, drawn smooth at any\n"
      "resolution), cinematic (stronger, like a film camera, with soft\n"
      "round highlights), original (the game's own as it was: blocky at\n"
      "high resolutions) or off." },
    { "audio", "mix", OPT_CHOICE, "xemu", "xemu", "RECOMP_APU_MIXDOWN",
      "xemu speakers evenodd front",
      "How sound is mixed to stereo: xemu (default; sounds like the xemu\n"
      "emulator) or speakers (wider stereo placement). evenodd and front\n"
      "are for testing (the F2 menu shows them with --debug)." },
    { "audio", "hrtf", OPT_BOOL, "1", "1", "RECOMP_APU_HRTF", NULL,
      "3D sound filtering (HRTF) for sounds placed around Conker:\n"
      "1 = on (default), 0 = off." },
    { "audio", "pause_silence", OPT_BOOL, "1", "1", NULL, NULL,
      "While the game is paused with F4, the sound stops too and carries\n"
      "on where it stopped: 1 = on (default), 0 = the sound keeps playing." },
    { "language", "language", OPT_CHOICE, "english", "english",
      "RECOMP_LANGUAGE", "english french german italian spanish japanese",
      "Language of the game's text, menus and subtitles: english (default),\n"
      "french, german, italian, spanish or japanese. Voices are English in\n"
      "every language. Applies when the game is started again. Saves made in\n"
      "one language load in all the others." },
    { "uncensored", "uncensored_audio", OPT_BOOL, "1", "1",
      "RECOMP_UNCENSORED_AUDIO", NULL,
      "With the decensor patch installed (paths.decensor_dir): the\n"
      "unbleeped cutscene voices and songs, 1 = on (default), 0 = the\n"
      "original audio. Without the patch this does nothing." },
    { "uncensored", "uncensored_text", OPT_BOOL, "1", "1",
      "RECOMP_UNCENSORED_TEXT", NULL,
      "The same for the subtitles and other text (English, French,\n"
      "Italian and Spanish): 1 = on (default), 0 = the original text." },
    { "menu", "menu_window", OPT_RECT, "", "", NULL, NULL,
      "Where the F2 menu sits and its size, as parts of the picture:\n"
      "left, top, width, height (0.02,0.02,0.3,0.5 = near the top left\n"
      "corner, 30 % of the width, half the height). Empty = the default\n"
      "(small, top left). The menu saves it when you move or resize it." },
    { "photo", "photo_speed", OPT_NUM, "100", "100", NULL, NULL,
      "Photo mode (F4): how fast the camera flies, 25 to 400 percent\n"
      "(default 100). The photo panel (Tab or Start in photo mode) changes\n"
      "this and the three below.", 25.0f, 400.0f },
    { "photo", "photo_blur", OPT_NUM, "50", "50", NULL, NULL,
      "Photo mode's depth of field: how strong the blur is, 0 to 100\n"
      "percent (default 50). Depth of field itself starts off every time.",
      0.0f, 100.0f },
    { "photo", "photo_folder", OPT_PATH, "", "", NULL, NULL,
      "The folder photos go to. Empty = the default: the Photos folder next\n"
      "to this file. The photo panel's \"Change Photo Directory\" sets it." },
    { "photo", "photo_window", OPT_RECT, "", "", NULL, NULL,
      "Where the photo panel sits and its size, like menu_window." },
    { "controls", "keyboard_mouse", OPT_BOOL, "1", "1", NULL, NULL,
      "Keyboard and mouse as a player's controller (beside a real one;\n"
      "keyboard_slot says which player):\n"
      "1 = on (default), 0 = off. F3 in the game changes the keys; the\n"
      "mouse looks around. In a window, click the picture to use the mouse\n"
      "(Alt+Tab, F2 or F3 let it go again)." },
    { "controls", "keyboard_slot", OPT_CHOICE, "1", "1", NULL, "1 2 3 4",
      "Which player (controller port 1-4) the keyboard and mouse are, when\n"
      "keyboard_mouse = 1: 1 (default) to 4. F3 > Slots." },
    { "controls", "controller_slot1", OPT_CHOICE, "any", "any", NULL,
      "none any 1 2 3 4 ps1 ps2 ps3 ps4",
      "Which controller is each player (controller port 1-4): any = the\n"
      "controller used last that no other port has (player 1's default),\n"
      "1-4 = Windows' controller 1-4, ps1-ps4 = PlayStation controller 1-4\n"
      "(DualShock 4 / DualSense, read directly), none = no controller (the\n"
      "default for ports 2-4). A port with no controller and no keyboard is\n"
      "empty, as if nothing were plugged in there. F3 > Slots." },
    { "controls", "controller_slot2", OPT_CHOICE, "none", "none", NULL,
      "none any 1 2 3 4 ps1 ps2 ps3 ps4", NULL },
    { "controls", "controller_slot3", OPT_CHOICE, "none", "none", NULL,
      "none any 1 2 3 4 ps1 ps2 ps3 ps4", NULL },
    { "controls", "controller_slot4", OPT_CHOICE, "none", "none", NULL,
      "none any 1 2 3 4 ps1 ps2 ps3 ps4", NULL },
    { "controls", "controls_window", OPT_RECT, "", "", NULL, NULL,
      "Where the F3 controls menu sits and its size, like menu_window." },
    { "controls_story", "story_mouse_x", OPT_NUM, "100", "100", NULL, NULL,
      "Keys and mouse in single player (story); F3 changes them.\n"
      "Mouse sensitivity left and right, 10 to 400 percent (default 100).",
      10.0f, 400.0f },
    { "controls_story", "story_mouse_y", OPT_NUM, "100", "100", NULL, NULL,
      "Mouse sensitivity up and down, 10 to 400 percent (default 100).",
      10.0f, 400.0f },
    { "controls_story", "story_invert_y", OPT_BOOL, "0", "0", NULL, NULL,
      "1 = moving the mouse forward looks down (default 0)." },
    { "controls_story", "story_scheme", OPT_CHOICE, "classic", "classic", NULL,
      "classic modern",
      "classic = the controller's layout (the weapon key takes the weapon\n"
      "out and puts it away, and is the context action); modern = hold\n"
      "story_aim to take the weapon out, Attack fires only while aiming,\n"
      "story_context is the context action only." },
    { "controls_story", "story_slingshot_aim", OPT_CHOICE, "hold", "hold", NULL,
      "hold toggle",
      "Modern only: how a slingshot pad's slingshot aims. hold (default;\n"
      "aimed while story_aim is held, as the other weapons) or toggle\n"
      "(story_context turns it on and off, as Classic's weapon key)." },
    { "controls_story", "story_move_forward", OPT_KEY, "w", "w", NULL, NULL,
      "One key per action; F3 says what each does (menus have fixed\n"
      "keys: WASD/arrows, Space/E/Enter, Q/Backspace, Esc).\n"
      "Key names: a-z, 0-9, num0-num9, space, enter, backspace, tab, esc,\n"
      "shift, ctrl, alt, capslock, up, down, left, right, insert, delete,\n"
      "home, end, pageup, pagedown, f4-f8, f10, f12, semicolon, equals,\n"
      "comma, minus, period, slash, backquote, lbracket, backslash,\n"
      "rbracket, quote, nummul, numadd, numsub, numdot, numdiv, mouse1\n"
      "(left), mouse2 (right), mouse3 (middle), mouse4, mouse5, wheelup,\n"
      "wheeldown, none." },
    { "controls_story", "story_move_back", OPT_KEY, "s", "s", NULL, NULL, NULL },
    { "controls_story", "story_move_left", OPT_KEY, "a", "a", NULL, NULL, NULL },
    { "controls_story", "story_move_right", OPT_KEY, "d", "d", NULL, NULL, NULL },
    { "controls_story", "story_walk", OPT_KEY, "shift", "shift", NULL, NULL, NULL },
    { "controls_story", "story_jump", OPT_KEY, "space", "space", NULL, NULL, NULL },
    { "controls_story", "story_crouch", OPT_KEY, "ctrl", "ctrl", NULL, NULL, NULL },
    { "controls_story", "story_weapon", OPT_KEY, "q", "q", NULL, NULL, NULL },
    { "controls_story", "story_context", OPT_KEY, "e", "e", NULL, NULL, NULL },
    { "controls_story", "story_aim", OPT_KEY, "mouse2", "mouse2", NULL, NULL, NULL },
    { "controls_story", "story_attack", OPT_KEY, "mouse1", "mouse1", NULL, NULL, NULL },
    { "controls_story", "story_reload", OPT_KEY, "r", "r", NULL, NULL, NULL },
    { "controls_story", "story_head_cam", OPT_KEY, "c", "c", NULL, NULL, NULL },
    { "controls_story", "story_zoom", OPT_KEY, "v", "v", NULL, NULL, NULL },
    { "controls_story", "story_pause", OPT_KEY, "esc", "esc", NULL, NULL, NULL },
    { "controls_story", "story_back_button", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_story", "story_dpad_up", OPT_KEY, "up", "up", NULL, NULL, NULL },
    { "controls_story", "story_dpad_down", OPT_KEY, "down", "down", NULL, NULL, NULL },
    { "controls_story", "story_dpad_left", OPT_KEY, "left", "left", NULL, NULL, NULL },
    { "controls_story", "story_dpad_right", OPT_KEY, "right", "right", NULL, NULL, NULL },
    { "controls_story", "story_look_up", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_story", "story_look_down", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_story", "story_look_left", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_story", "story_look_right", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_story", "story_dive", OPT_KEY, "q", "q", NULL, NULL,
      "Swimming: dive from the surface; swim up and down underwater. They take\n"
      "over in the water: underwater only Swim up and down press A and B; at\n"
      "the surface Dive presses B instead of the weapon key." },
    { "controls_story", "story_swim_up", OPT_KEY, "space", "space", NULL, NULL, NULL },
    { "controls_story", "story_swim_down", OPT_KEY, "ctrl", "ctrl", NULL, NULL, NULL },
    { "controls_story", "story_bat_launch", OPT_KEY, "space", "space", NULL, NULL,
      "The bat (Count Batula): launch from the ground; fly up and down, poop and\n"
      "aim in the air. They take over as the bat: in the air only these press\n"
      "A, B and the triggers. Aim: Classic toggles, Modern aims while held." },
    { "controls_story", "story_bat_up", OPT_KEY, "space", "space", NULL, NULL, NULL },
    { "controls_story", "story_bat_down", OPT_KEY, "ctrl", "ctrl", NULL, NULL, NULL },
    { "controls_story", "story_bat_poop", OPT_KEY, "mouse1", "mouse1", NULL, NULL, NULL },
    { "controls_story", "story_bat_aim", OPT_KEY, "mouse2", "mouse2", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_mouse_x", OPT_NUM, "100", "100", NULL, NULL,
      "Keys and mouse in multiplayer (the game's Emily layout); F3 changes them.\n"
      "Mouse sensitivity left and right, 10 to 400 percent (default 100).",
      10.0f, 400.0f },
    { "controls_multiplayer", "mp_mouse_y", OPT_NUM, "100", "100", NULL, NULL,
      "Mouse sensitivity up and down, 10 to 400 percent (default 100).",
      10.0f, 400.0f },
    { "controls_multiplayer", "mp_invert_y", OPT_BOOL, "0", "0", NULL, NULL,
      "1 = moving the mouse forward looks down (default 0)." },
    { "controls_multiplayer", "mp_move_forward", OPT_KEY, "w", "w", NULL, NULL,
      "One key per action; F3 says what each does (menus have fixed\n"
      "keys: WASD/arrows, Space/E/Enter, Q/Backspace, Esc).\n"
      "Key names: a-z, 0-9, num0-num9, space, enter, backspace, tab, esc,\n"
      "shift, ctrl, alt, capslock, up, down, left, right, insert, delete,\n"
      "home, end, pageup, pagedown, f4-f8, f10, f12, semicolon, equals,\n"
      "comma, minus, period, slash, backquote, lbracket, backslash,\n"
      "rbracket, quote, nummul, numadd, numsub, numdot, numdiv, mouse1\n"
      "(left), mouse2 (right), mouse3 (middle), mouse4, mouse5, wheelup,\n"
      "wheeldown, none." },
    { "controls_multiplayer", "mp_move_back", OPT_KEY, "s", "s", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_move_left", OPT_KEY, "a", "a", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_move_right", OPT_KEY, "d", "d", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_walk", OPT_KEY, "shift", "shift", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_jump", OPT_KEY, "space", "space", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_fire", OPT_KEY, "mouse1", "mouse1", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_zoom", OPT_KEY, "mouse2", "mouse2", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_zoom_mode", OPT_CHOICE, "toggle", "toggle", NULL,
      "toggle hold",
      "How the zoom key zooms on foot: toggle (default; press to zoom in,\n"
      "press again to zoom out, as the controller) or hold (zoomed in while\n"
      "the key is held). In a vehicle it is always as the controller." },
    { "controls_multiplayer", "mp_reload", OPT_KEY, "r", "r", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_interact", OPT_KEY, "e", "e", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_weapon_mode", OPT_KEY, "x", "x", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_ability", OPT_KEY, "f", "f", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_weapon", OPT_KEY, "q", "q", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_crouch", OPT_KEY, "ctrl", "ctrl", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_vote", OPT_KEY, "v", "v", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_radio", OPT_KEY, "t", "t", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_pause", OPT_KEY, "esc", "esc", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_back_button", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_dpad_up", OPT_KEY, "up", "up", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_dpad_down", OPT_KEY, "down", "down", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_dpad_left", OPT_KEY, "left", "left", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_dpad_right", OPT_KEY, "right", "right", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_look_up", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_look_down", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_look_left", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_multiplayer", "mp_look_right", OPT_KEY, "none", "none", NULL, NULL, NULL },
    { "controls_photo", "photo_toggle", OPT_KEY, "f4", "f4", NULL, NULL,
      "Photo mode (F3 > Photo mode): photo_toggle and pause_key work\n"
      "everywhere (pause_key holds the picture, as F4 did before); the rest\n"
      "only in photo mode. Key names as for single player; the controller's\n"
      "photo buttons are fixed (Back + RB turns photo mode on / off)." },
    { "controls_photo", "pause_key", OPT_KEY, "p", "p", NULL, NULL, NULL },
    { "controls_photo", "photo_forward", OPT_KEY, "w", "w", NULL, NULL, NULL },
    { "controls_photo", "photo_back", OPT_KEY, "s", "s", NULL, NULL, NULL },
    { "controls_photo", "photo_left", OPT_KEY, "a", "a", NULL, NULL, NULL },
    { "controls_photo", "photo_right", OPT_KEY, "d", "d", NULL, NULL, NULL },
    { "controls_photo", "photo_up", OPT_KEY, "space", "space", NULL, NULL, NULL },
    { "controls_photo", "photo_down", OPT_KEY, "ctrl", "ctrl", NULL, NULL, NULL },
    { "controls_photo", "photo_roll_left", OPT_KEY, "q", "q", NULL, NULL, NULL },
    { "controls_photo", "photo_roll_right", OPT_KEY, "e", "e", NULL, NULL, NULL },
    { "controls_photo", "photo_slow", OPT_KEY, "alt", "alt", NULL, NULL, NULL },
    { "controls_photo", "photo_fast", OPT_KEY, "shift", "shift", NULL, NULL, NULL },
    { "controls_photo", "photo_zoom_in", OPT_KEY, "wheelup", "wheelup", NULL, NULL, NULL },
    { "controls_photo", "photo_zoom_out", OPT_KEY, "wheeldown", "wheeldown", NULL, NULL,
      NULL },
    { "controls_photo", "photo_take", OPT_KEY, "mouse1", "mouse1", NULL, NULL, NULL },
    { "controls_photo", "photo_leave", OPT_KEY, "esc", "esc", NULL, NULL, NULL },
    { "controls_photo", "photo_reset", OPT_KEY, "r", "r", NULL, NULL, NULL },
    { "controls_photo", "photo_hud", OPT_KEY, "h", "h", NULL, NULL, NULL },
    { "controls_photo", "photo_conker", OPT_KEY, "c", "c", NULL, NULL, NULL },
    { "controls_photo", "photo_step", OPT_KEY, "f", "f", NULL, NULL, NULL },
    { "controls_photo", "photo_panel", OPT_KEY, "tab", "tab", NULL, NULL, NULL },
    { "controls_photo", "photo_cutscene_blur", OPT_KEY, "b", "b", NULL, NULL,
      NULL },
    { "paths", "game_dir", OPT_PATH, "", "", NULL, NULL,
      "Folder holding default.xbe (the extracted disc); it is only read.\n"
      "Empty = the default: a \"game\" folder next to the .exe (the launcher\n"
      "imports the disc there). --game-dir on the command line wins." },
    { "paths", "decensor_dir", OPT_PATH, "", "", NULL, NULL,
      "Folder holding the decensor patch, unpacked as it comes (it holds\n"
      "a dvddata folder); it is only read. Empty = a \"decensor\" folder\n"
      "next to the .exe. Applies when the game is started again." },
};
#define NUM_OPTIONS (sizeof(s_options) / sizeof(s_options[0]))

static char s_path[MAX_PATH];                    /* settings.ini */
static char s_dir[MAX_PATH];                     /* the save folder */
static char s_value[NUM_OPTIONS][MAX_PATH];      /* in the file now */
static char s_started[NUM_OPTIONS][MAX_PATH];    /* when the game started */
static int  s_env_set[NUM_OPTIONS];              /* env overrode the file */
static int  s_debug;

static int find(const char *key)
{
    size_t i;
    for (i = 0; i < NUM_OPTIONS; i++)
        if (!_stricmp(s_options[i].key, key))
            return (int)i;
    return -1;
}

const char *settings_game_dir(void)
{
    int i = find("game_dir");
    return i >= 0 && s_started[i][0] ? s_started[i] : NULL;
}

const char *settings_save_dir(void)
{
    return s_dir;
}

const char *settings_get(const char *key)
{
    int i = find(key);
    return i < 0 ? NULL : s_value[i];
}

const char *settings_default(const char *key)
{
    int i = find(key);
    return i < 0 ? NULL : s_options[i].def;
}

const char *settings_started(const char *key)
{
    int i = find(key);
    return i < 0 ? NULL : s_started[i];
}

const char *settings_env_override(const char *key)
{
    int i = find(key);
    return i < 0 || !s_env_set[i] ? NULL : s_options[i].env;
}

void settings_set_debug(int on)
{
    s_debug = on;
}

int settings_debug(void)
{
    return s_debug;
}

static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* Normalise a 0/1 option. Accepts the spellings people actually type. */
static const char *parse_bool(const char *v)
{
    if (!strcmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true")
            || !_stricmp(v, "yes"))
        return "1";
    if (!strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "false")
            || !_stricmp(v, "no"))
        return "0";
    return NULL;
}

/* Is v one of the space-separated words in list (case-insensitive)? */
static int in_choices(const char *v, const char *list)
{
    size_t n = strlen(v);
    while (*list) {
        const char *sp = strchr(list, ' ');
        size_t len = sp ? (size_t)(sp - list) : strlen(list);
        if (len == n && !_strnicmp(v, list, n))
            return 1;
        list += len;
        while (*list == ' ')
            list++;
    }
    return 0;
}

/* Check value for option o and write its stored form to out (MAX_PATH).
 * Returns 0, with the reason logged, when the option does not take it;
 * where says where the value came from. */
static int normalize(const Option *o, const char *value, char *out,
                     const char *where)
{
    const char *v = value;
    unsigned w, h;
    char extra;
    size_t n;

    switch (o->kind) {
    case OPT_BOOL:
        v = parse_bool(value);
        if (!v) {
            fprintf(stderr, "[SETTINGS] %s: %s = %s ignored: want 0 or 1\n",
                    where, o->key, value);
            return 0;
        }
        break;
    case OPT_CHOICE:
        if (!in_choices(value, o->choices)) {
            fprintf(stderr, "[SETTINGS] %s: %s = %s ignored: want one of "
                    "%s\n", where, o->key, value, o->choices);
            return 0;
        }
        break;
    case OPT_RES:
        /* Only the shape is checked here; the runtime checks the range and
         * logs it if it ignores the value ([FBWIN]). */
        if (_stricmp(value, "auto") && _stricmp(value, "auto-4:3")
                && _stricmp(value, "native")
                && sscanf(value, "%ux%u%c", &w, &h, &extra) != 2) {
            fprintf(stderr, "[SETTINGS] %s: %s = %s ignored: want auto, "
                    "auto-4:3, native or WxH (like 1920x1080)\n", where,
                    o->key, value);
            return 0;
        }
        break;
    case OPT_RECT: {
        /* Empty, or four fractions of the picture. */
        float x, y, w, h;
        if (!*value)
            break;
        if (sscanf(value, "%f , %f , %f , %f %c", &x, &y, &w, &h, &extra) != 4
                || x < 0.0f || y < 0.0f || x >= 1.0f || y >= 1.0f
                || w <= 0.0f || h <= 0.0f || w > 1.0f || h > 1.0f) {
            fprintf(stderr, "[SETTINGS] %s: %s = %s ignored: want empty or "
                    "left,top,width,height as parts of the picture (like "
                    "0.02,0.02,0.3,0.5)\n", where, o->key, value);
            return 0;
        }
        sprintf_s(out, MAX_PATH, "%.3f,%.3f,%.3f,%.3f", x, y, w, h);
        return 1;
    }
    case OPT_NUM: {
        /* Stored with at most two decimals and no trailing zeros ("1",
         * "0.85"), so the neutral value always reads the same. */
        char *end;
        double d = strtod(value, &end);
        size_t len;
        while (*end && isspace((unsigned char)*end))
            end++;
        if (!*value || *end || d < o->min - 1e-6 || d > o->max + 1e-6) {
            fprintf(stderr, "[SETTINGS] %s: %s = %s ignored: want a number "
                    "from %g to %g\n", where, o->key, value, o->min, o->max);
            return 0;
        }
        sprintf_s(out, MAX_PATH, "%.2f", d);
        len = strlen(out);
        while (out[len - 1] == '0')
            out[--len] = '\0';
        if (out[len - 1] == '.')
            out[--len] = '\0';
        if (!strcmp(out, "-0"))
            strcpy_s(out, MAX_PATH, "0");
        return 1;
    }
    case OPT_KEY: {
        int input = controls_parse_key(value);
        if (input < 0) {
            fprintf(stderr, "[SETTINGS] %s: %s = %s ignored: not a key name "
                    "(see the comment in [controls_story])\n", where, o->key,
                    value);
            return 0;
        }
        strcpy_s(out, MAX_PATH, controls_key_name(input));
        return 1;
    }
    case OPT_PATH:
        /* Quotes are optional around a path with spaces. */
        n = strlen(value);
        if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
            strncpy_s(out, MAX_PATH, value + 1, n - 2);
            return 1;
        }
        break;
    }
    strcpy_s(out, MAX_PATH, v);
    if (o->kind != OPT_PATH)
        _strlwr(out);
    return 1;
}

/* auto and auto-4:3: the main monitor's height, at most AUTO_MAX_H, at the
 * monitor's shape or at 4:3. The cap keeps 4K monitors at 1440p, where the
 * field still runs at full speed (~25 fps at 4K; architecture gotcha
 * "Render resolution"). The display mode, not GetSystemMetrics: the process
 * is not DPI aware yet, and on a monitor scaled to 150 % that would give a
 * 4K screen as 2560x1440. */
#define AUTO_MAX_H 1440

static int monitor_size(unsigned *mw, unsigned *mh)
{
    DEVMODEA dm;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)
            || !dm.dmPelsWidth || !dm.dmPelsHeight)
        return 0;
    *mw = dm.dmPelsWidth;
    *mh = dm.dmPelsHeight;
    return 1;
}

static int auto_resolution(int four_three, char *out, size_t n)
{
    unsigned mw, mh, w, h;

    if (!monitor_size(&mw, &mh))
        return 0;
    h = mh < AUTO_MAX_H ? mh : AUTO_MAX_H;
    w = four_three ? (h * 4 + 1) / 3
                   : (unsigned)(((unsigned long long)mw * h + mh / 2) / mh);
    sprintf_s(out, n, "%ux%u", w, h);
    return 1;
}

int settings_render_size(const char *value, unsigned *w, unsigned *h)
{
    int i = find("render_resolution");
    char res[32], extra;

    *w = *h = 0;
    if (!_stricmp(value, s_options[i].unset))
        return 1;                       /* the title's own 640x480 */
    if (!_strnicmp(value, "auto", 4)) {
        if (!auto_resolution(value[4] != '\0', res, sizeof(res)))
            return 0;
        value = res;
    } else if (!_stricmp(value, "native")) {
        /* The display mode, as for auto: right with or without DPI
         * awareness (the runtime's own native asks once it is aware). */
        return monitor_size(w, h);
    }
    if (sscanf(value, "%ux%u%c", w, h, &extra) != 2) {
        *w = *h = 0;
        return 0;
    }
    return 1;
}

/* Set the runtime switch for option i from its stored value, at start-up. */
static void apply_env(size_t i)
{
    const Option *o = &s_options[i];
    const char *v = s_value[i];
    char res[32];

    if (!o->env || !_stricmp(v, o->unset))
        return;   /* main.c reads it, or the runtime's own default */
    if (s_env_set[i]) {
        fprintf(stderr, "[SETTINGS] %s = %s ignored: %s is already set (%s)\n",
                o->key, v, o->env, getenv(o->env));
        return;
    }
    if (o->kind == OPT_RES && !_strnicmp(v, "auto", 4)) {
        if (!auto_resolution(v[4] != '\0', res, sizeof(res))) {
            fprintf(stderr, "[SETTINGS] %s = %s: no monitor size; using "
                    "640x480\n", o->key, v);
            return;
        }
        _putenv_s(o->env, res);
        fprintf(stderr, "[SETTINGS] %s = %s: %s (%s)\n", o->key, v, res,
                o->env);
        return;
    }
    _putenv_s(o->env, v);
    fprintf(stderr, "[SETTINGS] %s = %s (%s)\n", o->key, v, o->env);
}

/* Write every option with its comment and stored value. Written to a
 * temporary file first and moved over the old one, so a failed write never
 * leaves half a file. */
static int write_file(void)
{
    const char *section = "";
    char tmp[MAX_PATH];
    size_t i;
    FILE *f;

    sprintf_s(tmp, MAX_PATH, "%s.tmp", s_path);
    if (fopen_s(&f, tmp, "w") || !f) {
        fprintf(stderr, "[SETTINGS] could not write %s\n", tmp);
        return 0;
    }
    fputs("; Conker: Live & Reloaded (PC recompilation) settings.\n"
          "; Change them in the game with F2, or here with any text editor\n"
          "; (changes made here apply the next time the game starts). Lines\n"
          "; starting with ; are comments; F2 writes the whole file again,\n"
          "; without lines it does not know. Delete this file to get the\n"
          "; defaults back.\n", f);
    for (i = 0; i < NUM_OPTIONS; i++) {
        const Option *o = &s_options[i];
        const char *c = o->comment ? o->comment : "";
        if (strcmp(section, o->section)) {
            section = o->section;
            fprintf(f, "\n[%s]\n", section);
        } else if (o->comment) {
            fputc('\n', f);
        }
        /* One "; " per comment line. */
        while (*c) {
            const char *nl = strchr(c, '\n');
            size_t len = nl ? (size_t)(nl - c) : strlen(c);
            fprintf(f, "; %.*s\n", (int)len, c);
            c += len + (nl ? 1 : 0);
        }
        fprintf(f, "%s = %s\n", o->key, s_value[i]);
    }
    if (fclose(f) || !MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "[SETTINGS] could not write %s\n", s_path);
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

int settings_set(const char *key, const char *value)
{
    int i = find(key);
    char v[MAX_PATH];

    if (i < 0 || !normalize(&s_options[i], value, v, "F2 menu"))
        return 0;
    if (!strcmp(v, s_value[i]))
        return 1;
    strcpy_s(s_value[i], MAX_PATH, v);
    if (!write_file())
        return 0;
    fprintf(stderr, "[SETTINGS] %s = %s saved to %s\n", key, v, s_path);
    fflush(stderr);
    return 1;
}

void settings_load(const char *save_dir)
{
    char buf[1024], section[64] = "";
    FILE *f;
    int line = 0, old_windowed = 0, mode_seen = 0;
    size_t i;

    /* What the environment holds before the file sets anything. */
    for (i = 0; i < NUM_OPTIONS; i++) {
        strcpy_s(s_value[i], MAX_PATH, s_options[i].def);
        s_env_set[i] = s_options[i].env && getenv(s_options[i].env);
    }

    strcpy_s(s_dir, MAX_PATH, save_dir);
    sprintf_s(s_path, MAX_PATH, "%s\\settings.ini", save_dir);
    if (fopen_s(&f, s_path, "r") || !f) {
        /* The save folder itself may not exist yet on a first start. */
        CreateDirectoryA(save_dir, NULL);
        if (write_file())
            fprintf(stderr, "[SETTINGS] wrote defaults to %s\n", s_path);
        else
            fprintf(stderr, "[SETTINGS] using defaults\n");
    } else {
        fprintf(stderr, "[SETTINGS] reading %s\n", s_path);
        while (fgets(buf, sizeof(buf), f)) {
            char *s = buf, *eq, *key, where[MAX_PATH + 16];
            int o = -1;
            line++;
            /* Notepad may save a UTF-8 byte order mark. */
            if (line == 1 && !strncmp(s, "\xEF\xBB\xBF", 3))
                s += 3;
            s = trim(s);
            if (!*s || *s == ';' || *s == '#')
                continue;
            if (*s == '[') {
                char *end = strchr(s, ']');
                if (end) {
                    *end = '\0';
                    strncpy_s(section, sizeof(section), trim(s + 1), _TRUNCATE);
                    continue;
                }
            }
            eq = strchr(s, '=');
            if (!eq) {
                fprintf(stderr, "[SETTINGS] %s line %d: not \"key = value\", "
                        "ignored\n", s_path, line);
                continue;
            }
            *eq = '\0';
            key = trim(s);
            for (i = 0; i < NUM_OPTIONS; i++)
                if (!_stricmp(s_options[i].section, section)
                        && !_stricmp(s_options[i].key, key))
                    o = (int)i;
            /* The old full screen switch: 0 was a window. */
            if (o < 0 && !_stricmp(section, "display")
                    && !_stricmp(key, "fullscreen")) {
                const char *b = parse_bool(trim(eq + 1));
                old_windowed = b && !strcmp(b, "0");
                continue;
            }
            if (o >= 0 && !strcmp(s_options[o].key, "display_mode"))
                mode_seen = 1;
            if (o < 0 && !_stricmp(key, "widescreen")) {
                fprintf(stderr, "[SETTINGS] %s line %d: widescreen is no "
                        "longer an option (render_resolution sets the "
                        "shape), ignored\n", s_path, line);
                continue;
            }
            if (o < 0) {
                fprintf(stderr, "[SETTINGS] %s line %d: unknown option [%s] "
                        "%s, ignored\n", s_path, line, section, key);
                continue;
            }
            sprintf_s(where, sizeof(where), "%s line %d", s_path, line);
            normalize(&s_options[o], trim(eq + 1), s_value[o], where);
        }
        fclose(f);
        if (old_windowed && !mode_seen) {
            int o = find("display_mode");
            strcpy_s(s_value[o], MAX_PATH, "windowed");
            fprintf(stderr, "[SETTINGS] %s: fullscreen = 0 (the old switch) "
                    "read as display_mode = windowed\n", s_path);
        }
    }

    for (i = 0; i < NUM_OPTIONS; i++) {
        strcpy_s(s_started[i], MAX_PATH, s_value[i]);
        apply_env(i);
    }
}

void settings_use_defaults(const char *save_dir)
{
    size_t i;

    strcpy_s(s_dir, MAX_PATH, save_dir);
    sprintf_s(s_path, MAX_PATH, "%s\\settings.ini", save_dir);
    for (i = 0; i < NUM_OPTIONS; i++) {
        strcpy_s(s_value[i], MAX_PATH, s_options[i].def);
        strcpy_s(s_started[i], MAX_PATH, s_options[i].def);
        s_env_set[i] = 0;
    }
}

void settings_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", f);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

/* One object per option, in the file's order. comment: the lines written
 * above it in the file, when it has its own. help: the comment it falls
 * under (a key list shares the one above it), its line breaks, which only
 * fit the file's width, turned into spaces. */
void settings_dump_options(FILE *f)
{
    static const char *const kinds[] = {
        "bool", "choice", "resolution", "path", "rect", "number", "key",
    };
    const char *help = "";
    size_t i;

    fputs("  \"options\": [\n", f);
    for (i = 0; i < NUM_OPTIONS; i++) {
        const Option *o = &s_options[i];
        char text[2048], *p;

        if (o->comment)
            help = o->comment;
        strcpy_s(text, sizeof(text), help);
        for (p = text; *p; p++)
            if (*p == '\n')
                *p = ' ';
        fputs("    { \"section\": ", f);
        settings_json_string(f, o->section);
        fputs(", \"key\": ", f);
        settings_json_string(f, o->key);
        fprintf(f, ", \"kind\": \"%s\", \"default\": ", kinds[o->kind]);
        settings_json_string(f, o->def);
        if (o->choices) {
            const char *c = o->choices;
            fputs(", \"choices\": [", f);
            while (*c) {
                const char *sp = strchr(c, ' ');
                size_t len = sp ? (size_t)(sp - c) : strlen(c);
                fprintf(f, "%s\"%.*s\"", c == o->choices ? "" : ", ",
                        (int)len, c);
                c += len;
                while (*c == ' ')
                    c++;
            }
            fputc(']', f);
        }
        if (o->kind == OPT_NUM)
            fprintf(f, ", \"min\": %g, \"max\": %g", o->min, o->max);
        if (o->comment) {
            /* As written in the file, for a launcher writing a new one. */
            fputs(", \"comment\": ", f);
            settings_json_string(f, o->comment);
        }
        fputs(", \"help\": ", f);
        settings_json_string(f, text);
        fprintf(f, " }%s\n", i + 1 < NUM_OPTIONS ? "," : "");
    }
    fputs("  ]", f);
}
