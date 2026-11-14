/**
 * The cheats' state: see cheats.h, SPEC 10.3.
 *
 * The code words are game text (SPEC 3.3), so only their hashes are here: FNV-1a
 * over the letters, upper case, from the start value below. A new word's hash
 * is made outside the repo; the words themselves stay out of git.
 */
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "settings.h"

/* cheats_unlocked / cheats_on in settings.c take 24 bits (a float range). */
_Static_assert(CHEAT_COUNT <= 24, "widen cheats_unlocked / cheats_on in settings.c");

typedef struct {
    const char *name;
    const char *help;
    const char *command;
    uint64_t word;          /* its code word's hash */
} Cheat;

static const Cheat s_cheats[CHEAT_COUNT] = {
    { "God mode", "Energy never drops. Pits and scripted kills still reload.",
      "godmode", 0xEFF00FAE7B86DE53ull },
    { "No reload", "Every story gun with a clip.",
      "reloading", 0x070CB6D97991B675ull },
    { "$1,000,000", "Spending doesn't lower it; off puts the old amount back.",
      "rosebud", 0xF8DF5164873E5A1Cull },
    { "Infinite tail spin", "A held hovers; let go for a slow glide down.",
      "inftail", 0x0CED9AA5DF7C0036ull },
    { "Unlock all chapters", "The list shows every part; your progress stays.",
      "allchapters", 0x717334C164260B22ull },
    { "Infinite oxygen", "Conker never runs out of air.",
      "infoxygen", 0x0F7BBF60F4B86A90ull },
    { "Infinite lives", "Losing a life doesn't lower the count.",
      "inflives", 0x2194EA4B39A9DC44ull },
    { "Multiplayer unlocks", "Potty Mouth, Flame Fire and Purple Streak. Off: back to "
      "what was earned.", "mpunlocks", 0x6931E29F83B0DACAull },
    { "Glass Squirrel", "One hit and Conker dies (a life lost, as any death). "
      "God mode wins when both are on.", "glass", 0x1FFDFA46D98A9796ull },
    { "Bullet time", "B (F3 > Photo mode) or both sticks clicked at once switch slow "
      "motion on and off; the sound stays at normal speed.", "bullettime",
      0x538C0F4EB77B41A2ull },
    { "Moon Jump", "Jumps go about three times as high and float down; falls don't "
      "hurt while it is on.", "moonjump", 0xAD6E19426195FCCFull },
    { "Super Speed", "Conker runs and walks twice as fast.", "superspeed",
      0xAD0FFD2930257B89ull },
    { "Big Head", "Conker's head drawn twice as big.", "bighead", 0xBA2F7FFE3F237D9Cull },
    { "Iron Boots", "Conker doesn't swim: he sinks and walks on the bottom. He still runs out "
      "of air.", "ironboots", 0x76B62F9BD906D12Cull },
    { "Holy Squirrel", "Conker walks on water. Holy Squirrel wins when both are on.",
      "holysquirrel", 0xF577732E967BE2BCull },
    { "Dive Anywhere", "Conker can dive in any water, before he has learnt to dive too.",
      "diveanywhere", 0x1CBD697C597412E6ull },
    { "Conker the Mouse", "Conker at half his size, his voice higher. Conker the Bear "
      "goes off.", "mouse", 0xD8A32F55E6D41C39ull },
    { "Conker the Bear", "Conker at twice his size, his voice lower. Conker the Mouse "
      "goes off.", "bear", 0x153534EA0CE8E196ull },
};

#define WORD_MASTER 0x8F33F9C767D23E0Dull
#define WORD_UNLOCK 0x319DA781AF8B87BEull
#define TYPED_MAX   18          /* letters kept under Ctrl: the longest code word */

static uint64_t hash_start(void)
{
    return 0xCBF29CE484222325ull ^ 0xB4DF00D5ull;
}

