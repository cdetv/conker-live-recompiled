/*
 * conker.exe --pad-feed (SPEC 9.2, to-do 121): the launcher's controller
 * input. The launcher starts this as a hidden helper with its input and
 * output redirected and reads one line per change from its output:
 *
 *   pad-feed 1                  first line: the format's version
 *   conn <n> <0|1>              host pad n connected / gone
 *   pad <n> <xbox|ps> <hex>     host pad n's buttons now
 *
 * n is the toolkit's host pad: 0-3 Windows' (XInput) controllers, 4-7
 * PlayStation ones (SDL3, xboxrecomp/src/input/sdl_pads.c), the ones
 * --list-pads calls "1"-"4" and "ps1"-"ps4". The game's own pad code reads
 * them, so a PlayStation pad's buttons arrive by position in XInput's
 * layout, as the game maps them (Cross = A, Share or the touchpad = Back,
 * Options = Start), and a DualShock needs no driver of its own.
 *
 * <hex>: XInput's wButtons in the low 16 bits (0x0001 D-pad up, 0x0002
 * down, 0x0004 left, 0x0008 right, 0x0010 Start, 0x0020 Back, 0x0040 /
 * 0x0080 the stick clicks, 0x0100 LB, 0x0200 RB, 0x1000 A, 0x2000 B,
 * 0x4000 X, 0x8000 Y), and the analog parts as directions above them (the
 * PF_ values below). A stick or trigger counts as pressed past F2's
 * threshold (XInput's dead zone plus 10 % of the rest, as feed_pad in
 * src/overlay.cpp) and as released only back inside the dead zone, so one
 * resting near the threshold doesn't chatter. A pad that goes away with
 * something held reports 0 before its "conn <n> 0". PlayStation pads show
 * up about a second after the start (SDL's thread finds them), new pads
 * within a second (the toolkit's rescan).
 *
 * Reads every 8 ms. Quits when its input closes (the launcher closed it, or
 * closed or crashed itself) or its output can't be written. No window, no
 * game files, no settings and no log: nothing but the pads. With no window
 * the toolkit hears the pads whatever window is in front
 * (xbox_FramebufferInFront is true without one); the launcher acts on the
 * lines only while its own window is active.
 */

#include <windows.h>
#include <xinput.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The toolkit's (xinput_xbox.h, xinput_device.c); not included: its
 * XINPUT_STATE is Windows' here. The first read starts SDL's thread. */
DWORD xbox_InputHostPadRead(DWORD slot, XINPUT_STATE *st);

#define HOST_PADS   8       /* XBOX_INPUT_HOST_PADS */
#define XINPUT_PADS 4       /* host pads 0-3; 4-7 are PlayStation pads */

/* The analog parts as buttons, above XInput's 16 bits. */
#define PF_LT          0x00010000u
#define PF_RT          0x00020000u
#define PF_LS_UP       0x00040000u
#define PF_LS_DOWN     0x00080000u
#define PF_LS_LEFT     0x00100000u
#define PF_LS_RIGHT    0x00200000u
#define PF_RS_UP       0x00400000u
#define PF_RS_DOWN     0x00800000u
#define PF_RS_LEFT     0x01000000u
#define PF_RS_RIGHT    0x02000000u

static volatile LONG s_quit;

/* Reads the input until it closes: the launcher writes nothing, it only
 * holds the pipe open while it runs. */
static DWORD WINAPI watch_input(LPVOID arg)
{
    char buf[256];
    DWORD got;
    while (ReadFile((HANDLE)arg, buf, sizeof(buf), &got, NULL) && got > 0)
        ;
    InterlockedExchange(&s_quit, 1);
    return 0;
}

/* One line, written whole. False when the output is gone. */
static bool emit(const char *fmt, ...)
{
    char line[64];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return n > 0 && fwrite(line, 1, (size_t)n, stdout) == (size_t)n
        && fflush(stdout) == 0;
}

/* One stick axis to its two direction bits. On past F2's threshold, off
 * only back inside the dead zone (prev: last report's bits). */
static unsigned axis(int v, int dz, unsigned prev, unsigned neg, unsigned pos)
{
    int on = dz + (32767 - dz) / 10;
    unsigned out = 0;

    if (v > on || (v > dz && (prev & pos)))
        out |= pos;
    if (v < -on || (v < -dz && (prev & neg)))
        out |= neg;
    return out;
}

static unsigned trigger(int v, unsigned prev, unsigned bit)
{
    int dz = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    int on = dz + (255 - dz) / 10;
    return (v > on || (v > dz && (prev & bit))) ? bit : 0;
}

static unsigned buttons(const XINPUT_GAMEPAD *g, unsigned prev)
{
    return g->wButtons
        | trigger(g->bLeftTrigger, prev, PF_LT)
        | trigger(g->bRightTrigger, prev, PF_RT)
        | axis(g->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, prev,
               PF_LS_DOWN, PF_LS_UP)
        | axis(g->sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, prev,
               PF_LS_LEFT, PF_LS_RIGHT)
        | axis(g->sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, prev,
               PF_RS_DOWN, PF_RS_UP)
        | axis(g->sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, prev,
               PF_RS_LEFT, PF_RS_RIGHT);
}

int pad_feed_run(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE timer, t;
    LARGE_INTEGER due;
    unsigned last[HOST_PADS];
    bool conn[HOST_PADS];
    DWORD slot;

    /* A window program has no console: started without its input
     * redirected (double-clicked), nothing would ever stop it. */
    if (!in || in == INVALID_HANDLE_VALUE || _fileno(stdout) < 0) {
        fputs("--pad-feed is for the launcher: it needs its input and"
              " output redirected\n", stderr);
        return 1;
    }
    memset(last, 0, sizeof(last));
    memset(conn, 0, sizeof(conn));
    if (!emit("pad-feed 1\n"))
        return 1;
    t = CreateThread(NULL, 0, watch_input, in, 0, NULL);
    if (!t)
        return 1;
    CloseHandle(t);

    /* Every 8 ms on a high-resolution timer: Sleep(8) can be 15.6 ms. */
    timer = CreateWaitableTimerExW(NULL, NULL,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = -80000;          /* 100 ns units */

    while (!s_quit) {
        for (slot = 0; slot < HOST_PADS; slot++) {
            const char *type = slot < XINPUT_PADS ? "xbox" : "ps";
            XINPUT_STATE st;
            bool c = xbox_InputHostPadRead(slot, &st) == ERROR_SUCCESS;
            unsigned now = c ? buttons(&st.Gamepad, last[slot]) : 0;
            bool ok = true;

            if (c && !conn[slot])
                ok = emit("conn %lu 1\n", (unsigned long)slot);
            if (ok && now != last[slot])
                ok = emit("pad %lu %s %X\n", (unsigned long)slot, type, now);
            if (ok && !c && conn[slot])
                ok = emit("conn %lu 0\n", (unsigned long)slot);
            if (!ok)
                return 0;           /* the launcher is gone */
            conn[slot] = c;
            last[slot] = now;
        }
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(8);
    }
    return 0;
}
