/**
 * Conker: Live & Reloaded - Recompiled Game Entry Point
 *
 * Based on xboxrecomp/templates/new-game/src/main.c. Conker changes: the entry
 * point, and game/save folders taken from the command line:
 *
 *   conker.exe [--game-dir <folder>] [--save-dir <folder>] [--debug]
 *
 *   --game-dir  folder holding default.xbe and the disc files (read-only).
 *               Default: a "game" folder next to the .exe (the launcher
 *               imports the disc there; a dev build has a link there to
 *               CONKER_GAME_DIR, see CMakeLists.txt).
 *   --save-dir  where T:/U:/cache/partition images go (the user files).
 *               Default: "userdata" next to the .exe, or
 *               %LOCALAPPDATA%\conker-lr-recomp (default_save_dir).
 *   --debug     the F2 menu also shows debug-only choices, and the title
 *               bar the frame rate.
 *
 *   conker.exe --dump-settings
 *
 *               for the launcher (SPEC 9.2): every settings.ini option and
 *               the F2 and F3 menus' entries as JSON to stdout, then exit;
 *               reads and writes no file and starts nothing.
 *
 *   conker.exe --list-pads
 *
 *               for the launcher's Slots choices: the controllers the game
 *               sees, as JSON to stdout (takes 1.5 s), then exit.
 *
 * Started without its output captured (double-clicked, or from the
 * launcher), it writes its log to <save dir>\logs (open_log).
 *
 * The player's settings are in settings.ini in the save folder
 * (src/settings.c); it can also give the game folder. F2 in the game opens
 * a menu that edits them (src/overlay.cpp).
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * XBE Details (from tools.xbe_parser; PAL disc, hash in docs/conker-notes.md):
 *   Title:       Conker: Live and Reloaded
 *   Title ID:    0x4D530051
 *   Base addr:   0x00010000
 *   Entry point: 0x0042F0E9
 *   Code size:   ~4592 KB (.text at 0x00011000)
 *   XDK:         5849, built 2005-04-28
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <io.h>         /* _dup2, _get_osfhandle: open_log */
#include <shlobj.h>     /* SHCreateDirectoryExA */
#include <bcrypt.h>     /* the default.xbe check */

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

#define CONKER_ENTRY_POINT   0x0042F0E9  /* XBE entry point VA */

/* The window's and message boxes' title (SPEC 9.2). */
#define APP_TITLE    L"Conker: Live & Recompiled"
#define APP_TITLE_A  "Conker: Live & Recompiled"

/* The toolkit's window (fb_present.c). */
extern void xbox_FramebufferWindowSetAppTitle(const wchar_t *title, int stats);

/* ── Game and save folders ─────────────────────────────────── */

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];

/* settings.ini in the save folder (src/settings.c), and the F2 menu that
 * edits it (src/overlay.cpp). */
#include "settings.h"
#include "decensor.h"
#include "save_names.h"
extern void overlay_init(void);
extern void overlay_dump_settings(FILE *f);
extern void overlay_list_pads(FILE *f);

/* ── Built-in defaults for the runtime switches ────────────── */

/* The switches Conker needs to run at all, which the toolkit leaves off for
 * other titles. Each becomes the default here, so a plain double-click runs
 * the game; the environment (run.ps1 -Env) still wins. Most of them are read
 * in the toolkit as "set at all?", where =0 would mean on, so an off word
 * (0, off, false, no) removes the variable instead: the runtime then sees
 * exactly the old unset state. Applied after settings.ini (which holds none
 * of these) and before anything reads them. */
