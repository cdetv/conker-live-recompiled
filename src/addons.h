/**
 * Extras, the Recomp Addons of 0.6.0 (to-dos 44-47; SPEC 10.4): options
 * that change the game itself, all off by default (user 2026-11-16). F2's
 * "Extras" tab (the name: user 2026-11-23) and settings.ini's [extras].
 *
 * Built: Weapon Both / Bat / Frying Pan (44; Bat mode: the wrapper on
 * player_has_bat 0x002F6850 in recomp_manual.c asks addons_weapon(); Pan
 * mode: the held-object name and sound swaps at each level load, here),
 * Key Goblin (46: two task bytecode patches, from addons_frame), and the
 * Update Cutscene setting (47; the edits themselves: cutscene_edits.c). The
 * Milk Fix (45, "Intro Fix" in the plans) joins here later
 * (docs/research/r4-0.6.0-addons-plan.md).
 */
#ifndef CONKER_ADDONS_H
#define CONKER_ADDONS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { ADDON_WEAPON_BOTH = 0, ADDON_WEAPON_BAT = 1, ADDON_WEAPON_PAN = 2 };

/* settings.ini weapon (RECOMP_WEAPON): "both", "bat" or "pan". */
int addons_weapon(void);
void addons_set_weapon(const char *value);

/* The Frying Pan (Weapon "pan"): the carved pan is in the asset cache
 * (caff_cache.c recipe "pan"); on = 1 when the name and sound swaps are in
 * (they change only at a level load: addons_level_load), 0 off, < 0 the
 * tables not checked yet / not as expected. */
int addons_pan_available(void);
int addons_pan_on(void);
int addons_pan_now(void);           /* mid-level switch; 1 = drop a held bat */

/* settings.ini milk_fix (RECOMP_MILK_FIX), F2 "Milk Fix" (to-do 45): the
 * goblet a glass of milk, with the glass in the asset cache (caff_cache.c
 * recipe "milk"); applied at the next level load. */
int addons_milk_fix(void);
void addons_set_milk_fix(int on);
int addons_milk_available(void);

/* The start of each level load (hook on 0x002AA0C0): the Frying Pan and the
 * Milk Fix on or off. */
void addons_level_load(void);

/* settings.ini no_key_goblings (RECOMP_NO_KEY_GOBLINGS), F2 "No Key Goblings". */
int addons_key_goblin(void);
void addons_set_key_goblin(int on);

/* settings.ini update_cutscenes (RECOMP_UPDATE_CUTSCENES), F2 "Update
 * Cutscene": the training's weapon scenes match the weapon (cutscene_edits.c;
 * read at each scene start). */
int addons_update_cutscenes(void);
void addons_set_update_cutscenes(int on);

/* Each frame on the game thread (recomp_manual.c cheats_frame). */
void addons_frame(void);

/* From the Bat mode wrapper when it turned the pan into the bat. */
void addons_note_bat(void);

#ifdef __cplusplus
}
#endif

#endif
