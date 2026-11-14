/**
 * Sampling profiler: where does the time go? (RECOMP_SAMPLE_PROFILE)
 *
 *   RECOMP_SAMPLE_PROFILE=<seconds>   report every <seconds> (default 10)
 *   RECOMP_SAMPLE_CALLERS=1           also: system/driver time by our caller
 *   RECOMP_SAMPLE_CALLERS=2           and our own functions' callers too
 *   RECOMP_HITCH_MS=<ms>              long frames: see "Long frames" below;
 *                                     works with or without the line above
 *
 * A background thread wakes about every millisecond, suspends each other
 * thread in the process for a moment, reads its instruction pointer and
 * resumes it. Every <seconds> it prints, per thread, the functions those
 * samples landed in -- generated guest functions (sub_XXXXXXXX), runtime
 * functions (xf_rows, ...) and system calls (NtWaitForSingleObject = waiting).
 * Then it starts counting again, so each report covers one window and a run
 * killed by a timeout still reports.
 *
 * Self time only: a sample counts for the function the thread was executing,
 * not its callers. A thread that spends its window in NtWaitFor... or
 * NtDelayExecution is idle, not busy.
 *
 * Needs the .pdb next to the .exe (CMakeLists.txt links with /DEBUG) or
 * every runtime function comes out as an address.
 *
 * ponytail: nothing here takes a lock or allocates while a thread is
 * suspended -- that thread might hold the heap or loader lock, and waiting on
 * it would freeze both of us. The sample table is a fixed array; anything
 * that needs memory (thread list, symbol lookup, the report) runs with every
 * thread running.
 */

#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PROF_MAX_THREADS 64
#define PROF_TABLE_BITS  17
#define PROF_TABLE_SIZE  (1u << PROF_TABLE_BITS)
#define PROF_TOP_PER_THREAD 8

typedef struct {
    DWORD    tid;
    HANDLE   h;
    uint64_t start;          /* thread start address, names the thread */
    uint64_t stack_base;     /* top of its stack (TEB), 0 if unknown */
    uint32_t samples;        /* this window */
} ProfThread;

typedef struct {
    uint64_t rip;
    uint64_t caller[2];      /* RECOMP_SAMPLE_CALLERS: see prof_callers */
    uint32_t slot;           /* index into s_threads, +1 (0 = empty entry) */
    uint32_t count;
} ProfEntry;

static ProfThread s_threads[PROF_MAX_THREADS];
static int s_nthreads;
static ProfEntry s_table[PROF_TABLE_SIZE];
static uint32_t s_dropped, s_ticks;
static int s_window_on;      /* RECOMP_SAMPLE_PROFILE given: [PROF] reports */
static uint32_t s_top = PROF_TOP_PER_THREAD;  /* RECOMP_SAMPLE_TOP: lines a thread */
static double s_min_pct = 0.5;                /* RECOMP_SAMPLE_MIN: smallest share printed, % */

/* ── Long frames (RECOMP_HITCH_MS) ─────────────────────────────
 *
 * A window report averages 10 s, so a 100 ms stutter every few seconds is
 * lost in it. Here the game marks each frame (sample_profiler_frame, from
 * audio_update's entry hook: once per game_frame, via frame_background_
 * update), and samples also go into a second table that is emptied at every
 * frame mark. When a frame took RECOMP_HITCH_MS or longer, that table (the
 * samples of exactly that frame, to within a tick) is printed as [HITCH]
 * lines before it is emptied. Every window also prints how long the frames
 * were (a histogram), so runs can be compared.
 *
 * Frame marks only come while audio_update runs: in levels and menus, not
 * during the opening movies. A load screen is one long frame too. */
#define PROF_FTABLE_BITS 13
#define PROF_FTABLE_SIZE (1u << PROF_FTABLE_BITS)
#define HITCH_REPORTS_PER_WINDOW 8
#define HITCH_TOP 5
static int s_hitch_ms;                   /* 0 = off */
static double s_qpc_ms;                  /* QPC ticks per ms */
static ProfEntry s_ftable[PROF_FTABLE_SIZE];
static uint32_t s_fsamples[PROF_MAX_THREADS], s_fticks, s_fdropped;
static int s_fpartial;                   /* thread slots moved this frame */
/* Written by the game thread only. */
static volatile LONG s_frame_seq;        /* frame marks so far */
static volatile LONG s_hitch_seq;        /* s_frame_seq that ended a long frame */
static volatile LONG s_hitch_us;         /* how long that frame was */
static volatile LONG s_worst_us;         /* longest frame this window */
static volatile LONG s_hist[6];          /* <40, <55, <75, <100, <200, >=200 ms */
static volatile LONG64 s_frame_qpc_sum;  /* summed frame lengths, QPC ticks */
static const char *const HITCH_BUCKET[6] = {
    "<40 ms", "<55", "<75", "<100", "<200", ">=200"
};

