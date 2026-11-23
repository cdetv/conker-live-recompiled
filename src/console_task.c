/**
 * Console task and cutscene seek (to-do 245; --debug only, SPEC 10.3).
 *
 * task: the level scripts the game has loaded (its "tasks"), their state,
 * one task's variables (BSS) and its bytecode, disassembled as
 * docs/research/berri/r-berri-tasks.md describes the machine (0x0035E210).
 * cutscene seek: jumps the cutscene playing to a time with the game's own
 * seek, [0x0076F0A8] (what a speech start uses to keep the scene on its
 * voice).
 *
 * Commands run on the executor while the game thread runs on (console.c), so
 * everything here only reads guest memory through conker_Peek (an address
 * that isn't mapped reads as nothing, never a crash) and the seek is one
 * aligned dword the game thread picks up at its next cutscene frame. Kept
 * apart from console.c so the console's own edits and this stay apart.
 *
 * What the game does with a seek (Ghidra, 0x00055F20 at the end of the
 * cutscene event pass 0x00056050, per playing slot): the events this frame
 * found run unless they lie past the target; then the clock [0x0076F0F8 +
 * slot*4] = target, the one before it [0x0076F0F0 + slot*4] = target - 1,
 * seconds [0x00847118] = target / ticks a second, and the seek is -1 again.
 * So the next frame runs the events from the target on: a seek forward skips
 * what lies between (sounds, a weapon drawn, subtitles), a seek back plays
 * them again. The voice is a streamed sound of its own: it plays on from
 * where it was (and its lip sync with it), unless the seek goes back over
 * its start, which starts it again. A scene's start clears a seek left over
 * (cutscene_start 0x0004EC10 -> cutscene_state_reset 0x0004E9E0).
 */

#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "debug_tools.h"
#include "warp.h"

#define reply console_reply
#define complain console_complain

/* ---- guest memory ------------------------------------------------------------ */

static uint32_t rd32(uint32_t va, int *ok)
{
    uint32_t v = 0;
    if (!conker_Peek(va, &v, 4))
        *ok = 0;
    return v;
}

static uint8_t rd8(uint32_t va)
{
    uint8_t v = 0;
    conker_Peek(va, &v, 1);
    return v;
}

static float rdf(uint32_t va)
{
    float v = 0.0f;
    conker_Peek(va, &v, 4);
    return v;
}

/* ---- the task list ------------------------------------------------------------- */

#define TASK_HEAD     0x00801904u   /* first task; next +0x34 (sorted by id) */
#define TASK_COUNT    0x00801900u   /* u16 */
#define TASK_STATE    0x00801910u   /* a byte per id: 1 ran, 2 waits for its level, 3 done */
#define PROGRESS      0x0084B290u   /* -> the done bits, one per id */
#define CUR_LEVEL     0x007FA1F8u
#define MAX_TASKS     256
#define HDR           0x7C          /* header; the code starts here (event 0) */
#define MAX_BLOCK     0x10000       /* the biggest task file is 19,592 bytes */

/* A task's header (= its file's first 0x7C bytes, filled in at load). */
typedef struct {
    uint32_t at;            /* where it sits */
    uint32_t flags;         /* low 10 bits = id */
    uint32_t level;         /* +0x04 */
    uint32_t size;          /* +0x08 code + data (at run time the whole block) */
    uint32_t bss;           /* +0x0C variables' size */
    uint32_t data;          /* +0x10 data offset, 0 = none */
    uint32_t bssoff;        /* +0x14 */
    uint32_t ev[6];         /* +0x18 events 1-6' entries, 0 = none */
    char name[65];          /* +0x3C */
} Task;

static int task_read(uint32_t at, Task *t, uint32_t *next)
{
    uint8_t h[HDR];
    if (!conker_Peek(at, h, HDR))
        return 0;
    memset(t, 0, sizeof(*t));
    t->at = at;
    memcpy(&t->flags, h + 0x00, 4);
    memcpy(&t->level, h + 0x04, 4);
    memcpy(&t->size, h + 0x08, 4);
    memcpy(&t->bss, h + 0x0C, 4);
    memcpy(&t->data, h + 0x10, 4);
    memcpy(&t->bssoff, h + 0x14, 4);
    memcpy(t->ev, h + 0x18, sizeof(t->ev));
    memcpy(t->name, h + 0x3C, 64);
    t->name[64] = 0;
    for (char *c = t->name; *c; c++)
        if (*c < 0x20 || *c > 0x7E)
            *c = '?';
    memcpy(next, h + 0x34, 4);
    return 1;
}

