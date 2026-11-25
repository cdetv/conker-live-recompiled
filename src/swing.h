/**
 * The swing piece (Extras, 0.6.0; to-dos 42, 233, 234; SPEC 10.4 "Swing";
 * plan docs/research/r4-0.6.1-swing-plan.md): Quick Swing (hold attack to
 * keep swinging), Forgiving Combo (a press a little too early waits for its
 * moment), Fast Weapon Draw (weapons come out and go away faster), the
 * Vault suit's punches under Quick Swing, Free Swing (one button takes the
 * bat or frying pan out and swings) and the camera switch (normal camera or
 * over the shoulder with the bat or frying pan out). swing.c.
 */
#ifndef CONKER_SWING_H
#define CONKER_SWING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Settings (env first, as the other Extras; the F2 menu's setters). */
enum { SWING_QUICK_OFF, SWING_QUICK_NORMAL, SWING_QUICK_EASY };
int  swing_quick(void);
void swing_set_quick(const char *value);       /* off | normal | easy */
int  swing_forgiving(void);
void swing_set_forgiving(int on);
int  swing_fast_draw(void);
void swing_set_fast_draw(int on);
int  swing_free(void);                         /* Free Swing (free_swing) */
void swing_set_free(int on);
int  swing_camera_switch(void);                /* the camera switch (camera_switch) */
void swing_set_camera_switch(int on);
int  swing_air(void);                          /* Swing While Jumping (air_swing) */
void swing_set_air(int on);

/* Game thread, once a frame (controls_poll): set = the controls set
 * (CONTROLS_*), chr = the player (camera 0's character) or 0, foot = on foot
 * (no vehicle, not swimming), atk_key = the keyboard's Attack key held (on
 * the keyboard's port 0), aiming = Modern's aim input held (keys or pad). */
void swing_frame(int set, uint32_t chr, int foot, int atk_key, int aiming);

/* pad_filter (port 0, after the controller binds): the game's X now, i.e.
 * the controller's Reload button (Free Swing's button). */
void swing_pad_x(int held);

/* The B handler (0x002FD7D0, game thread): what B does for character chr
 * this frame, as the game picked it (player_b_kind_update 0x002F48A0: the
 * zone, the level's weapon or the bat / frying-pan fallback). */
void swing_b_kind(uint32_t chr, int kind);

/* Free Swing and the camera switch in the game's code (recomp_manual.c):
 *  - camera_update (0x0003C6F0): 1 = the camera is the normal one with the
 *    bat or frying pan out (its "aiming" byte +0 is hidden from the camera
 *    for the call);
 *  - the aim routine (0x002F99A0) of character aim data aimd: 1 = the right
 *    stick is kept from it (the camera orbits instead);
 *  - the move turn (0x002DEF80, ecx = character, eax = heading): 1 = turn
 *    him to *heading (run the way the stick points from the camera);
 *  - cam_zoom_toggle (0x0004A7D0): 1 = the Unlocked Camera's reset is off
 *    (the switch has the button). */
int swing_camera_normal(uint32_t cam);
int swing_mute_sound(uint32_t chr, uint32_t entry);

/* The Camera Switch's over-the-shoulder view (built on the normal camera;
 * its heading held, turned by the mouse / right stick only), in the
 * camera's code:
 *  - swing_shoulder(cam): it applies to camera cam now;
 *  - swing_shoulder_tick(cam): camera_update of camera 0 (eases the framing);
 *  - swing_shoulder_follow(cam, y0, dt): after cam_stick_look (y0 = the yaw
 *    +0x60 before it): the stick's turn goes to the view's held heading and
 *    Conker, the camera eases to that heading;
 *  - swing_shoulder_mouse(cam, d): the mouse's turn (look_turn), 1 = taken;
 *  - swing_shoulder_ease(&dist, &height, zoom): cam_ctl_ease_caller's
 *    targets (+0x26C / +0x270);
 *  - swing_shoulder_side(cam, &dx, &dz): camera_update_view_matrix's move of
 *    the camera and its look-at point to the side. */
int   swing_shoulder(uint32_t cam);
void  swing_shoulder_tick(uint32_t cam);
float swing_shoulder_amount(void);
void  swing_shoulder_follow(uint32_t cam, float y0, float dt, int stick);
int   swing_shoulder_mouse(uint32_t cam, float d);
void  swing_shoulder_ease(float *dist, float *height, float zoom);
void  swing_shoulder_side(uint32_t cam, float *dx, float *dz);
int swing_aim_still(uint32_t aimd);
int swing_heading(uint32_t chr, uint32_t *heading);
int swing_zoom_reset_off(void);

