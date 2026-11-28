/**
 * Camera motion blur (to-do 235; F2 Graphics "Camera motion blur", 0.7.0).
 *
 * The world draw of view 0 reaches the dof_update wrapper (sub_001C71A0,
 * recomp_manual.c) once its 3D picture is drawn: after the characters, water,
 * fur and the depth of field, before the screen drops, the glow, the HUD and
 * the speech bubbles. Its world_end asks mblur_frame how the render camera
 * [0x0084B868] moved since the last frame and, during a quick turn, sends
 * the runtime's motion blur (toolkit host effect NV_HOST_FX_MBLUR = 11,
 * mblur_d3d11.c): each pixel smeared along the path its point took on
 * screen. Only the camera's motion counts (things that move by themselves
 * are smeared as if they stood still), so the middle, where Conker is, is
 * kept sharp.
 *
 * The camera: view +0x10 (row vectors as D3D's, right-handed, looking down
 * -z) and its rigid inverse +0x50. A point v of this frame's view space was
 * at v x inverse(now) x view(last) in the last frame's; that rotation and
 * translation go to the runtime in its camera space (z = the distance in
 * front, so z flipped), the rotation as a quaternion.
 *
 * How much: the turn (the rotation's angle) over the wall time since the
 * last frame, in degrees a second: none under 40, all from 140 (smoothstep;
 * user 2026-11-25), times the strength (0-100 %, default 50): 100 % smears
 * the motion of 1/30 s, 50 % of 1/60 s, at any frame rate, so the share
 * sent is ramp x strength x (1/30 s) / frame time (averaged over ~10
 * frames: the game's camera steps evenly, the frames' times do not). Only the
 * turn starts it;
 * then the whole camera motion is smeared (a swing round Conker moves the
 * near walls more than the far ones). Longest smear 32 guest pixels; the
 * middle 0.25 of the half diagonal kept sharp, fading in to 0.5.
 *
 * Cuts (nothing drawn, measured again from this frame): the first frame,
 * over 45 degrees or 500 units in one frame, a sudden turn or move (over 10
 * degrees or 60 units and over 3 times the last frame's), over 0.2 s since
 * the last,
 * another level, a cutscene starting or ending, any frame it was off. Off
 * (user 2026-11-25): the main menu's level (0x1D), split screen
 * ([0x0084A144] != 0), photo mode, the pause key's freeze and the free
 * camera (photo mode's), the software renderer.
 *
 * RECOMP_MOTION_BLUR=0|1|2 (motion_blur, default 0; 2 = the motion as colour
 * each frame, without the ramp: red right, green left, blue up / down,
 * black none), RECOMP_MBLUR_STRENGTH (0-100, default 50), RECOMP_MBLUR_LOG=1
 * ([MBLUR] on / off and why, cuts, every 2 s what was drawn), 2 (also every
 * frame). The console's mblur tunes the ramp, longest smear and middle.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "photo_mode.h"
#include "dev_logs.h"
#include "motion_blur.h"

#define MBLUR_STRENGTH_DEF 50.0f
#define MBLUR_LO_DEF       40.0f        /* degrees a second: none below */
#define MBLUR_HI_DEF       140.0f       /* all from */
#define MBLUR_MAX_DEF      32.0f        /* guest pixels */
#define MBLUR_MIDDLE_DEF   0.25f        /* of the half diagonal */
#define CUT_DEG   45.0
#define CUT_UNITS 500.0
#define CUT_SEC   0.2
#define SUDDEN_DEG   10.0               /* and SUDDEN_X x the last frame's */
#define SUDDEN_UNITS 60.0
#define SUDDEN_X     3.0
#define DEG       57.29577951308232

static volatile int g_mode = -1;        /* < 0 = not read yet */
static volatile float g_strength = -1.0f;
static volatile float g_lo = MBLUR_LO_DEF, g_hi = MBLUR_HI_DEF;
static volatile float g_max = MBLUR_MAX_DEF, g_middle = MBLUR_MIDDLE_DEF;

