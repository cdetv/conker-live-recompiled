/*
 * Report Bug's small window (to-do 12c, SPEC 9.2 "Crash", "Freeze"): what
 * `conker.exe --bug-report --crash | --freeze` shows. A process of its own,
 * started by the game when it crashed (the question first) or by F10 with no
 * new picture (the game stays as it is), because neither game can be trusted
 * to draw: a window of the game's own would wait on whatever is stuck, and in
 * exclusive full screen it would not be seen at all.
 *
 * Plain Win32 and no dialog manager (IsDialogMessage in a plain window sends
 * Enter to IDOK and leaves Tab to the multi-line box's mercy): the message
 * loop handles Tab, Enter, Esc and Ctrl+A itself. Esc in the description only
 * leaves the box, as in the game's window: the text is all this report has.
 * The zip is written on the window's thread: under a second, even with a
 * 65 MB log, and nothing else is going on here.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bug_report.h"

/* Windows' own look for the buttons and boxes (comctl32 v6), here and in the
 * game's message boxes. */
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define APP_TITLE L"Conker: Live & Recompiled"

enum { ID_EDIT = 100, ID_SAVES, ID_B1, ID_B2, ID_B3 };
enum { STAGE_EDIT, STAGE_DONE, STAGE_FAILED };

static const wchar_t NOTE_EDIT[] =
    L"Makes a .zip to attach to a GitHub issue: the game's last logs, its "
    L"settings, the saves when ticked and the PC's details (Windows, graphics "
    L"card, processor). The Windows user name is left out.";
static const wchar_t NOTE_DONE[] =
    L"Open GitHub Issue opens a new issue with its text filled in and copies "
    L"that text too: if the page comes up empty, paste it there (Ctrl+V). Then "
    L"drag the zip into the issue. Posting needs a free GitHub account.";
static const wchar_t STATUS_ISSUE[] =
    L"Opening the issue page in your browser; its text is on the clipboard too.";

typedef struct {
    const char *save_dir;
    int kind, stage, dpi;
    HWND win, head, edit, saves, note, status, b[3];
    HFONT font;
    char zip[MAX_PATH * 2];
    char *url, *text;
} Rw;

static Rw s_rw;

static int S(int v)
{
    return MulDiv(v, s_rw.dpi, 96);
}

/* How tall `text` wraps at `width` in the window's font. */
static int text_h(const wchar_t *text, int width)
{
    HDC dc = GetDC(s_rw.win);
    HGDIOBJ old = SelectObject(dc, s_rw.font);
    RECT r = { 0, 0, width, 0 };

    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(s_rw.win, dc);
    return r.bottom;
}

static int button_w(HWND b)
{
    wchar_t t[64];
    HDC dc = GetDC(s_rw.win);
    HGDIOBJ old = SelectObject(dc, s_rw.font);
    SIZE z = { 0, 0 };

    GetWindowTextW(b, t, 64);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &z);
    SelectObject(dc, old);
    ReleaseDC(s_rw.win, dc);
    return z.cx + S(24) > S(84) ? z.cx + S(24) : S(84);
}

