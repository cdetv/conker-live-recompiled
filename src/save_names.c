/**
 * Save folders named after their saves (SPEC 9.1 user files, user 2026-10-31).
 *
 * XAPI keeps each save in U:\<12 hex digits>\, a hash of the save's name
 * (0x0042E6BB: the name's UTF-16 characters as one number, modulo
 * 2^48 - 59): "Game 1" is 10C2167B1950, the multiplayer profile "drop"
 * 0072006F177C. On disk they are <save dir>\saves\Game1 and saves\drop
 * instead; the game still sees the hex names:
 *
 *   - g_xbox_path_redirect sends U:\ itself to saves\ and U:\<hex>\... to the
 *     folder of the save whose SaveMeta.xbx name hashes to <hex>. U:\'s own
 *     files (TitleMeta.xbx, TitleImage.xbx, SaveImage.xbx) stay in the old
 *     UserData\4d530051\.
 *   - g_xbox_dir_entry_redirect lists saves\<folder> as its hex name, so the
 *     game's look through U:\ (multiplayer profiles) finds them.
 *   - A save the game creates gets its named folder at once: the entry hook
 *     on 0x0042E6BB (recomp_manual.c) tells save_names_seen which name a hex
 *     belongs to before XAPI creates the folder.
 *
 * Story slots are GameN in every language ("Game 1", "Spiel 1", "Jeu 1",
 * "Juego 1", "Partita 1"); other names keep their text with characters
 * Windows refuses replaced by "_", and " (2)" when a folder is taken. A
 * folder is found by its SaveMeta.xbx name, so renaming one by hand is
 * harmless. At start the old layout, UserData\4d530051\<hex>\, is moved in,
 * and a saves\<hex> folder with a name is renamed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "save_names.h"

/* kernel.h; the host path is WCHAR on Windows. */
extern int (*g_xbox_path_redirect)(const char *xbox_path,
                                   WCHAR *host_path, DWORD buf_size);
extern int (*g_xbox_dir_entry_redirect)(const WCHAR *host_dir,
                                        const WCHAR *host_name,
                                        char *guest_name, DWORD buf_size);

#define MAX_SAVES 128
#define MAX_SEEN  64

typedef struct {
    char  hex[13];
    WCHAR folder[MAX_PATH];     /* in saves\ */
} SaveFolder;

typedef struct {
    char  hex[13];
    WCHAR name[64];
} SeenName;

static SaveFolder s_map[MAX_SAVES];
static int s_count;
static SeenName s_seen[MAX_SEEN];
static int s_seen_next;
static CRITICAL_SECTION s_lock;
static int s_on;
static WCHAR s_saves[MAX_PATH];      /* <save dir>\saves */
static WCHAR s_saves_final[MAX_PATH];/* as GetFinalPathNameByHandleW gives it */
static WCHAR s_old[MAX_PATH];        /* <save dir>\UserData\4d530051 */
static WCHAR s_save_root[MAX_PATH];
static int (*s_prev)(const char *, WCHAR *, DWORD);

static void hash_name(const WCHAR *name, char out[13])
{
    uint64_t h = 0;
    for (; *name; name++)
        h = (h * 0x10000u + *name) % 0xFFFFFFFFFFC5ull;
    sprintf_s(out, 13, "%012llX", (unsigned long long)h);
}

static int is_hex12_w(const WCHAR *s)
{
    int i;
    for (i = 0; i < 12; i++)
        if (!iswxdigit(s[i]))
            return 0;
    return s[12] == L'\0';
}

/* "Name=..." from <dir>\SaveMeta.xbx (UTF-16). */
static int meta_name(const WCHAR *dir, WCHAR *name, size_t n)
{
    WCHAR path[MAX_PATH], buf[256];
    HANDLE h;
    DWORD got = 0;
    WCHAR *p, *end;

    swprintf_s(path, MAX_PATH, L"%s\\SaveMeta.xbx", dir);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE
                    | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(h, buf, sizeof(buf) - sizeof(WCHAR), &got, NULL);
    CloseHandle(h);
    buf[got / sizeof(WCHAR)] = L'\0';
    p = wcsstr(buf, L"Name=");
    if (!p)
        return 0;
    p += 5;
    end = p + wcscspn(p, L"\r\n");
    if (end == p || (size_t)(end - p) >= n)
        return 0;
    wcsncpy_s(name, n, p, (size_t)(end - p));
    return 1;
}