typedef LONG (NTAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG,
                                                 PULONG);
static NtQueryInformationThread_t s_ntqit;

/* RECOMP_SAMPLE_CALLERS=1: who called into the system?
 *
 * Self time puts a sample that lands in ntdll or a driver (NtDelayExecution,
 * RtlQueryPerformanceCounter) on that function, which says the thread slept
 * but not which of our functions put it to sleep. For such samples the
 * thread's registers and stack are copied while it is suspended (a plain
 * memcpy, no locks), and after it resumes the copy is unwound with the
 * modules' unwind tables until the first two frames in the .exe's code: our
 * two innermost functions on that call path.
 *
 * Scanning the copy for any value that looks like a return address instead
 * was tried first and lied: driver frames hold stale return addresses in
 * their uninitialised locals, so it named functions that never sleep.
 *
 * RECOMP_SAMPLE_CALLERS=2 unwinds samples in our own code too, past one
 * frame: the leaf's caller's caller and the one above it. Made to find who
 * called the C runtime's getenv (common_getenv_nolock <- common_getenv <-
 * getenv <- us) 3-5 % of the game thread in It's War (2026-11-04). */
static int s_callers;
static uint64_t s_text_lo, s_text_hi;    /* the .exe's code section */
#define PROF_STACK_COPY 65536
static uint64_t s_stack_copy[PROF_STACK_COPY / 8];

/* Copies a stopped thread's stack from rsp up to stack_base (at most
 * PROF_STACK_COPY bytes) into dst; returns the bytes copied. Only the region
 * rsp is in: past its end can be another stack's guard page, and touching
 * that raised 0x80000001 in the profiler (2026-11-11, It's War bench with
 * RECOMP_SAMPLE_CALLERS), and would break that stack's growth. */
static size_t prof_copy_stack(void *dst, uint64_t rsp, uint64_t stack_base)
{
    MEMORY_BASIC_INFORMATION mi;
    size_t n = (size_t)(stack_base - rsp);
    uint64_t end;

    if (n > PROF_STACK_COPY)
        n = PROF_STACK_COPY;
    if (!VirtualQuery((LPCVOID)rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT
        || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return 0;
    end = (uint64_t)mi.BaseAddress + mi.RegionSize;
    if (end - rsp < n)
        n = (size_t)(end - rsp);
    memcpy(dst, (const void *)rsp, n);
    return n;
}

/* Unwind c (taken from a thread whose stack [lo, hi) was copied to
 * s_stack_copy) to its first two frames in our code. The copy stands in for
 * the stack: rsp and a frame pointer into the stack are moved into it, so
 * every read the unwinder makes comes from the snapshot. */
static void prof_unwind(CONTEXT *c, uint64_t lo, uint64_t hi, uint64_t caller[2],
                        int skip)
{
    uint64_t clo = (uint64_t)s_stack_copy, chi = clo + (hi - lo);
    int64_t delta = (int64_t)(clo - lo);
    int frame, found = 0;

    c->Rsp += delta;
    if (c->Rbp >= lo && c->Rbp < hi)
        c->Rbp += delta;
    for (frame = 0; frame < 64 && found < 2; frame++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (fe) {
            PVOID hd;
            DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fe, c, &hd, &ef,
                             NULL);
        } else {                                  /* leaf: return at [rsp] */
            if (c->Rsp < clo || c->Rsp + 8 > chi)
                break;
            c->Rip = *(uint64_t *)c->Rsp;
            c->Rsp += 8;
        }
        if (!c->Rip || c->Rsp < clo || c->Rsp >= chi)
            break;
        if (c->Rbp >= lo && c->Rbp < hi)
            c->Rbp += delta;
        if (prof_in_text(c->Rip) && skip-- <= 0)
            caller[found++] = c->Rip;
    }
}

static void prof_find_text(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)
        (base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    WORD i;

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (memcmp(s->Name, ".text", 5) == 0) {
            s_text_lo = (uint64_t)(base + s->VirtualAddress);
            s_text_hi = s_text_lo + s->Misc.VirtualSize;
        }
}

static int prof_in_text(uint64_t a)
{
    return a >= s_text_lo && a < s_text_hi;
}

static void prof_record_in(ProfEntry *table, unsigned bits, uint32_t *dropped,
                           uint32_t slot, uint64_t rip, const uint64_t *caller)
{
    uint64_t c0 = caller ? caller[0] : 0, c1 = caller ? caller[1] : 0;
    uint64_t h = (rip ^ ((uint64_t)slot << 40) ^ (c0 * 31) ^ (c1 * 131))
               * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> (64 - bits)), n;

    for (n = 0; n < 64; n++, i = (i + 1) & ((1u << bits) - 1)) {
        ProfEntry *e = &table[i];
        if (e->slot == slot + 1 && e->rip == rip
                && e->caller[0] == c0 && e->caller[1] == c1) {
            e->count++;
            return;
        }
        if (!e->slot) {
            e->slot = slot + 1;
            e->rip = rip;
            e->caller[0] = c0;
            e->caller[1] = c1;
            e->count = 1;
            return;
        }
    }
    (*dropped)++;
}

