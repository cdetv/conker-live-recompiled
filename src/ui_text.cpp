/* ui_text.cpp - the recomp's own menus in the game's languages (to-do 229);
 * see ui_text.h. C++ for the hash maps; called from the overlay only (one
 * thread: the frame's). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>

#include "settings.h"
#include "ui_text.h"

static std::unordered_map<std::string, std::string> s_table;  /* English -> shown */
/* What T() / TL() handed out, by the English asked for (leading spaces and
 * all): unordered_map nodes stay put, so the pointers last until the cache
 * is cleared at a language change, between frames. An empty T value: none,
 * the English pointer itself goes back. */
static std::unordered_map<std::string, std::string> s_tcache, s_lcache;
static char s_code[3];                  /* "" = English */
static bool s_tried;                    /* ui_text_frame ran once */
static int s_english;
static int s_jp_font = -1;              /* -1 not known yet (treated as found) */

static const struct { const char *name, *code; } s_langs[] = {
    { "english", "" }, { "french", "fr" }, { "german", "de" },
    { "italian", "it" }, { "spanish", "es" }, { "japanese", "ja" },
};

static void clear_caches(void)
{
    s_tcache.clear();
    s_lcache.clear();
}

/* "\n" and "\\" in a line of the file. */
static std::string unescape(const char *s, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && (s[i + 1] == 'n' || s[i + 1] == '\\')) {
            out += s[i + 1] == 'n' ? '\n' : '\\';
            i++;
        } else {
            out += s[i];
        }
    }
    return out;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

/* lang\<code>.txt next to the .exe: "en: <English>" then "<code>: <text>"
 * (scripts\check_lang.py writes and checks them). */
static int load(const char *code)
{
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash;
    FILE *f;
    long size;
    char *buf;
    int lines = 0;

    s_table.clear();
    if (!*code)
        return 0;
    if (n == 0 || n >= MAX_PATH || !(slash = wcsrchr(path, L'\\')))
        return -1;
    swprintf(slash + 1, MAX_PATH + 31 - (slash + 1 - path), L"lang\\%hs.txt", code);
    if (_wfopen_s(&f, path, L"rb") || !f)
        return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[size] = '\0';

    std::string key;
    bool have_key = false;
    char *p = buf;
    if (!strncmp(p, "\xEF\xBB\xBF", 3))         /* a BOM an editor added */
        p += 3;
    while (*p) {
        char *end = strchr(p, '\n'), *next;
        size_t len;
        if (end) {
            next = end + 1;
        } else {
            end = p + strlen(p);
            next = end;
        }
        len = (size_t)(end - p);
        if (len && p[len - 1] == '\r')
            len--;
        if (len >= 4 && p[0] == 'e' && p[1] == 'n' && p[2] == ':' && p[3] == ' ') {
            const char *k = skip_spaces(p + 4);
            key = unescape(k, len - (size_t)(k - p));
            have_key = true;
        } else if (have_key && len >= 3 && !strncmp(p, code, 2) && p[2] == ':') {
            const char *v = p + 3;
            if (*v == ' ')
                v++;
            if ((size_t)(v - p) < len) {
                s_table[key] = unescape(v, len - (size_t)(v - p));
                lines++;
            }
            have_key = false;
        }
        p = next;
    }
    free(buf);
    return lines;
}

static const char *wanted_name(void)
{
    const char *ui = settings_env_override("ui_language")
                         ? getenv("RECOMP_UI_LANGUAGE") : settings_get("ui_language");
    if (!ui || !*ui || !_stricmp(ui, "auto"))
        ui = settings_env_override("language") ? getenv("RECOMP_LANGUAGE")
                                               : settings_get("language");
    return ui ? ui : "english";
}

extern "C" void ui_text_frame(void)
{
    const char *name = wanted_name(), *code = "";

    for (size_t i = 0; i < sizeof s_langs / sizeof *s_langs; i++)
        if (!_stricmp(name, s_langs[i].name))
            code = s_langs[i].code;
    if (!strcmp(code, "ja") && s_jp_font == 0)
        code = "";
    /* TL() keeps every label it made; a label made from changing text
     * would pile up, so past a size the caches start again (between
     * frames, so no pointer handed out is in use). */
    if (s_tcache.size() + s_lcache.size() > 8192) {
        fprintf(stderr, "[UITEXT] text cache over 8192: emptied\n");
        clear_caches();
    }
    if (s_tried && !strcmp(code, s_code))
        return;
    s_tried = true;
    strcpy_s(s_code, sizeof s_code, code);
    clear_caches();
    int lines = load(code);
    if (!*code)
        fprintf(stderr, "[UITEXT] menu language: English\n");
    else if (lines < 0)
        fprintf(stderr, "[UITEXT] menu language %s: lang\\%s.txt not found, English\n", code,
                code);
    else
        fprintf(stderr, "[UITEXT] menu language %s: %d lines (lang\\%s.txt)\n", code, lines,
                code);
}

extern "C" const char *T(const char *en)
{
    if (s_english || s_table.empty() || !en || !*en)
        return en;
    auto it = s_tcache.find(en);
    if (it != s_tcache.end())
        return it->second.c_str();
    /* Only what has a translation is kept: text made with numbers in it
     * (a note with the size drawn) misses every time and must not pile up. */
    const char *k = skip_spaces(en);
    auto t = s_table.find(k);
    if (t == s_table.end() || t->second.empty())
        return en;
    return s_tcache.emplace(en, std::string(en, (size_t)(k - en)) + t->second)
        .first->second.c_str();
}

extern "C" const char *TL(const char *en)
{
    if (!en)
        return en;
    auto it = s_lcache.find(en);
    if (it != s_lcache.end())
        return it->second.c_str();
    const char *id = strstr(en, "###"), *cut = strstr(en, "##");
    std::string shown(en, cut ? (size_t)(cut - en) : strlen(en));
    std::string v = T(shown.c_str());
    if (id) {
        v += id;
    } else {
        v += "###";
        v += en;
    }
    return s_lcache.emplace(en, v).first->second.c_str();
}

extern "C" void ui_text_english(int on)
{
    if (s_english != on)
        clear_caches();
    s_english = on;
}

extern "C" const char *ui_text_code(void)
{
    return s_code;
}

extern "C" void ui_text_set_japanese_font(int found)
{
    s_jp_font = found ? 1 : 0;
    s_tried = false;                    /* look again at the next frame */
}

extern "C" int ui_text_japanese_font(void)
{
    return s_jp_font != 0;
}
