/**
 * Mouse in menus (to-do 40 steps 3-4, SPEC 10.1): what the game side
 * (recomp_manual.c, game thread) tells the menu mouse (menu_mouse.cpp,
 * executor) about the page open now (a main menu board zoomed into: Game
 * 1-3, Options, Chapters; or the single player pause screen), and the
 * requests going back.
 *
 * Where each text is: every UI text is drawn by 0x00248C10, its letters
 * placed by 0x003D30C0, which leaves each letter's corners at 0x00807230
 * (page units) while the text's world x view x projection waits at
 * 0x008072B0; the wrappers keep each text's box, in the picture's normalised
 * coordinates (x -1 left .. 1 right, y -1 bottom .. 1 top), for the frame.
 * Which list the keys move: the one whose input (0x00249ED0, from the lists'
 * 0x00249D00) ran last.
 */
#ifndef CONKER_MENU_PAGE_H
#define CONKER_MENU_PAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENU_PAGE_ITEMS 24
#define MENU_PAGE_PROMPTS 6

enum {
    MENU_SCREEN_NONE,
    MENU_SCREEN_BOARD,  /* a main menu board's page (Game 1-3, Options, Chapters) */
    MENU_SCREEN_PAUSE   /* the single player pause screen */
};

enum { MENU_BTN_A, MENU_BTN_B, MENU_BTN_X, MENU_BTN_Y };

enum {
    MENU_LIST_NONE,     /* no list takes the keys */
    MENU_LIST_PLAIN,    /* rows (Options, Play / Erase, Yes / No) */
    MENU_LIST_WHEEL     /* the Chapters page's parts: the chosen one stays in the middle */
};

typedef struct {
    float x0, y0, x1, y1;   /* normalised: x0 < x1, y0 < y1 (y up) */
    int ok;                 /* drawn this frame */
} MenuBox;

typedef struct {
    MenuBox label;
    MenuBox no, yes;        /* a No / Yes line's two words */
    int yesno;              /* a No / Yes line (kind 'O') */
    int on;                 /* its value: Yes */
} MenuPageItem;

typedef struct {
    MenuBox box;            /* its text and button picture together */
    int button;             /* MENU_BTN_* a click presses */
    uint32_t text;          /* its words (the text element; it glows when hovered) */
} MenuPrompt;

typedef struct {
    uint32_t seq;           /* bracketed frames drawn so far */
    int screen;             /* MENU_SCREEN_* */
    int board;              /* the board open: 1 Options, 2 Chapters, 3-5 Game 1-3; -1 none */
    int pause_state;        /* the pause screen: 0 its page, 1 Options, 2 the Quit? box */
    int kind;               /* MENU_LIST_* of the list taking the keys */
    uint32_t list;          /* that list (requests name it) */
    int sideways;           /* its rows side by side (Play / Erase, Yes / No) */
    int n, cursor;
    MenuPageItem item[MENU_PAGE_ITEMS];
    MenuBox rows;           /* the wheel: every row drawn (parts and fillers) */
    MenuBox row_top, row_bot;   /* and its top and bottom ones */
    MenuPrompt prompt[MENU_PAGE_PROMPTS];   /* texts a button acts on (New Game; the
                                             * pause screen's A / B / X / Y lines) */
    int nprompt;
    int chapter;            /* the Chapters page: the chapter shown 0..8, else -1 */
    int chapter_still;      /* not cross-fading: left / right are taken */
    MenuBox chapter_name;
} MenuPageSnap;

/* The multiplayer menus (to-do 40 step 5): one list module draws every
 * list there (Welcome, Dumbots, Profiles, the map list, Configure, team
 * select, the multiplayer pause menu ...) into a 256 x 256 texture
 * (0x0020C870, its camera -128..128, y down) and wraps it round a glass
 * cylinder in front of the camera (0x002170E0). The game side keeps where
 * each row was drawn on the texture and where the cylinder is on the
 * screen (a grid of points down and across it), so the mouse can be
 * carried back onto the texture. */
#define MENU_CYL_ROWS 48
#define MENU_CYL_COLS 32        /* the game's columns round the cylinder */
#define MENU_CYL_VSTEPS 8       /* our grid's steps down it */

typedef struct {
    float x0, y0, x1, y1;   /* its letters, texture units (-128..128, y down) */
    int ok;                 /* drawn this frame */
    float vx0, vy0, vx1, vy1;   /* its second line's (a picker's value) */
    int vok;
    int grey;               /* greyed (+0x103) */
    int picker;             /* a value changed with left / right (+0x108: the game's side icons) */
} MenuCylRow;