static void prof_record(uint32_t slot, uint64_t rip, const uint64_t *caller)
{
    if (s_window_on)
        prof_record_in(s_table, PROF_TABLE_BITS, &s_dropped, slot, rip, caller);
    if (s_hitch_ms) {
        prof_record_in(s_ftable, PROF_FTABLE_BITS, &s_fdropped, slot, rip,
                       caller);
        s_fsamples[slot]++;
    }
}

/* Add threads that appeared since the last refresh; forget ones that ended.
 * A forgotten thread's slot is reused only after the next report, so the
 * table never mixes two threads under one slot within a window. */
static void prof_refresh_threads(DWORD self)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();

    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            int k, known = 0;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            for (k = 0; k < s_nthreads; k++)
                if (s_threads[k].tid == te.th32ThreadID)
                    known = 1;
            if (!known && s_nthreads < PROF_MAX_THREADS) {
                ProfThread *t = &s_threads[s_nthreads];
                t->h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                  | THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID);
                if (!t->h)
                    continue;
                t->tid = te.th32ThreadID;
                t->start = 0;
                t->stack_base = 0;
                t->samples = 0;
                if (s_ntqit) {
                    struct {                    /* THREAD_BASIC_INFORMATION */
                        LONG status; PVOID teb; PVOID cid[2];
                        ULONG_PTR affinity; LONG prio, base_prio;
                    } tbi;
                    s_ntqit(t->h, 9 /* ThreadQuerySetWin32StartAddress */,
                            &t->start, sizeof t->start, NULL);
                    if (s_callers && s_ntqit(t->h, 0 /* ThreadBasicInformation */,
                                             &tbi, sizeof tbi, NULL) == 0
                            && tbi.teb)
                        t->stack_base = (uint64_t)((NT_TIB *)tbi.teb)->StackBase;
                }
                s_nthreads++;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void prof_sample_all(void)
{
    int k;
    for (k = 0; k < s_nthreads; k++) {
        ProfThread *t = &s_threads[k];
        CONTEXT c;
        size_t copied = 0;
        if (!t->h || SuspendThread(t->h) == (DWORD)-1)
            continue;
        c.ContextFlags = s_callers ? CONTEXT_CONTROL | CONTEXT_INTEGER
                                   : CONTEXT_CONTROL;
        if (!GetThreadContext(t->h, &c)) {
            ResumeThread(t->h);
            continue;
        }
        if (s_callers && (s_callers == 2 || !prof_in_text(c.Rip))
                && t->stack_base > c.Rsp && !(c.Rsp & 7)) {
            copied = prof_copy_stack(s_stack_copy, c.Rsp, t->stack_base);
        }
        ResumeThread(t->h);
        if (copied) {
            uint64_t caller[2] = {0, 0};
            CONTEXT u = c;
            prof_unwind(&u, c.Rsp, c.Rsp + copied, caller,
                        prof_in_text(c.Rip));
            prof_record((uint32_t)k, c.Rip, caller);
        } else {
            prof_record((uint32_t)k, c.Rip, NULL);
        }
        t->samples++;
    }
    s_ticks++;
    s_fticks++;
}

/* ── Report ────────────────────────────────────────────────── */

typedef struct {
    uint32_t slot;
    uint64_t func;           /* function start, or the raw rip if unknown */
    uint64_t caller[2];      /* function starts; 0 = none captured */
    uint32_t count;
} ProfFunc;