static uint64_t hash_letter(uint64_t h, char upper)
{
    return (h ^ (unsigned char)upper) * 0x100000001B3ull;
}

/* ---- the state ---------------------------------------------------------------- */

static volatile LONG s_loaded;
static volatile LONG s_menu_ok;       /* the unlock code was entered */
static volatile LONG s_unlocked;      /* CHEAT_BIT mask */
static volatile LONG s_on;
static volatile LONG s_dirty_menu;    /* settings.ini is behind: executor writes */
static volatile LONG s_dirty_unlocked;
static volatile LONG s_dirty_cash;
static volatile LONG s_cash[3];
static volatile LONG s_sounds;
static volatile LONG s_code_sound;    /* the code whose sound is wanted, + 2; 0 none */
static volatile LONG s_imp;

static void log_active(const char *why);
static void poll_now(void);

static void load(void)
{
    if (s_loaded)
        return;
    s_menu_ok = settings_get("cheat_menu")[0] == '1';
    s_unlocked = atol(settings_get("cheats_unlocked")) & ((1 << CHEAT_COUNT) - 1);
    s_on = atol(settings_get("cheats_on")) & ((1 << CHEAT_COUNT) - 1);
    s_cash[0] = atol(settings_get("cheat_cash_1"));
    s_cash[1] = atol(settings_get("cheat_cash_2"));
    s_cash[2] = atol(settings_get("cheat_cash_3"));
    InterlockedExchange(&s_loaded, 1);
    if (s_on)
        log_active("from settings.ini");
}

const char *cheat_name(int id)
{
    return id >= 0 && id < CHEAT_COUNT ? s_cheats[id].name : NULL;
}

const char *cheat_help(int id)
{
    return id >= 0 && id < CHEAT_COUNT ? s_cheats[id].help : NULL;
}

const char *cheat_command(int id)
{
    return id >= 0 && id < CHEAT_COUNT ? s_cheats[id].command : NULL;
}

int cheat_of_command(const char *name)
{
    for (int i = 0; i < CHEAT_COUNT; i++)
        if (!strcmp(name, s_cheats[i].command))
            return i;
    return -1;
}

int cheats_menu_ok(void)
{
    load();
    return settings_debug() || s_menu_ok;
}

int cheat_unlocked(int id)
{
    load();
    return settings_debug() || (s_unlocked & CHEAT_BIT(id));
}

int cheat_is_set(int id)
{
    load();
    return (s_on & CHEAT_BIT(id)) != 0;
}

uint32_t cheats_active(void)
{
    load();
    return (uint32_t)s_on & (settings_debug() ? 0xFFFFFFFFu : (uint32_t)s_unlocked);
}

/* The log says which cheats are on, so a Report Bug zip shows it (SPEC 10.3). */
static void log_active(const char *why)
{
    char text[256] = "";
    uint32_t on = cheats_active();

    for (int i = 0; i < CHEAT_COUNT; i++)
        if (on & CHEAT_BIT(i)) {
            if (text[0])
                strcat_s(text, sizeof(text), ", ");
            strcat_s(text, sizeof(text), s_cheats[i].name);
        }
    fprintf(stderr, "[CHEATS] on (%s): %s\n", why, text[0] ? text : "none");
    fflush(stderr);
}

static void save_mask(const char *key, LONG mask)
{
    char v[16];
    snprintf(v, sizeof(v), "%ld", mask);
    settings_set(key, v);
}

int cheats_menu_unlocked(void)
{
    load();
    return s_menu_ok != 0;
}

/* The Debug menu's Unlock cheats (to-do 146): what the unlock code does, without
 * its sound and the imp. */
void cheats_unlock_menu(void)
{
    load();
    InterlockedExchange(&s_menu_ok, 1);
    settings_set("cheat_menu", "1");
    fprintf(stderr, "[CHEATS] the cheat menu is unlocked (Debug menu)\n");
    fflush(stderr);
}

