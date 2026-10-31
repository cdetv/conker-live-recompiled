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
/* Nonzero while the world is frozen for photo mode (any thread). */
int photo_frozen(void);
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
/* Game thread: open the photo panel; take a photo at the next present
 * (the picture as shown, before any of the overlay's text). */
void overlay_photo_panel(void);
void overlay_photo_shot(void);

#ifdef __cplusplus
}
#endif

#endif
