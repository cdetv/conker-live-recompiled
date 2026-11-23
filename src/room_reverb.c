/**
 * Enhanced room reverb (to-do 232, SPEC 10.1 Audio): room sound by level
 * with the game's own reverb presets, which story mode never turns on (all
 * its levels hold preset 25, no reverb; docs/research/r3-sound.md B).
 *
 * Past the game, not through it (plan r4-0.5.0-sound-plan.md [232]): the
 * game's own path (0x00018A60) would need a guest call, is reset by every
 * level load and changes XACT's indoor / outdoor variable. Instead the
 * toolkit's reverb is handed one of the game's presets as its listener
 * (xbox_ApuSetRoomReverb; it plays in Standard and Full DSP alike), so the
 * game's own state stays as the original.
 *
 * Which preset: the level's in k_rooms (the user's picks by ear,
 * 2026-11-22, _work\s232\listen\ and listen2\), underwater (24) whenever
 * Conker is under water in any level (the controls' underwater set; held
 * while paused, in a level menu, photo mode or a cutscene). Nothing of ours
 * on the main menu, in multiplayer and in levels without a room: the game's
 * own listener plays there (preset 25 in story mode). The 12 values are read
 * from the game's preset table 0x00687940 at run time (no game data here).
 *
 * Settings: room_reverb (0 / 1, off by default) and room_reverb_strength
 * (0-100 %), F2 Sound and the launcher; RECOMP_ROOM_REVERB=0|1,
 * RECOMP_ROOM_REVERB_STRENGTH=<0-100> set before the game win (the console's
 * reverb off / auto switches it anyway, as processing does); reverb
 * <0-25|name> and RECOMP_ROOM_REVERB_PRESET=<0-25> force one preset
 * everywhere (debug). [ROOMREV] lines say each change.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>

#include "recomp_types.h"
#include "room_reverb.h"
#include "settings.h"
#include "controls.h"
#include "debug_tools.h"
#include "warp.h"

void xbox_ApuSetRoomReverb(const void *listener48, float strength);   /* toolkit apu.h */

#define PRESET_TABLE      0x00687940u   /* 26 DSI3DL2LISTENER, 0x30 bytes each */
#define PRESET_UNDERWATER 24
#define MP_MATCH          0x007819D5u   /* a multiplayer match runs */
#define FIRST_MP_MAP      0x4B

/* The I3DL2 list in the game's order (entry 1 is an "off" variant). */
static const char *const k_names[ROOM_REVERB_PRESETS] = {
    "default", "off", "generic", "padded cell", "room", "bathroom", "living room",
    "stone room", "auditorium", "concert hall", "cave", "arena", "hangar",
    "carpeted hallway", "hallway", "stone corridor", "alley", "forest", "city",
    "mountains", "quarry", "plain", "parking lot", "sewer pipe", "underwater", "none",
};

/* The user's picks (2026-11-22). Hell is left out: it is a scene that hands
 * over to BrontoCavern (its round-2 example was BrontoCavern's statue cave). */
static const struct { const char *level; int preset; } k_rooms[] = {
    /* round 1 */
    { "BrontoCavern", 10 },         /* cave */
    { "UBend", 23 },                /* sewer pipe */
    { "HauntedHouse", 14 },         /* hallway */
    { "Bank", 8 },                  /* auditorium */
    { "BarnInside", 6 },            /* living room */
    { "BoilerRoom", 12 },           /* hangar */
    { "ThroneRoom", 7 },            /* stone room */
    { "ThroneRoomExtended", 7 },
    { "DogFishNursery", 24 },       /* underwater */
    /* round 2 */
    { "BeeCavern", 15 },            /* stone corridor */
    { "TrainingCave", 7 },          /* stone room */
    { "VolcanoInside", 10 },        /* cave */
    { "RaptorNest", 10 },           /* cave */
    { "BrontoThroat", 23 },         /* sewer pipe */
    { "RockSolid", 4 },             /* room */
    { "PooBoss", 10 },              /* cave */
    { "PooCabin", 4 },              /* room */
    { "CogLand", 7 },               /* stone room */
    { "InsideSafe", 4 },            /* room */
    { "FlameTunnel", 10 },          /* cave */
    { "LiftTunnel", 14 },           /* hallway */
    { "TankTunnel", 15 },           /* stone corridor */
    { "Lab", 14 },                  /* hallway */
    { "WarBoss", 11 },              /* arena */
};

