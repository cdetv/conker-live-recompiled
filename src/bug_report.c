/*
 * Report Bug (to-do 12, SPEC 9.2 "Report Bug"): one .zip a player attaches to
 * a GitHub issue, in <user files>\reports\report_<date>_<time>.zip:
 *
 *   report.txt         where it was made, the player's description, game
 *                      version and release, PC info, the files in the zip
 *   screenshot.png     the moment (F10 in the game only)
 *   logs\              the last LOG_RUNS runs' conker_*.log + _kernel.log
 *   settings.ini, launcher.ini, shader_cache\precompiled.ini (when there)
 *   saves\             when ticked (a few KB a slot)
 *
 * Reports go on public issues, so the Windows user name (and the profile
 * folder's name) in every copied text file becomes "<user>". A log over
 * TRIM_OVER keeps its start and end.
 *
 * The zip is written here, compression included (deflate with fixed Huffman
 * codes, RFC 1951 3.2.6; logs shrink about 5x): no tar.exe (missing in a
 * Wine prefix, r-linux-proton.md) and no new library. Written as .part,
 * renamed when complete.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dxgi.h>
#include <intrin.h>
#include <share.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bug_report.h"

#ifndef CONKER_VERSION_STR
#define CONKER_VERSION_STR "dev"
#endif

#define LOG_RUNS    5                       /* main.c LOG_KEEP: all kept */
#define TRIM_OVER   (16u << 20)             /* a log past this is cut */
#define TRIM_HEAD   (1u << 20)              /* ... to its first MB */
#define TRIM_TAIL   (15u << 20)             /* ... and last 15 MB */
#define SAVE_MAX    (4u << 20)              /* a bigger file in saves\: skipped */
#define MAX_ENTRIES 64

static char s_release[16], s_sha1[48];

void bug_report_set_release(const char *release, const char *sha1)
{
    strcpy_s(s_release, sizeof s_release, release ? release : "");
    strcpy_s(s_sha1, sizeof s_sha1, sha1 ? sha1 : "");
}

/* ── Growable byte buffer ─────────────────────────────────── */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int fail;
} Buf;

static int buf_reserve(Buf *b, size_t extra)
{
    size_t cap;
    uint8_t *q;

    if (b->fail)
        return -1;
    if (b->n + extra <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    q = (uint8_t *)realloc(b->p, cap);
    if (!q) {
        b->fail = 1;
        return -1;
    }
    b->p = q;
    b->cap = cap;
    return 0;
}

static void buf_add(Buf *b, const void *d, size_t n)
{
    if (buf_reserve(b, n))
        return;
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void buf_addf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = _vscprintf(fmt, ap);
    va_end(ap);
    if (len < 0 || buf_reserve(b, (size_t)len + 1))
        return;
    va_start(ap, fmt);
    vsprintf_s((char *)b->p + b->n, (size_t)len + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)len;
}

static void put16(Buf *b, unsigned v)
{
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    buf_add(b, x, 2);
}

static void put32(Buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    buf_add(b, x, 4);
}

/* ── CRC-32 and deflate (fixed Huffman codes) ─────────────── */

static uint32_t s_crc[256];
static uint16_t s_lit_code[288];            /* bit-reversed, LSB first */
static uint8_t s_lit_bits[288];
static uint8_t s_dist_code[30];
static uint8_t s_len_sym[259];              /* length 3..258 -> 0..28 */
static uint8_t s_dist_sym[32769];           /* distance 1..32768 -> 0..29 */

static const uint16_t s_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t s_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0 };
static const uint16_t s_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t s_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13 };