static const struct { const char *name, *value; } s_defaults[] = {
    { "RECOMP_EXT_VMA",       "1" },
    { "RECOMP_AC97_READY",    "1" },
    { "RECOMP_APU_DSP_ACK",   "0x82AC8810" },  /* conker-notes: DSP doorbell */
    { "RECOMP_FB_WINDOW",     "1" },
    { "RECOMP_PB_EXEC",       "1" },
    { "RECOMP_VBLANK",        "1" },
    { "RECOMP_PGRAPH_SWM",    "1;5;C;D;E" },
    { "RECOMP_TITLE_KEVENTS", "1" },
    { "RECOMP_USB",           "1" },
    { "RECOMP_FLIP_PACE",     "title" },
    { "RECOMP_GPU",           "d3d11" },
    { "RECOMP_VSH_GPU",       "1" },      /* vertex programs on the GPU */
    { "RECOMP_PAD_FOLLOW",    "1" },      /* last pad used is player 1 */
    { "RECOMP_IO_LATENCY_MS", "2" },      /* async reads pending 2 ms, as on disc */
    { "RECOMP_CUTOUT_DEPTH",  "1" },      /* barbed wire hides smoke behind it */
    { "RECOMP_VSH_PRECOMPILE", "1" },     /* shaders found in memory compiled early */
};

static void apply_builtin_defaults(void)
{
    size_t i;

    for (i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        const char *name = s_defaults[i].name;
        const char *v = getenv(name);
        if (!v) {
            _putenv_s(name, s_defaults[i].value);
        } else if (!strcmp(v, "0") || !_stricmp(v, "off")
                   || !_stricmp(v, "false") || !_stricmp(v, "no")) {
            fprintf(stderr, "[DEFAULTS] %s=%s: off\n", name, v);
            _putenv_s(name, "");    /* an empty value removes it */
        } else if (strcmp(v, s_defaults[i].value)) {
            fprintf(stderr, "[DEFAULTS] %s=%s from the environment "
                    "(built-in %s)\n", name, v, s_defaults[i].value);
        }
    }
}

/* The log of a run nobody captures (SPEC 9.2): stdout and stderr, which
 * the game and the runtime print everything to, go to
 * <save dir>\logs\conker_<date>_<time>.log, and the runtime's kernel log
 * next to it; the newest LOG_KEEP runs are kept. A run whose output is
 * already going somewhere (run.ps1, a console) is left alone. */
