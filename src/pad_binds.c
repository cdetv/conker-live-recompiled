/**
 * Controller binds (pad_binds.h; to-do 39's remapping, the user's design
 * and defaults 2026-11-17): single player's actions on slot 1's controller,
 * Classic and Modern apart, each pressing the game's own button for it.
 *
 * How a reading becomes the game's pad (pad_binds_step), as the keyboard's
 * rows do it (controls.c apply_set):
 *  - An action's button held presses the game's button for it: Jump A,
 *    Weapon / Context B, Reload X, the head-cam the left trigger (held, or
 *    toggled by a tap), Zoom the right stick click, Pause Start; Crouch and
 *    Attack both the right trigger, and on foot in Classic with the two on
 *    different buttons only the one that applies (Attack with a weapon out,
 *    Crouch without). Modern on foot hands Context, Aim, Attack and Crouch
 *    to its own logic (recomp_manual.c modern_poll), as before.
 *  - Swimming and the bat: their actions work only there and take their
 *    buttons there (a button bound to Swim down underwater is only that);
 *    there they alone press what the game reads for them (underwater A and
 *    B, at the surface and the bat on the ground B, the bat flying A, B and
 *    the triggers).
 *  - Y, LB, RB, the left stick click, Back and the D-pad no action has stay
 *    themselves (Back and the D-pad can't be picked: chords and the
 *    Unlocked Camera's zoom use them). A, B, X, the triggers, the right
 *    stick click and Start come only from actions: one moved away does
 *    nothing where it was.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "pad_binds.h"
#include "chords.h"
#include "settings.h"
#include "dev_logs.h"

#define TRIGGER_ON 30            /* XBOX_ANALOG_BUTTON_THRESHOLD */

/* ---- buttons ---------------------------------------------------------- */

static const struct {
    unsigned bit;
    const char *name, *label;
} s_buttons[] = {
    { CHORD_A, "a", "A" },
    { CHORD_B, "b", "B" },
    { CHORD_X, "x", "X" },
    { CHORD_Y, "y", "Y" },
    { CHORD_LB, "lb", "LB" },
    { CHORD_RB, "rb", "RB" },
    { PADB_LT, "lt", "Left trigger" },
    { PADB_RT, "rt", "Right trigger" },
    { CHORD_LS, "ls", "Left stick click" },
    { CHORD_RS, "rs", "Right stick click" },
    { CHORD_START, "start", "Start" },
};
#define NUM_BUTTONS ((int)(sizeof(s_buttons) / sizeof(s_buttons[0])))

int pad_binds_choices(void)
{
    return NUM_BUTTONS;
}

unsigned pad_binds_choice(int i)
{
    return i >= 0 && i < NUM_BUTTONS ? s_buttons[i].bit : 0u;
}

const char *pad_binds_button_name(unsigned bit)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
        if (s_buttons[i].bit == bit)
            return s_buttons[i].name;
    return "none";
}

const char *pad_binds_button_label(unsigned bit)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
        if (s_buttons[i].bit == bit)
            return s_buttons[i].label;
    return "-";
}

unsigned pad_binds_parse(const char *name)
{
    if (!name || !*name || !_stricmp(name, "none"))
        return 0u;
    for (int i = 0; i < NUM_BUTTONS; i++)
        if (!_stricmp(s_buttons[i].name, name))
            return s_buttons[i].bit;
    return ~0u;
}

/* ---- actions ---------------------------------------------------------- */

/* Where an action works: the main list everywhere but where a section's
 * actions take over; a section's only in its place (pad_binds.h's PADB_*
 * places, + 1; 0 = the main list). */
static const struct {
    const char *name;             /* settings.ini: pad_<scheme>_<name> */
    const char *label[2];         /* Classic, Modern; NULL = not in it */
    int place;
} s_actions[PADB_ACTIONS] = {
    { "jump", { "Jump", "Jump" }, 0 },
    { "crouch", { "Crouch (hold)", "Crouch (hold)" }, 0 },
    { "context", { "Weapon out / away, context action", "Context action" }, 0 },
    { "aim", { NULL, "Aim weapon (hold)" }, 0 },
    { "attack", { "Attack (while aiming)", "Attack (while aiming)" }, 0 },
    { "reload", { "Reload", "Reload" }, 0 },
    { "head_cam_hold", { "Head-cam (hold)", "Head-cam (hold)" }, 0 },
    { "head_cam_toggle", { "Head-cam (toggle)", "Head-cam (toggle)" }, 0 },
    { "zoom", { "Zoom (Unlocked Camera: reset)", "Zoom (Unlocked Camera: reset)" }, 0 },
    { "pause", { "Pause", "Pause" }, 0 },
    { "dive", { "Dive under", "Dive under" }, PADB_SURFACE + 1 },
    { "swim_up", { "Swim up", "Swim up" }, PADB_UNDER + 1 },
    { "swim_down", { "Swim down", "Swim down" }, PADB_UNDER + 1 },
    { "bat_launch", { "Launch", "Launch" }, PADB_BAT_GROUND + 1 },
    { "bat_up", { "Fly up", "Fly up" }, PADB_BAT_AIR + 1 },
    { "bat_down", { "Fly down", "Fly down" }, PADB_BAT_AIR + 1 },
    { "bat_poop", { "Poop", "Poop" }, PADB_BAT_AIR + 1 },
    { "bat_aim_toggle", { "Aim (toggle)", "Aim (toggle)" }, PADB_BAT_AIR + 1 },
    { "bat_aim_hold", { "Aim (hold)", "Aim (hold)" }, PADB_BAT_AIR + 1 },
};