/* Reset cheats: locked and hidden again, no cheat unlocked, every cheat off. The
 * money and Multiplayer unlocks go back by themselves, as when each is turned off. */
void cheats_reset(void)
{
    load();
    InterlockedExchange(&s_menu_ok, 0);
    InterlockedExchange(&s_dirty_menu, 0);
    InterlockedExchange(&s_unlocked, 0);
    InterlockedExchange(&s_dirty_unlocked, 0);
    InterlockedExchange(&s_on, 0);
    settings_set("cheat_menu", "0");
    save_mask("cheats_unlocked", 0);
    save_mask("cheats_on", 0);
    fprintf(stderr, "[CHEATS] reset (Debug menu): the cheat menu is locked, no cheat unlocked\n");
    log_active("reset");
}

void cheat_set(int id, int on)
{
    load();
    if (id < 0 || id >= CHEAT_COUNT || !cheat_unlocked(id))
        return;
    if (on && (id == CHEAT_MOUSE || id == CHEAT_BEAR))  /* one size at a time */
        InterlockedAnd(&s_on, ~(LONG)CHEAT_BIT(id == CHEAT_MOUSE ? CHEAT_BEAR : CHEAT_MOUSE));
    if (on)
        InterlockedOr(&s_on, (LONG)CHEAT_BIT(id));
    else
        InterlockedAnd(&s_on, ~(LONG)CHEAT_BIT(id));
    save_mask("cheats_on", s_on);
    log_active(s_cheats[id].command);
}

/* A code was found: unlock it (CHEAT_CODE_ALL = every cheat; CHEAT_CODE_UNLOCK
 * the menu) or say it was wrong, with its sound and the imp. Any thread:
 * settings.ini is written by cheats_poll. */
static void code_result(int found, const char *how)
{
    load();
    if (found == CHEAT_CODE_WRONG) {
        InterlockedOr(&s_sounds, CHEAT_SOUND_WRONG);
        InterlockedExchange(&s_imp, 2);
        fprintf(stderr, "[CHEATS] a wrong code was entered (%s)\n", how);
        fflush(stderr);
        return;
    }
    InterlockedExchange(&s_code_sound, found + 2);
    InterlockedOr(&s_sounds, CHEAT_SOUND_RIGHT);
    InterlockedExchange(&s_imp, 1);
    if (found == CHEAT_CODE_UNLOCK) {
        if (!s_menu_ok) {
            InterlockedExchange(&s_menu_ok, 1);
            InterlockedExchange(&s_dirty_menu, 1);
        }
        fprintf(stderr, "[CHEATS] the cheat menu is unlocked (%s)\n", how);
    } else {
        InterlockedOr(&s_unlocked, found == CHEAT_CODE_ALL ? (1 << CHEAT_COUNT) - 1
                                                           : (LONG)CHEAT_BIT(found));
        InterlockedExchange(&s_dirty_unlocked, 1);
        fprintf(stderr, "[CHEATS] code entered (%s): %s\n", how,
                found == CHEAT_CODE_ALL ? "all cheats" : s_cheats[found].name);
    }
    fflush(stderr);
}

/* Which code a hash is: the unlock word always, the master code and the
 * cheats only once the menu is unlocked (user 2026-11-11: before that they are
 * wrong codes). CHEAT_CODE_WRONG for none. */
static int code_of(uint64_t h)
{
    if (h == WORD_UNLOCK)
        return CHEAT_CODE_UNLOCK;
    if (!cheats_menu_ok())
        return CHEAT_CODE_WRONG;
    if (h == WORD_MASTER)
        return CHEAT_CODE_ALL;
    for (int i = 0; i < CHEAT_COUNT; i++)
        if (s_cheats[i].word && h == s_cheats[i].word)
            return i;
    return CHEAT_CODE_WRONG;
}