#define LOG_KEEP 5

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void prune_logs(const char *dir)
{
    static char names[64][MAX_PATH];
    char pattern[MAX_PATH], path[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;

    sprintf_s(pattern, MAX_PATH, "%s\\conker_*.log", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strstr(fd.cFileName, "_kernel.log") && n < 64)
            strcpy_s(names[n++], MAX_PATH, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    /* conker_<date>_<time>.log: name order is time order. */
    qsort(names, n, MAX_PATH, cmp_names);
    for (i = 0; i + LOG_KEEP <= n; i++) {        /* <=: room for this run's */
        char *dot;
        sprintf_s(path, sizeof path, "%s\\%s", dir, names[i]);
        DeleteFileA(path);
        dot = strrchr(path, '.');
        if (dot) {
            *dot = '\0';
            strcat_s(path, sizeof path, "_kernel.log");
            DeleteFileA(path);
        }
    }
}

static void open_log(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char dir[MAX_PATH], path[MAX_PATH], kpath[MAX_PATH];
    SYSTEMTIME t;
    FILE *f;

    if (out && out != INVALID_HANDLE_VALUE
        && GetFileType(out) != FILE_TYPE_UNKNOWN)
        return;
    sprintf_s(dir, MAX_PATH, "%s\\logs", s_save_dir);
    SHCreateDirectoryExA(NULL, dir, NULL);
    prune_logs(dir);
    GetLocalTime(&t);
    sprintf_s(path, MAX_PATH, "%s\\conker_%04u%02u%02u_%02u%02u%02u.log", dir,
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (freopen_s(&f, path, "w", stdout) || !f)
        return;
    /* A windowed .exe starts with no stderr descriptor (_fileno = -2), and
     * _dup2 needs a real one to point at the log: open stderr on NUL first,
     * or every stderr line (settings, [CRASH], most diagnostics) is lost
     * (to-do 111). */
    if (_fileno(stderr) < 0) {
        FILE *e;
        if (freopen_s(&e, "NUL", "w", stderr) || !e)
            return;
    }
    if (_dup2(_fileno(stdout), _fileno(stderr)) != 0)
        fprintf(stdout, "[LOG] stderr not captured: _dup2 failed\n");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    if (!getenv("XBOX_KERNEL_LOG")) {
        strcpy_s(kpath, MAX_PATH, path);
        kpath[strlen(kpath) - 4] = '\0';                   /* ".log" */
        strcat_s(kpath, MAX_PATH, "_kernel.log");
        _putenv_s("XBOX_KERNEL_LOG", kpath);
    }
}

/* The save folder without --save-dir (SPEC 9.1, user files): "userdata"
 * next to the .exe, or %LOCALAPPDATA%\conker-lr-recomp when
 * user_files_in_appdata.txt sits next to the .exe (the launcher's choice),
 * when the folder can't be written (Program Files), or when it doesn't exist
 * yet while the AppData one holds an older version's files: the launcher
 * moves those in at its next start, and until then the game keeps using
 * them. The launcher always passes --save-dir, so these rules only matter
 * for a double-clicked conker.exe. */
static void default_save_dir(char *out)
{
    char exe[MAX_PATH], path[MAX_PATH], appdata_dir[MAX_PATH], *slash;
    const char *appdata = getenv("LOCALAPPDATA");
    BOOL use_appdata;
    HANDLE h;

    /* Our own folder rather than the runtime's shared default
     * (%LOCALAPPDATA%\xboxrecomp), so saves and partition images from
     * other recompiled games never mix with Conker's. */
    sprintf_s(appdata_dir, MAX_PATH, "%s\\conker-lr-recomp",
              appdata ? appdata : ".");
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    slash = strrchr(exe, '\\');
    if (slash)
        *slash = '\0';
    sprintf_s(out, MAX_PATH, "%s\\userdata", exe);

    sprintf_s(path, MAX_PATH, "%s\\user_files_in_appdata.txt", exe);
    use_appdata = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    if (!use_appdata && GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
        char ini[MAX_PATH], udata[MAX_PATH];
        sprintf_s(ini, MAX_PATH, "%s\\settings.ini", appdata_dir);
        sprintf_s(udata, MAX_PATH, "%s\\UserData", appdata_dir);
        use_appdata = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES
                      || GetFileAttributesA(udata) != INVALID_FILE_ATTRIBUTES;
    }
    if (!use_appdata) {
        /* Writable? A probe file, as a folder's read-only flag says nothing. */
        CreateDirectoryA(out, NULL);
        sprintf_s(path, MAX_PATH, "%s\\write_test.tmp", out);
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                        NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return;
        }
    }
    strcpy_s(out, MAX_PATH, appdata_dir);
}

/* The language (settings.ini language, RECOMP_LANGUAGE; the toolkit answers
 * the game's XGetLanguage with it) needs its text on the disc, in
 * dvddata\aid\text\<Language>: the PAL disc has all six. Without it the game
 * would have no menus, so English instead. */
static void check_language(void)
{
    static const char *const folders[] = { "English", "Japanese", "German",
                                           "French", "Spanish", "Italian" };
    const char *lang = getenv("RECOMP_LANGUAGE");
    char dir[MAX_PATH];
    int i, id = 0;

    if (!lang || !*lang)
        return;
    for (i = 0; i < 6; i++)
        if (!_stricmp(lang, folders[i]))
            id = i + 1;
    if (!id)
        id = atoi(lang);
    if (id < 1 || id > 6) {
        fprintf(stderr, "[LANGUAGE] %s is not one of Conker's: English\n", lang);
        _putenv_s("RECOMP_LANGUAGE", "");
        return;
    }
    sprintf_s(dir, MAX_PATH, "%s\\dvddata\\aid\\text\\%s", s_game_dir,
              folders[id - 1]);
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[LANGUAGE] no %s text on this disc: English\n",
                folders[id - 1]);
        _putenv_s("RECOMP_LANGUAGE", "");
        return;
    }
    fprintf(stderr, "[LANGUAGE] %s\n", folders[id - 1]);
}