static int cmp_slot_func(const void *a, const void *b)
{
    const ProfFunc *x = a, *y = b;
    if (x->slot != y->slot) return x->slot < y->slot ? -1 : 1;
    if (x->func != y->func) return x->func < y->func ? -1 : 1;
    return 0;
}

static int cmp_slot_func_callers(const void *a, const void *b)
{
    const ProfFunc *x = a, *y = b;
    int r = cmp_slot_func(a, b);
    if (r) return r;
    if (x->caller[0] != y->caller[0]) return x->caller[0] < y->caller[0] ? -1 : 1;
    if (x->caller[1] != y->caller[1]) return x->caller[1] < y->caller[1] ? -1 : 1;
    return 0;
}

static int cmp_slot_count(const void *a, const void *b)
{
    const ProfFunc *x = a, *y = b;
    if (x->slot != y->slot) return x->slot < y->slot ? -1 : 1;
    return (x->count < y->count) - (x->count > y->count);
}

static const char *prof_name(uint64_t addr, char *buf, size_t n)
{
    char sb[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sb;
    DWORD64 disp = 0;

    memset(sb, 0, sizeof sb);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym))
        _snprintf_s(buf, n, _TRUNCATE, "%s", sym->Name);
    else
        _snprintf_s(buf, n, _TRUNCATE, "0x%llX", (unsigned long long)addr);
    return buf;
}

static uint64_t prof_func_start(uint64_t rip)
{
    char sb[sizeof(SYMBOL_INFO) + 8];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sb;
    DWORD64 disp = 0;

    memset(sb, 0, sizeof sb);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 1;
    return SymFromAddr(GetCurrentProcess(), rip, &disp, sym)
           ? sym->Address : rip;
}

static void prof_print_window(double secs)
{
    ProfFunc *f, *g = NULL;
    uint32_t i, n = 0, j, ng = 0;
    int k;
    char name[256], c0[256], c1[256];

    f = (ProfFunc *)malloc(sizeof *f * PROF_TABLE_SIZE);
    if (!f)
        return;
    for (i = 0; i < PROF_TABLE_SIZE; i++) {
        if (!s_table[i].slot)
            continue;
        f[n].slot = s_table[i].slot - 1;
        f[n].func = prof_func_start(s_table[i].rip);
        f[n].caller[0] = s_table[i].caller[0]
                       ? prof_func_start(s_table[i].caller[0]) : 0;
        f[n].caller[1] = s_table[i].caller[1]
                       ? prof_func_start(s_table[i].caller[1]) : 0;
        f[n].count = s_table[i].count;
        n++;
    }
    /* Merge samples of one function and caller pair within a thread; keep
     * those with callers aside, then merge again ignoring callers. */
    qsort(f, n, sizeof *f, cmp_slot_func_callers);
    for (i = 0, j = 0; i < n; i++) {
        if (j && cmp_slot_func_callers(&f[j - 1], &f[i]) == 0)
            f[j - 1].count += f[i].count;
        else
            f[j++] = f[i];
    }
    n = j;
    if (s_callers && (g = (ProfFunc *)malloc(sizeof *g * n)) != NULL) {
        for (i = 0; i < n; i++)
            if (f[i].caller[0])
                g[ng++] = f[i];
        qsort(g, ng, sizeof *g, cmp_slot_count);
    }
    for (i = 0, j = 0; i < n; i++) {
        if (j && f[j - 1].slot == f[i].slot && f[j - 1].func == f[i].func)
            f[j - 1].count += f[i].count;
        else
            f[j++] = f[i];
    }
    n = j;
    qsort(f, n, sizeof *f, cmp_slot_count);

    fprintf(stderr, "[PROF] %.1f s window, %u ticks (%.0f/s), %d threads%s\n",
            secs, s_ticks, s_ticks / secs, s_nthreads,
            s_dropped ? ", some samples dropped (table full)" : "");
    for (k = 0; k < s_nthreads; k++) {
        ProfThread *t = &s_threads[k];
        uint32_t shown = 0;
        if (!t->samples)
            continue;
        fprintf(stderr, "[PROF] thread %5lu  start %s  %u samples\n",
                (unsigned long)t->tid,
                t->start ? prof_name(t->start, name, sizeof name) : "?",
                t->samples);
        for (i = 0; i < n && shown < s_top; i++) {
            if (f[i].slot != (uint32_t)k)
                continue;
            if (100.0 * f[i].count < s_min_pct * t->samples)
                break;
            fprintf(stderr, "[PROF]     %5.1f%%  %s\n",
                    100.0 * f[i].count / t->samples,
                    prof_name(f[i].func, name, sizeof name));
            shown++;
        }
        /* System time by who called it: "<- a <- b" = the innermost two of
         * our functions found on the stack. */
        for (i = 0, shown = 0; g && i < ng && shown < s_top; i++) {
            if (g[i].slot != (uint32_t)k)
                continue;
            if (100.0 * g[i].count < s_min_pct * t->samples)
                break;
            fprintf(stderr, "[PROF]       %5.1f%%  %s <- %s <- %s\n",
                    100.0 * g[i].count / t->samples,
                    prof_name(g[i].func, name, sizeof name),
                    prof_name(g[i].caller[0], c0, sizeof c0),
                    g[i].caller[1] ? prof_name(g[i].caller[1], c1, sizeof c1)
                                   : "?");
            shown++;
        }
    }
    free(g);
    free(f);
}