static char s_key[2][PADB_ACTIONS][40];

const char *pad_binds_key(int action, int modern)
{
    int m = modern ? 1 : 0;
    if (action < 0 || action >= PADB_ACTIONS || !s_actions[action].label[m])
        return NULL;
    if (!s_key[m][action][0])
        snprintf(s_key[m][action], sizeof(s_key[0][0]), "pad_%s_%s",
                 m ? "modern" : "classic", s_actions[action].name);
    return s_key[m][action];
}

const char *pad_binds_label(int action, int modern)
{
    return action >= 0 && action < PADB_ACTIONS
           ? s_actions[action].label[modern ? 1 : 0] : NULL;
}

const char *pad_binds_name(int action)
{
    return action >= 0 && action < PADB_ACTIONS ? s_actions[action].name : NULL;
}

int pad_binds_place(int action)
{
    return action >= 0 && action < PADB_ACTIONS ? s_actions[action].place : 0;
}

static unsigned bind_of(const char *key, int def)
{
    unsigned b;
    if (!key)
        return 0u;
    b = pad_binds_parse(def ? settings_default(key) : settings_get(key));
    return b == ~0u ? 0u : b;
}

/* As last applied, for the controller's thread. */
static volatile LONG s_bind[2][PADB_ACTIONS];

unsigned pad_binds_get(int action, int modern)
{
    return action >= 0 && action < PADB_ACTIONS
           ? (unsigned)s_bind[modern ? 1 : 0][action] : 0u;
}

unsigned pad_binds_default(int action, int modern)
{
    return bind_of(pad_binds_key(action, modern), 1);
}

int pad_binds_required(int action)
{
    return action == PADB_PAUSE;
}

int pad_binds_clashes(int action, int modern, unsigned button, int *out,
                      int max)
{
    int n = 0;
    if (!button || !pad_binds_key(action, modern))
        return 0;
    for (int b = 0; b < PADB_ACTIONS; b++) {
        int pa = s_actions[action].place, pb = s_actions[b].place;
        if (b == action || !pad_binds_key(b, modern)
            || bind_of(pad_binds_key(b, modern), 0) != button)
            continue;
        if ((pa == 0) != (pb == 0) || (pa && pa != pb))
            continue;              /* a section's and the main list's, or two places */
        if ((action == PADB_CROUCH && b == PADB_ATTACK)
            || (action == PADB_ATTACK && b == PADB_CROUCH))
            continue;              /* a weapon out says which */
        if (n < max)
            out[n] = b;
        n++;
    }
    return n;
}

void pad_binds_apply(void)
{
    static char said[2][512];
    for (int m = 0; m < 2; m++) {
        char line[512];
        size_t len;
        snprintf(line, sizeof(line), "%s:", m ? "modern" : "classic");
        len = strlen(line);
        for (int a = 0; a < PADB_ACTIONS; a++) {
            const char *key = pad_binds_key(a, m);
            unsigned b = bind_of(key, 0);
            InterlockedExchange(&s_bind[m][a], (LONG)b);
            if (key && b != bind_of(key, 1)) {
                snprintf(line + len, sizeof(line) - len, " %s=%s",
                         s_actions[a].name, pad_binds_button_name(b));
                len = strlen(line);
            }
        }
        if (len == strlen(m ? "modern:" : "classic:"))
            snprintf(line + len, sizeof(line) - len, " defaults");
        if (strcmp(line, said[m])) {
            strcpy_s(said[m], sizeof(said[m]), line);
            fprintf(stderr, "[PADBINDS] %s\n", line);
        }
    }
}

/* ---- a reading -------------------------------------------------------- */

static int s_toggle_was, s_latch;