/* Every loaded task, in the game's order; returns how many (at most max). */
static int task_list(Task *out, int max)
{
    int ok = 1, n = 0;
    uint32_t at = rd32(TASK_HEAD, &ok), next;
    uint16_t count = 0;

    conker_Peek(TASK_COUNT, &count, 2);
    while (ok && at && n < max && n < (int)count + 8) {
        if (!task_read(at, &out[n], &next))
            break;
        n++;
        at = next;
    }
    return n;
}

static int task_id(const Task *t)
{
    return (int)(t->flags & 0x3FFu);
}

static const char *state_name(int s)
{
    switch (s) {
    case 0: return "-";
    case 1: return "runs";
    case 2: return "waits";     /* bound to a level that isn't this one */
    case 3: return "done";
    default: return "?";
    }
}

static int task_done(int id)
{
    int ok = 1;
    uint32_t bits = rd32(PROGRESS, &ok);
    return ok && bits && (rd8(bits + (uint32_t)id / 8u) >> (id & 7)) & 1;
}

static void flags_text(const Task *t, char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
#define ADD(...) do { if (n < size) n += (size_t)snprintf(out + n, size - n, __VA_ARGS__); } while (0)
    if (t->flags & 0x8000u)
        ADD("%sfinished", n ? ", " : "");
    if (t->flags & 0x4000u)
        ADD("%sstart due", n ? ", " : "");
    if (t->flags & 0x2000u)
        ADD("%sonly in level 0x%02X", n ? ", " : "", t->level & 0xFFu);
    else if (t->flags & 0x1000u)
        ADD("%sleaves with its level", n ? ", " : "");
    if (t->flags & 0x0800u)
        ADD("%skeeps its BSS", n ? ", " : "");
    if (t->flags & 0x0400u)
        ADD("%sno start event", n ? ", " : "");
    if (!n)
        ADD("stays loaded");
#undef ADD
}

/* text holds part, case aside. */
static int has_part(const char *text, const char *part)
{
    size_t n = strlen(part);
    for (; *text; text++)
        if (!_strnicmp(text, part, n))
            return 1;
    return 0;
}

/* By id (a number) or name (the whole name, else its start, else a part of
 * it, case aside). Complains and returns -1 when none or several. */
static int task_find(const Task *list, int n, const char *arg)
{
    char *end;
    long id = strtol(arg, &end, 0);
    int found = -1, count = 0;

    if (!*end) {
        for (int i = 0; i < n; i++)
            if (task_id(&list[i]) == id)
                return i;
        complain("No task %ld loaded (task lists them)", id);
        return -1;
    }
    for (int pass = 0; pass < 3 && found < 0; pass++) {
        count = 0;
        for (int i = 0; i < n; i++) {
            const char *nm = list[i].name;
            int hit = pass == 0 ? !_stricmp(nm, arg)
                    : pass == 1 ? !_strnicmp(nm, arg, strlen(arg))
                    : has_part(nm, arg);
            if (hit) {
                if (!count)
                    found = i;
                count++;
            }
        }
        if (count > 1) {
            complain("%d tasks have %s in their name: give its id (task lists them)", count, arg);
            return -1;
        }
    }
    if (found < 0)
        complain("No task called %s loaded (task lists them)", arg);
    return found;
}

/* The whole block (header, code, data, BSS) into a new buffer. */
static uint8_t *task_block(const Task *t, uint32_t *len)
{
    uint32_t total = t->size;
    uint8_t *b;

    if (t->bssoff && t->bssoff + t->bss > total)
        total = t->bssoff + t->bss;
    if (total < HDR)
        total = HDR;
    if (total > MAX_BLOCK)
        total = MAX_BLOCK;
    b = (uint8_t *)calloc(1, total);
    if (!b)
        return NULL;
    /* Page by page: one part not mapped leaves zeros, not nothing. */
    for (uint32_t o = 0; o < total; o += 0x400) {
        uint32_t k = total - o < 0x400 ? total - o : 0x400;
        conker_Peek(t->at + o, b + o, k);
    }
    *len = total;
    return b;
}

/* Where the code ends: the data, else the BSS, else the block's end. */
static uint32_t code_end(const Task *t, uint32_t len)
{
    uint32_t e = t->data ? t->data : t->bssoff ? t->bssoff : t->size;
    return e > len ? len : e;
}