/* Asked (any thread), taken by the game frame. */
static volatile LONG s_dirty = 1;
static volatile LONG s_force = -1;              /* -1: the table */
static volatile LONG s_live_strength = -1;      /* F2's slider while it moves, else -1 */
static volatile LONG s_console_on = -1;         /* the console's reverb off / auto: 0 / 1, else -1 */
static int s_env_on = -1, s_env_strength = -1;  /* set before the game, else -1 */

/* The game frame's. */
static int s_on;
static float s_strength = 100.0f;
static int s_sent = -1;                 /* preset handed over, -1 = the game's own */
static float s_sent_strength = -1.0f;
static const char *s_why = "off";
static int s_level = -1, s_under;

static int env_int(const char *name, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e)
        return -1;
    v = atoi(e);
    return v < lo ? lo : v > hi ? hi : v;
}

void room_reverb_init(void)
{
    int force = env_int("RECOMP_ROOM_REVERB_PRESET", 0, ROOM_REVERB_PRESETS - 1);
    s_env_on = env_int("RECOMP_ROOM_REVERB", 0, 1);
    s_env_strength = env_int("RECOMP_ROOM_REVERB_STRENGTH", 0, 100);
    if (force >= 0)
        s_force = force;
    room_reverb_apply_settings();
    fprintf(stderr, "[ROOMREV] room reverb %s%s, strength %.0f %%%s%s\n",
            room_reverb_on() ? "on" : "off",
            s_env_on >= 0 ? " (RECOMP_ROOM_REVERB)" : "",
            s_env_strength >= 0 ? (float)s_env_strength
            : (float)atof(settings_get("room_reverb_strength")),
            force >= 0 ? ", preset forced: " : "", force >= 0 ? k_names[force] : "");
}

void room_reverb_apply_settings(void)
{
    InterlockedExchange(&s_live_strength, -1);
    InterlockedExchange(&s_dirty, 1);
}

void room_reverb_set_strength(float percent)
{
    InterlockedExchange(&s_live_strength, (LONG)(percent * 100.0f + 0.5f));
}

void room_reverb_force(int preset)
{
    InterlockedExchange(&s_force, preset >= 0 && preset < ROOM_REVERB_PRESETS ? preset : -1);
}

void room_reverb_switch(int on)
{
    InterlockedExchange(&s_force, -1);
    InterlockedExchange(&s_console_on, on != 0);
    InterlockedExchange(&s_dirty, 1);
}

int room_reverb_on(void)
{
    if (s_console_on >= 0)
        return (int)s_console_on;
    if (s_env_on >= 0)
        return s_env_on;
    return strcmp(settings_get("room_reverb"), "0") != 0;
}

int room_reverb_forced(void)
{
    return (int)s_force;
}

int room_reverb_now(const char **why)
{
    if (why)
        *why = s_why;
    return s_sent;
}

float room_reverb_strength(void)
{
    LONG live = s_live_strength;
    if (live >= 0)
        return (float)live / 100.0f;
    if (s_env_strength >= 0)
        return (float)s_env_strength;
    return (float)atof(settings_get("room_reverb_strength"));
}

int room_reverb_env_decides(void)
{
    return s_env_on >= 0 && s_console_on < 0;
}

const char *room_reverb_preset_name(int preset)
{
    return preset >= 0 && preset < ROOM_REVERB_PRESETS ? k_names[preset] : "the game's own";
}