/* The folder name a save name gets (before " (2)"). */
static void folder_for(const WCHAR *name, WCHAR *out, size_t n)
{
    static const WCHAR *const slots[] = { L"Game ", L"Spiel ", L"Jeu ",
                                          L"Juego ", L"Partita " };
    static const WCHAR *const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"LPT1", L"LPT2", L"LPT3" };
    size_t i, len;

    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        size_t k = wcslen(slots[i]);
        if (!wcsncmp(name, slots[i], k) && name[k] >= L'1' && name[k] <= L'9'
            && !name[k + 1]) {
            swprintf_s(out, n, L"Game%c", name[k]);
            return;
        }
    }
    wcscpy_s(out, n, name);
    for (i = 0; out[i]; i++)
        if (out[i] < 32 || wcschr(L"<>:\"/\\|?*", out[i]))
            out[i] = L'_';
    len = wcslen(out);
    while (len && (out[len - 1] == L' ' || out[len - 1] == L'.'))
        out[--len] = L'\0';
    if (!len)
        wcscpy_s(out, n, L"_");
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (!_wcsicmp(out, reserved[i]))
            wcscat_s(out, n, L"_");
}

static int folder_exists(const WCHAR *folder)
{
    WCHAR path[MAX_PATH];
    DWORD a;
    swprintf_s(path, MAX_PATH, L"%s\\%s", s_saves, folder);
    a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* <base>, else "<base> (2)", ... : one not in saves\ yet. */
static void free_folder(const WCHAR *base, WCHAR *out, size_t n)
{
    int k;
    wcscpy_s(out, n, base);
    for (k = 2; folder_exists(out) && k < 1000; k++)
        swprintf_s(out, n, L"%s (%d)", base, k);
}

static SaveFolder *find_hex(const char *hex)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (!_stricmp(s_map[i].hex, hex))
            return &s_map[i];
    return NULL;
}

static void put(const char *hex, const WCHAR *folder)
{
    SaveFolder *e = find_hex(hex);
    if (!e) {
        if (s_count >= MAX_SAVES)
            return;
        e = &s_map[s_count++];
        strcpy_s(e->hex, sizeof(e->hex), hex);
    }
    wcscpy_s(e->folder, MAX_PATH, folder);
}

/* The hex a folder in saves\ stands for: its save's name, else its own
 * name when that is a hex name (a folder XAPI made before its name came). */
static int folder_hex(const WCHAR *folder, char hex[13])
{
    WCHAR dir[MAX_PATH], name[64];
    swprintf_s(dir, MAX_PATH, L"%s\\%s", s_saves, folder);
    if (meta_name(dir, name, 64)) {
        hash_name(name, hex);
        return 1;
    }
    if (is_hex12_w(folder)) {
        WideCharToMultiByte(CP_ACP, 0, folder, -1, hex, 13, NULL, NULL);
        _strupr_s(hex, 13);
        return 1;
    }
    return 0;
}