/* Installs the toolkit's final pad hook (xbox_InputSetPadFinal: what the
 * game gets, keyboard merged in), which changes the right trigger while the
 * swing is ours. Once, from controls_poll. */
void swing_install(void);

/* Fast Weapon Draw: the factor for an animation request of character c,
 * index idx (0x002F1790: a full-body request; layered = 1 for 0x002F1BD0's
 * slot 3, the take-out of a weapon drawn while moving). 1.0 = unchanged. */
float swing_draw_factor(uint32_t c, uint32_t idx, int layered);

/* The bat / frying pan's take-out (out = 1) or put-away (out = 0) asked for
 * character chr (0x00319BC0 / 0x0031A210 entry): 1 = ask it layered, on the
 * right arm only, so a moving Conker keeps moving (Free Swing's own draws,
 * the camera switch on). swing_layer_started: 0x002F1BD0's slot-3 request
 * that followed (index, speed), for the draw's events. */
int  swing_layered(uint32_t chr, int out);
void swing_layer_started(uint32_t chr, uint32_t idx, float speed);
/* The draw clip's end event's re-pick of the movement (0x002D5B30 from
 * 0x0030AA60, wanted speed 0): 1 = give it *want (after a layered put-away
 * while running: no stop). */
int  swing_pick_speed(uint32_t chr, float *want);
/* recomp_manual.c: that re-pick asked by us (0x002D5B30, eax = chr, wanted
 * speed want), from the game thread. */
void swing_move_pick(uint32_t chr, float want);

/* The weaponless strafe over the shoulder (round 5):
 *  - swing_strafe_draw(chr): his matrix build's entry (0x000808F0): his body
 *    drawn facing the view while he strafes;
 *  - swing_strafe_anim(chr, idx, speed, &name, &force): char_set_animation's
 *    entry (0x002F1790; speed = its speed word): the plain sneak / walk / run
 *    shown as the bat's side or back clip (name: the clip to load, force: the
 *    game must take it as a new request);
 *  - swing_arm_pose(list, pose): the animation mixer's exit (0x00036230;
 *    list = the character's [+0x3C4]+4, pose = the mixed pose): while the
 *    player strafes, his right arm takes his left arm's motion, mirrored
 *    (round 7: the side / back clips hold the right arm for the bat). */
void swing_strafe_draw(uint32_t chr);
int  swing_strafe_anim(uint32_t chr, uint32_t idx, float speed, uint32_t *name, int *force);
void swing_arm_pose(uint32_t list, uint32_t pose);

/* Swing While Jumping (to-do 48), in the game's code (recomp_manual.c):
 *  - swing_air_gate(chr): the bat / frying pan update (0x0032A300, game
 *    thread) for character chr: 1 = its swing test off the floor
 *    (0x0032A502, [chr+0xDC] == 0) is to pass (the height reads 0 from
 *    0x0031A520's return until the combo 0x00329E80 has run);
 *  - swing_air_draw(chr): the draw / put-away start (0x003197F0, edi = chr):
 *    1 = its air test (+0xDC, +0xD0) is to pass (Free Swing's own draw in
 *    the air);
 *  - swing_air_anim(chr, idx, from, &idx2, &blend): char_set_animation's
 *    entry (0x002F1790; from = anim_by_index's caller, else 0; blend = the
 *    request's 6th word, in / out): 1 = ask *idx2 instead of idx (a swing
 *    that ends in the air falls in the bat's JumpPt2, not FireHold standing;
 *    in the tail spin / noclip the spin's pose; a draw's end in the air
 *    leaves a swing alone);
 *  - swing_air_pose(list, pose): the animation mixer's exit (0x00036230), as
 *    swing_arm_pose: his legs keep the jump's pose during a swing in the air;
 *  - swing_air_noclip(chr): noclip's flight (recomp_manual.c, each frame;
 *    marks noclip for his legs): bit 1 = run the B / weapon step there
 *    (Free Swing on: the movement it replaces held it), bit 2 = leave his
 *    animation alone this frame (a weapon out or coming, or a swing). */
int  swing_air_gate(uint32_t chr);
int  swing_air_draw(uint32_t chr);
int  swing_air_anim(uint32_t chr, uint32_t idx, uint32_t from, uint32_t *idx2, float *blend);
void swing_air_pose(uint32_t list, uint32_t pose);
int  swing_air_noclip(uint32_t chr);
/* The bat / frying pan out or coming, Swing While Jumping on (the player):
 * Infinite Tail Spin leaves the right trigger (the swing then, not the
 * slam) to his movement. */
int  swing_air_weapon(uint32_t chr);

#ifdef __cplusplus
}
#endif

#endif
