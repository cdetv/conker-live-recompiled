/**
 * Photo mode (photo_mode.c, SPEC 10.1): the world frozen, the game still
 * drawing every frame, the picture taken from a free camera.
 */
#ifndef CONKER_PHOTO_MODE_H
#define CONKER_PHOTO_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Game thread, from recomp_manual.c's entry hooks: frame_timer_update
 * (0x002A7B60, before the game reads its pause flag), the in-game frame's
 * draw (0x002A3FD0) and 0x002AF250 (after the swap, before game_frame
 * compares the pause flags). */
void photo_frame_begin(void);
void photo_render_begin(void);
void photo_frame_end(void);
/* Nonzero while the world is frozen, for photo mode or by the pause key
 * (any thread); photo_active: photo mode itself (its camera, keys, panel);
 * photo_paused: the pause key's freeze (to-do 117). */
int photo_frozen(void);
int photo_active(void);
int photo_paused(void);
/* Photo mode with the world moving at normal speed (any thread; to-do 31):
 * frame step held, or the world resumed. photo_active stays on: the camera,
 * keys and panel are still photo mode's, but the sound plays. */
int photo_world_running(void);
/* Resumed with the key, not just held (the corner note says so). */
int photo_world_resumed(void);
/* Freecam (to-do 33): photo mode's camera with the world running; _game: the pad
 * and keys are Conker's just now. _set: any thread, from the next frame. */
int photo_freecam(void);
int photo_freecam_pos(float *x, float *y, float *z);
int photo_freecam_game(void);
void photo_freecam_set(int on);
/* Nonzero while photo mode's keys are in use: frozen, or just left with a
 * key the game would take (Esc, its pause) still held. */
int photo_keys_wanted(void);
/* The photo mode key (window thread): on, or off, from the next frame. */
void photo_request_toggle(void);
/* Mouse wheel movement (window thread, WM_MOUSEWHEEL's delta) while frozen. */
void photo_wheel(int delta);
/* Game thread, from recomp_manual.c: the HUD is to be drawn while frozen
 * (0x002A5DB0 then runs with the pause flag clear); the draw's other HUD
 * call, after 0x002A60B0; character index (char_render_pass's first
 * argument) or a character id (byte +0x10B; an attachment's byte +0) is
 * not to be drawn. */
int photo_hud_shown(void);
void photo_hud_draw(void);
int photo_hides_char(unsigned index);
int photo_hides_char_id(unsigned id);
/* The HUD is to be left out (frozen, not shown): for HUD pieces drawn
 * outside the two HUD calls, such as the weapon sight. */
int photo_hud_hidden(void);

/* What the photo panel (overlay.cpp, executor thread) shows and changes;
 * the game thread uses it every frame (camera input stops while the panel
 * is open, so the two never write at once). Valid while frozen. */
typedef struct {
    float fov;          /* field of view, degrees (5-100) */
    float fov_game;     /* the game's own, put back on leaving */
    float roll;         /* radians, more = anticlockwise */
    float speed;        /* fly speed, percent (photo_speed) */
    int   hud, hide_conker;
    int   dof;          /* depth of field on */
    float focus;        /* its focus distance, game units (Conker ~60 tall) */
    float blur;         /* its strength, percent (photo_blur) */
} PhotoOptions;
PhotoOptions *photo_options(void);
/* Photo mode's cutscene blur: -1 as the F2 menu says, else 0 off / 1 on. */
void photo_set_cut_blur(int v);
/* Asked from the panel, done by the game thread at its next frame. */
void photo_ask_reset(void);
void photo_ask_focus_conker(void);
/* Game thread, from recomp_manual.c's depth of field hook: nonzero = draw
 * photo mode's depth of field instead of the game's, focused at *focus
 * units away, *blur guest pixels at twice that distance; *cn / *cf the
 * camera's near and far planes. */
int photo_dof(float *focus, float *blur, float *cn, float *cf);
/* The cutscene blur (the game's depth of field) in photo mode, B / pad B
 * or the panel: -1 = as the F2 menu says (not frozen, or not changed), 0 =
 * off, 1 = on (the F2 choice, "updated" when that is off). Toggle: any
 * thread. */
int photo_cut_blur(void);
void photo_toggle_cut_blur(void);
/* From recomp_manual.c: the F2 cutscene blur is not off. */
int conker_CutsceneBlurOn(void);
/* From recomp_manual.c (game thread): camera cam's aspect (+0x108) to the
 * display shape, which the frozen world update no longer does. */
void conker_AspectRefresh(unsigned cam);

/* From overlay.cpp: a menu (F2 / F3 / the photo panel) is open, so the pad
 * and keys are its own; and whether the menu has the pad (blocked for the
 * game). */
int overlay_menu_open(void);
int overlay_pad_menu(void);
/* One of our menus is up (F2, F3, F8, the cheat menu, the pad's list,
 * Report Bug, Quit; not the console or the photo panel): the game's pause
 * (Start) is kept from it, keys and pad (to-do 181). Any thread. */
int overlay_blocks_pause(void);
/* The pause key (P, the F2 checkbox), for the game thread: nonzero while a
 * pause is wanted (not the debug choice old_pause, which holds the flip
 * instead), *age_ms since it was asked; refused clears it (not here). */
int overlay_pause_wanted(unsigned *age_ms);
void overlay_pause_refused(void);
/* Game thread: open the photo panel; take a photo at the next present
 * (the picture as shown, before any of the overlay's text). */
void overlay_photo_panel(void);
void overlay_photo_shot(void);

#ifdef __cplusplus
}
#endif

#endif