static uint32_t rev_bits(uint32_t v, int n)
{
    uint32_t r = 0;
    while (n--) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static void tables_init(void)
{
    static int done;
    uint32_t i, c, k;

    if (done)
        return;
    done = 1;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc[i] = c;
    }
    for (i = 0; i < 288; i++) {
        uint32_t code;
        int len;
        if (i < 144)      { code = 0x30 + i;          len = 8; }
        else if (i < 256) { code = 0x190 + (i - 144); len = 9; }
        else if (i < 280) { code = i - 256;           len = 7; }
        else              { code = 0xC0 + (i - 280);  len = 8; }
        s_lit_code[i] = (uint16_t)rev_bits(code, len);
        s_lit_bits[i] = (uint8_t)len;
    }
    for (i = 0; i < 30; i++)
        s_dist_code[i] = (uint8_t)rev_bits(i, 5);
    for (i = 0; i < 28; i++)
        for (k = s_len_base[i]; k < s_len_base[i] + (1u << s_len_extra[i])
                                && k <= 258; k++)
            s_len_sym[k] = (uint8_t)i;
    s_len_sym[258] = 28;
    for (i = 0; i < 30; i++)
        for (k = s_dist_base[i]; k < s_dist_base[i] + (1u << s_dist_extra[i])
                                 && k <= 32768; k++)
            s_dist_sym[k] = (uint8_t)i;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct {
    Buf *out;
    uint64_t acc;
    int bits;
} Bits;

static void put_bits(Bits *w, uint32_t v, int n)
{
    w->acc |= (uint64_t)v << w->bits;
    w->bits += n;
    if (w->bits >= 32) {
        if (!buf_reserve(w->out, 4)) {
            memcpy(w->out->p + w->out->n, &w->acc, 4);     /* x64: LE */
            w->out->n += 4;
        }
        w->acc >>= 32;
        w->bits -= 32;
    }
}

#define WSIZE    32768u
#define WMASK    (WSIZE - 1)
#define HBITS    15
#define MAXCHAIN 48

static uint32_t hash3(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return (v * 2654435761u) >> (32 - HBITS);
}

/* One final fixed-Huffman block (BTYPE 01). Returns 0 or -1. */
static int deflate_fixed(const uint8_t *in, size_t n, Buf *out)
{
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * WSIZE);
    Bits w = { out, 0, 0 };
    size_t i = 0;

    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    memset(head, 0xFF, sizeof(int32_t) << HBITS);
    buf_reserve(out, n + n / 8 + 64);
    put_bits(&w, 1, 1);                     /* BFINAL */
    put_bits(&w, 1, 2);                     /* BTYPE = 01, fixed codes */
    while (i < n) {
        size_t best = 0, bdist = 0;

        if (i + 3 <= n) {
            uint32_t h = hash3(in + i);
            int32_t cand = head[h];
            int chain = MAXCHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;

            while (cand >= 0 && chain-- > 0) {
                size_t d = i - (size_t)cand, len = 0;
                int32_t next;
                if (d >= WSIZE)
                    break;
                if (in[cand + best] == in[i + best]) {
                    while (len < maxlen && in[cand + len] == in[i + len])
                        len++;
                    if (len > best) {
                        best = len;
                        bdist = d;
                        if (len == maxlen)
                            break;
                    }
                }
                next = prev[cand & WMASK];
                if (next >= cand)
                    break;
                cand = next;
            }
            prev[i & WMASK] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= 3) {
            unsigned ls = s_len_sym[best], ds = s_dist_sym[bdist], k;
            put_bits(&w, s_lit_code[257 + ls], s_lit_bits[257 + ls]);
            if (s_len_extra[ls])
                put_bits(&w, (uint32_t)(best - s_len_base[ls]), s_len_extra[ls]);
            put_bits(&w, s_dist_code[ds], 5);
            if (s_dist_extra[ds])
                put_bits(&w, (uint32_t)(bdist - s_dist_base[ds]), s_dist_extra[ds]);
            for (k = 1; k < best; k++) {
                size_t p = i + k;
                if (p + 3 <= n) {
                    uint32_t h = hash3(in + p);
                    prev[p & WMASK] = head[h];
                    head[h] = (int32_t)p;
                }
            }
            i += best;
        } else {
            put_bits(&w, s_lit_code[in[i]], s_lit_bits[in[i]]);
            i++;
        }
    }
    put_bits(&w, s_lit_code[256], s_lit_bits[256]);         /* end of block */
    while (w.bits > 0) {
        uint8_t byte = (uint8_t)w.acc;
        buf_add(out, &byte, 1);
        w.acc >>= 8;
        w.bits -= 8;
    }
    free(head);
    free(prev);
    return out->fail ? -1 : 0;
}

/* ── Files to collect ─────────────────────────────────────── */

typedef struct {
    char name[160];             /* in the zip, '/' separated */
    char path[MAX_PATH * 2];    /* on disk; empty = mem */
    uint64_t size;
    FILETIME mtime;
    int text;                   /* user name hidden in it */
    int stored;                 /* already compressed (PNG): no deflate */
} Entry;

typedef struct {
    Entry e[MAX_ENTRIES];
    int n;
} Entries;