/* ---- the bytecode (0x0035E210; r-berri-tasks.md "The machine") ---------------- */

static const signed char s_nops[33] = {
    0, 3, 3, 3, 3, 3, 2, 2, 2, 3, 3, 3, 3, 3, 3, 2, 1, 2, 2, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0, 3, 3, 1
};
static const char *const s_ops[33] = {
    "END", "ADD", "SUB", "MUL", "DIV", "MOD", "NEG", "NOT", "BNOT", "BAND", "BOR", "BXOR", "SHL",
    "SHR", "LAND", "COPY", "JMP", "JZ", "JNZ", "LT", "GT", "LE", "GE", "EQ", "NE", "PUSH", "CALL",
    "RETV", "ENTER", "RET", "LOAD", "STORE", "LINE"
};
/* Library calls (CALL with an immediate). */
static const char *lib_name(int v)
{
    static const char *const n[16] = {
        "stub", "stub", "stub", "CREATE", "DESTROY", "ASK", "TELL", "random", "byte7FA250",
        "timer+", "timer-", "mul16", "div16", "f35E0D0", "ticks/s", "bssoff"
    };
    return v >= 0 && v < 16 ? n[v] : NULL;
}
static const char *entity_type(int t)
{
    static const char *const n[16] = {
        "t0", "cutscene", "char", "object", "zone", "task", "audio", "sound", "fx8", "camera",
        "music", "clock", "t12", "t13", "effects", "game"
    };
    return n[t & 15];
}
static const char *const s_event_names[7] = {
    "load", "level load", "event 2", "start", "every frame", "end", "done at load"
};

typedef struct { int mode; int32_t v; } Operand;

static int imm_mode(int m)
{
    return m == 3 || m == 4 || m == 6;
}

static void operand_text(const Operand *o, char *out, size_t size)
{
    switch (o->mode) {
    case 0: snprintf(out, size, "fp%+d", (int)o->v); break;
    case 1: snprintf(out, size, "bss[%d]", (int)o->v); break;
    case 2: snprintf(out, size, "data[%d]", (int)o->v); break;
    case 5: snprintf(out, size, "acc"); break;
    default:
        if (o->v > -1000 && o->v < 1000)
            snprintf(out, size, "#%d", (int)o->v);
        else
            snprintf(out, size, "#0x%X", (unsigned)o->v);
    }
}

/* One instruction at pos into text; returns its length in bytes, 0 when the
 * word is no opcode. stack: the PUSHes since the last call, for its note. */