int cheats_enter_code(const char *text)
{
    uint64_t h = hash_start();
    int found;

    load();
    for (; *text; text++)
        if (isalpha((unsigned char)*text))
            h = hash_letter(h, (char)toupper((unsigned char)*text));
    found = code_of(h);
    if (found == CHEAT_CODE_UNLOCK)     /* the box is the menu's own: nothing to unlock */
        found = CHEAT_CODE_WRONG;
    code_result(found, "the cheat menu");
    poll_now();
    return found;
}

void cheats_poll(void)
{
    poll_now();
}

static void poll_now(void)
{
    load();
    if (s_dirty_menu && InterlockedExchange(&s_dirty_menu, 0))
        settings_set("cheat_menu", "1");
    if (s_dirty_unlocked && InterlockedExchange(&s_dirty_unlocked, 0))
        save_mask("cheats_unlocked", s_unlocked);
    if (s_dirty_cash && InterlockedExchange(&s_dirty_cash, 0)) {
        static const char *const keys[3] = {
            "cheat_cash_1", "cheat_cash_2", "cheat_cash_3"
        };
        for (int i = 0; i < 3; i++)
            save_mask(keys[i], s_cash[i]);
    }
}

/* ---- the unlock code ---------------------------------------------------------- */

/* Either Ctrl held on those screens (to-do 199): the letters go here and
 * nothing to the game. After each letter every ending of what was typed is
 * checked against the codes (they differ in length); a code found empties it.
 * Ctrl let go with letters that made no code = a wrong code. Each Ctrl key is
 * tracked, so letting go of one while the other is down doesn't end it. A
 * key-up always goes on, so nothing is left down in the game's keyboard state.
 * Ctrl + letter also makes a WM_CHAR, taken too. */
int cheats_key_message(unsigned msg, uintptr_t w, intptr_t l, int screen_ok)
{
    static int held_l, held_r, n;
    static char typed[TYPED_MAX];

    if (msg == WM_KILLFOCUS) {
        held_l = held_r = n = 0;        /* no wrong code: the player didn't let go */
        return 0;
    }
    if (w == VK_CONTROL && (msg == WM_KEYDOWN || msg == WM_KEYUP)) {
        int right = (HIWORD(l) & KF_EXTENDED) != 0;
        int was = held_l || held_r;

        if (msg == WM_KEYUP) {
            if (right)
                held_r = 0;
            else
                held_l = 0;
            if (was && !held_l && !held_r && n) {
                n = 0;
                code_result(CHEAT_CODE_WRONG, "Ctrl let go");
            }
            return 0;
        }
        if (l & (1L << 30))             /* auto-repeat */
            return was;
        if (!was) {
            if (!screen_ok)
                return 0;
            n = 0;
            InterlockedOr(&s_sounds, CHEAT_SOUND_HOLD);
            fprintf(stderr, "[CHEATS] %s Ctrl held\n", right ? "Right" : "Left");
        }
        if (right)
            held_r = 1;
        else
            held_l = 1;
        return 1;
    }
    if (!held_l && !held_r)
        return 0;
    if (!screen_ok) {
        held_l = held_r = n = 0;
        return 0;
    }
    if (msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR)
        return 1;
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)
        return 0;
    if (!(l & (1L << 30)) && w >= 'A' && w <= 'Z') {
        if (n == TYPED_MAX) {
            memmove(typed, typed + 1, TYPED_MAX - 1);
            n--;
        }
        typed[n++] = (char)w;
        for (int from = n - 1; from >= 0; from--) {
            uint64_t h = hash_start();
            int found;
            for (int i = from; i < n; i++)
                h = hash_letter(h, typed[i]);
            found = code_of(h);
            if (found != CHEAT_CODE_WRONG) {
                n = 0;
                code_result(found, "keyboard");
                break;
            }
        }
    }
    return msg == WM_KEYDOWN;
}

static volatile int s_pad_held, s_pad_latch;

/* The Konami code under RB. A wrong press starts it again (or counts as its
 * first press). Anything still held when RB is let go stays from the game until
 * it too is let go, or a B pressed under RB would resume the pause menu. */