static float s_last_view[16];
static int s_have_last;
static uint32_t s_last_level;
static uint8_t s_last_cut;
static LARGE_INTEGER s_last_t;
static double s_dt_avg;                 /* smoothed frame time, s; 0 = none */
static double s_prev_deg, s_prev_move;  /* the last frame's turn and move */

static void read_env(void)
{
    if (g_mode < 0) {
        const char *e = getenv("RECOMP_MOTION_BLUR");
        g_mode = e && *e >= '0' && *e <= '2' ? *e - '0' : 0;
    }
    if (g_strength < 0.0f) {
        const char *e = getenv("RECOMP_MBLUR_STRENGTH");
        float v = e && *e ? (float)atof(e) : MBLUR_STRENGTH_DEF;
        g_strength = v >= 0.0f && v <= 100.0f ? v : MBLUR_STRENGTH_DEF;
    }
}

void conker_SetMotionBlur(int mode, float strength)
{
    read_env();
    if (mode >= 0 && mode <= 2)
        g_mode = mode;
    if (strength >= 0.0f && strength <= 100.0f)
        g_strength = strength;
}

void conker_GetMotionBlur(int *mode, float *strength)
{
    read_env();
    if (mode) *mode = g_mode;
    if (strength) *strength = g_strength;
}

void conker_SetMotionBlurTuning(float lo, float hi, float max_px,
                                float middle)
{
    if (lo >= 0.0f && hi > lo && hi <= 3600.0f) {
        g_lo = lo;
        g_hi = hi;
    }
    if (max_px >= 1.0f && max_px <= 128.0f)
        g_max = max_px;
    if (middle >= 0.0f && middle <= 1.0f)
        g_middle = middle;
}

void conker_GetMotionBlurTuning(float *lo, float *hi, float *max_px,
                                float *middle)
{
    if (lo) *lo = g_lo;
    if (hi) *hi = g_hi;
    if (max_px) *max_px = g_max;
    if (middle) *middle = g_middle;
}

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static long now_ms(void)
{
    return (long)(clock() * 1000 / CLOCKS_PER_SEC);
}

static double smooth(double e0, double e1, double x)
{
    double t = (x - e0) / (e1 - e0);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

/* ---- the log ------------------------------------------------------------- */

static const char *s_state = "?";       /* what was said last: "" = on */
static struct {
    long since;
    unsigned frames, sent;
    double top_dps, amount_sum;
} s_sum;

/* On / off and why, said once per change. */
static void note_state(const char *why)
{
    const char *now = why ? why : "";
    if (strcmp(now, s_state) && dev_log(DLOG_MBLUR))
        fprintf(stderr, "[MBLUR] t=%ld %s%s\n", now_ms(),
                why ? "off: " : "on", why ? why : "");
    s_state = now;
}

static void note_cut(const char *why, double a, double b)
{
    if (dev_log(DLOG_MBLUR))
        fprintf(stderr, "[MBLUR] t=%ld cut: %s (%.1f, %.1f)\n", now_ms(), why,
                a, b);
}

static void note_frame(int sent, double dps, double amount)
{
    long t = now_ms();
    if (!dev_log(DLOG_MBLUR))
        return;
    if (!s_sum.since)
        s_sum.since = t;
    s_sum.frames++;
    if (sent) {
        s_sum.sent++;
        s_sum.amount_sum += amount;
    }
    if (dps > s_sum.top_dps)
        s_sum.top_dps = dps;
    if (t - s_sum.since >= 2000) {
        if (s_sum.sent)
            fprintf(stderr, "[MBLUR] t=%ld last %.1f s: %u frames, %u blurred"
                    " (mean share %.2f), quickest turn %.0f deg/s\n", t,
                    (t - s_sum.since) / 1000.0, s_sum.frames, s_sum.sent,
                    s_sum.amount_sum / s_sum.sent, s_sum.top_dps);
        memset(&s_sum, 0, sizeof s_sum);
        s_sum.since = t;
    }
}

/* ---- the camera ---------------------------------------------------------- */

/* m = a x b, 4x4 row-major. */
static void mul4(const float *a, const float *b, double *m)
{
    int r, c, k;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            double s = 0.0;
            for (k = 0; k < 4; k++)
                s += (double)a[r * 4 + k] * b[k * 4 + c];
            m[r * 4 + c] = s;
        }
}