void pad_binds_idle(int keep)
{
    if (!keep)
        s_latch = 0;
}

static void names(char *out, size_t size, unsigned bits, unsigned char lt,
                  unsigned char rt)
{
    static const struct { unsigned bit; const char *name; } extra[] = {
        { CHORD_BACK, "back" }, { CHORD_UP, "up" }, { CHORD_DOWN, "down" },
        { CHORD_LEFT, "left" }, { CHORD_RIGHT, "right" },
    };
    size_t len;
    out[0] = '\0';
    for (int i = 0; i < NUM_BUTTONS; i++)
        if (bits & s_buttons[i].bit & ~(PADB_LT | PADB_RT)) {
            len = strlen(out);
            snprintf(out + len, size - len, " %s", s_buttons[i].name);
        }
    for (int i = 0; i < (int)(sizeof(extra) / sizeof(extra[0])); i++)
        if (bits & extra[i].bit) {
            len = strlen(out);
            snprintf(out + len, size - len, " %s", extra[i].name);
        }
    len = strlen(out);
    if (lt)
        snprintf(out + len, size - len, " lt%u", lt), len = strlen(out);
    if (rt)
        snprintf(out + len, size - len, " rt%u", rt);
    if (!out[0])
        snprintf(out, size, " -");
}

/* What these buttons press (no state: the head-cam toggle's latch and the
 * held-back buttons are pad_binds_step's). *toggle: the head-cam toggle's
 * button is held. */
static void map(const PadBindsIn *in, unsigned p, PadBindsOut *o, int *toggle)
{
    const int m = in->modern ? 1 : 0, here = in->place + 1;
    const unsigned pass = CHORD_Y | CHORD_LB | CHORD_RB | CHORD_LS | CHORD_BACK
                        | CHORD_UP | CHORD_DOWN | CHORD_LEFT | CHORD_RIGHT;
    unsigned bind[PADB_ACTIONS], bound = 0, taken = 0, own, g = 0;
    int held[PADB_ACTIONS];
    unsigned lt = 0, rt = 0;

    memset(o, 0, sizeof(*o));
    for (int a = 0; a < PADB_ACTIONS; a++) {
        bind[a] = s_actions[a].label[m] ? (unsigned)s_bind[m][a] : 0u;
        bound |= bind[a];
        if (s_actions[a].place == here)
            taken |= bind[a];
    }
    for (int a = 0; a < PADB_ACTIONS; a++)
        held[a] = bind[a] && (p & bind[a])
                  && (s_actions[a].place ? s_actions[a].place == here
                                         : !(bind[a] & taken));
#define LEVEL(a) (bind[a] == PADB_LT ? in->lt : bind[a] == PADB_RT ? in->rt : 255u)
#define RAISE(v, a) do { unsigned l_ = LEVEL(a); if (l_ > (v)) (v) = l_; } while (0)

    /* What a section alone presses where it works (the bat on the ground:
     * only Launch presses B, the game's launch). */
    own = in->place == PADB_SURFACE || in->place == PADB_BAT_GROUND ? CHORD_B
        : in->place == PADB_UNDER ? CHORD_A | CHORD_B
        : in->place == PADB_BAT_AIR ? CHORD_A | CHORD_B | PADB_LT | PADB_RT : 0u;

    /* The main list. */
    if (held[PADB_JUMP] && !(own & CHORD_A))
        g |= CHORD_A;
    if (held[PADB_RELOAD])
        g |= CHORD_X;
    if (held[PADB_ZOOM])
        g |= CHORD_RS;
    if (held[PADB_PAUSE])
        g |= CHORD_START;
    if (in->modern_foot) {
        o->ctx = held[PADB_CONTEXT];
        o->aim = held[PADB_AIM];
        o->attack = held[PADB_ATTACK];
        o->crouch = held[PADB_CROUCH];
    } else {
        int c = held[PADB_CROUCH], at = held[PADB_ATTACK];
        if (held[PADB_CONTEXT] && !(own & CHORD_B))
            g |= CHORD_B;
        if (in->foot && !in->modern && bind[PADB_CROUCH] != bind[PADB_ATTACK]) {
            if (in->weapon_out)
                c = 0;
            else
                at = 0;
        }
        if (!(own & PADB_RT)) {
            if (c)
                RAISE(rt, PADB_CROUCH);
            if (at)
                RAISE(rt, PADB_ATTACK);
        }
        /* Modern's Aim blocks in The Vault's suit (as the keyboard's). */
        if (in->suit && held[PADB_AIM])
            RAISE(lt, PADB_AIM);
    }
    /* The head-cam held (the toggle's latch: pad_binds_step). The bat
     * flying has the left trigger for its aim. */
    *toggle = held[PADB_HEADCAM_TOGGLE] && !(own & PADB_LT);
    if (held[PADB_HEADCAM_HOLD] && !(own & PADB_LT))
        RAISE(lt, PADB_HEADCAM_HOLD);

    /* The sections, where they work (held says so). */
    if (held[PADB_DIVE] || held[PADB_SWIM_UP] || held[PADB_BAT_LAUNCH]
        || held[PADB_BAT_UP])
        g |= CHORD_B;
    if (held[PADB_SWIM_DOWN] || held[PADB_BAT_DOWN])
        g |= CHORD_A;
    if (held[PADB_BAT_POOP])
        RAISE(rt, PADB_BAT_POOP);
    o->bat_toggle = held[PADB_BAT_AIM_TOGGLE];
    o->bat_hold = held[PADB_BAT_AIM_HOLD];

    /* Buttons no action has and the game reads as themselves. */
    g |= p & pass & ~bound;

    o->buttons = g;
    o->lt = (unsigned char)lt;
    o->rt = (unsigned char)rt;
#undef LEVEL
#undef RAISE
}