static void scan(void)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char hex[13];

    swprintf_s(pattern, MAX_PATH, L"%s\\*", s_saves);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")
            && folder_hex(fd.cFileName, hex))
            put(hex, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The folder in saves\ for a hex name (s_lock held). */
static void lookup(const char *hex, WCHAR *folder, size_t n)
{
    SaveFolder *e = find_hex(hex);
    int i;

    if (!e || !folder_exists(e->folder)) {
        scan();
        e = find_hex(hex);
    }
    if (e && folder_exists(e->folder)) {
        wcscpy_s(folder, n, e->folder);
        return;
    }
    /* Not on disk (yet): named after the save when XAPI just hashed it. */
    for (i = 0; i < MAX_SEEN; i++)
        if (!_stricmp(s_seen[i].hex, hex)) {
            WCHAR base[MAX_PATH];
            folder_for(s_seen[i].name, base, MAX_PATH);
            free_folder(base, folder, n);
            put(hex, folder);
            return;
        }
    MultiByteToWideChar(CP_ACP, 0, hex, -1, folder, (int)n);
    put(hex, folder);
}

void save_names_seen(const wchar_t *name)
{
    char hex[13];
    int i;

    if (!s_on || !*name || wcslen(name) >= 64)
        return;
    hash_name(name, hex);
    EnterCriticalSection(&s_lock);
    for (i = 0; i < MAX_SEEN; i++)
        if (!strcmp(s_seen[i].hex, hex))
            break;
    if (i == MAX_SEEN) {
        i = s_seen_next;
        s_seen_next = (s_seen_next + 1) % MAX_SEEN;
        strcpy_s(s_seen[i].hex, sizeof(s_seen[i].hex), hex);
        wcscpy_s(s_seen[i].name, 64, name);
    }
    LeaveCriticalSection(&s_lock);
}

static int redirect(const char *xbox_path, WCHAR *host, DWORD n)
{
    static const char prefix[] = "\\Device\\Harddisk0\\partition1\\UDATA\\4d530051";
    const size_t len = sizeof(prefix) - 1;
    const char *rest;
    char hex[13];
    WCHAR folder[MAX_PATH], tail[MAX_PATH];
    int i;

    if (_strnicmp(xbox_path, prefix, len)
        || (xbox_path[len] && xbox_path[len] != '\\'))
        return s_prev ? s_prev(xbox_path, host, n) : 0;
    rest = xbox_path + len;
    if (*rest == '\\')
        rest++;
    if (!*rest) {                       /* U:\ itself: the list of saves */
        wcscpy_s(host, n, s_saves);
        return 1;
    }
    for (i = 0; i < 12; i++)
        if (!isxdigit((unsigned char)rest[i]))
            return 0;                   /* U:\'s own files */
    if (rest[12] && rest[12] != '\\')
        return 0;
    memcpy(hex, rest, 12);
    hex[12] = '\0';
    _strupr_s(hex, sizeof(hex));
    MultiByteToWideChar(CP_ACP, 0, rest + 12, -1, tail, MAX_PATH);
    EnterCriticalSection(&s_lock);
    lookup(hex, folder, MAX_PATH);
    LeaveCriticalSection(&s_lock);
    swprintf_s(host, n, L"%s\\%s%s", s_saves, folder, tail);
    {
        size_t k = wcslen(host);
        while (k > 1 && host[k - 1] == L'\\')
            host[--k] = L'\0';
    }
    return 1;
}

static int dir_entry(const WCHAR *host_dir, const WCHAR *host_name,
                     char *guest_name, DWORD n)
{
    int i, found = 0;

    if (_wcsicmp(host_dir, s_saves_final))
        return 0;
    EnterCriticalSection(&s_lock);
    for (int pass = 0; pass < 2 && !found; pass++) {
        if (pass)
            scan();
        for (i = 0; i < s_count; i++)
            if (!_wcsicmp(s_map[i].folder, host_name)) {
                strcpy_s(guest_name, n, s_map[i].hex);
                found = 1;
                break;
            }
    }
    LeaveCriticalSection(&s_lock);
    return found;
}

/* Moves <from> to saves\<folder>, or to backups\ when that save is in
 * saves\ already (the folder there wins: it is the one played since). */
static void move_in(const WCHAR *from, const char *hex, const WCHAR *base)
{
    WCHAR folder[MAX_PATH], to[MAX_PATH];
    SaveFolder *e;

    scan();
    e = find_hex(hex);
    if (e && folder_exists(e->folder)) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf_s(to, MAX_PATH, L"%s\\backups\\%04u-%02u-%02u %02u%02u%02u old "
                   L"save layout", s_save_root, t.wYear, t.wMonth, t.wDay,
                   t.wHour, t.wMinute, t.wSecond);
        {
            WCHAR backups[MAX_PATH];
            swprintf_s(backups, MAX_PATH, L"%s\\backups", s_save_root);
            CreateDirectoryW(backups, NULL);
        }
        CreateDirectoryW(to, NULL);
        wcscat_s(to, MAX_PATH, wcsrchr(from, L'\\'));
        fprintf(stderr, "[SAVES] %S is in saves\\%S already: old copy to %S\n",
                from, e->folder, to);
    } else {
        free_folder(base, folder, MAX_PATH);
        swprintf_s(to, MAX_PATH, L"%s\\%s", s_saves, folder);
        fprintf(stderr, "[SAVES] %S -> saves\\%S\n", wcsrchr(from, L'\\') + 1,
                folder);
    }
    if (!MoveFileExW(from, to, 0))
        fprintf(stderr, "[SAVES] could not move %S (error %lu)\n", from,
                GetLastError());
}