/* q (x y z w, w >= 0) of the rotation r (3x3, column vectors: v' = r v). */
static void quat_of(const double r[3][3], double q[4])
{
    double tr = r[0][0] + r[1][1] + r[2][2], s, n;
    int i;

    if (tr > 0.0) {
        s = sqrt(tr + 1.0) * 2.0;
        q[3] = 0.25 * s;
        q[0] = (r[2][1] - r[1][2]) / s;
        q[1] = (r[0][2] - r[2][0]) / s;
        q[2] = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        s = sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
        q[3] = (r[2][1] - r[1][2]) / s;
        q[0] = 0.25 * s;
        q[1] = (r[0][1] + r[1][0]) / s;
        q[2] = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        s = sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
        q[3] = (r[0][2] - r[2][0]) / s;
        q[0] = (r[0][1] + r[1][0]) / s;
        q[1] = 0.25 * s;
        q[2] = (r[1][2] + r[2][1]) / s;
    } else {
        s = sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
        q[3] = (r[1][0] - r[0][1]) / s;
        q[0] = (r[0][2] + r[2][0]) / s;
        q[1] = (r[1][2] + r[2][1]) / s;
        q[2] = 0.25 * s;
    }
    n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (q[3] < 0.0)
        n = -n;
    for (i = 0; i < 4; i++)
        q[i] = n != 0.0 ? q[i] / n : (i == 3);
}