/* Does button b alone do something else in now than in was? */
static int changes(const PadBindsIn *was, const PadBindsIn *now, unsigned b)
{
    PadBindsOut x, y;
    int tx, ty;
    map(was, b, &x, &tx);
    map(now, b, &y, &ty);
    return x.buttons != y.buttons || (x.lt > 0) != (y.lt > 0)
        || (x.rt > 0) != (y.rt > 0) || x.ctx != y.ctx || x.aim != y.aim
        || x.attack != y.attack || x.crouch != y.crouch
        || x.bat_toggle != y.bat_toggle || x.bat_hold != y.bat_hold
        || tx != ty;
}

void pad_binds_step(const PadBindsIn *in, PadBindsOut *o)
{
    static const char *const place_names[] = {
        "on foot", "surface", "underwater", "bat ground", "bat flying",
    };
    static PadBindsIn last;
    static int have_last;
    static unsigned held_back;
    static unsigned was_in, was_out;
    static unsigned char was_lt, was_rt;
    static int was_place = -1;
    PadBindsIn use = *in;
    int toggle;

    /* The place or the way the buttons work changed (surface to underwater,
     * Modern's own on foot to swimming, into The Vault's suit): a button
     * held across it that now does something else is kept from the game
     * until let go. Modern's B dives at the surface and swims down under: the
     * dive's B, still held, would swim down and stop the dive. */
    if (have_last && (in->place != last.place || in->modern != last.modern
                      || in->modern_foot != last.modern_foot
                      || in->suit != last.suit)) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            unsigned b = s_buttons[i].bit;
            if ((in->buttons & b) && !(held_back & b) && changes(&last, in, b))
                held_back |= b;
        }
    }
    last = *in;
    have_last = 1;
    held_back &= in->buttons;
    use.buttons = in->buttons & ~held_back;
    if (held_back & PADB_LT)
        use.lt = 0;
    if (held_back & PADB_RT)
        use.rt = 0;
    map(&use, use.buttons, o, &toggle);

    /* The head-cam toggle: a tap on, a tap off; off where it can't be. */
    if (toggle && !s_toggle_was)
        s_latch = !s_latch;
    s_toggle_was = toggle;
    if (in->place == PADB_BAT_AIR)
        s_latch = 0;
    else if (s_latch)
        o->lt = 255u;

    if (dev_log(DLOG_PADBINDS)
        && (in->buttons != was_in || o->buttons != was_out
            || (o->lt > 0) != (was_lt > 0) || (o->rt > 0) != (was_rt > 0)
            || in->place != was_place)) {
        char a[160], b[160];
        names(a, sizeof(a), in->buttons, (in->buttons & PADB_LT) ? in->lt : 0,
              (in->buttons & PADB_RT) ? in->rt : 0);
        names(b, sizeof(b), o->buttons, o->lt, o->rt);
        fprintf(stderr, "[PADBINDS] %s, %s%s%s:%s ->%s%s%s\n",
                in->modern ? "modern" : "classic", place_names[in->place],
                in->modern_foot ? " (modern's own)" : "",
                in->foot && !in->modern && in->weapon_out ? ", weapon out" : "",
                a, b, o->ctx || o->aim || o->attack || o->crouch
                ? " (+ modern's inputs)" : "",
                held_back ? " (some held back until let go)" : "");
        was_in = in->buttons;
        was_out = o->buttons;
        was_lt = o->lt;
        was_rt = o->rt;
        was_place = in->place;
    }
}