typedef struct {
    uint32_t seq;
    int input;              /* where its presses go: 1 the front end's menus, 2 the multiplayer
                             * pause menu, 0 neither ran lately */
    uint32_t id;            /* the list's menu id ([0x006BD4BC]; the pause's lists share 0) */
    uint32_t sig;           /* its rows' texts hashed: another list, another sig */
    int still;              /* not sliding in or out */
    int scrolls;            /* the scrolling kind: the chosen row kept in the bracket in the middle */
    int grid;               /* the three-across kind (the game's 0x1F panes) */
    int n, sel;             /* rows; the game's selection */
    int centre;             /* the row placed in the middle (= sel unless the mouse holds the list) */
    float pitch;            /* rows' spacing, texture units */
    float bracket;          /* the bracket bars' place down the texture, 0..1 */
    int grid_ok;            /* the cylinder was drawn */
    int cls;                /* class select (the waiting room, one player): the class
                             * shown ([0x00864EBF]), the model standing in the empty
                             * cylinder (n = 0); -1 not class select */
    float pt[MENU_CYL_VSTEPS + 1][MENU_CYL_COLS + 1][2];   /* the cylinder on the screen:
                             * normalised (x -1..1, y -1 bottom..1 top); [v step][u step] */
    MenuCylRow row[MENU_CYL_ROWS];
} MenuCylSnap;

/* Any thread: the last frame's cylinder list; 1 = one is up (or class
 * select: no rows, cls >= 0). */
int conker_MenuCylSnap(MenuCylSnap *s);
enum { MENU_CYL_NONE, MENU_CYL_A, MENU_CYL_LEFT, MENU_CYL_RIGHT };
/* Any thread: move the game's selection to row target (-1 = leave it) with
 * the menus' own up / down, one step a frame, then press (MENU_CYL_*: A, or
 * left / right on a picker). A new request replaces the last. */
void conker_MenuCylGo(int target, int press);
/* Any thread: keep row centre in the middle of the scrolling list sig
 * (MenuCylSnap.sig) while the selection moves (-1 = the game's own: the
 * selection in the middle); another list drops it. */
void conker_MenuCylHold(int centre, uint32_t sig);

/* The multiplayer screens' button prompts (to-do 40 step 5b): the plate at
 * the foot of each side panel (A Select, B Back, X Configure, the waiting
 * room's A Ready / Y Mission Brief ...), its words and button picture drawn
 * into a texture that a tilted square in the room shows; and the curved
 * bands above and below the middle column (a scrolling "A Select", "B
 * Back"). */
#define MENU_PLATE_PROMPTS 8
#define MENU_PLATE_PTS 18

typedef struct {
    int n;                  /* 4: a plate's square (top left, top right, bottom right,
                             * bottom left); more: a band's outline (its top edge left to
                             * right, then its bottom edge right to left) */
    float pt[MENU_PLATE_PTS][2];    /* on the screen, normalised (x -1..1, y -1 bottom..1 top) */
    int button;             /* MENU_BTN_* a click presses */
    uint32_t key;           /* its text: names it (the glow, a click's press and let go) */
} MenuPlatePrompt;

typedef struct {
    int n;
    int popup;              /* a popup is over the screen (the waiting room's Level
                             * Details ...): only its own prompts here, and the list
                             * behind it takes no mouse */
    MenuPlatePrompt p[MENU_PLATE_PROMPTS];
} MenuPlateSnap;

/* Any thread: the prompts drawn lately (s->popup filled in either way); 1 = some. */
int conker_MenuPlateSnap(MenuPlateSnap *s);

/* The profile name keyboard (to-do 40 step 5d; New Profile / Rename): the
 * game's own on-screen keyboard, flat in the 640 x 480 frame. Keys are
 * (row, col): col 0 the buttons on the left (rows 0-5 Create, Cancel,
 * Shift, Caps Lock, Symbol, Accented); cols 1-10 rows 0-3 the grid (row 3
 * col 7 Backspace); row 4 col 1 Space, 7 the left arrow, 9 the right one.
 * page 0 letters, 1 accented, 2 symbols; shift 0 off, 1 Shift, 2 Caps Lock. */
#define NAMEKB_BACKSPACE (-1)
#define NAMEKB_LEFT 0xFFFC
#define NAMEKB_RIGHT 0xFFFD
#define NAMEKB_CREATE 0xFFFE
#define NAMEKB_CANCEL 0xFFFF
/* Any thread: 1 = the keyboard is up; its highlight, page and Shift. */
int conker_NameKbSnap(int *row, int *col, int *page, int *shift);
/* Is (row, col) a key on that page (some hide on the accented and symbol pages)? */
int conker_NameKbValid(int page, int shift, int row, int col);
/* Any thread: typed on the PC's keyboard: a character (UTF-16) or a NAMEKB_*
 * code, handed to the game one a frame (dropped while the keyboard is not up). */
void conker_NameKbType(int code);
/* Any thread: the highlight to (row, col) (-1 = none asked), with the game's
 * move sound; a click: the highlight there and A pressed on it. */
void conker_NameKbHover(int row, int col);
void conker_NameKbClick(int row, int col);

/* Any thread: the last frame's page; 1 = a board's page or the pause screen
 * is open. */
int conker_MenuPageSnap(MenuPageSnap *s);
/* Any thread: move list's cursor to target (-1 = leave it), one step at a
 * time as the keys would (the game's own sound), then set = 1 Yes / 2 No on
 * the row there (0 none). A new request replaces the last. */
void conker_MenuListGo(uint32_t list, int target, int set);
/* Any thread: the prompt words under the pointer (MenuPrompt.text or
 * MenuPlatePrompt.key), 0 none: they glow, easing in and out. */
void conker_MenuGlow(uint32_t text);

#ifdef __cplusplus
}
#endif

#endif