/* "stone corridor", "stone_corridor", "StoneCorridor" and "15" alike. */
int room_reverb_preset_of(const char *text)
{
    char a[32], b[32];
    int n = 0;
    char *end;
    long v;

    if (!text)
        return -1;
    while (*text == ' ')
        text++;
    v = strtol(text, &end, 10);
    if (end != text && (!*end || *end == ' '))
        return v >= 0 && v < ROOM_REVERB_PRESETS ? (int)v : -1;
    for (; *text && n < (int)sizeof a - 1; text++)
        if (isalpha((unsigned char)*text))
            a[n++] = (char)tolower((unsigned char)*text);
    a[n] = 0;
    for (int p = 0; p < ROOM_REVERB_PRESETS; p++) {
        int m = 0;
        for (const char *s = k_names[p]; *s && m < (int)sizeof b - 1; s++)
            if (isalpha((unsigned char)*s))
                b[m++] = *s;
        b[m] = 0;
        if (n && !strcmp(a, b))
            return p;
    }
    return -1;
}

static int table_preset(int level)
{
    const char *name = conker_WarpLevelName(level);
    if (!name)
        return -1;
    for (size_t i = 0; i < sizeof k_rooms / sizeof k_rooms[0]; i++)
        if (!_stricmp(name, k_rooms[i].level))
            return k_rooms[i].preset;
    return -1;
}

/* Under water: the controls' underwater set (as the console's state says).
 * Swimming at the surface, on foot or flying ends it; pause, a level menu,
 * a cutscene and photo mode keep what it was. A new level starts dry. */
static int under_water(int level)
{
    int mode = controls_mode();
    if (level != s_level) {
        s_level = level;
        s_under = 0;
    }
    if (mode == CONTROLS_SWIM_UNDER)
        s_under = 1;
    else if (mode != CONTROLS_PAUSE && mode != CONTROLS_MENU_GAME
             && mode != CONTROLS_CUTSCENE && mode != CONTROLS_PHOTO)
        s_under = 0;
    return s_under;
}

/* The preset for now, -1 = the game's own listener; why for the log. */
static int pick(const char **why)
{
    int level = conker_Level(), p;
    LONG force = s_force;

    if (force >= 0) {
        *why = "forced (console)";
        return (int)force;
    }
    if (!s_on) {
        *why = "off";
        return -1;
    }
    if (conker_OnMainMenu()) {
        s_level = -1;
        *why = "main menu: the game's own";
        return -1;
    }
    if (level >= FIRST_MP_MAP || MEM8(MP_MATCH)) {
        *why = "multiplayer: the game's own";
        return -1;
    }
    if (under_water(level)) {
        *why = "under water";
        return PRESET_UNDERWATER;
    }
    p = table_preset(level);
    *why = p >= 0 ? "the level's room" : "no room for this level: the game's own";
    return p;
}

void room_reverb_frame(void)
{
    const char *why;
    float strength;
    int p;

    if (InterlockedExchange(&s_dirty, 0))
        s_on = room_reverb_on();
    strength = room_reverb_strength();
    p = pick(&why);
    if (p == s_sent && (p < 0 || strength == s_sent_strength)) {
        s_why = why;
        return;
    }
    if (p >= 0) {
        uint32_t l[12];
        for (int i = 0; i < 12; i++)
            l[i] = MEM32(PRESET_TABLE + (uint32_t)p * 0x30u + 4u * (uint32_t)i);
        xbox_ApuSetRoomReverb(l, strength / 100.0f);
    } else {
        xbox_ApuSetRoomReverb(NULL, 1.0f);
    }
    if (p != s_sent || why != s_why) {
        int level = conker_Level();
        const char *name = conker_WarpLevelName(level);
        if (p >= 0)
            fprintf(stderr, "[ROOMREV] level 0x%02X %s -> preset %d %s (%s), strength %.0f %%\n",
                    level & 0xFF, name ? name : "?", p, k_names[p], why, strength);
        else
            fprintf(stderr, "[ROOMREV] level 0x%02X %s -> the game's own reverb (%s)\n",
                    level & 0xFF, name ? name : "?", why);
    }
    s_sent = p;
    s_sent_strength = strength;
    s_why = why;
}
