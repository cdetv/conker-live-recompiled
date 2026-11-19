/* The PC's own ANSI code page, under conker.exe's UTF-8 one (to-do 262).
 *
 * The manifest (src/conker.manifest) makes CP_ACP and GetACP() UTF-8, so
 * folder names with any letters work through every ANSI call. Two kinds of
 * text still come in the PC's own code page ("language for non-Unicode
 * programs", 1252 on Western Windows) and need it by number:
 *   - WM_CHAR in the game window, an ANSI one (the toolkit's fb_present.c):
 *     Windows converts typed characters to the PC's code page, not the
 *     process's (tested 2026-11-18: e-acute still 0xE9, l-stroke 'l');
 *   - a path a player typed into settings.ini with an editor that saved in
 *     that code page (settings.c reads it as such when it isn't UTF-8).
 * Header only, for both C and C++ files. */
#ifndef CONKER_LEGACY_CP_H
#define CONKER_LEGACY_CP_H

#include <windows.h>

/* What GetACP() returned before the manifest: the registry value it reads
 * (Nls\CodePage\ACP), else the system locale's, else 1252. */
static inline UINT conker_legacy_cp(void)
{
    static UINT cp;
    if (!cp) {
        WCHAR v[16];
        DWORD n = sizeof v, num = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage",
                         L"ACP", RRF_RT_REG_SZ, NULL, v, &n) == ERROR_SUCCESS)
            cp = (UINT)wcstoul(v, NULL, 10);
        if (!cp && GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT,
                                   LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                                   (LPWSTR)&num, sizeof num / sizeof(WCHAR)))
            cp = num;
        if (!cp)
            cp = 1252;
    }
    return cp;
}

/* A typed character from WM_CHAR (an ANSI window: past ASCII, a byte in the
 * PC's code page) as a Unicode character, as before the manifest. */
static inline unsigned conker_wm_char(unsigned c)
{
    if (c >= 0x80 && c < 0x100) {
        char b = (char)c;
        wchar_t wc;
        if (MultiByteToWideChar(conker_legacy_cp(), MB_ERR_INVALID_CHARS, &b, 1, &wc, 1) == 1)
            c = wc;
    }
    return c;
}

/* Whether s is valid UTF-8 (plain ASCII is). */
static inline int conker_is_utf8(const char *s)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0;
}

#endif