/* The old layout's save folders, and saves\<hex> folders that have a name. */
static void tidy(void)
{
    WCHAR pattern[MAX_PATH], dir[MAX_PATH], name[64], base[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char hex[13];
    int pass;

    for (pass = 0; pass < 2; pass++) {
        swprintf_s(pattern, MAX_PATH, L"%s\\*", pass ? s_saves : s_old);
        h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                || !is_hex12_w(fd.cFileName))
                continue;
            swprintf_s(dir, MAX_PATH, L"%s\\%s", pass ? s_saves : s_old,
                       fd.cFileName);
            if (!meta_name(dir, name, 64)) {
                if (pass)
                    continue;           /* no name: keeps its hex name */
                wcscpy_s(base, MAX_PATH, fd.cFileName);
                WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, hex, 13,
                                    NULL, NULL);
                _strupr_s(hex, 13);
            } else {
                folder_for(name, base, MAX_PATH);
                hash_name(name, hex);
            }
            if (pass) {
                WCHAR folder[MAX_PATH], to[MAX_PATH];
                free_folder(base, folder, MAX_PATH);
                swprintf_s(to, MAX_PATH, L"%s\\%s", s_saves, folder);
                fprintf(stderr, "[SAVES] saves\\%S -> saves\\%S\n",
                        fd.cFileName, folder);
                if (!MoveFileExW(dir, to, 0))
                    fprintf(stderr, "[SAVES] could not rename %S (error %lu)\n",
                            dir, GetLastError());
            } else {
                move_in(dir, hex, base);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void save_names_init(const char *save_dir)
{
    HANDLE h;
    WCHAR userdata[MAX_PATH];

    /* The same conversion xbox_path_init uses, so the paths match. */
    MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_root, MAX_PATH);
    swprintf_s(s_saves, MAX_PATH, L"%s\\saves", s_save_root);
    swprintf_s(userdata, MAX_PATH, L"%s\\UserData", s_save_root);
    swprintf_s(s_old, MAX_PATH, L"%s\\4d530051", userdata);
    CreateDirectoryW(s_saves, NULL);
    CreateDirectoryW(userdata, NULL);
    CreateDirectoryW(s_old, NULL);      /* U:\'s own files stay there */
    InitializeCriticalSection(&s_lock);

    h = CreateFileW(s_saves, 0, FILE_SHARE_READ | FILE_SHARE_WRITE
                    | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[SAVES] no saves folder (%S): old layout kept\n",
                s_saves);
        return;
    }
    {
        WCHAR final[MAX_PATH];
        DWORD k = GetFinalPathNameByHandleW(h, final, MAX_PATH,
                                            FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (!k || k >= MAX_PATH) {
            fprintf(stderr, "[SAVES] saves folder unreadable: old layout kept\n");
            return;
        }
        wcscpy_s(s_saves_final, MAX_PATH,
                 wcsncmp(final, L"\\\\?\\", 4) ? final : final + 4);
    }

    tidy();
    scan();
    s_on = 1;
    s_prev = g_xbox_path_redirect;
    g_xbox_path_redirect = redirect;
    g_xbox_dir_entry_redirect = dir_entry;
    fprintf(stderr, "[SAVES] %d save folder(s) in %S\n", s_count, s_saves);
}
