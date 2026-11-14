/**
 * The command pipe (to-do 174, --debug only; SPEC 10.3).
 *
 * A local named pipe, \\.\pipe\conker-console, through which any program on
 * this PC runs console commands as if typed: same console_run, same
 * [CONSOLE] lines in the log, same replies. It replaces posting keys to the
 * window (needs the window's focus and timing, loses keys, can take the
 * user's keyboard) and works with the game behind other windows.
 *
 * Protocol (text, one connection at a time, as many lines as it likes):
 *   client: a command line, ended by '\n' ("\r\n" works too)
 *   game:   everything the console printed for it, a line each, then
 *           "= ok" or "= error" (some line of it was an error)
 * A reply line that would start with '=' gets a space in front, so the end
 * line can't be mistaken. Commands that take time (wait, waitload, exec,
 * shot) answer when they are done. An empty line answers "= ok".
 *
 * The name: RECOMP_CMD_PIPE=<name> (without \\.\pipe\), else conker-console.
 * Taken already (another game with --debug), it is <name>-<pid>; the
 * [PIPE] line in the log says which. Script runs set a name of their own so
 * they never reach the user's game.
 *
 * Threads: this one blocks on the pipe; it hands a line to the executor
 * (cmd_pipe_take in the console's frame) and waits for cmd_pipe_done. Only
 * this PC: PIPE_REJECT_REMOTE_CLIENTS.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd_pipe.h"

#define LINE_MAX_ 1024

static CRITICAL_SECTION s_cs;
static HANDLE s_done_ev;
static int s_state;             /* 0 idle, 1 a line waits, 2 taken, 3 answered */
static char s_line[LINE_MAX_];
static char *s_reply;
static int s_errors;

int cmd_pipe_take(char *line, int size)
{
    int got = 0;

    if (!s_done_ev || s_state != 1)
        return 0;
    EnterCriticalSection(&s_cs);
    if (s_state == 1) {
        strncpy_s(line, (size_t)size, s_line, _TRUNCATE);
        s_state = 2;
        got = 1;
    }
    LeaveCriticalSection(&s_cs);
    return got;
}

void cmd_pipe_done(const char *reply, int errors)
{
    size_t n = strlen(reply);

    EnterCriticalSection(&s_cs);
    free(s_reply);
    s_reply = (char *)malloc(n + 1);
    if (s_reply)
        memcpy(s_reply, reply, n + 1);
    s_errors = errors;
    s_state = 3;
    LeaveCriticalSection(&s_cs);
    SetEvent(s_done_ev);
}

static int write_all(HANDLE h, const char *p, size_t n)
{
    while (n) {
        DWORD put = 0;
        if (!WriteFile(h, p, (DWORD)(n > 65536 ? 65536 : n), &put, NULL) || !put)
            return 0;
        p += put;
        n -= put;
    }
    return 1;
}

/* One line from the client: run it, send the answer. 0 = the client is gone. */
static int serve_line(HANDLE h, const char *line)
{
    char *reply = NULL;
    int errors = 0, ok = 1;

    if (line[0]) {
        EnterCriticalSection(&s_cs);
        strncpy_s(s_line, sizeof(s_line), line, _TRUNCATE);
        s_state = 1;
        LeaveCriticalSection(&s_cs);
        WaitForSingleObject(s_done_ev, INFINITE);
        EnterCriticalSection(&s_cs);
        reply = s_reply;
        s_reply = NULL;
        errors = s_errors;
        s_state = 0;
        LeaveCriticalSection(&s_cs);
    }
    if (reply) {
        const char *p = reply;
        while (*p && ok) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (*p == '=')
                ok = write_all(h, " ", 1);
            ok = ok && write_all(h, p, len) && write_all(h, "\n", 1);
            p += len + (nl ? 1 : 0);
        }
        free(reply);
    }
    ok = ok && write_all(h, errors ? "= error\n" : "= ok\n",
                         errors ? 8 : 5);
    return ok;
}

static HANDLE make_pipe(const char *name, int first)
{
    char path[256];

    snprintf(path, sizeof(path), "\\\\.\\pipe\\%s", name);
    return CreateNamedPipeA(path,
                            PIPE_ACCESS_DUPLEX
                            | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                            | PIPE_REJECT_REMOTE_CLIENTS,
                            1, 64 * 1024, 64 * 1024, 0, NULL);
}

static DWORD WINAPI pipe_thread(void *arg)
{
    const char *env = getenv("RECOMP_CMD_PIPE");
    char name[128], line[LINE_MAX_], buf[4096];
    size_t n = 0;
    HANDLE h;

    (void)arg;
    snprintf(name, sizeof(name), "%s", env && *env ? env : "conker-console");
    h = make_pipe(name, 1);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char base[96];
        snprintf(base, sizeof(base), "%s", name);
        snprintf(name, sizeof(name), "%s-%lu", base, GetCurrentProcessId());
        fprintf(stderr, "[PIPE] \\\\.\\pipe\\%s is taken (error %lu: another game "
                "with --debug?)\n", base, e);
        h = make_pipe(name, 1);
    }
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[PIPE] no command pipe (error %lu)\n", GetLastError());
        return 0;
    }
    fprintf(stderr, "[PIPE] commands on \\\\.\\pipe\\%s\n", name);
    fflush(stderr);
    for (;;) {
        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            Sleep(100);
            continue;
        }
        n = 0;
        for (;;) {
            DWORD got = 0;
            int alive = 1;
            if (!ReadFile(h, buf, sizeof(buf), &got, NULL) || !got)
                break;
            for (DWORD i = 0; i < got && alive; i++) {
                char c = buf[i];
                if (c == '\n') {
                    while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
                        n--;
                    line[n] = '\0';
                    alive = serve_line(h, line);
                    n = 0;
                } else if (n < sizeof(line) - 1) {
                    line[n++] = c;
                }
            }
            if (!alive)
                break;
        }
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
    }
}

void cmd_pipe_start(void)
{
    HANDLE t;

    if (s_done_ev)
        return;
    InitializeCriticalSection(&s_cs);
    s_done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}