/* Is this (a system function's name) a thread waiting rather than working? */
static int prof_is_wait(const char *name)
{
    return strstr(name, "Wait") || strstr(name, "Delay")
        || strstr(name, "Yield") || strstr(name, "RemoveIoCompletion");
}

/* One [HITCH] block: the samples of the frame that just ended, ms long.
 * Threads that only waited are counted, not listed. */
static void hitch_print_frame(LONG frame, double ms)
{
    ProfFunc *f;
    uint32_t i, j, n = 0;
    int k, idle = 0;
    char name[256], c0[256], c1[256], line[1024];

    int lead[PROF_MAX_THREADS], members[PROF_MAX_THREADS];
    uint32_t total[PROF_MAX_THREADS];

    /* Threads with one start function (raster_worker x12) are one line:
     * their samples are filed under the first of them. */
    for (k = 0; k < s_nthreads; k++) {
        int m;
        lead[k] = k;
        for (m = 0; m < k; m++)
            if (s_threads[k].start && s_threads[m].start == s_threads[k].start) {
                lead[k] = m;
                break;
            }
        members[k] = 0;
        total[k] = 0;
    }
    for (k = 0; k < s_nthreads; k++) {
        members[lead[k]]++;
        total[lead[k]] += s_fsamples[k];
    }
    f = (ProfFunc *)malloc(sizeof *f * PROF_FTABLE_SIZE);
    if (!f)
        return;
    for (i = 0; i < PROF_FTABLE_SIZE; i++) {
        if (!s_ftable[i].slot)
            continue;
        f[n].slot = (uint32_t)lead[s_ftable[i].slot - 1];
        f[n].func = prof_func_start(s_ftable[i].rip);
        f[n].caller[0] = s_ftable[i].caller[0]
                       ? prof_func_start(s_ftable[i].caller[0]) : 0;
        f[n].caller[1] = s_ftable[i].caller[1]
                       ? prof_func_start(s_ftable[i].caller[1]) : 0;
        f[n].count = s_ftable[i].count;
        n++;
    }
    /* Merge by function within a thread; for system functions keep the
     * callers of the biggest share (the first after sorting by count). */
    qsort(f, n, sizeof *f, cmp_slot_func);
    for (i = 0, j = 0; i < n; i++) {
        if (j && cmp_slot_func(&f[j - 1], &f[i]) == 0) {
            if (f[i].count > f[j - 1].count) {
                f[j - 1].caller[0] = f[i].caller[0];
                f[j - 1].caller[1] = f[i].caller[1];
            }
            f[j - 1].count += f[i].count;
        } else {
            f[j++] = f[i];
        }
    }
    n = j;
    qsort(f, n, sizeof *f, cmp_slot_count);

    fprintf(stderr, "[HITCH] frame %ld: %.0f ms, %u ticks%s%s\n",
            (long)frame, ms, s_fticks,
            s_fdropped ? ", some samples dropped (table full)" : "",
            s_fpartial ? ", partial (samples from a thread-list refresh on)" : "");
    for (k = 0; k < s_nthreads; k++) {
        uint32_t busy = 0, shown = 0;
        size_t len;
        if (lead[k] != k || !total[k])
            continue;
        for (i = 0; i < n; i++)
            if (f[i].slot == (uint32_t)k
                    && !prof_is_wait(prof_name(f[i].func, name, sizeof name)))
                busy += f[i].count;
        if (busy * 20u < total[k]) {              /* under 5% working */
            idle += members[k];
            continue;
        }
        /* Busy % of a group is of all its threads together. */
        if (members[k] > 1)
            len = (size_t)_snprintf_s(line, sizeof line, _TRUNCATE,
                    "[HITCH]   %d threads %s, %u%% busy:", members[k],
                    prof_name(s_threads[k].start, name, sizeof name),
                    100u * busy / total[k]);
        else
            len = (size_t)_snprintf_s(line, sizeof line, _TRUNCATE,
                    "[HITCH]   thread %5lu %s, %u%% busy:",
                    (unsigned long)s_threads[k].tid,
                    s_threads[k].start
                        ? prof_name(s_threads[k].start, name, sizeof name)
                        : "?",
                    100u * busy / total[k]);
        for (i = 0; i < n && shown < HITCH_TOP && len < sizeof line - 200; i++) {
            if (f[i].slot != (uint32_t)k)
                continue;
            if (f[i].count * 50u < total[k])       /* under 2% */
                break;
            prof_name(f[i].func, name, sizeof name);
            if (f[i].caller[0])
                len += (size_t)_snprintf_s(line + len, sizeof line - len,
                        _TRUNCATE, " %u%% %s (<- %s <- %s)",
                        100u * f[i].count / total[k], name,
                        prof_name(f[i].caller[0], c0, sizeof c0),
                        f[i].caller[1]
                            ? prof_name(f[i].caller[1], c1, sizeof c1) : "?");
            else
                len += (size_t)_snprintf_s(line + len, sizeof line - len,
                        _TRUNCATE, " %u%% %s", 100u * f[i].count / total[k],
                        name);
            shown++;
        }
        fprintf(stderr, "%s\n", line);
    }
    fprintf(stderr, "[HITCH]   %d threads only waited\n", idle);
    fflush(stderr);
    free(f);
}

