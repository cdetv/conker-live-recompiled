/**
 * The cheats' state (to-do 22, SPEC 10.3; cheats.c): which are unlocked and on,
 * the code words as hashes, the codes typed under Ctrl or the Konami code under RB, and what
 * the money cheat remembers per save slot. What each cheat does to the game is
 * in recomp_manual.c (section "cheats"), the menu in overlay.cpp, the commands
 * in console.c.
 *
 * Threads: the masks are read from any thread. cheat_set, cheats_enter_code
 * and cheats_poll run on the executor (the only one that writes settings.ini);
 * cheats_key_message on the window thread, cheats_pad on the thread reading
 * the pad, the game thread takes the sounds and keeps the money.
 */
#ifndef CONKER_CHEATS_H
#define CONKER_CHEATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CHEAT_GOD,
    CHEAT_NO_RELOAD,
    CHEAT_MONEY,
    CHEAT_TAIL,
    CHEAT_CHAPTERS,
    CHEAT_OXYGEN,
    CHEAT_LIVES,
    CHEAT_MPUNLOCKS,
    CHEAT_GLASS,            /* to-do 160 */
    CHEAT_BULLET,           /* to-do 161 */
    CHEAT_MOON,             /* to-do 162 */
    CHEAT_SPEED,            /* to-do 163 */
    CHEAT_BIGHEAD,          /* to-do 164 */
    CHEAT_IRONBOOTS,        /* to-do 190 */
    CHEAT_HOLY,             /* to-do 191 */
    CHEAT_DIVE,             /* to-do 195 */
    CHEAT_MOUSE,            /* to-do 212: Conker the Mouse */
    CHEAT_BEAR,             /* to-do 212: Conker the Bear */
    CHEAT_COUNT
};

#define CHEAT_BIT(id) (1u << (id))

/* What cheats_enter_code found. */
#define CHEAT_CODE_WRONG (-1)
#define CHEAT_CODE_ALL   CHEAT_COUNT   /* the master code */
#define CHEAT_CODE_UNLOCK (CHEAT_COUNT + 1) /* the cheat menu's unlock (word or Konami) */

/* Names for the menu and the console; NULL past the last. */
const char *cheat_name(int id);
const char *cheat_help(int id);
const char *cheat_command(int id);
int cheat_of_command(const char *name);    /* -1 for none */

/* The menu can be opened: --debug, or the unlock code was entered. */
int cheats_menu_ok(void);
int cheat_unlocked(int id);                /* every cheat with --debug */
int cheat_is_set(int id);                  /* its box in the menu */
void cheat_set(int id, int on);

/* The Debug menu's buttons (to-do 146; executor). Unlocked: the unlock code was
 * entered (or the button), --debug aside. Reset: locked, none unlocked, all off. */
int cheats_menu_unlocked(void);
void cheats_unlock_menu(void);
void cheats_reset(void);

/* The cheats in effect (unlocked and on), a mask of CHEAT_BIT; any thread. */
uint32_t cheats_active(void);

/* A code from the menu, in any case; letters count, nothing else does.
 * CHEAT_CODE_WRONG, a cheat, or CHEAT_CODE_ALL. Plays the code's own sound or
 * a wrong one. While the menu is locked every code is wrong. */
int cheats_enter_code(const char *text);

/* Once per present, executor: writes settings.ini for what other threads
 * changed. */
void cheats_poll(void);

/* The unlock code (SPEC 10.3). screen_ok: Options is highlighted or open on the
 * main menu, or the pause menu is up, and no menu of ours. */

/* A window message on the window thread: 1 = it was a Ctrl key (either) held
 * on those screens, or a key pressed under it, and the game must not get it.
 * The unlock word unlocks the menu; once it is, a cheat's code (or the master
 * code) unlocks that cheat (every cheat). Ctrl let go with letters that made
 * no code = a wrong code (to-do 199). */
int cheats_key_message(unsigned msg, uintptr_t wparam, intptr_t lparam,
                       int screen_ok);

/* Slot 1's controller as the game is about to get it: 1 = RB is held on those
 * screens (or something is still down after it), so the game gets nothing.
 * dirs: bit 0 up, 1 down, 2 left, 3 right (d-pad or stick). */
int cheats_pad(unsigned dirs, int a, int b, int rb, int any_input,
               int screen_ok);

/* cheats_pad is taking the pad: B there is the code's, not the quit question's. */
int cheats_pad_holding(void);

/* Sounds asked for, for the game thread to play: a mask of these. */
#define CHEAT_SOUND_HOLD  1u
#define CHEAT_SOUND_RIGHT 2u
#define CHEAT_SOUND_WRONG 4u
unsigned cheats_take_sounds(void);
/* With CHEAT_SOUND_RIGHT: which code it was for (a cheat, CHEAT_CODE_ALL or
 * CHEAT_CODE_UNLOCK), each has its own sound; CHEAT_CODE_WRONG = none. */
int cheats_take_code_sound(void);

/* The fire imp's reaction wanted at the main menu: 0 none, 1 right, 2 wrong. */
int cheats_take_imp(void);

/* The money cheat: the amount a save slot (0-2) had before it, -1 for none,
 * and a backup of that slot's save folder, once, before the first change. */
int32_t cheats_cash_kept(int slot);
void cheats_cash_keep(int slot, int32_t cash);
void cheats_backup_slot(int slot);

#ifdef __cplusplus
}
#endif

#endif