static int add_file(Entries *es, const char *name, const char *path, int text)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    Entry *e;

    if (es->n >= MAX_ENTRIES
        || !GetFileAttributesExA(path, GetFileExInfoStandard, &fa)
        || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    e = &es->e[es->n++];
    memset(e, 0, sizeof *e);
    strcpy_s(e->name, sizeof e->name, name);
    strcpy_s(e->path, sizeof e->path, path);
    e->size = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    e->mtime = fa.ftLastWriteTime;
    e->text = text;
    return 1;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* The newest LOG_RUNS conker_<date>_<time>.log (name order = time order)
 * and their _kernel.log. Returns the newest one's path in newest. */
static void add_logs(Entries *es, const char *save_dir, char *newest, size_t nn)
{
    static char names[256][MAX_PATH];
    char pattern[MAX_PATH * 2], path[MAX_PATH * 2], zname[160];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;

    newest[0] = '\0';
    sprintf_s(pattern, sizeof pattern, "%s\\logs\\conker_*.log", save_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strstr(fd.cFileName, "_kernel.log") && n < 256)
            strcpy_s(names[n++], MAX_PATH, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, MAX_PATH, cmp_names);
    for (i = n > LOG_RUNS ? n - LOG_RUNS : 0; i < n; i++) {
        char *dot;
        sprintf_s(path, sizeof path, "%s\\logs\\%s", save_dir, names[i]);
        sprintf_s(zname, sizeof zname, "logs/%s", names[i]);
        add_file(es, zname, path, 1);
        if (i == n - 1)
            strcpy_s(newest, nn, path);
        dot = strrchr(path, '.');
        *dot = '\0';
        strcat_s(path, sizeof path, "_kernel.log");
        dot = strrchr(zname, '.');
        *dot = '\0';
        strcat_s(zname, sizeof zname, "_kernel.log");
        add_file(es, zname, path, 1);
    }
}

/* saves\ two levels deep (saves\Game1\conker.bin ...). */
static void add_saves(Entries *es, const char *dir, const char *zdir, int depth)
{
    char pattern[MAX_PATH * 2], path[MAX_PATH * 2], zname[160];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    sprintf_s(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == '.')
            continue;
        sprintf_s(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        sprintf_s(zname, sizeof zname, "%s/%s", zdir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2)
                add_saves(es, path, zname, depth + 1);
        } else if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow <= SAVE_MAX) {
            add_file(es, zname, path, 0);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* A whole file, opened beside a writer (this run's log is open). A text
 * file past TRIM_OVER keeps its first and last part. NULL on failure. */
static uint8_t *read_file(const Entry *e, size_t *out_n)
{
    HANDLE h = CreateFileA(e->path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    uint8_t *p = NULL;
    size_t want;
    DWORD got;

    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0)
        goto out;
    if (e->text && (uint64_t)sz.QuadPart > TRIM_OVER) {
        static const char note[] =
            "\r\n[... the middle of this log was left out of the report ...]\r\n";
        LARGE_INTEGER at;
        want = TRIM_HEAD + (sizeof note - 1) + TRIM_TAIL;
        p = (uint8_t *)malloc(want);
        if (!p || !ReadFile(h, p, TRIM_HEAD, &got, NULL) || got != TRIM_HEAD)
            goto fail;
        memcpy(p + TRIM_HEAD, note, sizeof note - 1);
        at.QuadPart = sz.QuadPart - TRIM_TAIL;
        if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN)
            || !ReadFile(h, p + TRIM_HEAD + sizeof note - 1, TRIM_TAIL, &got, NULL)
            || got != TRIM_TAIL)
            goto fail;
        *out_n = want;
        goto out;
    }
    want = (size_t)sz.QuadPart;
    p = (uint8_t *)malloc(want ? want : 1);
    if (!p)
        goto out;
    if (want && (!ReadFile(h, p, (DWORD)want, &got, NULL) || got == 0))
        goto fail;
    *out_n = want ? got : 0;            /* a growing log: what was there */
    goto out;
fail:
    free(p);
    p = NULL;
out:
    CloseHandle(h);
    return p;
}

/* ── Hiding the Windows user name ─────────────────────────── */

static char s_hide[2][128];                 /* user name, profile folder name */

static void hide_init(void)
{
    DWORD n = sizeof s_hide[0];
    const char *prof = getenv("USERPROFILE");

    if (!GetUserNameA(s_hide[0], &n))
        s_hide[0][0] = '\0';
    s_hide[1][0] = '\0';
    if (prof) {
        const char *slash = strrchr(prof, '\\');
        if (slash && _stricmp(slash + 1, s_hide[0]) != 0)
            strcpy_s(s_hide[1], sizeof s_hide[1], slash + 1);
    }
}

static int word_ch(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

/* Every whole-word match of a hidden name -> "<user>", into a new buffer.
 * Names under 2 characters are left alone (they would hit everywhere). */
static uint8_t *hide_names(uint8_t *p, size_t *n)
{
    int k;

    for (k = 0; k < 2; k++) {
        size_t len = strlen(s_hide[k]), i, hits = 0;
        Buf out = { 0 };

        if (len < 2 || *n < len)
            continue;
        for (i = 0; i + len <= *n; i++) {
            if (_strnicmp((const char *)p + i, s_hide[k], len) == 0
                && (i == 0 || !word_ch(p[i - 1]))
                && (i + len == *n || !word_ch(p[i + len]))) {
                hits++;
                break;
            }
        }
        if (!hits)
            continue;
        buf_reserve(&out, *n + 64);
        for (i = 0; i < *n;) {
            if (i + len <= *n
                && _strnicmp((const char *)p + i, s_hide[k], len) == 0
                && (i == 0 || !word_ch(p[i - 1]))
                && (i + len == *n || !word_ch(p[i + len]))) {
                buf_add(&out, "<user>", 6);
                i += len;
            } else {
                buf_add(&out, p + i, 1);
                i++;
            }
        }
        if (out.fail) {
            free(out.p);
            continue;
        }
        free(p);
        p = out.p;
        *n = out.n;
    }
    return p;
}

/* ── report.txt ───────────────────────────────────────────── */

static void pc_windows(Buf *b)
{
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    typedef const char *(CDECL *WineVersionFn)(void);
    typedef void (CDECL *WineHostFn)(const char **, const char **);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn rtl = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    WineVersionFn wine = nt ? (WineVersionFn)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
    WineHostFn host = nt ? (WineHostFn)(void *)GetProcAddress(nt, "wine_get_host_version") : NULL;
    OSVERSIONINFOW vi;
    char disp[64] = "";
    DWORD ubr = 0, sz = sizeof disp;

    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (!rtl || rtl(&vi) != 0)
        vi.dwMajorVersion = 0;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                 "DisplayVersion", RRF_RT_REG_SZ, NULL, disp, &sz);
    sz = sizeof ubr;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                 "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &sz);
    if (vi.dwMajorVersion == 10)
        buf_addf(b, "Windows: Windows %s%s%s (build %lu.%lu)\r\n",
                 vi.dwBuildNumber >= 22000 ? "11" : "10", disp[0] ? " " : "",
                 disp, vi.dwBuildNumber, ubr);
    else
        buf_addf(b, "Windows: %lu.%lu (build %lu)\r\n", vi.dwMajorVersion,
                 vi.dwMinorVersion, vi.dwBuildNumber);
    if (wine) {
        const char *sys = NULL, *rel = NULL;
        if (host)
            host(&sys, &rel);
        buf_addf(b, "Wine: %s%s%s%s%s\r\n", wine(), sys ? " on " : "",
                 sys ? sys : "", rel ? " " : "", rel ? rel : "");
    }
}

static void pc_graphics(Buf *b)
{
    typedef HRESULT (WINAPI *CreateFactoryFn)(REFIID, void **);
    static const GUID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba,
        { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    static const GUID iid_device = { 0x54ec77fa, 0x1377, 0x44e6,
        { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
    HMODULE dx = LoadLibraryA("dxgi.dll");
    CreateFactoryFn create = dx ? (CreateFactoryFn)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad;
    Buf lines = { 0 };
    UINT i;

    if (!create || FAILED(create(&iid_factory1, (void **)&fac)) || !fac) {
        buf_addf(b, "Graphics: (unknown)\r\n");
        return;
    }
    for (i = 0; IDXGIFactory1_EnumAdapters1(fac, i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        LARGE_INTEGER umd;
        char name[256];
        Buf line = { 0 };

        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d))
            && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name,
                                NULL, NULL);
            buf_addf(&line, "Graphics: %s (%.1f GB", name,
                     (double)d.DedicatedVideoMemory / (1024.0 * 1024 * 1024));
            if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(ad, &iid_device, &umd))) {
                unsigned c = HIWORD(umd.LowPart), dd = LOWORD(umd.LowPart);
                buf_addf(&line, ", driver %u.%u.%u.%u", HIWORD(umd.HighPart),
                         LOWORD(umd.HighPart), c, dd);
                if (d.VendorId == 0x10DE)       /* NVIDIA's own number */
                    buf_addf(&line, " = %u.%02u", ((c % 10) * 10000 + dd) / 100,
                             ((c % 10) * 10000 + dd) % 100);
            }
            buf_addf(&line, ", id %04X:%04X)\r\n", d.VendorId, d.DeviceId);
            buf_add(&line, "", 1);
            /* One card can be listed twice with different LUIDs (seen
             * 2026-11-07: an RX 7800 XT beside a virtual monitor): an
             * identical line once. */
            if (!line.fail && (!lines.p || !strstr((const char *)lines.p,
                                                   (const char *)line.p))) {
                buf_add(b, line.p, line.n - 1);
                if (lines.n)
                    lines.n--;                      /* drop the old NUL */
                buf_add(&lines, line.p, line.n);
            }
        }
        free(line.p);
        IDXGIAdapter1_Release(ad);
    }
    free(lines.p);
    IDXGIFactory1_Release(fac);
}