/* Start counting the next frame. */
static void hitch_clear_frame(void)
{
    memset(s_ftable, 0, sizeof s_ftable);
    memset(s_fsamples, 0, sizeof s_fsamples);
    s_fticks = 0;
    s_fdropped = 0;
    s_fpartial = 0;
}

/* Every window: how long the frames were. */
static void hitch_print_window(double secs, uint32_t shown, uint32_t skipped)
{
    static LONG last_hist[6];
    static LONG64 last_sum;
    LONG64 sum = s_frame_qpc_sum;
    LONG hist[6], frames = 0;
    LONG worst = InterlockedExchange(&s_worst_us, 0);
    int b;

    for (b = 0; b < 6; b++) {
        LONG now = s_hist[b];
        hist[b] = now - last_hist[b];
        last_hist[b] = now;
        frames += hist[b];
    }
    fprintf(stderr, "[HITCH] %.1f s: %ld frames", secs, (long)frames);
    if (frames)
        fprintf(stderr, ", %.1f ms average", (double)(sum - last_sum)
                / s_qpc_ms / frames);
    last_sum = sum;
    fprintf(stderr, " |");
    for (b = 0; b < 6; b++)
        fprintf(stderr, " %s %ld", HITCH_BUCKET[b], (long)hist[b]);
    fprintf(stderr, " | worst %.0f ms | %u long frames shown, %u more not\n",
            worst / 1000.0, shown, skipped);
    fflush(stderr);
}

static void prof_report(double secs)
{
    int k, j;

    if (s_window_on)
        prof_print_window(secs);
    memset(s_table, 0, sizeof s_table);
    s_dropped = 0;
    s_ticks = 0;
    /* Thread slots are about to move, so the frame table's entries would be
     * read under the wrong thread: this frame restarts here. */
    hitch_clear_frame();
    s_fpartial = 1;
    /* Drop ended threads now that no entry refers to their slots. */
    for (k = 0, j = 0; k < s_nthreads; k++) {
        DWORD code = 0;
        if (s_threads[k].h && GetExitCodeThread(s_threads[k].h, &code)
                && code == STILL_ACTIVE) {
            s_threads[k].samples = 0;
            s_threads[j++] = s_threads[k];
        } else if (s_threads[k].h) {
            CloseHandle(s_threads[k].h);
        }
    }
    s_nthreads = (int)j;
}

