/* ui_text.h - the recomp's own menus in the game's languages (to-do 229).
 *
 * The F2 / F3 / F1 / photo / pad menu text is English in the code; T() gives
 * it in the menu language from lang\<code>.txt next to the .exe (de, fr, es,
 * it, ja), English where a file or a line is missing. The English text is the
 * key. Strings meant for translation are marked in the code so that
 * scripts\check_lang.py finds them: T("...") / TL("...") where they are drawn,
 * N_("...") where they are handed to a helper that calls T() itself (the F2
 * option helpers, combo name lists). --debug pages, the Report Bug window,
 * the console and log lines stay English (user 2026-11-17).
 *
 * The menu language: settings.ini [language] ui_language (auto = the Text
 * language, or one of them), RECOMP_UI_LANGUAGE; it switches at once. While
 * --dump-settings runs, everything stays English (the launcher stays English).
 */
#ifndef UI_TEXT_H
#define UI_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Marks a string for the translation list; the helper it is handed to
 * translates it. */
#define N_(s) s

/* The text in the menu language (or en itself). Leading spaces (the notes'
 * indent) are kept and not part of the key. The pointer lasts until the next
 * ui_text_frame(). */
const char *T(const char *en);

/* A widget label: the shown text plus "###" and the English text, so the
 * widget's ImGui id (focus, the pad's cursor, open lists) stays the same in
 * every language. "Foo##id" keeps its own id part. */
const char *TL(const char *en);

/* Once a frame, before any menu is built: follows the menu language setting
 * and loads its file when it changed. */
void ui_text_frame(void);

/* While on, T() and TL() give the English text (the --dump-settings run). */
void ui_text_english(int on);

/* The language now shown: "" English, else "de", "fr", "es", "it", "ja". */
const char *ui_text_code(void);

/* Whether a Japanese font was found (set by the overlay at start); without
 * one Japanese shows in English. */
void ui_text_set_japanese_font(int found);
int ui_text_japanese_font(void);

#ifdef __cplusplus
}
#endif

#endif