static BOOL CALLBACK monitor_line(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    MONITORINFOEXA mi;
    DEVMODEA dm;

    (void)dc;
    (void)r;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoA(m, (MONITORINFO *)&mi)
        && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        buf_addf((Buf *)lp, "Monitor: %lux%lu at %lu Hz%s\r\n", dm.dmPelsWidth,
                 dm.dmPelsHeight, dm.dmDisplayFrequency,
                 (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (main)" : "");
    return TRUE;
}

static void pc_info(Buf *b)
{
    int r[4], brand[12];
    char *name = (char *)brand;
    MEMORYSTATUSEX ms;

    pc_windows(b);
    pc_graphics(b);
    memset(brand, 0, sizeof brand);
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) {
        __cpuid(brand, (int)0x80000002);
        __cpuid(brand + 4, (int)0x80000003);
        __cpuid(brand + 8, (int)0x80000004);
    }
    name[sizeof brand - 1] = '\0';
    while (*name == ' ')
        name++;
    {
        size_t len = strlen(name);
        while (len && name[len - 1] == ' ')
            name[--len] = '\0';
    }
    buf_addf(b, "CPU: %s (%lu threads)\r\n", *name ? name : "(unknown)",
             GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        buf_addf(b, "RAM: %.1f GB\r\n",
                 (double)ms.ullTotalPhys / (1024.0 * 1024 * 1024));
    EnumDisplayMonitors(NULL, NULL, monitor_line, (LPARAM)b);
}

/* The release from the newest log's "default.xbe SHA-1 <hex>" line (main.c
 * prints it at every start), when the game didn't say. */
static void release_from_log(const char *log)
{
    /* main.c s_xbe_sha1. */
    static const char pal[] = "68A37F0D03D73DABC86A848DF5EC1820DD551254";
    static const char usa[] = "2232A1B3874D67B4BB4CF53A940420D2DAB59535";
    char head[16384], *at;
    FILE *f;
    size_t n;

    if (s_release[0] || !log[0])
        return;
    /* Not fopen_s: it opens unshared, and a frozen game holds this log open
     * for writing (the F10 window of a game still running never gets here). */
    f = _fsopen(log, "rb", _SH_DENYNO);
    if (!f)
        return;
    n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    head[n] = '\0';
    at = strstr(head, "default.xbe SHA-1 ");
    if (!at || strlen(at) < 18 + 40)
        return;
    at += 18;
    memcpy(s_sha1, at, 40);
    s_sha1[40] = '\0';
    strcpy_s(s_release, sizeof s_release,
             !strcmp(s_sha1, pal) ? "PAL" : !strcmp(s_sha1, usa) ? "USA" : "unknown");
}

static void make_report_txt(Buf *b, const BugReport *r, const Entries *es,
                            const SYSTEMTIME *t)
{
    int i;

    buf_addf(b, "Conker: Live & Recompiled bug report\r\n\r\n");
    buf_addf(b, "Made: %04u-%02u-%02u %02u:%02u:%02u, %s\r\n", t->wYear, t->wMonth,
             t->wDay, t->wHour, t->wMinute, t->wSecond,
             r->origin ? r->origin : "conker.exe --bug-report");
    buf_addf(b, "Game version: %s\r\n", CONKER_VERSION_STR);
    if (s_release[0])
        buf_addf(b, "Game release: %s (default.xbe SHA-1 %s)\r\n", s_release, s_sha1);
    else
        buf_addf(b, "Game release: (not in the logs)\r\n");
    if (r->extra && r->extra[0])
        buf_add(b, r->extra, strlen(r->extra));
    buf_addf(b, "\r\nDescription:\r\n");
    if (r->description && r->description[0]) {
        const char *p;
        for (p = r->description; *p; p++) {             /* LF -> CRLF */
            if (*p == '\n' && (p == r->description || p[-1] != '\r'))
                buf_add(b, "\r", 1);
            buf_add(b, p, 1);
        }
        buf_addf(b, "\r\n");
    } else {
        buf_addf(b, "(none given)\r\n");
    }
    buf_addf(b, "\r\nPC:\r\n");
    pc_info(b);
    buf_addf(b, "\r\nFiles:\r\n");
    for (i = 0; i < es->n; i++)
        buf_addf(b, "%s (%llu KB%s)\r\n", es->e[i].name,
                 (unsigned long long)((es->e[i].size + 1023) / 1024),
                 es->e[i].text && es->e[i].size > TRIM_OVER
                 ? ", its middle left out" : "");
}

/* ── The GitHub issue ─────────────────────────────────────── */

#define ISSUE_PAGE "https://github.com/cdetv/conker-live-recompiled/issues/new"
#define URL_MAX    6000         /* GitHub refuses much longer addresses */

/* s[0..n) percent-encoded (RFC 3986 unreserved kept), CRs left out. */
static void url_enc(Buf *b, const char *s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '\r')
            continue;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.'
            || c == '~') {
            buf_add(b, &c, 1);
        } else {
            char e[3];
            e[0] = '%';
            e[1] = hex[c >> 4];
            e[2] = hex[c & 15];
            buf_add(b, e, 3);
        }
    }
}