static uint32_t dis_one(const uint8_t *code, uint32_t pos, uint32_t end, char *out, size_t size,
                        Operand *stack, int *nstack, int *is_line)
{
    uint16_t w;
    int op, wide, modes[3];
    uint32_t p = pos + 2;
    Operand ops[3];
    char txt[160] = "", note[200] = "", a[40];
    size_t n = 0;

    *is_line = 0;
    if (pos + 2 > end)
        return 0;
    memcpy(&w, code + pos, 2);
    op = w & 0x3F;
    wide = (w & 0x40) != 0;
    modes[0] = (w >> 13) & 7;
    modes[1] = (w >> 10) & 7;
    modes[2] = (w >> 7) & 7;
    if (op >= 33) {
        snprintf(out, size, "%05X  ???    %04X", (unsigned)pos, (unsigned)w);
        return 2;
    }
    for (int k = 0; k < s_nops[op]; k++) {
        ops[k].mode = modes[k];
        if (wide && imm_mode(modes[k])) {
            uint16_t hi, lo;
            if (p + 4 > end)
                return 0;
            memcpy(&hi, code + p, 2);
            memcpy(&lo, code + p + 2, 2);
            ops[k].v = (int32_t)(((uint32_t)hi << 16) | lo);
            p += 4;
            wide = 0;
        } else {
            int16_t v;
            if (p + 2 > end)
                return 0;
            memcpy(&v, code + p, 2);
            ops[k].v = v;
            p += 2;
        }
    }
    if (op == 32) {             /* LINE: a source line number, does nothing */
        *is_line = 1;
        return p - pos;
    }
    if (op == 16) {             /* JMP t */
        snprintf(txt, sizeof txt, "0x%X", (unsigned)(ops[0].v & 0xFFFF));
    } else if (op == 17 || op == 18) {  /* JZ / JNZ a, t */
        operand_text(&ops[0], a, sizeof a);
        snprintf(txt, sizeof txt, "%s, 0x%X", a, (unsigned)(ops[1].v & 0xFFFF));
    } else {
        for (int k = 0; k < s_nops[op]; k++) {
            operand_text(&ops[k], a, sizeof a);
            if (n < sizeof txt)
                n += (size_t)snprintf(txt + n, sizeof txt - n, "%s%s", k ? ", " : "", a);
        }
    }
    if (op == 25 && *nstack < 16) {
        stack[(*nstack)++] = ops[0];
    } else if (op == 26) {
        if (ops[0].mode == 6) {
            int v = (int)ops[0].v;
            const char *ln = lib_name(v);
            size_t m = ln ? (size_t)snprintf(note, sizeof note, "  ; %s", ln)
                          : (size_t)snprintf(note, sizeof note, "  ; lib%d", v);
            if (ln && v >= 3 && v <= 6 && *nstack) {
                /* Top of the stack = the entity, then the message and arguments. */
                const Operand *e = &stack[*nstack - 1];
                if (imm_mode(e->mode)) {
                    int t = (int)((e->v >> 12) & 0xF), id = (int)(e->v & 0xFFF);
                    if (t == 5 && id == 0xFF)
                        m += (size_t)snprintf(note + m, sizeof note - m, " task:self");
                    else
                        m += (size_t)snprintf(note + m, sizeof note - m, " %s:%d",
                                              entity_type(t), id);
                } else {
                    operand_text(e, a, sizeof a);
                    m += (size_t)snprintf(note + m, sizeof note - m, " %s", a);
                }
                for (int k = *nstack - 2; k >= 0 && m < sizeof note; k--) {
                    operand_text(&stack[k], a, sizeof a);
                    m += (size_t)snprintf(note + m, sizeof note - m, " %s", a);
                }
            }
        }
        *nstack = 0;
    } else if (op == 0 || op == 16 || op == 17 || op == 18 || op == 29) {
        *nstack = 0;
    }
    snprintf(out, size, "%05X  %-6s %s%s", (unsigned)pos, s_ops[op], txt, note);
    return p - pos;
}

/* ---- task ---------------------------------------------------------------------- */

static void task_show_list(void)
{
    static Task list[MAX_TASKS];
    int n = task_list(list, MAX_TASKS), ok = 1;
    uint32_t lv = rd32(CUR_LEVEL, &ok);
    char fl[120];

    if (!n) {
        reply("No tasks loaded");
        return;
    }
    reply("%d tasks loaded, level 0x%02X (state: runs / waits for its level / done; "
          "task <id> for one)", n, lv & 0xFFu);
    reply("   id  %-30s state  done  bss  flags", "name");
    for (int i = 0; i < n; i++) {
        const Task *t = &list[i];
        int id = task_id(t);
        flags_text(t, fl, sizeof fl);
        reply("  %3d  %-30s %-5s  %-4s %4u  %s", id, t->name,
              state_name(id < 238 ? rd8(TASK_STATE + (uint32_t)id) : 0),
              id < 238 && task_done(id) ? "yes" : "-", t->bss, fl);
    }
}