static void place(HWND h, int x, int y, int w, int ht)
{
    SetWindowPos(h, NULL, x, y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(h, SW_SHOWNA);
}

/* Lay out the stage and make the window fit it. */
static void layout(void)
{
    const int cw = S(480), m = S(12), gap = S(8), inner = cw - 2 * m;
    const int bh = S(26);
    wchar_t t[1024];
    int y = m, x, i, count;
    RECT r;

    GetWindowTextW(s_rw.head, t, 1024);
    i = text_h(t, inner);
    place(s_rw.head, m, y, inner, i);
    y += i + gap;
    if (s_rw.stage == STAGE_EDIT) {
        place(s_rw.edit, m, y, inner, S(150));
        y += S(150) + gap;
        place(s_rw.saves, m, y, inner, S(20));
        y += S(20) + gap;
    } else {
        ShowWindow(s_rw.edit, SW_HIDE);
        ShowWindow(s_rw.saves, SW_HIDE);
    }
    if (s_rw.stage != STAGE_FAILED) {
        GetWindowTextW(s_rw.note, t, 1024);
        i = text_h(t, inner);
        place(s_rw.note, m, y, inner, i);
        y += i + gap;
    } else {
        ShowWindow(s_rw.note, SW_HIDE);
    }
    /* Room for a line of what the last button did, so nothing jumps. */
    i = text_h(s_rw.stage == STAGE_DONE ? STATUS_ISSUE : L"Creating the report...",
               inner);
    place(s_rw.status, m, y, inner, i);
    y += i + gap;

    count = s_rw.stage == STAGE_DONE ? 3 : 2;
    x = cw - m;
    for (i = count - 1; i >= 0; i--) {
        int w = button_w(s_rw.b[i]);
        x -= w;
        place(s_rw.b[i], x, y, w, bh);
        x -= gap;
    }
    if (count < 3)
        ShowWindow(s_rw.b[2], SW_HIDE);
    y += bh + m;

    r.left = r.top = 0;
    r.right = cw;
    r.bottom = y;
    AdjustWindowRectEx(&r, GetWindowLongW(s_rw.win, GWL_STYLE), FALSE,
                       GetWindowLongW(s_rw.win, GWL_EXSTYLE));
    SetWindowPos(s_rw.win, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(s_rw.win, NULL, TRUE);
}

static void set_stage(int stage, const wchar_t *head)
{
    s_rw.stage = stage;
    SetWindowTextW(s_rw.head, head);
    SetWindowTextW(s_rw.status, L"");
    if (stage == STAGE_EDIT) {
        SetWindowTextW(s_rw.note, NOTE_EDIT);
        SetWindowTextW(s_rw.b[0], L"Create Report");
        SetWindowTextW(s_rw.b[1], L"Cancel");
    } else if (stage == STAGE_DONE) {
        SetWindowTextW(s_rw.note, NOTE_DONE);
        SetWindowTextW(s_rw.b[0], L"Open Folder");
        SetWindowTextW(s_rw.b[1], L"Open GitHub Issue");
        SetWindowTextW(s_rw.b[2], L"Close");
    } else {
        SetWindowTextW(s_rw.b[0], L"Back");
        SetWindowTextW(s_rw.b[1], L"Close");
    }
    layout();
    SetFocus(stage == STAGE_EDIT ? s_rw.edit
             : stage == STAGE_DONE ? s_rw.b[1] : s_rw.b[0]);
}

static const wchar_t *edit_head(void)
{
    return s_rw.kind == BUG_WINDOW_CRASH
        ? L"Conker crashed. What were you doing when it happened? (optional)"
        : L"The game has stopped responding. What were you doing? (optional)";
}

/* The description as a malloc'd UTF-8 string, or NULL. */
static char *description(void)
{
    int n = GetWindowTextLengthW(s_rw.edit) + 1, u;
    wchar_t *w = (wchar_t *)malloc(n * sizeof(wchar_t));
    char *out = NULL;

    if (!w)
        return NULL;
    GetWindowTextW(s_rw.edit, w, n);
    u = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u > 0 && (out = (char *)malloc(u)) != NULL)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, u, NULL, NULL);
    free(w);
    return out;
}

static void create_report(void)
{
    char *desc = description();
    wchar_t head[MAX_PATH * 2 + 64], wzip[MAX_PATH * 2];
    const char *name;
    BugReport r;
    int rc;

    SetWindowTextW(s_rw.status, L"Creating the report...");
    EnableWindow(s_rw.b[0], FALSE);
    EnableWindow(s_rw.b[1], FALSE);
    UpdateWindow(s_rw.win);
    SetCursor(LoadCursor(NULL, IDC_WAIT));

    memset(&r, 0, sizeof r);
    r.save_dir = s_rw.save_dir;
    r.description = desc;
    r.include_saves = SendMessageW(s_rw.saves, BM_GETCHECK, 0, 0) == BST_CHECKED;
    r.origin = s_rw.kind == BUG_WINDOW_CRASH ? "after a crash" : "F10 in a freeze";
    rc = bug_report_write(&r, s_rw.zip, sizeof s_rw.zip);
    if (rc == 0)
        bug_report_issue(desc, s_rw.zip, &s_rw.url, &s_rw.text);
    free(desc);

    SetCursor(LoadCursor(NULL, IDC_ARROW));
    EnableWindow(s_rw.b[0], TRUE);
    EnableWindow(s_rw.b[1], TRUE);
    name = rc == 0 ? strrchr(s_rw.zip, '\\') : NULL;
    MultiByteToWideChar(CP_ACP, 0, name ? name + 1 : s_rw.zip, -1, wzip,
                        MAX_PATH * 2);
    swprintf(head, MAX_PATH * 2 + 64, rc == 0 ? L"Report saved: %ls"
             : L"The report could not be made: %ls", wzip);
    set_stage(rc == 0 ? STAGE_DONE : STAGE_FAILED, head);
}

/* The frozen game stays where it is; only this window comes off the top, so
 * what the button opens can be seen. */