/* n bytes of s at most, backed up to a whole UTF-8 character. */
static size_t utf8_cut(const char *s, size_t n)
{
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

/* The body: the description's first desc_n bytes (cut = a note that the
 * rest is in the zip), then the game and PC lines (info), then the zip to
 * attach. */
static void issue_body(Buf *b, const char *desc, size_t desc_n, int cut,
                       const Buf *info, const char *zip_name)
{
    if (desc_n) {
        const char *p;
        for (p = desc; p < desc + desc_n; p++) {      /* LF -> CRLF */
            if (*p == '\n' && (p == desc || p[-1] != '\r'))
                buf_add(b, "\r", 1);
            buf_add(b, p, 1);
        }
        if (cut)
            buf_addf(b, "\r\n[... the rest is in report.txt in the zip]");
    } else {
        buf_addf(b, "(no description)");
    }
    buf_addf(b, "\r\n\r\n");
    buf_add(b, info->p, info->n);
    buf_addf(b, "\r\nReport: %s (drag the file into this box to attach it)\r\n",
             zip_name);
}

int bug_report_issue(const char *description, const char *zip_path,
                     char **url, char **text)
{
    const char *desc = description ? description : "";
    const char *zip_name = zip_path ? zip_path : "";
    const char *slash = strrchr(zip_name, '\\');
    size_t desc_n, keep, n;
    Buf title = { 0 }, body = { 0 }, u = { 0 }, info = { 0 };
    uint8_t *t;
    int cut = 0;

    *url = *text = NULL;
    if (slash)
        zip_name = slash + 1;
    hide_init();
    /* The description without blank lines and spaces at its ends. */
    while (*desc == ' ' || *desc == '\t' || *desc == '\r' || *desc == '\n')
        desc++;
    desc_n = strlen(desc);
    while (desc_n && (desc[desc_n - 1] == ' ' || desc[desc_n - 1] == '\t'
                      || desc[desc_n - 1] == '\r' || desc[desc_n - 1] == '\n'))
        desc_n--;

    /* The title: the first line, 100 bytes at most. */
    n = strcspn(desc, "\r\n");
    if (n > desc_n)
        n = desc_n;
    while (n && (desc[n - 1] == ' ' || desc[n - 1] == '\t'))
        n--;
    if (!n) {
        buf_addf(&title, "Bug report");
    } else if (n > 100) {
        buf_add(&title, desc, utf8_cut(desc, 97));
        buf_addf(&title, "...");
    } else {
        buf_add(&title, desc, n);
    }
    buf_add(&title, "", 1);
    n = title.n - 1;
    t = hide_names(title.p, &n);
    title.p = t;
    title.n = n;

    /* The game and the PC, once (DXGI is slow to ask). */
    buf_addf(&info, "Game version: %s\r\n", CONKER_VERSION_STR);
    if (s_release[0])
        buf_addf(&info, "Game release: %s\r\n", s_release);
    pc_windows(&info);
    pc_graphics(&info);

    /* The body, whole for the clipboard. */
    issue_body(&body, desc, desc_n, 0, &info, zip_name);
    n = body.n;
    body.p = hide_names(body.p, &n);
    body.n = n;

    /* The address, the description shortened until it fits. */
    for (keep = desc_n;; ) {
        Buf part = { 0 };
        u.n = 0;
        u.fail = 0;
        issue_body(&part, desc, keep, cut, &info, zip_name);
        n = part.n;
        part.p = hide_names(part.p, &n);
        part.n = n;
        buf_addf(&u, "%s?title=", ISSUE_PAGE);
        url_enc(&u, (const char *)title.p, title.n);
        buf_addf(&u, "&body=");
        url_enc(&u, (const char *)part.p, part.n);
        free(part.p);
        if (u.fail || u.n <= URL_MAX || keep == 0)
            break;
        keep = utf8_cut(desc, keep - (keep / 4 + 1));
        cut = 1;
    }
    buf_add(&u, "", 1);
    buf_add(&body, "", 1);
    free(title.p);
    free(info.p);
    if (u.fail || body.fail || info.fail) {
        free(u.p);
        free(body.p);
        return -1;
    }
    *url = (char *)u.p;
    *text = (char *)body.p;
    return 0;
}

/* ── The zip ──────────────────────────────────────────────── */

static void dos_time(const FILETIME *ft, unsigned *date, unsigned *time)
{
    FILETIME lt;
    SYSTEMTIME st;

    if (!FileTimeToLocalFileTime(ft, &lt) || !FileTimeToSystemTime(&lt, &st)
        || st.wYear < 1980) {
        *date = (0 << 9) | (1 << 5) | 1;
        *time = 0;
        return;
    }
    *date = (unsigned)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    *time = (unsigned)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
}

/* One entry: local header + data to the file, its central record to cd. */
static int zip_entry(HANDLE f, Buf *cd, uint32_t *offset, const char *name,
                     const uint8_t *data, size_t n, const FILETIME *mt, int stored)
{
    Buf hdr = { 0 }, packed = { 0 };
    const uint8_t *body = data;
    size_t body_n = n, name_n = strlen(name);
    unsigned method = 0, date, time;
    uint32_t crc;
    DWORD put;
    int ok;

    tables_init();
    crc = crc32_of(data, n);
    if (!stored && n > 64 && deflate_fixed(data, n, &packed) == 0
        && packed.n < n) {
        body = packed.p;
        body_n = packed.n;
        method = 8;
    }
    dos_time(mt, &date, &time);

    put32(&hdr, 0x04034B50);
    put16(&hdr, 20);
    put16(&hdr, 0);                         /* flags */
    put16(&hdr, method);
    put16(&hdr, time);
    put16(&hdr, date);
    put32(&hdr, crc);
    put32(&hdr, (uint32_t)body_n);
    put32(&hdr, (uint32_t)n);
    put16(&hdr, (unsigned)name_n);
    put16(&hdr, 0);
    buf_add(&hdr, name, name_n);

    put32(cd, 0x02014B50);
    put16(cd, 20);                          /* made by: MS-DOS, 2.0 */
    put16(cd, 20);
    put16(cd, 0);
    put16(cd, method);
    put16(cd, time);
    put16(cd, date);
    put32(cd, crc);
    put32(cd, (uint32_t)body_n);
    put32(cd, (uint32_t)n);
    put16(cd, (unsigned)name_n);
    put16(cd, 0);                           /* extra */
    put16(cd, 0);                           /* comment */
    put16(cd, 0);                           /* disk */
    put16(cd, 0);                           /* internal attributes */
    put32(cd, 0);                           /* external attributes */
    put32(cd, *offset);
    buf_add(cd, name, name_n);

    ok = !hdr.fail && !packed.fail
        && WriteFile(f, hdr.p, (DWORD)hdr.n, &put, NULL) && put == hdr.n
        && (body_n == 0 || (WriteFile(f, body, (DWORD)body_n, &put, NULL)
                            && put == body_n));
    *offset += (uint32_t)(hdr.n + body_n);
    free(hdr.p);
    free(packed.p);
    return ok ? 0 : -1;
}

int bug_report_write(const BugReport *r, char *zip_out, size_t zn)
{
    static Entries es;                      /* ~25 KB: off the stack */
    char dir[MAX_PATH * 2], path[MAX_PATH * 2], part[MAX_PATH * 2 + 8];
    char newest[MAX_PATH * 2], save[MAX_PATH * 2];
    Buf txt = { 0 }, cd = { 0 };
    SYSTEMTIME t;
    FILETIME now;
    HANDLE f;
    uint32_t offset = 0;
    int i, err = 0, entries = 0;
    DWORD put;

    if (!r || !r->save_dir || !r->save_dir[0]) {
        strcpy_s(zip_out, zn, "no user files folder");
        return -1;
    }
    /* A full path: SHCreateDirectoryExA takes no relative one, and the
     * caller gets a zip path that works from anywhere. */
    if (!GetFullPathNameA(r->save_dir, sizeof save, save, NULL))
        strcpy_s(save, sizeof save, r->save_dir);
    hide_init();
    es.n = 0;
    add_logs(&es, save, newest, sizeof newest);
    release_from_log(newest);
    if (r->shot_png && r->shot_png[0] && add_file(&es, "screenshot.png", r->shot_png, 0))
        es.e[es.n - 1].stored = 1;
    sprintf_s(path, sizeof path, "%s\\settings.ini", save);
    add_file(&es, "settings.ini", path, 1);
    sprintf_s(path, sizeof path, "%s\\launcher.ini", save);
    add_file(&es, "launcher.ini", path, 1);
    sprintf_s(path, sizeof path, "%s\\shader_cache\\precompiled.ini", save);
    add_file(&es, "shader_cache/precompiled.ini", path, 1);
    if (r->include_saves) {
        sprintf_s(path, sizeof path, "%s\\saves", save);
        add_saves(&es, path, "saves", 1);
    }

    GetLocalTime(&t);
    GetSystemTimeAsFileTime(&now);
    make_report_txt(&txt, r, &es, &t);
    if (txt.fail) {
        strcpy_s(zip_out, zn, "out of memory");
        free(txt.p);
        return -1;
    }
    {
        size_t n = txt.n;
        txt.p = hide_names(txt.p, &n);
        txt.n = n;
    }

    sprintf_s(dir, sizeof dir, "%s\\reports", save);
    SHCreateDirectoryExA(NULL, dir, NULL);
    sprintf_s(path, sizeof path, "%s\\report_%04u%02u%02u_%02u%02u%02u.zip", dir,
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    for (i = 2; GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && i < 100; i++)
        sprintf_s(path, sizeof path, "%s\\report_%04u%02u%02u_%02u%02u%02u_%d.zip",
                  dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, i);
    sprintf_s(part, sizeof part, "%s.part", path);
    f = CreateFileA(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        sprintf_s(zip_out, zn, "can't write %s (error %lu)", part, GetLastError());
        free(txt.p);
        return -1;
    }

    err |= zip_entry(f, &cd, &offset, "report.txt", txt.p, txt.n, &now, 0);
    entries++;
    free(txt.p);
    for (i = 0; i < es.n && !err; i++) {
        size_t n = 0;
        uint8_t *data = read_file(&es.e[i], &n);
        if (!data)
            continue;                       /* gone or locked: skipped */
        if (es.e[i].text)
            data = hide_names(data, &n);
        err |= zip_entry(f, &cd, &offset, es.e[i].name, data, n, &es.e[i].mtime,
                         es.e[i].stored);
        entries++;
        free(data);
    }
    if (!err && !cd.fail) {
        Buf end = { 0 };
        put32(&end, 0x06054B50);
        put16(&end, 0);
        put16(&end, 0);
        put16(&end, (unsigned)entries);
        put16(&end, (unsigned)entries);
        put32(&end, (uint32_t)cd.n);
        put32(&end, offset);
        put16(&end, 0);
        err = end.fail
            || !WriteFile(f, cd.p, (DWORD)cd.n, &put, NULL) || put != cd.n
            || !WriteFile(f, end.p, (DWORD)end.n, &put, NULL) || put != end.n;
        free(end.p);
    } else {
        err = 1;
    }
    free(cd.p);
    CloseHandle(f);
    if (err || !MoveFileExA(part, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(part);
        strcpy_s(zip_out, zn, "writing the zip failed (disk full?)");
        return -1;
    }
    strcpy_s(zip_out, zn, path);
    fprintf(stderr, "[REPORT] %s: %d files, %lu KB\n", path, entries,
            (unsigned long)((offset + cd.n + 1023) / 1024));
    return 0;
}

/* ── Open Folder, Open GitHub Issue ───────────────────────── */

/* Text in a code page as a malloc'd wide string, or NULL. */
static wchar_t *to_wide(const char *s, UINT cp)
{
    int n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc(n * sizeof(wchar_t)) : NULL;

    if (w && MultiByteToWideChar(cp, 0, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}

/* A window already showing that folder is reused. */
static DWORD WINAPI folder_job(void *arg)
{
    wchar_t *path = (wchar_t *)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED
                                 | COINIT_DISABLE_OLE1DDE);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path);
    HRESULT hr = pidl ? SHOpenFolderAndSelectItems(pidl, 0, NULL, 0) : E_FAIL;

    if (pidl)
        ILFree(pidl);
    if (FAILED(hr)) {
        size_t n = wcslen(path) + 16;
        wchar_t *args = (wchar_t *)malloc(n * sizeof(wchar_t));
        if (args) {
            swprintf(args, n, L"/select,\"%ls\"", path);
            ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL,
                          SW_SHOWNORMAL);
            free(args);
        }
    }
    fprintf(stderr, "[REPORT] folder opened%s\n",
            FAILED(hr) ? " (explorer /select)" : "");
    free(path);
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

void bug_report_open_folder(const char *zip_path)
{
    wchar_t *w = to_wide(zip_path, CP_ACP);
    HANDLE t = w ? CreateThread(NULL, 0, folder_job, w, 0, NULL) : NULL;

    if (t)
        CloseHandle(t);
    else
        free(w);
}

typedef struct {
    wchar_t *url, *text;
    HWND owner;
} IssueJob;

/* The text first: the window says to paste it if the page comes up empty,
 * e.g. after GitHub's sign-in. */
static DWORD WINAPI issue_job(void *arg)
{
    IssueJob *j = (IssueJob *)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED
                                 | COINIT_DISABLE_OLE1DDE);
    int copied = 0, i;
    HINSTANCE r;

    for (i = 0; i < 10 && j->text; i++) {
        if (OpenClipboard(j->owner)) {
            size_t bytes = (wcslen(j->text) + 1) * sizeof(wchar_t);
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
            void *p = g ? GlobalLock(g) : NULL;
            EmptyClipboard();
            if (p) {
                memcpy(p, j->text, bytes);
                GlobalUnlock(g);
                copied = SetClipboardData(CF_UNICODETEXT, g) != NULL;
            }
            if (g && !copied)
                GlobalFree(g);
            CloseClipboard();
            break;
        }
        Sleep(20);                  /* another program has it open */
    }
    r = ShellExecuteW(NULL, L"open", j->url, NULL, NULL, SW_SHOWNORMAL);
    fprintf(stderr, "[REPORT] issue page opened%s; text %s\n",
            (INT_PTR)r > 32 ? "" : " (failed)",
            copied ? "copied" : "not copied");
    free(j->url);
    free(j->text);
    free(j);
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

int bug_report_open_issue(const char *url, const char *text, void *owner)
{
    IssueJob *j = (IssueJob *)calloc(1, sizeof *j);
    HANDLE t = NULL;

    if (!j)
        return -1;
    j->url = url ? to_wide(url, CP_ACP) : NULL;
    j->text = text ? to_wide(text, CP_UTF8) : NULL;
    j->owner = (HWND)owner;
    if (j->url)
        t = CreateThread(NULL, 0, issue_job, j, 0, NULL);
    if (t) {
        CloseHandle(t);
        return 0;
    }
    free(j->url);
    free(j->text);
    free(j);
    return -1;
}