static void task_show_one(const Task *t)
{
    uint32_t len = 0;
    uint8_t *b = task_block(t, &len);
    int id = task_id(t);
    uint32_t end;
    char fl[120], line[200];
    size_t n;

    if (!b) {
        complain("No memory for the task's copy");
        return;
    }
    end = code_end(t, len);
    flags_text(t, fl, sizeof fl);
    reply("Task %d %s at 0x%08X: %s, done bit %s, flags 0x%04X (%s), level 0x%02X",
          id, t->name, t->at, state_name(id < 238 ? rd8(TASK_STATE + (uint32_t)id) : 0),
          id < 238 && task_done(id) ? "set" : "clear", t->flags & 0xFC00u, fl, t->level & 0xFFu);
    {
        uint32_t de = t->bssoff && t->bssoff > t->data ? t->bssoff : t->size;
        if (t->data && de > t->data)
            reply("  code +0x%X-0x%X (%u bytes), data +0x%X-0x%X, BSS +0x%X (%u bytes)", HDR,
                  end, end - HDR, t->data, de, t->bssoff, t->bss);
        else
            reply("  code +0x%X-0x%X (%u bytes), no data, BSS +0x%X (%u bytes)", HDR, end,
                  end - HDR, t->bssoff, t->bss);
    }
    n = (size_t)snprintf(line, sizeof line, "  events: 0 %s +0x7C", s_event_names[0]);
    for (int e = 0; e < 6 && n < sizeof line; e++)
        if (t->ev[e])
            n += (size_t)snprintf(line + n, sizeof line - n, ", %d %s +0x%X", e + 1,
                                  s_event_names[e + 1], t->ev[e]);
    reply("%s", line);
    /* The variables: 8 dwords a line, by their byte offset (bss[n] in the code). */
    if (t->bss && t->bssoff && t->bssoff < len) {
        uint32_t bss = t->bss, top = len - t->bssoff;
        if (bss > top)
            bss = top;
        if (bss > 1024)
            bss = 1024;
        reply("  BSS (bss[n] = the dword at +n):");
        for (uint32_t o = 0; o < bss; o += 32) {
            n = (size_t)snprintf(line, sizeof line, "  +%03X ", o);
            for (uint32_t k = o; k < o + 32 && k + 4 <= bss && n < sizeof line; k += 4) {
                uint32_t v;
                memcpy(&v, b + t->bssoff + k, 4);
                n += (size_t)snprintf(line + n, sizeof line - n, " %08X", v);
            }
            reply("%s", line);
        }
        if (bss < t->bss)
            reply("  (first %u of %u bytes)", bss, t->bss);
    }
    /* The data's strings (names the script passes on). */
    if (t->data && t->data < len) {
        uint32_t de = t->bssoff && t->bssoff > t->data ? t->bssoff : t->size;
        int shown = 0;
        if (de > len)
            de = len;
        n = (size_t)snprintf(line, sizeof line, "  data strings:");
        for (uint32_t o = t->data; o < de && shown < 12; ) {
            uint32_t s = o;
            while (o < de && b[o] >= 0x20 && b[o] < 0x7F)
                o++;
            if (o - s >= 4) {
                if (n + (o - s) + 4 >= sizeof line) {
                    reply("%s", line);
                    n = (size_t)snprintf(line, sizeof line, "   ");
                }
                n += (size_t)snprintf(line + n, sizeof line - n, " \"%.*s\"", (int)(o - s),
                                      (const char *)b + s);
                shown++;
            }
            o++;
        }
        if (shown)
            reply("%s", line);
    }
    reply("  task %d code [0-6 | +offset] [lines]: its bytecode", id);
    free(b);
}

/* task <id|name> code [event | +offset | 0xoffset] [lines] */
static void task_show_code(const Task *t, const char *from, const char *count)
{
    uint32_t len = 0, end, pos = HDR;
    uint8_t *b = task_block(t, &len);
    int lines = count ? atoi(count) : 200, shown = 0, is_line, nstack = 0;
    Operand stack[16];
    char out[400];

    if (!b) {
        complain("No memory for the task's copy");
        return;
    }
    if (lines < 1)
        lines = 1;
    if (lines > 3000)
        lines = 3000;
    end = code_end(t, len);
    if (from) {
        char *e;
        const char *f = from[0] == '+' ? from + 1 : from;
        long v = strtol(f, &e, from[0] == '+' ? 16 : 0);
        if (*e || v < 0) {
            complain("code wants an event (0-6) or an offset (+1A4 or 0x1A4)");
            free(b);
            return;
        }
        if (from[0] != '+' && v <= 6) {
            if (v == 0)
                pos = HDR;
            else if (!t->ev[v - 1]) {
                complain("Task %d has no event %ld (%s)", task_id(t), v, s_event_names[v]);
                free(b);
                return;
            } else
                pos = t->ev[v - 1];
        } else {
            pos = (uint32_t)v;
        }
    }
    if (pos < HDR || pos >= end) {
        complain("Task %d's code is +0x%X-0x%X", task_id(t), HDR, end);
        free(b);
        return;
    }
    reply("Task %d %s, code from +0x%X:", task_id(t), t->name, pos);
    while (pos < end && shown < lines) {
        uint32_t k;
        if (pos == HDR)
            reply("--- event 0 (%s) ---", s_event_names[0]);
        for (int e = 0; e < 6; e++)
            if (t->ev[e] && t->ev[e] == pos)
                reply("--- event %d (%s) ---", e + 1, s_event_names[e + 1]);
        k = dis_one(b, pos, end, out, sizeof out, stack, &nstack, &is_line);
        if (!k) {
            reply("%05X  (cut short)", (unsigned)pos);
            break;
        }
        if (!is_line) {
            reply("%s", out);
            shown++;
        }
        pos += k;
    }
    if (pos < end)
        reply("(more: task %d code +%X)", task_id(t), (unsigned)pos);
    free(b);
}