/* Fill s_game_dir and s_save_dir from the command line, falling back to the
 * defaults described at the top of this file. settings.ini is read in
 * between: it lives in the save folder, and its game_dir comes after
 * --game-dir but before the built-in default. Returns FALSE on a bad
 * argument. */
static BOOL parse_args(int argc, char **argv)
{
    const char *game_dir = NULL, *save_dir = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) {
            save_dir = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            settings_set_debug(1);
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n"
                    "Usage: conker [--game-dir <folder>] "
                    "[--save-dir <folder>] [--debug]\n", argv[i]);
            return FALSE;
        }
    }

    if (save_dir) {
        strcpy_s(s_save_dir, MAX_PATH, save_dir);
    } else {
        default_save_dir(s_save_dir);
    }
    open_log();

    /* Sets the runtime switches it holds, so before anything reads one. */
    settings_load(s_save_dir);
    apply_builtin_defaults();
    {
        /* Compiled vertex-program shaders kept between runs (the toolkit's
         * nv2a_vsh_gpu.c), in the save folder: a program then compiles once
         * per machine, not once per run. The environment wins; an off word
         * turns it off. */
        const char *v = getenv("RECOMP_VSH_GPU_CACHE");
        if (!v) {
            char dir[MAX_PATH];
            sprintf_s(dir, MAX_PATH, "%s\\shader_cache", s_save_dir);
            _putenv_s("RECOMP_VSH_GPU_CACHE", dir);
        } else if (!strcmp(v, "0") || !_stricmp(v, "off")
                   || !_stricmp(v, "false") || !_stricmp(v, "no")) {
            fprintf(stderr, "[DEFAULTS] RECOMP_VSH_GPU_CACHE=%s: off\n", v);
            _putenv_s("RECOMP_VSH_GPU_CACHE", "");
        }
    }
    {
        /* Replacement textures (the toolkit's tex_replace.h), PNGs named
         * after the texture's hash, in a "textures" folder next to the .exe:
         * the "Live & Recompiled" title logo. Same rules as above. */
        const char *v = getenv("RECOMP_TEX_REPLACE");
        if (!v) {
            WCHAR exe[MAX_PATH], *slash;
            char dir[MAX_PATH * 3];     /* UTF-8, as the toolkit reads it */
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            slash = wcsrchr(exe, L'\\');
            if (slash)
                *slash = L'\0';
            wcscat_s(exe, MAX_PATH, L"\\textures");
            WideCharToMultiByte(CP_UTF8, 0, exe, -1, dir, sizeof dir, NULL,
                                NULL);
            _putenv_s("RECOMP_TEX_REPLACE", dir);
        } else if (!strcmp(v, "0") || !_stricmp(v, "off")
                   || !_stricmp(v, "false") || !_stricmp(v, "no")) {
            fprintf(stderr, "[DEFAULTS] RECOMP_TEX_REPLACE=%s: off\n", v);
            _putenv_s("RECOMP_TEX_REPLACE", "");
        }
    }
    if (!game_dir)
        game_dir = settings_game_dir();

    if (game_dir) {
        strcpy_s(s_game_dir, MAX_PATH, game_dir);
    } else {
        /* "game" next to the .exe, not the current directory, so the .exe
         * can be started from anywhere. */
        char exe[MAX_PATH], *slash;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        slash = strrchr(exe, '\\');
        if (slash)
            *slash = '\0';
        sprintf_s(s_game_dir, MAX_PATH, "%s\\game", exe);
    }

    /* A trailing separator would double up when default.xbe is joined on. */
    {
        size_t n = strlen(s_game_dir);
        while (n > 1 && (s_game_dir[n - 1] == '\\' || s_game_dir[n - 1] == '/'))
            s_game_dir[--n] = '\0';
    }
    check_language();
    return TRUE;
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);
static BOOL xbe_supported(const void *xbe, size_t size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The emulated USB host controllers (xboxrecomp/src/usb/ohci.h), the same
 * situation as the APU: XAPI is linked into the game and drives the OHCI
 * registers itself, so the pad exists only if xbox_OhciInit runs and the VEH
 * hands it the faults. Both do nothing unless RECOMP_USB is set. */
extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            /* USB host controller registers -> the emulated OHCI. */
            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    char xbe_path[MAX_PATH];

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (__argc == 2 && strcmp(__argv[1], "--dump-settings") == 0) {
        settings_use_defaults(".");
        overlay_dump_settings(stdout);
        return 0;
    }
    if (__argc == 2 && strcmp(__argv[1], "--list-pads") == 0) {
        overlay_list_pads(stdout);
        return 0;
    }

    printf("=== Conker: Live & Reloaded - Static Recompilation ===\n");

    /* __argc/__argv: the MSVC CRT's parsed command line, the same whether
     * the process came in through WinMain or main. */
    if (!parse_args(__argc, __argv))
        return 1;
    /* The window's title; --debug adds the toolkit's FPS and draw count. */
    xbox_FramebufferWindowSetAppTitle(APP_TITLE, settings_debug());
    sprintf_s(xbe_path, MAX_PATH, "%s\\default.xbe", s_game_dir);
    printf("Game dir: %s\n", s_game_dir);
    printf("Save dir: %s\n", s_save_dir);
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* RECOMP_SAMPLE_PROFILE: which functions each thread spends its time in
     * (src/sample_profiler.c). */
    {
        extern void sample_profiler_start(void);
        sample_profiler_start();
    }

    /* The F2 settings menu: hooks into the window and the GPU back end,
     * which only call them once they exist. */
    overlay_init();
    /* Sharper shadows (src/shadows.c): the shadow pictures drawn bigger. */
    {
        extern void shadows_init(void);
        shadows_init();
    }
    /* Smoother world shadows (src/lightmaps.c): light maps as a B-spline. */
    {
        extern void lightmaps_init(void);
        lightmaps_init();
    }
    /* Keyboard and mouse (src/controls.c): settings.ini's keys to the
     * toolkit's pad, which then also holds the mouse in the window. */
    {
        extern void controls_apply(void);
        controls_apply();
    }

    /* Step 1: Load XBE. The launcher imports the disc (SPEC 9.3); without
     * it, say so rather than which file is missing. */
    if (GetFileAttributesA(xbe_path) == INVALID_FILE_ATTRIBUTES) {
        printf("No default.xbe in the game folder\n");
        MessageBoxW(NULL, L"Conker Live & Reloaded game files not imported "
                    L"yet. Please use the launcher to set up the game.",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (!load_xbe(xbe_path, &xbe_data, &xbe_size)) {
        char msg[MAX_PATH + 128];
        sprintf_s(msg, sizeof(msg), "Failed to load default.xbe from:\n%s\n\n"
                  "Re-import the game files with the launcher.", xbe_path);
        MessageBoxA(NULL, msg, APP_TITLE_A, MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);
    if (!xbe_supported(xbe_data, xbe_size)) {
        MessageBoxW(NULL, L"These Conker: Live & Reloaded game files are a "
                    L"version this port doesn't support. It runs the PAL "
                    L"(Europe) and USA discs: please re-import one of those "
                    L"with the launcher.",
                    APP_TITLE, MB_OK | MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    APP_TITLE_A, MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* D3D's GPU-time fence (docs/conker-notes.md, D3D table).
     *
     * D3D_BlockOnTime spins until the dword at [D3D_g_pDevice+0x30] (a block
     * the device init allocates) reaches the last fence time it submitted,
     * [D3D_g_pDevice+0x2C]. On hardware the GPU writes it when it passes the
     * fence; nothing here executes the push buffer, so the runtime copies the
     * submitted time across instead. Without it the first fence after device
     * creation never completes. */
    xbox_Nv2aMirrorFence(0x005499E8u /* D3D_g_pDevice */,
                         0x2Cu /* last fence time */,
                         0x30u /* -> GPU-time dword */);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. It lives in xemu_apu.dll next to the .exe (SPEC 9.5): without
     * it the title's first sound register access would crash, so a missing
     * or wrong DLL stops here with a message instead. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
        if (!g_apu_state) {
            MessageBoxW(NULL, L"The sound emulation (xemu_apu.dll) is missing "
                        L"from the game's folder or doesn't fit this version. "
                        L"Please install the game again from its zip.",
                        APP_TITLE, MB_OK | MB_ICONERROR);
            free(xbe_data);
            return 1;
        }
    }

    /* The USB host controllers, for the gamepad (RECOMP_USB). After the
     * memory layout, because it protects the register blocks in guest
     * memory so XAPI's accesses fault into the VEH above. */
    xbox_OhciInit();

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(s_game_dir, s_save_dir);
    }
    /* The decensor patch's redirect (src/decensor.c), before any open. */
    decensor_init(s_save_dir);
    /* Save folders named after their saves (src/save_names.c); after the
     * decensor redirect, which it passes other paths on to. */
    save_names_init(s_save_dir);

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", CONKER_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── Xbox Dashboard hand-off ───────────────────────────────── */

extern void xbox_FramebufferWindowSetFullscreen(int on);

/* The game is about to launch the Dashboard (or another image), which ends
 * the process (recomp_manual.c, sub_0042E562_enter): say so instead of the
 * window just vanishing. Out of full screen first, so the box is seen. */
void conker_dashboard_notice(void)
{
    static LONG shown;

    fprintf(stderr, "[LIVE] the game is launching the Xbox Dashboard: closing\n");
    fflush(stderr);
    if (InterlockedExchange(&shown, 1))
        return;
    xbox_FramebufferWindowSetFullscreen(0);
    MessageBoxW(NULL,
        L"The game wants to open the Xbox Dashboard (for Xbox Live or to free "
        L"up save space). The PC version has no Dashboard, so the game will "
        L"close now.",
        APP_TITLE, MB_OK | MB_ICONINFORMATION | MB_TOPMOST
        | MB_SETFOREGROUND);
}

/* ── XBE Loading ───────────────────────────────────────────── */

/* default.xbe SHA-1s the recompiled code was made from (PAL) or matches
 * byte for byte outside the header (USA; docs/conker-notes.md 2026-10-30). */
static const char *const s_xbe_sha1[] = {
    "68A37F0D03D73DABC86A848DF5EC1820DD551254",     /* PAL */
    "2232A1B3874D67B4BB4CF53A940420D2DAB59535",     /* USA */
};

static BOOL xbe_supported(const void *xbe, size_t size)
{
    BCRYPT_ALG_HANDLE alg;
    UCHAR digest[20];
    char hex[41];
    int i;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0))
        return TRUE;                    /* can't tell: let it run */
    i = BCryptHash(alg, NULL, 0, (PUCHAR)xbe, (ULONG)size, digest, 20);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (i)
        return TRUE;
    for (i = 0; i < 20; i++)
        sprintf_s(hex + i * 2, 3, "%02X", digest[i]);
    printf("default.xbe SHA-1 %s\n", hex);
    for (i = 0; i < (int)(sizeof s_xbe_sha1 / sizeof *s_xbe_sha1); i++)
        if (!strcmp(hex, s_xbe_sha1[i]))
            return TRUE;
    return FALSE;
}

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
