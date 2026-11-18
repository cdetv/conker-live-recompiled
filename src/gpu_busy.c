/* GPU busy (to-do 220): how much of the time the video card's 3D engine
 * worked for this process, from Windows' own counter (Task Manager's GPU
 * column: "\GPU Engine(*)\Utilization Percentage", the instances named
 * pid_<ours>_..._engtype_3D, summed).
 *
 * Why: the D3D11 timestamps' frame span (toolkit xbox_GpuFrameMs) runs from
 * a frame's first command to its last, so it counts the GPU's idle gaps: at
 * a 120 fps cap with VRR it read 8.2 ms a frame at 2x in a light scene, as
 * long as the frame. It can't tell how much room the GPU has; this can, and
 * a busy wait doesn't inflate it as it does the CPU threads' cycle counts
 * (user's report 2026-11-16: "CPU busy" every half second at 4x, 80 fps).
 *
 * A thread of its own collects every 250 ms (a collect took ~0.5 ms with
 * ~650 engine instances) while someone wants it (gpu_busy_want: F1's "what
 * limits" or Automatic resolution); gpu_busy_take gives the mean since the
 * last take. A partial wildcard (pid_<n>_*) finds nothing (PDH_NO_DATA), so
 * every instance is read and ours picked by name. The query is made afresh
 * when ours are missing for 4 s (the game's instance appears with its D3D11
 * device). Where the counter doesn't exist (Wine) take returns -1 and the
 * callers fall back to the frame span. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "gpu_busy.h"

#define GB_EVERY_MS   250
#define GB_REOPEN_MS  4000

static volatile LONG s_want;
static HANDLE s_thread, s_wake;
static SRWLOCK s_lock = SRWLOCK_INIT;
static double s_sum, s_span;          /* busy fraction x seconds, seconds */
static volatile LONG s_state;         /* 0 not known yet, 1 works, -1 none */

static PDH_HQUERY s_q;
static PDH_HCOUNTER s_c;
static PDH_FMT_COUNTERVALUE_ITEM_W *s_items;
static DWORD s_items_size;
static wchar_t s_prefix[40];

static void gb_close(void)
{
    if (s_q) {
        PdhCloseQuery(s_q);
        s_q = NULL;
    }
}

static int gb_open(void)
{
    PDH_STATUS st;

    gb_close();
    st = PdhOpenQueryW(NULL, 0, &s_q);
    if (st == ERROR_SUCCESS)
        st = PdhAddEnglishCounterW(s_q, L"\\GPU Engine(*)\\Utilization Percentage",
                                   0, &s_c);
    if (st == ERROR_SUCCESS)
        st = PdhCollectQueryData(s_q);      /* the first sample: no value yet */
    if (st != ERROR_SUCCESS) {
        gb_close();
        if (s_state == 0) {
            s_state = -1;
            fprintf(stderr, "[GPUBUSY] no GPU usage counter here (PDH 0x%08lX):"
                    " the frame span stands in\n", (unsigned long)st);
            fflush(stderr);
        }
        return 0;
    }
    return 1;
}

/* This process's 3D engines' busy share since the last collect, 0..1;
 * -1 when none of ours was found or nothing could be read. */
static double gb_read(DWORD *ours)
{
    DWORD size = 0, n = 0, k;
    size_t plen = wcslen(s_prefix);
    double sum = 0.0;
    PDH_STATUS st;

    *ours = 0;
    if (PdhCollectQueryData(s_q) != ERROR_SUCCESS)
        return -1.0;
    st = PdhGetFormattedCounterArrayW(s_c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                      &size, &n, NULL);
    if (st != (PDH_STATUS)PDH_MORE_DATA || !size)
        return -1.0;
    if (size > s_items_size) {
        free(s_items);
        s_items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(size);
        s_items_size = s_items ? size : 0;
        if (!s_items)
            return -1.0;
    }
    size = s_items_size;
    if (PdhGetFormattedCounterArrayW(s_c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                     &size, &n, s_items) != ERROR_SUCCESS)
        return -1.0;
    for (k = 0; k < n; k++) {
        const wchar_t *nm = s_items[k].szName;
        size_t len = wcslen(nm);
        if (len < plen + 10 || wcsncmp(nm, s_prefix, plen)
            || wcscmp(nm + len - 10, L"engtype_3D"))
            continue;
        (*ours)++;
        if (s_items[k].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA
            || s_items[k].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            sum += s_items[k].FmtValue.doubleValue;
    }
    if (!*ours)
        return -1.0;
    sum /= 100.0;
    return sum > 1.0 ? 1.0 : sum < 0.0 ? 0.0 : sum;
}

static DWORD WINAPI gb_thread(LPVOID arg)
{
    LARGE_INTEGER freq, prev, now;
    ULONGLONG missing_since = 0;
    int have_prev = 0;

    (void)arg;
    QueryPerformanceFrequency(&freq);
    swprintf(s_prefix, sizeof s_prefix / sizeof s_prefix[0], L"pid_%lu_",
             (unsigned long)GetCurrentProcessId());
    for (;;) {
        double v, dt;
        DWORD ours;

        if (!s_want) {
            gb_close();
            have_prev = 0;
            AcquireSRWLockExclusive(&s_lock);
            s_sum = s_span = 0.0;
            ReleaseSRWLockExclusive(&s_lock);
            WaitForSingleObject(s_wake, INFINITE);
            continue;
        }
        if (s_state < 0)
            return 0;
        if (!s_q) {
            if (!gb_open())
                return 0;
            QueryPerformanceCounter(&prev);
            have_prev = 1;
            missing_since = GetTickCount64();
        }
        Sleep(GB_EVERY_MS);
        v = gb_read(&ours);
        QueryPerformanceCounter(&now);
        dt = have_prev ? (double)(now.QuadPart - prev.QuadPart)
                         / (double)freq.QuadPart : 0.0;
        prev = now;
        have_prev = 1;
        if (v < 0.0) {
            if (GetTickCount64() - missing_since > GB_REOPEN_MS)
                gb_close();                 /* made afresh next round */
            continue;
        }
        missing_since = GetTickCount64();
        if (s_state == 0) {
            s_state = 1;
            fprintf(stderr, "[GPUBUSY] GPU usage counter on (%lu 3D engine"
                    " instance%s of ours)\n", (unsigned long)ours,
                    ours == 1 ? "" : "s");
            fflush(stderr);
        }
        if (dt > 0.0 && dt < 5.0) {
            AcquireSRWLockExclusive(&s_lock);
            s_sum += v * dt;
            s_span += dt;
            ReleaseSRWLockExclusive(&s_lock);
        }
    }
}

void gpu_busy_want(int on)
{
    InterlockedExchange(&s_want, on ? 1 : 0);
    if (!on)
        return;
    if (!s_wake)
        s_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_thread && s_wake && s_state >= 0) {
        s_thread = CreateThread(NULL, 0, gb_thread, NULL, 0, NULL);
        if (s_thread)
            SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    if (s_wake)
        SetEvent(s_wake);
}

double gpu_busy_take(void)
{
    double v = -1.0;

    AcquireSRWLockExclusive(&s_lock);
    if (s_span > 0.0)
        v = s_sum / s_span;
    s_sum = s_span = 0.0;
    ReleaseSRWLockExclusive(&s_lock);
    return v;
}