static DWORD WINAPI prof_thread(LPVOID arg)
{
    double every = *(double *)arg;
    DWORD self = GetCurrentThreadId();
    LARGE_INTEGER freq, t0, now;
    uint32_t tick = 0, shown = 0, skipped = 0;
    LONG seen = 0;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        double secs;
        if ((tick++ & 255) == 0)
            prof_refresh_threads(self);
        prof_sample_all();
        if (s_hitch_ms) {
            /* A frame mark since the last tick: print the frame if it was
             * long (s_hitch_seq is written before s_frame_seq), then start
             * the next one. */
            LONG seq = s_frame_seq;
            if (seq != seen) {
                if (s_hitch_seq == seq && !s_fpartial) {
                    if (shown < HITCH_REPORTS_PER_WINDOW) {
                        hitch_print_frame(seq, s_hitch_us / 1000.0);
                        shown++;
                    } else {
                        skipped++;
                    }
                }
                hitch_clear_frame();
                seen = seq;
            }
        }
        Sleep(1);
        QueryPerformanceCounter(&now);
        secs = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (secs >= every) {
            if (s_hitch_ms) {
                hitch_print_window(secs, shown, skipped);
                shown = skipped = 0;
            }
            prof_report(secs);
            prof_refresh_threads(self);
            QueryPerformanceCounter(&t0);
        }
    }
    return 0;
}

/* RECOMP_HITCH_MS: called by the game thread once per game frame (the
 * audio_update entry hook). Cheap when off. */
void sample_profiler_frame(void)
{
    static LARGE_INTEGER last;
    LARGE_INTEGER now;

    if (!s_hitch_ms)
        return;
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        LONG64 d = now.QuadPart - last.QuadPart;
        double ms = (double)d / s_qpc_ms;
        LONG us = ms > 2.0e6 ? 2000000000L : (LONG)(ms * 1000.0);
        int b = ms < 40 ? 0 : ms < 55 ? 1 : ms < 75 ? 2 : ms < 100 ? 3
              : ms < 200 ? 4 : 5;
        s_hist[b]++;
        s_frame_qpc_sum += d;
        if (us > s_worst_us)
            s_worst_us = us;
        if (ms >= s_hitch_ms) {
            s_hitch_us = us;
            InterlockedExchange(&s_hitch_seq, s_frame_seq + 1);
        }
    }
    last = now;
    InterlockedIncrement(&s_frame_seq);
}

/* ── Where every thread is right now (Report Bug in a freeze) ── */

#define DUMP_FRAMES 24
static uint64_t s_dump_stack[PROF_STACK_COPY / 8];

/* Unwind c (from a thread whose stack [lo, hi) was copied to s_dump_stack) as
 * far as the copy goes: the frames' addresses, innermost first. */
static int dump_unwind(CONTEXT *c, uint64_t lo, uint64_t hi, uint64_t *frames)
{
    uint64_t clo = (uint64_t)s_dump_stack, chi = clo + (hi - lo);
    int64_t delta = (int64_t)(clo - lo);
    int n = 0;

    frames[n++] = c->Rip;
    c->Rsp += delta;
    if (c->Rbp >= lo && c->Rbp < hi)
        c->Rbp += delta;
    while (n < DUMP_FRAMES) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (fe) {
            PVOID hd;
            DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fe, c, &hd, &ef,
                             NULL);
        } else {                                  /* leaf: return at [rsp] */
            if (c->Rsp < clo || c->Rsp + 8 > chi)
                break;
            c->Rip = *(uint64_t *)c->Rsp;
            c->Rsp += 8;
        }
        if (!c->Rip || c->Rsp < clo || c->Rsp >= chi)
            break;
        if (c->Rbp >= lo && c->Rbp < hi)
            c->Rbp += delta;
        frames[n++] = c->Rip;
    }
    return n;
}

/* "module+0xoffset" and, where dbghelp knows it (system DLLs' exports, our
 * functions next to a .pdb, which a release has not), the function's name. */
static void dump_where(uint64_t a, char *out, size_t n)
{
    char sb[sizeof(SYMBOL_INFO) + 128], mod[MAX_PATH] = "?", *name = mod;
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sb;
    DWORD64 disp = 0;
    HMODULE m = NULL;
    uint64_t off = a;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        char *slash;
        off = a - (uint64_t)m;
        GetModuleFileNameA(m, mod, sizeof mod);
        slash = strrchr(mod, '\\');
        if (slash)
            name = slash + 1;
    }
    memset(sb, 0, sizeof sb);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 127;
    if (SymFromAddr(GetCurrentProcess(), a, &disp, sym))
        _snprintf_s(out, n, _TRUNCATE, "%s+0x%llX (%s+0x%llX)", name,
                    (unsigned long long)off, sym->Name, (unsigned long long)disp);
    else
        _snprintf_s(out, n, _TRUNCATE, "%s+0x%llX", name, (unsigned long long)off);
}