int mblur_frame(int gpu, uint32_t prm[MBLUR_PARAMS])
{
    static LARGE_INTEGER freq;
    static int said_layout;
    uint32_t cam = MEM32(0x0084B868u), level = MEM32(0x007FA1F8u);
    uint8_t cut_flag = MEM8(0x0076F0E0u);
    const char *why = NULL;
    LARGE_INTEGER now;
    float view[16], inv[16];
    double m[16], r[3][3], q[4], t[3], dt, deg, move, dps, share, amount;
    int i, j, mode;

    read_env();
    mode = g_mode;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (!mode)
        why = "switched off";
    else if (!gpu)
        why = "software renderer";
    else if (!cam)
        why = "no camera";
    else if (level == 0x1Du)
        why = "main menu";
    else if (MEM32(0x0084A144u))
        why = "split screen";
    else if (photo_frozen())
        why = "photo mode or the pause key";
    else if (photo_freecam())
        why = "free camera";
    note_state(why);
    if (why) {
        s_have_last = 0;
        return 0;
    }
    for (i = 0; i < 16; i++) {
        view[i] = MEMF(cam + 0x10u + 4u * i);
        inv[i] = MEMF(cam + 0x50u + 4u * i);
    }
    if (!said_layout && (fabsf(view[3]) > 1e-3f || fabsf(view[7]) > 1e-3f
                         || fabsf(view[11]) > 1e-3f
                         || fabsf(view[15] - 1.0f) > 1e-3f)) {
        said_layout = 1;
        fprintf(stderr, "[MBLUR] the camera's view is not row-vector affine"
                " (%g %g %g %g): motion blur will be wrong\n", view[3],
                view[7], view[11], view[15]);
    }

    dt = s_have_last ? (double)(now.QuadPart - s_last_t.QuadPart)
                       / (double)freq.QuadPart : 0.0;
    /* v_last = v_now x inverse(now) x view(last), row vectors. */
    mul4(inv, s_last_view, m);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)                 /* column vectors, z flipped */
            r[i][j] = m[j * 4 + i] * ((i == 2) != (j == 2) ? -1.0 : 1.0);
    t[0] = m[12];
    t[1] = m[13];
    t[2] = -m[14];
    quat_of(r, q);
    deg = 2.0 * atan2(sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), q[3])
          * DEG;
    move = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);

    {
        /* A camera cut jumps in one frame; a real swing (a respawn's, a
         * mouse flick) builds up over a few. Cutscene shot changes of 31-32
         * degrees and 170-260 units got past the 45 / 500 limits alone
         * (TrainingLevel 2, 2026-11-25). */
        int sudden = deg > SUDDEN_DEG && deg > SUDDEN_X * s_prev_deg;
        int jump = move > SUDDEN_UNITS && move > SUDDEN_X * s_prev_move;
        const char *cut = !s_have_last ? NULL
                        : level != s_last_level ? "another level"
                        : cut_flag != s_last_cut ? "cutscene on / off"
                        : dt > CUT_SEC ? "frame time over 0.2 s"
                        : deg > CUT_DEG ? "turned over 45 degrees"
                        : move > CUT_UNITS ? "moved over 500 units"
                        : dt <= 0.0 ? "no time since the last frame"
                        : sudden ? "sudden turn"
                        : jump ? "sudden move" : NULL;
        int first = !s_have_last;
        memcpy(s_last_view, view, sizeof view);
        s_have_last = 1;
        s_last_level = level;
        s_last_cut = cut_flag;
        s_last_t = now;
        /* After a sudden one the next frame is measured against it, so a
         * swing that goes on is blurred from its second frame. */
        s_prev_deg = first || (cut && !sudden && !jump) ? 0.0 : deg;
        s_prev_move = first || (cut && !sudden && !jump) ? 0.0 : move;
        if (first || cut) {
            if (cut)
                note_cut(cut, cut[0] == 'f' ? dt : deg, move);
            note_frame(0, 0.0, 0.0);
            return 0;
        }
    }

    /* The game moves its camera by a steady step a frame while the time
     * between frames jitters (7-20 ms at 60 fps, field 2026-11-25): over the
     * raw time the turn speed and the smear jumped +-25 % a frame. Over the
     * frame time averaged across ~10 frames they follow the camera. */
    s_dt_avg = s_dt_avg > 0.0 ? s_dt_avg + 0.1 * (dt - s_dt_avg) : dt;
    dps = deg / s_dt_avg;
    share = g_strength / 100.0 * (1.0 / 30.0) / s_dt_avg;
    amount = mode == 2 ? share : share * smooth(g_lo, g_hi, dps);
    if (dev_log(DLOG_MBLUR) >= 2)
        fprintf(stderr, "[MBLUR] t=%ld %.2f deg in %.1f ms (%.1f averaged) ="
                " %.0f deg/s, moved %.1f: share %.3f\n", now_ms(), deg,
                dt * 1000.0, s_dt_avg * 1000.0, dps, move, amount);
    if (!(amount > 1e-4)) {
        note_frame(0, dps, 0.0);
        return 0;
    }
    for (i = 0; i < 4; i++)
        prm[i] = f2u((float)q[i]);
    for (i = 0; i < 3; i++)
        prm[4 + i] = f2u((float)t[i]);
    prm[7] = MEM32(0x0076F7ACu);                /* near */
    prm[8] = MEM32(0x0076F7B0u);                /* far */
    prm[9] = MEM32(cam + 0x90u);                /* projection m00 */
    prm[10] = MEM32(cam + 0xA4u);               /* m11 */
    prm[11] = f2u((float)amount);
    prm[12] = f2u(g_max);
    prm[13] = f2u(g_middle);
    prm[14] = mode == 2 ? 1u : 0u;              /* NV_HOST_MBLUR_VIEW */
    note_frame(1, dps, amount);
    return 1;
}