/* task | task list | task <id|name> | task <id|name> code [event|+offset] [lines] */
void console_cmd_task(int argc, char **argv)
{
    static Task list[MAX_TASKS];
    int n, i;

    if (conker_LevelLoading()) {
        complain("A level is loading: its tasks are changing (try again once it has loaded)");
        return;
    }
    if (argc < 2 || !_stricmp(argv[1], "list")) {
        task_show_list();
        return;
    }
    n = task_list(list, MAX_TASKS);
    if (!n) {
        complain("No tasks loaded");
        return;
    }
    i = task_find(list, n, argv[1]);
    if (i < 0)
        return;
    if (argc >= 3 && !_stricmp(argv[2], "code"))
        task_show_code(&list[i], argc >= 4 ? argv[3] : NULL, argc >= 5 ? argv[4] : NULL);
    else if (argc >= 3)
        complain("task <id|name> [code [0-6 | +offset] [lines]]");
    else
        task_show_one(&list[i]);
}

/* ---- cutscene seek ----------------------------------------------------------- */

#define CUT_ON        0x0076F0E0u   /* a byte per slot: 1 = a scene plays */
#define CUT_SCENE     0x0076F0DCu   /* a byte per slot */
#define CUT_END       0x0076F0E8u   /* ticks, a dword per slot */
#define CUT_NOW       0x0076F0F8u
#define CUT_SEEK      0x0076F0A8u   /* -1 = none */
#define CUT_TPS       0x007F9F5Cu   /* float: ticks a second (60) */
#define SKIP_MODE     0x00765601u   /* 2 = the skip is running the scene to its end */

/* cutscene seek <seconds> | +<seconds> | -<seconds> (console.c's cutscene
 * passes argv from "cutscene"). */
void console_cutscene_seek(int argc, char **argv)
{
    int slot = -1, ok = 1;
    int32_t now, end, to, pending;
    float tps;
    double s;
    char *e;
    const char *arg;

    if (argc < 3) {
        complain("cutscene seek <seconds> (or +<s> / -<s> from now)");
        return;
    }
    arg = argv[2];
    s = strtod(arg, &e);
    if (*e || e == arg || !isfinite(s)) {
        complain("cutscene seek wants seconds: 12.5, +5 or -3");
        return;
    }
    for (int i = 0; i < 2 && slot < 0; i++)
        if (rd8(CUT_ON + (uint32_t)i) == 1)
            slot = i;
    if (slot < 0) {
        complain("No cutscene playing (cutscene <level> <scene> plays one)");
        return;
    }
    if (rd8(SKIP_MODE) == 2) {
        complain("The cutscene is being skipped");
        return;
    }
    tps = rdf(CUT_TPS);
    now = (int32_t)rd32(CUT_NOW + (uint32_t)slot * 4u, &ok);
    end = (int32_t)rd32(CUT_END + (uint32_t)slot * 4u, &ok);
    pending = (int32_t)rd32(CUT_SEEK, &ok);
    if (!ok || !(tps > 1.0f) || end <= 0) {
        complain("The cutscene's clock can't be read");
        return;
    }
    if (arg[0] == '+' || arg[0] == '-')
        s += (pending >= 0 ? pending : now) / (double)tps;
    to = (int32_t)lround(s * tps);
    if (to < 0)
        to = 0;
    if (to >= end) {
        complain("Cutscene %d ends at %.1f s: seek to before that (holding the skip ends it)",
                 rd8(CUT_SCENE + (uint32_t)slot), end / tps);
        return;
    }
    if (!conker_Poke(CUT_SEEK, &to, 4)) {
        complain("The seek can't be written");
        return;
    }
    reply("Cutscene %d: %.1f -> %.1f s of %.1f at the next game frame. %s; the voice "
          "plays on from where it was", rd8(CUT_SCENE + (uint32_t)slot), now / tps, to / tps,
          end / tps, to >= now ? "What lies between is skipped (sounds, subtitles, a weapon "
          "drawn)" : "What lies between plays again");
}