static void leave_top(void)
{
    SetWindowPos(s_rw.win, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void on_button(int id)
{
    if (s_rw.stage == STAGE_EDIT) {
        if (id == ID_B1)
            create_report();
        else if (id == ID_B2)
            DestroyWindow(s_rw.win);
    } else if (s_rw.stage == STAGE_DONE) {
        if (id == ID_B1) {
            leave_top();
            bug_report_open_folder(s_rw.zip);
        } else if (id == ID_B2) {
            leave_top();
            SetWindowTextW(s_rw.status,
                           bug_report_open_issue(s_rw.url, s_rw.text, s_rw.win) == 0
                           ? STATUS_ISSUE : L"The issue page could not be opened.");
        } else if (id == ID_B3) {
            DestroyWindow(s_rw.win);
        }
    } else {
        if (id == ID_B1)
            set_stage(STAGE_EDIT, edit_head());
        else if (id == ID_B2)
            DestroyWindow(s_rw.win);
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) >= ID_B1 && LOWORD(w) <= ID_B3)
            on_button(LOWORD(w));
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == s_rw.note || (HWND)l == s_rw.status) {
            /* Grey, as the game's window says the same things. */
            SetTextColor((HDC)w, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)w, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Tab, Enter, Esc and Ctrl+A. Nonzero = taken. */
static int key_down(const MSG *msg)
{
    HWND focus = GetFocus();
    int ctrl = GetKeyState(VK_CONTROL) < 0;

    switch (msg->wParam) {
    case VK_TAB: {
        HWND next = GetNextDlgTabItem(s_rw.win, focus,
                                      GetKeyState(VK_SHIFT) < 0);
        if (next) {
            if (next == s_rw.edit)
                SendMessageW(next, EM_SETSEL, 0, -1);
            SetFocus(next);
        }
        return 1;
    }
    case VK_RETURN:
        if (focus != s_rw.edit && focus) {
            SendMessageW(focus, BM_CLICK, 0, 0);
            return 1;
        }
        return 0;
    case VK_ESCAPE:
        if (focus == s_rw.edit)
            SetFocus(s_rw.b[0]);
        else if (s_rw.stage == STAGE_FAILED)
            on_button(ID_B2);
        else
            on_button(s_rw.stage == STAGE_DONE ? ID_B3 : ID_B2);
        return 1;
    case 'A':
        if (ctrl && focus == s_rw.edit) {
            SendMessageW(s_rw.edit, EM_SETSEL, 0, -1);
            return 1;
        }
        return 0;
    }
    return 0;
}

static HWND make_child(const wchar_t *cls, const wchar_t *text, DWORD style,
                       int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10,
                             s_rw.win, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)s_rw.font, FALSE);
    return h;
}

int bug_report_window(const char *save_dir, int kind)
{
    WNDCLASSW wc;
    NONCLIENTMETRICSW nm;
    HINSTANCE inst = GetModuleHandleW(NULL);
    HWND under = GetForegroundWindow();
    MONITORINFO mi;
    HDC dc;
    MSG msg;
    RECT r;
    int i;

    SetProcessDPIAware();
    if (kind == BUG_WINDOW_CRASH
        && MessageBoxW(NULL, L"Conker crashed. Create a bug report?", APP_TITLE,
                       MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND)
           != IDYES)
        return 0;

    memset(&s_rw, 0, sizeof s_rw);
    s_rw.save_dir = save_dir;
    s_rw.kind = kind;
    dc = GetDC(NULL);
    s_rw.dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    nm.cbSize = sizeof nm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0))
        s_rw.font = CreateFontIndirectW(&nm.lfMessageFont);
    if (!s_rw.font)
        s_rw.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"ConkerReportBug";
    RegisterClassW(&wc);
    /* Topmost over a frozen game, which holds the screen. */
    s_rw.win = CreateWindowExW(kind == BUG_WINDOW_FREEZE ? WS_EX_TOPMOST : 0,
                               wc.lpszClassName, L"Report Bug - " APP_TITLE,
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               0, 0, S(500), S(400), NULL, NULL, inst, NULL);
    if (!s_rw.win)
        return 1;
    s_rw.head = make_child(L"STATIC", L"", SS_NOPREFIX, 0);
    s_rw.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE
                                | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10,
                                s_rw.win, (HMENU)(INT_PTR)ID_EDIT, inst, NULL);
    SendMessageW(s_rw.edit, WM_SETFONT, (WPARAM)s_rw.font, FALSE);
    SendMessageW(s_rw.edit, EM_SETLIMITTEXT, 16000, 0);
    s_rw.saves = make_child(L"BUTTON", L"Include save files",
                            WS_TABSTOP | BS_AUTOCHECKBOX, ID_SAVES);
    SendMessageW(s_rw.saves, BM_SETCHECK, BST_CHECKED, 0);
    s_rw.note = make_child(L"STATIC", L"", SS_NOPREFIX, 0);
    s_rw.status = make_child(L"STATIC", L"", SS_NOPREFIX, 0);
    for (i = 0; i < 3; i++)
        s_rw.b[i] = make_child(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON,
                               ID_B1 + i);
    /* Show the focus box from the start: this window is made for the keys. */
    SendMessageW(s_rw.win, WM_CHANGEUISTATE,
                 MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);

    set_stage(STAGE_EDIT, edit_head());

    /* In the middle of the monitor the game is on. */
    mi.cbSize = sizeof mi;
    GetWindowRect(s_rw.win, &r);
    if (GetMonitorInfoW(MonitorFromWindow(under, MONITOR_DEFAULTTOPRIMARY), &mi))
        SetWindowPos(s_rw.win, NULL,
                     (mi.rcWork.left + mi.rcWork.right - (r.right - r.left)) / 2,
                     (mi.rcWork.top + mi.rcWork.bottom - (r.bottom - r.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(s_rw.win, SW_SHOW);
    SetForegroundWindow(s_rw.win);
    SetFocus(s_rw.edit);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && key_down(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    free(s_rw.url);
    free(s_rw.text);
    return 0;
}