/* To stderr: each thread of the process, its start and its call stack. The
 * guest stacks (xbox_WatchdogDump) cannot show a thread stuck on the host's
 * side, like the one that draws, which has none. Nothing here takes a lock
 * while a thread is suspended (top of this file). A release has no .pdb, so
 * its own frames come out as conker.exe+offset: look them up in that release's
 * conker.pdb, kept in _work\release\<version>\symbols. */
void sample_profiler_dump_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NtQueryInformationThread_t ntqit = ntdll ? (NtQueryInformationThread_t)(void *)
        GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
    THREADENTRY32 te;

    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            HANDLE h;
            CONTEXT c;
            uint64_t start = 0, stack_base = 0, frames[DUMP_FRAMES];
            size_t copied = 0;
            char where[300];
            int i, n;
            struct {                        /* THREAD_BASIC_INFORMATION */
                LONG status; PVOID teb; PVOID cid[2];
                ULONG_PTR affinity; LONG prio, base_prio;
            } tbi;

            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                           | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            if (ntqit) {
                ntqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &start,
                      sizeof start, NULL);
                if (ntqit(h, 0 /* ThreadBasicInformation */, &tbi, sizeof tbi,
                          NULL) == 0 && tbi.teb)
                    stack_base = (uint64_t)((NT_TIB *)tbi.teb)->StackBase;
            }
            if (SuspendThread(h) == (DWORD)-1) {
                CloseHandle(h);
                continue;
            }
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (!GetThreadContext(h, &c)) {
                ResumeThread(h);
                CloseHandle(h);
                continue;
            }
            if (stack_base > c.Rsp && !(c.Rsp & 7)) {
                copied = prof_copy_stack(s_dump_stack, c.Rsp, stack_base);
            }
            ResumeThread(h);
            CloseHandle(h);

            dump_where(start, where, sizeof where);
            fprintf(stderr, "[THREAD] %lu, started at %s\n",
                    (unsigned long)te.th32ThreadID, where);
            if (copied) {
                n = dump_unwind(&c, c.Rsp, c.Rsp + copied, frames);
            } else {
                frames[0] = c.Rip;
                n = 1;
            }
            for (i = 0; i < n; i++) {
                dump_where(frames[i], where, sizeof where);
                fprintf(stderr, "    #%-2d %s\n", i, where);
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* Start the profiler if RECOMP_SAMPLE_PROFILE or RECOMP_HITCH_MS is set.
 * Call after SymInitialize. */
void sample_profiler_start(void)
{
    static double every;
    const char *e = getenv("RECOMP_SAMPLE_PROFILE");
    const char *h = getenv("RECOMP_HITCH_MS");
    LARGE_INTEGER freq;
    HMODULE ntdll;

    if (!e && !h)
        return;
    s_window_on = e != NULL;
    every = e ? atof(e) : 10.0;
    if (every < 1.0)
        every = 10.0;
    if (h) {
        QueryPerformanceFrequency(&freq);
        s_qpc_ms = (double)freq.QuadPart / 1000.0;
        s_hitch_ms = atoi(h);
        if (s_hitch_ms < 1)
            s_hitch_ms = 45;
    }
    if (getenv("RECOMP_SAMPLE_TOP") && atoi(getenv("RECOMP_SAMPLE_TOP")) > 0)
        s_top = (uint32_t)atoi(getenv("RECOMP_SAMPLE_TOP"));
    if (getenv("RECOMP_SAMPLE_MIN") && atof(getenv("RECOMP_SAMPLE_MIN")) > 0.0)
        s_min_pct = atof(getenv("RECOMP_SAMPLE_MIN"));
    if (getenv("RECOMP_SAMPLE_CALLERS")) {
        prof_find_text();
        s_callers = s_text_hi > s_text_lo
                  ? 1 + (atoi(getenv("RECOMP_SAMPLE_CALLERS")) == 2) : 0;
    }
    ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll)
        s_ntqit = (NtQueryInformationThread_t)(void *)
                  GetProcAddress(ntdll, "NtQueryInformationThread");
    CloseHandle(CreateThread(NULL, 0, prof_thread, &every, 0, NULL));
    if (s_window_on)
        fprintf(stderr, "[PROF] sampling profiler on, report every %.0f s\n",
                every);
    if (s_hitch_ms)
        fprintf(stderr, "[HITCH] long frames on: frames of %d ms or more are"
                " shown (at most %d per %.0f s)\n", s_hitch_ms,
                HITCH_REPORTS_PER_WINDOW, every);
}