int cheats_pad(unsigned dirs, int a, int b, int rb, int any_input, int screen_ok)
{
    /* up up down down left right left right B A */
    static const unsigned char code[10] = { 1, 1, 2, 2, 4, 8, 4, 8, 16, 32 };
    static int step, rb_was;
    static unsigned was;
    unsigned now = dirs | (b ? 16u : 0u) | (a ? 32u : 0u);
    unsigned pressed = now & ~was;

    was = now;
    if (!s_pad_held && !s_pad_latch && rb && !rb_was && screen_ok) {
        s_pad_held = 1;
        step = 0;
        pressed = 0;                    /* what was already down is no press */
        InterlockedOr(&s_sounds, CHEAT_SOUND_HOLD);
        fprintf(stderr, "[CHEATS] RB held\n");
    }
    rb_was = rb;
    if (s_pad_held && (!rb || !screen_ok)) {
        s_pad_held = 0;
        s_pad_latch = 1;
    }
    if (s_pad_held) {
        for (unsigned bit = 1; bit <= 32; bit <<= 1)
            if (pressed & bit) {
                if (bit == code[step])
                    step++;
                else
                    step = bit == code[0] ? 1 : 0;
                if (step == 10) {
                    step = 0;
                    code_result(CHEAT_CODE_UNLOCK, "controller");
                }
            }
        return 1;
    }
    if (s_pad_latch) {
        if (any_input)
            return 1;
        s_pad_latch = 0;
    }
    return 0;
}

int cheats_pad_holding(void)
{
    return s_pad_held || s_pad_latch;
}

unsigned cheats_take_sounds(void)
{
    return s_sounds ? (unsigned)InterlockedExchange(&s_sounds, 0) : 0;
}

int cheats_take_code_sound(void)
{
    return s_code_sound ? (int)InterlockedExchange(&s_code_sound, 0) - 2 : CHEAT_CODE_WRONG;
}

int cheats_take_imp(void)
{
    return s_imp ? (int)InterlockedExchange(&s_imp, 0) : 0;
}

/* ---- the money cheat ---------------------------------------------------------- */

int32_t cheats_cash_kept(int slot)
{
    load();
    return slot >= 0 && slot < 3 ? (int32_t)s_cash[slot] : -1;
}

void cheats_cash_keep(int slot, int32_t cash)
{
    if (slot < 0 || slot >= 3)
        return;
    InterlockedExchange(&s_cash[slot], (LONG)cash);
    InterlockedExchange(&s_dirty_cash, 1);
}

/* Every file of one folder (a save has no others below it, but a subfolder is
 * copied too). A file that can't be copied is logged and skipped. */
static void copy_folder(const char *from, const char *to)
{
    char pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    CreateDirectoryA(to, NULL);
    snprintf(pat, sizeof(pat), "%s\\*", from);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(a, sizeof(a), "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof(b), "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            copy_folder(a, b);
        else if (!CopyFileA(a, b, FALSE))
            fprintf(stderr, "[CHEATS] backup: couldn't copy %s (%lu)\n", a,
                    GetLastError());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* <save dir>\cheat_backups\GameN: beside saves\, as a folder in there would
 * be taken for a save. */
void cheats_backup_slot(int slot)
{
    char from[MAX_PATH], to[MAX_PATH];

    snprintf(from, sizeof(from), "%s\\saves\\Game%d", settings_save_dir(),
             slot + 1);
    snprintf(to, sizeof(to), "%s\\cheat_backups", settings_save_dir());
    CreateDirectoryA(to, NULL);
    snprintf(to, sizeof(to), "%s\\cheat_backups\\Game%d", settings_save_dir(),
             slot + 1);
    copy_folder(from, to);
    fprintf(stderr, "[CHEATS] saved a copy of Game %d in %s\n", slot + 1, to);
    fflush(stderr);
}
