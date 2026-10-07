// Terminal -- a TERMINAL EMULATOR, with tabs.
//
// It used to be a shell that happened to have a window: it linked
// `tosh` as a LIBRARY, called tosh_run_line() from its key handler, and
// captured output through a sink. That worked, and it left this window
// unable to do the one thing a terminal is for -- `Ctrl-C` did nothing,
// because a window has no console, no foreground group, and no terminal
// at all to have them on.
//
// Now it is what its name has always claimed:
//
//     keys ──► [pty master] ──► line discipline ──► [slave] ──► /bin/tosh
//     paint ◄── [pty master] ◄── line discipline ◄── [slave] ◄──┘
//
// It opens a pty (SYS_OPENPTY), spawns `/bin/tosh` on the slave, writes
// keystrokes into the master and paints what comes out. **The shell in a
// Terminal window is now a REAL PROCESS**, with a pid, visible in `ps`,
// killable, reaped when the window closes.
//
// AND `Ctrl-C` WORKS HERE FOR THE SAME REASON IT WORKS ON THE PHYSICAL
// KEYBOARD -- not for a similar reason. The key arrives as the byte
// 0x03, is written to the master, and `kernel/tty/ldisc.c` recognises
// it as INTR and signals this terminal's foreground group. That is the
// same function, on the same object, as the one the keyboard IRQ feeds.
//
// **ITS SCREEN IS A GRID, WHICH IS WHAT MAKES A FULL-SCREEN PROGRAM
// WORK HERE.** A terminal is a grid of cells with a cursor over it;
// lines that scroll off the top become scrollback, which is the only
// part that is a stream.
//
// **AND THE ANSI PARSER IS THE KERNEL'S OWN, COMPILED TWICE.**
// kernel/lib/ansi.c is a pure state machine that knows nothing about a
// screen, so the physical console and this window resolve `ESC[4;12H`
// through the SAME code. A second parser here would be a second set of
// answers to "what does ESC[0m clear", and the two would drift the
// first time either was extended.
//
// --- TABS, AND WHAT THEY COST ---------------------------------------
//
// **A TAB IS A SESSION: its own pty, its own shell, its own grid,
// scrollback, alternate screen and title.** That is Konsole's model,
// and GNOME Terminal's and Windows Terminal's -- the tab strip belongs
// to the application, because only the application can give a tab a
// terminal's state. Per-tab job control comes free: each shell is
// spawned into its own process group, so `Ctrl-C` reaches the job in
// the tab you are looking at and nothing else.
//
// **EACH SESSION HAS A READER THREAD**, blocked in a real read on its
// pty, which is how a background tab keeps running. Before tabs this
// window polled one master every 30 ms -- 33 wake-ups a second whether
// or not the shell had said anything, and up to 30 ms of lag on every
// echoed keystroke. N tabs would have been N polls. A thread per
// session blocks instead, and there is no cadence left at all: the loop
// wakes on a keystroke, on a compositor event, or because a reader
// posted (ui/uapp.h's uapp_post).
//
// **THE READER TOUCHES NOTHING BUT ITS RING.** It copies bytes into a
// single-producer/single-consumer ring and posts; the MAIN thread
// drains every ring and feeds the parsers. That is the toolkit's rule
// (docs/conventions/gui.md) and it is not a formality here: the grid,
// the scrollback and the parser are read by the painter on every frame.
//
// **A TITLE COMES FROM THE SHELL.** `ESC]0;text BEL` sets the tab's
// label, which is what every terminal does and why a tab can say where
// its shell is standing. The parser handles it (api/ansi.h's ANSI_OSC);
// a session with nothing to say keeps its generated "Shell N".
#include <stdint.h>
#include "termkey.h"   // a keysym becomes an ANSI sequence here
#include "lib/usetting.h"  // system.shell
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "lib/uclip.h"
#include "ui/ulog.h"
#include "ui/uui_tabs.h"
#include "ui/uui_menubar.h"
#include "ui/uui_textbox.h"
#include "ui/uui_dialog.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_findbar.h"
#include "ui/uui_anim.h"
#include "lib/utween.h"
#include <unistd.h>   // chdir/getcwd, around a new tab's spawn
#include "keyboard.h"
#include "kpath.h"    // k_path_basename, a -e tab's name
#include "ansi.h"   // the kernel's parser, compiled into libuapp too
#include "ui/umonofont.h"   // the grid's face at a chosen size
#include "term/term.h"    // /etc/terminal.conf and the Preferences dialog

// THE WINDOW IS SIZED FOR A GRID, NOT IN PIXELS. A fixed pair does not
// survive the font: 640x400 was 78x21 at size 14 and 44x12 at size 24,
// so the one setting that is meant to reflow the whole desktop
// (kernel/drivers/gfx.c) made this window progressively useless.
//
// 120x30 is Windows Terminal's default. The VT100 grid every other
// terminal still opens at -- xterm, GNOME Terminal, Konsole, macOS
// Terminal, all 80x24 -- is inherited rather than chosen, and this
// window is resizable anyway.
// The grid a window opens at is /etc/terminal.conf's `cols`/`rows`
// (120x30 by default, Windows Terminal's).

#define SHELL_FALLBACK "/bin/tosh"

// `uterm -e PATH`: this window's first tab runs PATH instead of a shell
// -- the File Manager's "Run in Terminal" (lib/ulaunch.h). xterm's and
// Konsole's -e.
static char g_cmd[256];

// WHICH SHELL, from the registry rather than baked in (`system.shell`,
// kernel/lib/shell_config.c). Read per session rather than once at
// startup, so changing the setting affects the NEXT window or login
// instead of needing a restart -- and the fallback keeps a session
// possible when the registry cannot answer.
static const char *shell_path(char *buf, size_t cap) {
    if (usetting_get("system.shell", buf, cap) && buf[0]) return buf;
    return SHELL_FALLBACK;
}

// The screen. The grid GROWS to fit the window rather than being a
// fixed 200x60: a maximized terminal at 1920x1080 already wants
// ~240x67 cells, so the old fixed
// cap left a dead band below the last row and to the right of the last
// column, silently. xterm reallocates on resize and CROPS (no reflow);
// so does this. What bounds it now is the WINDOW, which the compositor
// already clamps -- a program still cannot ask for an unbounded grid.
//
// ONE ALLOCATED GEOMETRY FOR EVERY SESSION (g_cap_rows x g_cap_cols):
// there is one window, so every tab is the same size, and capacity only
// ever grows -- shrinking the window narrows g_rows/g_cols and leaves
// the buffers alone, exactly as the fixed grid did.
// Scrollback depth, the margin and the palette are all
// /etc/terminal.conf's now (userland/term/term.h). They are read
// into plain globals because every one of them is asked for per row
// drawn, and a struct field behind a pointer would be the same
// number with an indirection in front of it.
static struct term_conf g_conf;
static struct term_scheme g_scheme;
// TERM_MARGIN, or more while a curved screen effect needs the corners
// clear (ucrt_margin()); set by size_changed().
static int g_margin = TERM_MARGIN;
// The screen effect: this window's, toggled from the menu without
// touching the file (as the menu bar and the panel are); g_conf.crt is
// what it looks like.
static int g_effect;
static struct ucrt g_crt;
static int g_sb_rows = 240;   // scrollback lines kept above the screen
#define SB_ROWS g_sb_rows

// **EIGHT, AND THE REASON IS MEMORY RATHER THAN TASTE.** A session is
// about 220 KiB of grid, scrollback and saved screen, so the cap is
// what bounds this window's heap at under two megabytes. Sessions are
// allocated on demand and their slots REUSED, so a Terminal that never
// opens a second tab pays for one.
#define MAX_TABS 8

// The reader's ring. Big enough that a burst of output does not make the
// reader wait on the UI for every byte, small enough to be noise beside
// the grid it feeds.
#define RING_BYTES 8192

#define TITLE_MAX 40

struct cell { char ch; uint8_t fg, bg; };

struct session {
    // Slot bookkeeping. `live` means this session has a shell; `done`
    // means its reader thread has finished and the slot may be reused.
    int live;
    volatile int done;
    volatile int eof;
    int index;                  // its slot, and what a post carries

    // Heap-backed, g_cap_rows/g_cap_cols geometry, allocated at first
    // use and REALLOCATED by grow_caps() when the window outgrows them.
    // session_start() zeroes the whole struct on slot reuse, so it
    // saves and restores these three pointers around the wipe.
    struct cell *grid;          // g_cap_rows x g_cap_cols
    struct cell *sb;            // SB_ROWS   x g_cap_cols
    int sb_count;               // lines of scrollback held
    int sb_view;                // how far back the reader has scrolled

    // THE ALTERNATE SCREEN (ESC[?1049h/l). A second grid, plus the
    // cursor the switch saved, so a full-screen program leaves the
    // terminal exactly as it found it -- which is what `less` and `vim`
    // do on every real terminal.
    //
    // A COPY OF THE GRID RATHER THAN A SECOND LIVE ONE. Swapping a
    // pointer between two grids would be cheaper, but everything here
    // indexes the grid directly. The saved copy is written once per
    // switch, not per frame.
    //
    // **SCROLLBACK IS NOT SAVED, DELIBERATELY.** On a real terminal the
    // alternate screen has no scrollback at all -- that is why you
    // cannot scroll back through a `less` session.
    struct cell *saved;         // g_cap_rows x g_cap_cols
    int alt;
    int saved_cr, saved_cc;

    int cr, cc;                 // the cursor, in cells
    int cursor_shown;

    struct ansi_parser vt;

    int master;                 // our end of the pty
    int child;                  // the shell's pid, for reaping and `ps`

    char title[TITLE_MAX];
    // Where the shell last said it was standing: its OSC title when that
    // is an absolute path, which is what tosh sends. A new tab opened
    // "here" starts in it, and the Session panel shows it.
    char cwd[TITLE_MAX];
    char shell[TERM_SHELL_MAX];  // what was spawned, for a restart and the panel
    // THE SHELL HAS EXITED AND THE TAB WAS KEPT (`on_exit=keep`): reaped,
    // its last output still on screen, Enter starts a new shell in it.
    int exited;
    // A `-e` PROGRAM, not a shell: always kept when it ends, whatever
    // on_exit says -- its output is the reason the window exists -- and
    // Enter runs it again. session_start() zeroes it; mark_cmd() sets it.
    int is_cmd;

    // TYPE-AHEAD IS HELD UNTIL THE SHELL FIRST SPEAKS. A pty starts with
    // the line discipline echoing (abi/tty_abi.h), and tosh only turns
    // that off once it has loaded -- so keys typed while it loads were
    // echoed by the kernel ahead of the banner AND redrawn by tosh at its
    // prompt. Held here, they arrive after the shell has set up its
    // terminal however it likes. on_tick lets them go after a couple of
    // seconds for a shell that never prints.
    int spoke;
    char held[256];
    int nheld;
    unsigned long long started_ns;
    // A HAND-GIVEN TITLE OUTRANKS THE SHELL'S. Konsole's rule: once you
    // name a tab, its shell's OSC sequences stop moving the label --
    // otherwise the next `cd` silently undoes the rename.
    int title_locked;

    // --- the reader thread's ring ------------------------------------
    //
    // SINGLE PRODUCER, SINGLE CONSUMER: the reader writes at `head` and
    // the main thread reads at `tail`, so neither ever writes the
    // other's index and no lock is needed. The acquire/release pairs
    // are what stop the COMPILER reordering a byte's store past the
    // index that publishes it -- x86-64's TSO handles the CPU half
    // (kernel/include/kernel/barrier.h says the same thing about
    // virtqueues).
    volatile unsigned head, tail;
    char ring[RING_BYTES];
    pthread_t reader;
    int reader_started;
};

static struct session *g_slot[MAX_TABS];

// The strip, and the mapping from a TAB position to a session SLOT.
// They differ as soon as a middle tab is closed, and conflating them is
// how a click lands on the wrong shell.
static struct uui_tabs g_strip;
static struct uui_tab  g_tablabels[MAX_TABS];
static int g_tab_slot[MAX_TABS];
static int g_ntabs;

// --- the menu bar -----------------------------------------------------
//
// **THE EDIT MENU IS PASTE AND ONLY PASTE.** There is a system text
// clipboard now (lib/uclip.h), and pasting into a terminal is typing:
// the characters go to the pty as bytes, so the shell's own line editor
// sees them exactly as it would a fast typist. COPY is missing because
// this terminal has no SELECTION -- the grid can be read but not
// marked, and a Copy row that could only ever copy nothing would be
// worse than no row. Selecting text in the grid is a roadmap item.
//
// **Ctrl+Shift+V, not Ctrl+V**, because Ctrl+V is a control code the
// shell may want (and in a full-screen program certainly does). Konsole,
// GNOME Terminal and Windows Terminal all shift the paste binding for
// the same reason.
//
// The bar can be hidden, as Konsole's can, because a terminal is the one
// app where a row of chrome is a row of the product. **F10 brings it
// back** -- Konsole's own Ctrl+Shift+M is unavailable here, since Ctrl
// folds 'M' to 0x0D and the binding would be indistinguishable from
// Shift+Enter (api/keyboard.h).
enum {
    CMD_NEW_TAB = 1, CMD_CLOSE_TAB, CMD_EXIT, CMD_PASTE, CMD_COPY,
    CMD_SELECT_ALL,
    CMD_RENAME, CMD_CLEAR_SCREEN, CMD_CLEAR_SB, CMD_RESET,
    CMD_INTR, CMD_EOF,
    CMD_TOP, CMD_BOTTOM, CMD_MENUBAR,
    CMD_NEXT_TAB, CMD_PREV_TAB,
    CMD_PREFS, CMD_FIND, CMD_PANEL, CMD_EFFECT,
    CMD_MENU_BUTTON, CMD_NEWTAB_MENU,   // the bar's two popups
    CMD_SHELL_BASE = 100,               // + an index into /etc/shells
};

static const struct uui_menu_item file_items[] = {
    UUI_MENU("New Tab",       CMD_NEW_TAB,   "Ctrl+Shift+T"),
    UUI_MENU("Close Tab",     CMD_CLOSE_TAB, "Ctrl+Shift+W"),
    UUI_MENU_SEP,
    UUI_MENU("Exit",          CMD_EXIT,      "Alt+F4"),
};

static const struct uui_menu_item term_items[] = {
    UUI_MENU("Rename Tab...", CMD_RENAME,       0),
    UUI_MENU_SEP,
    UUI_MENU("Clear Screen",     CMD_CLEAR_SCREEN, 0),
    UUI_MENU("Clear Scrollback", CMD_CLEAR_SB,     0),
    UUI_MENU("Reset Terminal",   CMD_RESET,        0),
    UUI_MENU_SEP,
    UUI_MENU("Send Interrupt", CMD_INTR, "Ctrl-C"),
    UUI_MENU("Send EOF",       CMD_EOF,  "Ctrl-D"),
};

static const struct uui_menu_item edit_items[] = {
    UUI_MENU("Copy",       CMD_COPY,       "Ctrl+Shift+C"),
    UUI_MENU("Paste",      CMD_PASTE,      "Ctrl+Shift+V"),
    UUI_MENU_SEP,
    UUI_MENU("Select All", CMD_SELECT_ALL, 0),
    UUI_MENU("Find...",    CMD_FIND,       "Ctrl+Shift+F"),
    UUI_MENU_SEP,
    // GNOME Terminal's placement. Konsole calls it Settings > Configure
    // Konsole and Windows Terminal hangs it off the tab strip; Edit is
    // the one of the three this menu bar already has.
    UUI_MENU("Options...", CMD_PREFS, 0),
};

static const struct uui_menu_item view_items[] = {
    UUI_MENU("Scroll to Top",    CMD_TOP,    "PgUp"),
    UUI_MENU("Scroll to Bottom", CMD_BOTTOM, "PgDn"),
    UUI_MENU_SEP,
    UUI_MENU("Session Panel",    CMD_PANEL,   "F9"),
    UUI_MENU("Screen Effect",    CMD_EFFECT,  "Ctrl+Shift+E"),
    UUI_MENU("Menu Bar",         CMD_MENUBAR, "F10"),
};

static const struct uui_menu_item tabs_items[] = {
    UUI_MENU("Next Tab",     CMD_NEXT_TAB, "Ctrl+PgDn"),
    UUI_MENU("Previous Tab", CMD_PREV_TAB, "Ctrl+PgUp"),
};

static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File",     file_items),
    UUI_SUBMENU("Edit",     edit_items),
    UUI_SUBMENU("Terminal", term_items),
    UUI_SUBMENU("View",     view_items),
    UUI_SUBMENU("Tabs",     tabs_items),
};

static struct uui_menubar g_menu;
static int g_menu_shown;

// --- the bar: tabs on the left, these on the right --------------------
//
// **THE MENU BAR IS HIDDEN BY DEFAULT AND THE ☰ BUTTON HOLDS ALL OF IT**
// -- Windows Terminal's and GNOME Console's shape: a terminal's chrome
// rows are rows taken from the terminal. The ☰ menu is the menu bar's
// commands flattened, so hiding the bar hides no command; F10 brings the
// bar back.
static const struct uui_menu_item term_sub_items[] = {
    UUI_MENU("Clear Screen",     CMD_CLEAR_SCREEN, 0),
    UUI_MENU("Clear Scrollback", CMD_CLEAR_SB,     0),
    UUI_MENU("Reset Terminal",   CMD_RESET,        0),
    UUI_MENU_SEP,
    UUI_MENU("Send Interrupt",   CMD_INTR, "Ctrl-C"),
    UUI_MENU("Send EOF",         CMD_EOF,  "Ctrl-D"),
    UUI_MENU_SEP,
    UUI_MENU("Previous Tab",     CMD_PREV_TAB, "Ctrl+PgUp"),
    UUI_MENU("Next Tab",         CMD_NEXT_TAB, "Ctrl+PgDn"),
};
static const struct uui_menu_item burger_items[] = {
    UUI_MENU_ICON("New Tab",   CMD_NEW_TAB,   "Ctrl+Shift+T", "tb-new",   UTHEME_ACT_CREATE),
    UUI_MENU("Close Tab",      CMD_CLOSE_TAB, "Ctrl+Shift+W"),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Copy",      CMD_COPY,      "Ctrl+Shift+C", "tb-copy",  UTHEME_ACT_EDIT),
    UUI_MENU_ICON("Paste",     CMD_PASTE,     "Ctrl+Shift+V", "tb-paste", UTHEME_ACT_EDIT),
    UUI_MENU("Select All",     CMD_SELECT_ALL, 0),
    UUI_MENU_ICON("Find...",   CMD_FIND,      "Ctrl+Shift+F", "tb-find",  UTHEME_ACT_VIEW),
    UUI_MENU_SEP,
    UUI_MENU("Rename Tab...",  CMD_RENAME,    0),
    UUI_SUBMENU("Terminal",    term_sub_items),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Session Panel", CMD_PANEL, "F9",           "tb-pane",  UTHEME_ACT_VIEW),
    UUI_MENU("Screen Effect",  CMD_EFFECT,    "Ctrl+Shift+E"),
    UUI_MENU("Menu Bar",       CMD_MENUBAR,   "F10"),
    UUI_MENU_ICON("Options...", CMD_PREFS,    0,              "tb-gear",  0),
    UUI_MENU_SEP,
    UUI_MENU("Exit",           CMD_EXIT,      "Alt+F4"),
};

// The ▾ beside the "+": a new tab with a named shell. Rebuilt from
// /etc/shells each time it opens, so an installed shell appears without
// a restart.
static char g_shells[TERM_SHELLS_MAX][TERM_SHELL_MAX];
static int g_nshells;
static struct uui_menu_item g_newtab_items[TERM_SHELLS_MAX + 2];

enum { TB_NEWTAB = 0, TB_FIND, TB_PANEL, TB_MENU, TB_COUNT };
static const struct uui_toolbar_item tb_items[TB_COUNT] = {
    { "tb-chevron", "New tab with",          CMD_NEWTAB_MENU, 0, 0,          0, 0 },
    { "tb-find",    "Find (Ctrl+Shift+F)",   CMD_FIND,        0, UUI_TB_END, 0, UTHEME_ACT_VIEW },
    { "tb-pane",    "Session panel (F9)",    CMD_PANEL,       0, 0,          0, UTHEME_ACT_VIEW },
    { "tb-menu",    "Menu",                  CMD_MENU_BUTTON, 0, 0,          0, 0 },
};
static struct uui_toolbar g_tb;

// The popups those two buttons open: a menu with no bar strip
// (uui_menubar_open_at()'s rule -- never the bar's own instance).
static struct uui_menubar g_ctx;

static struct uui_findbar g_find;
static int g_find_open;
static int g_panel_open;

// --- renaming a tab ---------------------------------------------------
//
// A field over the GRID, never over the strip: the grid carries no
// routed widget, so the prompt cannot be clicked through onto a tab.
// A press anywhere outside it cancels, which is what a lightweight
// prompt does -- this is not a modal and does not pretend to be one.
static struct uui_textbox g_rename;
static int g_rename_open;
static int g_rename_slot = -1;

// The chrome, declared for INPUT and drawn by the toolkit after on_draw
// (ui/uapp.c's order); on_draw positions both -- see ui/uapp.h on why a
// widget array is two declarations and what happens when an app writes
// only one.
//
// **THE MENU BAR IS FIRST, AND THE ORDER IS LOAD-BEARING.** Its popup
// drops over the tab strip, and a routed widget claiming an overlay is
// offered every press before anything is hit-tested (uui_route.c) -- so
// declaring the menu here is what stops a click on the File menu's first
// row ALSO landing on the tab underneath it.
//
// **THE PREFERENCES DIALOG IS APPENDED AT OPEN, AND IT GOES LAST.** A
// modal has to outrank the menu bar it was opened from, and the router
// offers overlays in array order.
#define ID_MENU 1
#define ID_TABS 2
#define ID_QUIT_ASK 3
#define ID_CTX 4
#define ID_TB 5
#define ID_FIND 6

// Closing a window with more than one tab ASKS, which is Konsole's
// "confirm when closing a window with multiple tabs" and GNOME
// Terminal's close-confirmation alike -- one careless Alt+F4 otherwise
// takes every shell in the window with it.
//
// **IT MAKES CLOSING TWO-PHASE, and that is why it cannot just return
// the answer.** `wm_request_close()` ASKS this client and takes 0 for
// "no" (docs/conventions/gui.md), while a modal's answer arrives
// frames later through on_widget -- so the refusal goes out first and
// the real close is a uapp_quit() from the dialog's commit.
static struct uui_dialog g_quit_ask;

static int modal_up(void) {
    return term_prefs_is_open() || uui_dialog_is_open(&g_quit_ask);
}
enum { QUIT_YES = 1, QUIT_NO };
// The ☰/▾ popup goes FIRST of all, for the menu bar's reason: it drops
// over the strip and the grid.
enum { W_CTX, W_MENU, W_TABS, W_TB, W_FIND, W_PANEL0, W_QUIT = W_PANEL0 + TERM_PANEL_BUTTONS,
       W_COUNT };
static struct uui_item g_widgets[W_COUNT] = {
    [W_CTX]  = { .ops = &uui_menubar_ops, .widget = &g_ctx,   .id = ID_CTX,  .name = "ctx" },
    [W_MENU] = { .ops = &uui_menubar_ops, .widget = &g_menu,  .id = ID_MENU, .name = "menu" },
    [W_TABS] = { .ops = &uui_tabs_ops,    .widget = &g_strip, .id = ID_TABS, .name = "tabs" },
    [W_TB]   = { .ops = &uui_toolbar_ops, .widget = &g_tb,    .id = ID_TB,   .name = "tb" },
    [W_FIND] = { .ops = &uui_findbar_ops, .widget = &g_find,  .id = ID_FIND, .name = "find" },
    // W_PANEL0.. are term_panel_item(), copied in on_open
    [W_QUIT] = { .ops = &uui_dialog_ops, .widget = &g_quit_ask, .id = ID_QUIT_ASK, .name = "quit-ask" },
};

static struct uapp *g_app;
static int g_rows = 24, g_cols = 80;  // one window, so one size for all

// --- the scrollbar ----------------------------------------------------
//
// AN OVERLAY, thin at rest and full under the pointer -- see bar_rect().
// The full width comes from the widget (uui_scrollbar_natural_size()), so
// it tracks the font like everything else here; hardcoding one is what
// gave Notepad an 8px strip that was genuinely hard to click.
static int chrome_h(void);

// **CHROME IS MEASURED IN THE UI FACE, WHATEVER IS SELECTED.** This is
// called from inside draw()'s grid-font bracket AND from the layout and
// hit-test paths outside it, and uui_scrollbar_natural_size() reads the
// CURRENT font -- so a width that depended on the caller drew the bar
// one size and hit-tested another. It did: the thumb answered a drag
// several pixels short of where it was painted.
// **A MODAL OWNS THE INPUT.** The router offers an open overlay every
// event first, but one it does not consume still reaches the app-level
// handler -- so without this test the menu bar opened and the grid
// selected text underneath an open dialog, which is not a modal. Every
// input handler in this file asks, and so does the caret.
static int bar_w(void) {
    if (!g_conf.scrollbar) return 0;   // hidden: no gutter either
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int w = 0, h = 0;
    uui_scrollbar_natural_size(&w, &h);
    ugfx_set_font(was);
    return w;
}

// The Session panel's width while it is open, in the UI face for the
// same reason as bar_w(). The grid gives it room rather than being
// covered (docs/gui-guidelines.md's side panel).
static int panel_w(void) {
    if (!g_panel_open) return 0;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int w = term_panel_width();
    ugfx_set_font(was);
    return w;
}

// Where the grid's area ends on the right: the window, less the panel.
static int grid_right(int win_w) { return win_w - panel_w(); }

// Where the bar is, given the window. The ONE geometry function draw,
// hit-testing and the drag maths all call -- two derivations is the
// classic way a scrollbar draws in one place and responds in another.
//
// **AN OVERLAY, NOT A GUTTER** -- the Start menu's bar, Windows 11's and
// GNOME's: it reserves no columns. At rest it is a 3 px thumb inside the
// right margin, over no text; under the pointer or a drag it grows to
// the full bar over the last column, which is what a gutter cost all the
// time. This rect is the FULL bar, and the hover zone.
static void bar_rect(int win_w, int win_h, int *x, int *y, int *w, int *h) {
    *w = bar_w();
    *x = grid_right(win_w) - *w - 2;
    *y = chrome_h() + g_margin;
    *h = win_h - *y - g_margin;
    if (*h < 1) *h = 1;
}

// THE WIDENING IS A TWEEN, 0 (thin) to SB_FULL (the full bar): out over
// SB_GROW_MS, back only after the pointer has been gone SB_LINGER_MS, so
// a hand drifting off the bar does not snatch it away -- the Start
// menu's numbers (userland/wm/start_menu.c). Frames come from
// uui_anim_request() only while it moves.
#define SB_FULL      256
#define SB_GROW_MS   150
#define SB_LINGER_MS 400
static struct utween g_sb_tw;
static int g_sb_want;                     // heading wide (1) or thin (0)
static unsigned long long g_sb_collapse;  // when a pending shrink starts, else 0

static void sb_hover(int over) {
    unsigned long long now = uui_anim_now_ns();
    if (over) {
        g_sb_collapse = 0;
        if (!g_sb_want) utween_retarget(&g_sb_tw, SB_FULL, SB_GROW_MS, now);
        g_sb_want = 1;
    } else if (g_sb_want && !g_sb_collapse) {
        g_sb_collapse = now + (unsigned long long)SB_LINGER_MS * 1000000ull;
    }
    if (g_app) uapp_redraw(g_app);
}

// A thumb drag in progress: how far down the thumb the press landed, or
// -1 for no drag. Point 1 of docs/gui-guidelines.md's scrollbar section
// -- passing 0 here makes the thumb leap so its TOP sits under the
// cursor, and the control is then usable only by catching its top edge.
static int g_bar_grab = -1;

// --- selection --------------------------------------------------------
//
// A POINT IS A LINE IN THE SESSION'S VIRTUAL BUFFER, not a screen row:
// lines 0..sb_count-1 are scrollback and sb_count..sb_count+g_rows-1 are
// the screen, so view row `r` is line `sb_count - sb_view + r` whichever
// half it comes from. That single expression is why scrolling during a
// drag keeps the anchor on the text it was put on rather than on the row
// it happened to be over.
//
// `col` may be g_cols -- one past the last cell -- so a selection can
// reach a line's break, which is what makes a multi-line copy end its
// lines rather than run them together.
struct selpoint { int line, col; };

static struct selpoint g_sel_a, g_sel_b;
static int g_selecting;   // a drag is live: motion extends g_sel_b
static int g_sel_on;      // there is a selection to draw and copy

// **THE SELECTION IS DROPPED WHEN THE TEXT MOVES UNDER IT.** vt_scroll()
// shifts every line by one once the scrollback is full, and a program
// clearing the screen replaces what was selected outright -- so rather
// than tracking the text through both, the selection goes. Konsole keeps
// it, at the cost of a line identity this terminal's scrollback (a ring
// of evicted rows) does not carry.
static void sel_clear(void) { g_sel_on = 0; g_selecting = 0; }

// The ALLOCATED geometry every session's buffers share, and the draw
// scratch sized to it. Grows in grow_caps(), never shrinks.
static int g_cap_rows, g_cap_cols;
static char *g_runbuf;   // draw_run's text scratch, g_cap_cols + 1
static char *g_rowbuf;   // draw_row's run scratch,  g_cap_cols + 1

// The 16 colours as RGB, and which two slots the default pair uses.
// The parser resolves a sequence to an `enum vga_color`, which is an
// INDEX -- turning an index into light is the display's business, and
// the kernel console does exactly the same thing with its own table.
// What changed is only where the table comes from: a scheme file rather
// than a constant (userland/term/term.h).
#define VGA_RGB (g_scheme.pal)
#define VT_FG (g_scheme.fg)
#define VT_BG (g_scheme.bg)

// --- the terminal's own font (Konsole's, GNOME Terminal's) ------------
//
// **THE GRID MEASURES WITH cell_w()/cell_h() AND THE CHROME DOES NOT.**
// ugfx's font selection is per PROCESS, so the menu bar and tab strip --
// drawn by the toolkit after on_draw -- keep the UI family while the
// grid uses the MONOSPACE one, and draw() is the only place the
// selection moves.
//
// `font_size = 0`, the default, rasterizes nothing and follows the
// desktop's monospace family (`system.font_mono`, ui/ugfx.h). That is
// the pair this window needs: the desktop's SIZE, in a face whose
// cells line up.
static struct umonofont g_font;   // the grid's face at the configured size
static int g_cell_w, g_cell_h;

// The caret's blink phase. Toggled by on_tick when cursor_blink is
// on and pinned to 1 when it is off, so draw() tests one flag either
// way rather than the flag AND the setting.
static int g_caret_on = 1;

// The font the GRID draws in: this terminal's own atlas when it has
// one, else the session's MONOSPACE family. Never the UI family, whose
// face may be proportional -- a grid of cells drawn in one does not
// line up, which is the whole reason the two families exist.
static const struct ugfx_font *grid_font(void) {
    return umonofont_get(&g_font, g_conf.font_size, "uterm");
}

static void font_sync(void) {
    // Re-read EVERY call, not only after a reload: while we are
    // following the desktop, its size can move under us (WIN_EV_FONT)
    // and these two are what the whole grid is derived from.
    const struct ugfx_font *was = ugfx_set_font(grid_font());
    g_cell_w = ugfx_char_w();
    g_cell_h = ugfx_char_h();
    ugfx_set_font(was);
}

static int cell_w(void) { if (g_cell_w <= 0) font_sync(); return g_cell_w; }
static int cell_h(void) { if (g_cell_h <= 0) font_sync(); return g_cell_h; }

static struct session *active(void) {
    if (g_ntabs <= 0) return 0;
    int slot = g_tab_slot[g_strip.selected];
    if (slot < 0 || slot >= MAX_TABS) return 0;
    return g_slot[slot];
}

// --- the screen -------------------------------------------------------

// Row accessors: the buffers are flat, strided by the ALLOCATED width,
// which is what lets one realloc change the geometry without touching
// any of the code below.
static struct cell *grid_row(struct session *s, int r) {
    return s->grid + (size_t)r * g_cap_cols;
}
static struct cell *sb_row(struct session *s, int r) {
    return s->sb + (size_t)r * g_cap_cols;
}
static struct cell *saved_row(struct session *s, int r) {
    return s->saved + (size_t)r * g_cap_cols;
}

// --- the virtual buffer, and what is selected in it -------------------

// The line a view row shows. One expression for both halves -- see the
// note beside struct selpoint.
static int virt_of_row(const struct session *s, int r) {
    return s->sb_count - s->sb_view + r;
}

// The cells of a virtual line, or NULL when it is off either end.
static const struct cell *virt_row(struct session *s, int line) {
    if (line < 0) return 0;
    if (line < s->sb_count) return sb_row(s, line);
    int r = line - s->sb_count;
    if (r >= g_rows) return 0;
    return grid_row(s, r);
}

// Reading order: is `a` before `b`?
static int sel_before(struct selpoint a, struct selpoint b) {
    return a.line < b.line || (a.line == b.line && a.col < b.col);
}

// The selection, sorted. Returns 0 when there is nothing selected --
// which includes an anchor and a cursor at the same point, the state a
// plain click leaves behind.
static int sel_range(struct selpoint *from, struct selpoint *to) {
    if (!g_sel_on) return 0;
    if (sel_before(g_sel_b, g_sel_a)) { *from = g_sel_b; *to = g_sel_a; }
    else                              { *from = g_sel_a; *to = g_sel_b; }
    return sel_before(*from, *to);
}

// The columns of `line` that are selected, as [*c0, *c1). Zero-width
// when none of it is.
static void sel_cols(int line, int *c0, int *c1) {
    struct selpoint from, to;
    *c0 = *c1 = 0;
    if (!sel_range(&from, &to)) return;
    if (line < from.line || line > to.line) return;
    *c0 = (line == from.line) ? from.col : 0;
    *c1 = (line == to.line)   ? to.col   : g_cols;
    if (*c1 > g_cols) *c1 = g_cols;
    if (*c0 > *c1) *c0 = *c1;
}

static struct cell *cells_new(int rows, int cols) {
    size_t n = (size_t)rows * (size_t)cols;
    struct cell *p = (struct cell *)malloc(n * sizeof *p);
    if (!p) return 0;
    for (size_t i = 0; i < n; i++) {
        p[i].ch = ' '; p[i].fg = VT_FG; p[i].bg = VT_BG;
    }
    return p;
}

// Grows the shared geometry to hold rows x cols, migrating EVERY live
// session's buffers (a crop-preserving copy -- xterm's behaviour; there
// is no reflow). Two passes on purpose: allocate everything first, then
// swap, so a failed malloc leaves no session with a stride the globals
// do not describe. Returns 0 on failure with everything as it was --
// the caller then clamps to the old capacity, which is exactly the old
// fixed-grid behaviour.
static int grow_caps(int rows, int cols) {
    if (rows <= g_cap_rows && cols <= g_cap_cols) return 1;
    int nr = rows > g_cap_rows ? rows : g_cap_rows;
    int nc = cols > g_cap_cols ? cols : g_cap_cols;

    struct cell *ng[MAX_TABS] = {0}, *nb[MAX_TABS] = {0}, *nv[MAX_TABS] = {0};
    char *rb = (char *)malloc((size_t)nc + 1);
    char *ob = (char *)malloc((size_t)nc + 1);
    int ok = rb && ob;
    for (int i = 0; ok && i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->grid) continue;
        ng[i] = cells_new(nr, nc);
        nb[i] = cells_new(SB_ROWS, nc);
        nv[i] = cells_new(nr, nc);
        if (!ng[i] || !nb[i] || !nv[i]) ok = 0;
    }
    if (!ok) {
        for (int i = 0; i < MAX_TABS; i++) { free(ng[i]); free(nb[i]); free(nv[i]); }
        free(rb); free(ob);
        return 0;
    }

    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->grid) continue;
        for (int r = 0; r < g_cap_rows; r++)
            for (int c = 0; c < g_cap_cols; c++) {
                ng[i][(size_t)r * nc + c] = grid_row(s, r)[c];
                nv[i][(size_t)r * nc + c] = saved_row(s, r)[c];
            }
        for (int r = 0; r < SB_ROWS; r++)
            for (int c = 0; c < g_cap_cols; c++)
                nb[i][(size_t)r * nc + c] = sb_row(s, r)[c];
        free(s->grid); free(s->sb); free(s->saved);
        s->grid = ng[i]; s->sb = nb[i]; s->saved = nv[i];
    }
    free(g_runbuf); g_runbuf = rb;
    free(g_rowbuf); g_rowbuf = ob;
    g_cap_rows = nr;
    g_cap_cols = nc;
    return 1;
}

// --- applying a configuration change ----------------------------------

// The scheme's default pair moved, so every cell still holding the OLD
// one has to follow it: a cell stores an INDEX and has no "this is the
// default" bit, so switching Default (7 on 0) for Solarized Dark (12 on
// 8) would otherwise leave the whole scrollback drawn in two colours
// the new scheme reserves for something else.
//
// The two halves move independently, because `ls --color` leaves a row
// with an explicit foreground over a default background. The
// imprecision is a cell that explicitly asked for the old default
// colour, which moves with the rest -- invisible in practice and the
// only alternative is a wider cell.
static void remap_default_pair(int old_fg, int old_bg) {
    if (old_fg == VT_FG && old_bg == VT_BG) return;
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->grid) continue;
        struct cell *banks[3] = { s->grid, s->saved, s->sb };
        int rows[3] = { g_cap_rows, g_cap_rows, g_sb_rows };
        for (int b = 0; b < 3; b++) {
            if (!banks[b]) continue;
            size_t n = (size_t)rows[b] * (size_t)g_cap_cols;
            for (size_t k = 0; k < n; k++) {
                if (banks[b][k].fg == old_fg) banks[b][k].fg = (uint8_t)VT_FG;
                if (banks[b][k].bg == old_bg) banks[b][k].bg = (uint8_t)VT_BG;
            }
        }
        s->vt.fg = (enum vga_color)VT_FG;
        s->vt.bg = (enum vga_color)VT_BG;
    }
}

// Grows or shrinks every session's scrollback, KEEPING THE NEWEST
// lines: the ones at the bottom are the ones somebody scrolling up is
// about to look for, and dropping those instead would make "give me
// more history" delete history. A failed malloc leaves the old depth,
// which is a smaller scrollback and never a broken one.
static void resize_scrollback(int lines) {
    if (lines == g_sb_rows || lines < 1) return;
    struct cell *nb[MAX_TABS] = {0};
    int ok = 1;
    for (int i = 0; ok && i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->sb) continue;
        nb[i] = cells_new(lines, g_cap_cols);
        if (!nb[i]) ok = 0;
    }
    if (!ok) {
        for (int i = 0; i < MAX_TABS; i++) free(nb[i]);
        ulogf("uterm: no memory for a %d-line scrollback; keeping %d\n", lines, g_sb_rows);
        return;
    }
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->sb) continue;
        int keep = s->sb_count < lines ? s->sb_count : lines;
        int from = s->sb_count - keep;
        for (int r = 0; r < keep; r++)
            memcpy(nb[i] + (size_t)r * g_cap_cols, sb_row(s, from + r),
                    (size_t)g_cap_cols * sizeof(struct cell));
        free(s->sb);
        s->sb = nb[i];
        s->sb_count = keep;
        if (s->sb_view > keep) s->sb_view = keep;
    }
    g_sb_rows = lines;
}

static void row_clear(struct cell *row, int from) {
    for (int c = from; c < g_cap_cols; c++) {
        row[c].ch = ' ';
        row[c].fg = VT_FG;
        row[c].bg = VT_BG;
    }
}

static void vt_reset_screen(struct session *s) {
    sel_clear();
    for (int r = 0; r < g_cap_rows; r++) row_clear(grid_row(s, r), 0);
    s->cr = s->cc = 0;
}

// The top line leaves the screen and becomes history. THE ONLY PLACE A
// STREAM STILL EXISTS -- and it is the right place: scrollback is a
// record of what went past, while the screen is a thing being drawn on.
static void vt_scroll(struct session *s) {
    sel_clear();
    size_t rowbytes = (size_t)g_cap_cols * sizeof(struct cell);
    if (s->sb_count == SB_ROWS) {
        memmove(sb_row(s, 0), sb_row(s, 1), (size_t)(SB_ROWS - 1) * rowbytes);
        s->sb_count--;
    }
    memcpy(sb_row(s, s->sb_count), grid_row(s, 0), rowbytes);
    s->sb_count++;

    memmove(grid_row(s, 0), grid_row(s, 1), (size_t)(g_rows - 1) * rowbytes);
    row_clear(grid_row(s, g_rows - 1), 0);
}

static void vt_newline(struct session *s) {
    s->cr++;
    if (s->cr >= g_rows) { s->cr = g_rows - 1; vt_scroll(s); }
}

static void vt_putc_raw(struct session *s, char c) {
    if (c == '\n') { s->cc = 0; vt_newline(s); return; } // no OPOST: LF is CRLF here
    if (c == '\r') { s->cc = 0; return; }
    if (c == '\b') { if (s->cc > 0) s->cc--; return; }
    if (c == '\t') {
        do { s->cc++; } while (s->cc % 8 && s->cc < g_cols);
        if (s->cc >= g_cols) { s->cc = 0; vt_newline(s); }
        return;
    }
    if ((unsigned char)c < 32) return; // anything else unprintable is dropped

    if (s->cc >= g_cols) { s->cc = 0; vt_newline(s); }
    struct cell *cell = &grid_row(s, s->cr)[s->cc];
    cell->ch = c;
    cell->fg = (uint8_t)s->vt.fg;
    cell->bg = (uint8_t)s->vt.bg;
    s->cc++;
}

// --- what a row-count change does to the screen ------------------------
//
// **SHRINKING SCROLLS; GROWING PULLS BACK.** Merely clamping the cursor
// into the shorter window -- which is what this did -- leaves the text
// where it was and drops the cursor on top of it, so the prompt lands
// in the middle of old output; and growing then leaves it stranded
// there with blank rows below, because nothing ever moved it back. That
// is what "resize it smaller then larger and the prompt keeps the small
// window's position" is.
//
// xterm's semantics, which Konsole and VTE share: rows the cursor would
// fall past scroll off the TOP into scrollback exactly as if the
// program had printed them, and growing takes them back out again. That
// makes the two a ROUND TRIP, which is the property the bug was the
// absence of.
//
// **THE WHOLE GRID MOVES BY THE SAME AMOUNT**, which is what keeps a
// shell's own idea of where its paint started valid across the resize:
// the prompt row and the cursor row shift together, so the offset
// between them -- the only thing the shell recorded -- still holds.
//
// Counts are passed in rather than read from g_rows, because this runs
// while the old and new heights are both live.
static void rows_shrunk(struct session *s, int old_rows, int new_rows) {
    int over = s->cr - (new_rows - 1);
    if (over <= 0) return;              // the cursor already fits
    if (over > old_rows) over = old_rows;
    size_t rowbytes = (size_t)g_cap_cols * sizeof(struct cell);
    for (int i = 0; i < over; i++) {
        if (s->sb_count == SB_ROWS) {
            memmove(sb_row(s, 0), sb_row(s, 1), (size_t)(SB_ROWS - 1) * rowbytes);
            s->sb_count--;
        }
        memcpy(sb_row(s, s->sb_count), grid_row(s, 0), rowbytes);
        s->sb_count++;
        memmove(grid_row(s, 0), grid_row(s, 1),
                (size_t)(old_rows - 1) * rowbytes);
        row_clear(grid_row(s, old_rows - 1), 0);
    }
    s->cr -= over;
}

static void rows_grown(struct session *s, int old_rows, int new_rows) {
    int want = new_rows - old_rows;
    int have = s->sb_count < want ? s->sb_count : want;
    if (have <= 0) return;              // nothing kept to put back
    size_t rowbytes = (size_t)g_cap_cols * sizeof(struct cell);
    // Down first, from the bottom, so a row is read before it is
    // overwritten -- the grid is one array and the two ranges overlap.
    for (int r = old_rows - 1; r >= 0; r--)
        memcpy(grid_row(s, r + have), grid_row(s, r), rowbytes);
    for (int i = 0; i < have; i++)
        memcpy(grid_row(s, have - 1 - i), sb_row(s, --s->sb_count), rowbytes);
    for (int r = old_rows + have; r < new_rows; r++) row_clear(grid_row(s, r), 0);
    s->cr += have;
}

static void clamp_cursor(struct session *s) {
    if (s->cr < 0) s->cr = 0;
    if (s->cr >= g_rows) s->cr = g_rows - 1;
    if (s->cc < 0) s->cc = 0;
    if (s->cc >= g_cols) s->cc = g_cols - 1;
}

// One completed cursor/erase sequence. The parser has already applied
// every default -- "a missing or zero count means 1", 1-based rows and
// columns -- so this only has to act, which is the point of it being a
// shared parser rather than a second reading of the spec.
static void vt_ctrl(struct session *s) {
    switch (s->vt.op) {
    case ANSI_OP_MOVE_TO: s->cr = s->vt.a - 1; s->cc = s->vt.b - 1; break;
    case ANSI_OP_UP:      s->cr -= s->vt.a; break;
    case ANSI_OP_DOWN:    s->cr += s->vt.a; break;
    case ANSI_OP_RIGHT:   s->cc += s->vt.a; break;
    case ANSI_OP_LEFT:    s->cc -= s->vt.a; break;
    case ANSI_OP_COLUMN:  s->cc = s->vt.a - 1; break;
    case ANSI_OP_ROW:     s->cr = s->vt.a - 1; break;
    case ANSI_OP_ERASE_LINE:
        if (s->vt.a == 0) row_clear(grid_row(s, s->cr), s->cc);
        else if (s->vt.a == 1)
            for (int c = 0; c <= s->cc && c < g_cap_cols; c++)
                grid_row(s, s->cr)[c].ch = ' ';
        else row_clear(grid_row(s, s->cr), 0);
        break;
    case ANSI_OP_ERASE_DISPLAY:
        if (s->vt.a == 0) {
            row_clear(grid_row(s, s->cr), s->cc);
            for (int r = s->cr + 1; r < g_rows; r++) row_clear(grid_row(s, r), 0);
        } else if (s->vt.a == 1) {
            for (int r = 0; r < s->cr; r++) row_clear(grid_row(s, r), 0);
            for (int c = 0; c <= s->cc && c < g_cap_cols; c++)
                grid_row(s, s->cr)[c].ch = ' ';
        } else {
            for (int r = 0; r < g_rows; r++) row_clear(grid_row(s, r), 0);
        }
        break;
    case ANSI_OP_ALT_ON:
        // Idempotent: a program that switches twice must not overwrite
        // the screen it saved the first time with the alternate one.
        if (!s->alt) {
            memcpy(s->saved, s->grid,
                   (size_t)g_cap_rows * g_cap_cols * sizeof(struct cell));
            s->saved_cr = s->cr;
            s->saved_cc = s->cc;
            s->alt = 1;
        }
        for (int r = 0; r < g_rows; r++) row_clear(grid_row(s, r), 0);
        s->cr = s->cc = 0;
        break;
    case ANSI_OP_ALT_OFF:
        if (s->alt) {
            memcpy(s->grid, s->saved,
                   (size_t)g_cap_rows * g_cap_cols * sizeof(struct cell));
            s->cr = s->saved_cr;
            s->cc = s->saved_cc;
            s->alt = 0;
        }
        break;
    case ANSI_OP_SHOW: s->cursor_shown = 1; break;
    case ANSI_OP_HIDE: s->cursor_shown = 0; break;
    // SAVE/RESTORE have no users here yet, and a half-remembered
    // position is worse than none.
    default: break;
    }
    clamp_cursor(s);
}

// A title the SHELL sent. Copied rather than pointed at: the parser
// rewrites its own buffer on the next OSC, and the tab strip holds this
// pointer for as long as the tab exists.
// **JUST "Shell", WITH NO NUMBER.** It used to append the session's SLOT,
// which the strip's own numbering (uui_tabs.numbered) then contradicted:
// a recycled slot made the third tab read `3: Shell 1`. The strip numbers
// by position; the title says what the tab IS.
static void session_default_title(struct session *s, char *dst) {
    (void)s;
    const char *d = "Shell";
    int n = 0;
    while (d[n]) { dst[n] = d[n]; n++; }
    dst[n] = '\0';
}

static void vt_title(struct session *s) {
    // A PATH IS ALSO WHERE THE SHELL IS -- tosh's title is its cwd. Taken
    // before the lock test: a renamed tab still knows where its shell is.
    if (s->vt.osc[0] == '/') strlcpy(s->cwd, s->vt.osc, sizeof s->cwd);
    if (s->title_locked) return;
    char want[TITLE_MAX];
    int i = 0;
    while (s->vt.osc[i] && i < TITLE_MAX - 1) { want[i] = s->vt.osc[i]; i++; }
    want[i] = '\0';
    // An EMPTY title is a request to go back to the default rather than
    // a request for a blank tab -- a tab with no label is unreadable,
    // and a shell clearing its title is common.
    if (i == 0) session_default_title(s, want);

    if (strcmp(want, s->title) == 0) return;   // nothing to say
    strlcpy(s->title, want, sizeof s->title);

    // ON ITS OWN LINE, and only when it CHANGED: a title is text, so a
    // test can assert on it rather than on the pixels of a tab label,
    // and a shell that re-announces the same directory must not fill
    // the log. Not part of the layout line, because a title may contain
    // spaces and that line is parsed field by field.
    ulogf("uterm: tab %d title %s\n", s->index, s->title);
}

static void vt_write(struct session *s, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        switch (ansi_feed(&s->vt, buf[i])) {
        case ANSI_PASS:  vt_putc_raw(s, buf[i]); break;
        case ANSI_CTRL:  vt_ctrl(s); break;
        case ANSI_OSC:   vt_title(s); break;
        case ANSI_SGR:   break; // the colours are read off the parser per cell
        case ANSI_EATEN: break;
        }
    }
    // NEW OUTPUT PINS THE VIEW TO THE BOTTOM, which is what every
    // terminal does: a program printing while you are reading history
    // brings you back, because otherwise the thing you asked to run
    // appears to have done nothing. A SETTING, because the other
    // reading is also defensible -- a long build scrolling the page you
    // were reading out from under you is the reason Konsole offers it.
    if (g_conf.scroll_on_output) s->sb_view = 0;
}

// --- the reader thread ------------------------------------------------
//
// **IT TOUCHES `ring`, `head`, `eof` AND `done`, AND NOTHING ELSE.**
// Not the grid, not the parser, not a widget -- see ui/uapp.h. The main
// thread owns everything a frame reads.

static void *reader_main(void *arg) {
    struct session *s = (struct session *)arg;
    char buf[512];

    for (;;) {
        // BLOCKING, which is the whole point: this is the read the old
        // 30 ms poll was standing in for.
        int64_t n = sys_read(s->master, buf, sizeof buf);
        if (n <= 0) break;   // the shell is gone, or the master went away

        for (int64_t i = 0; i < n; i++) {
            unsigned head = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
            unsigned next = (head + 1) % RING_BYTES;
            // BACK-PRESSURE RATHER THAN DROPPING. A full ring means the
            // window has not drained yet; waiting costs this thread a
            // slice, while dropping would corrupt the screen in a way
            // nobody could diagnose from the result.
            while (next == __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE))
                sys_yield();
            s->ring[head] = buf[i];
            __atomic_store_n(&s->head, next, __ATOMIC_RELEASE);
        }
        uapp_post(g_app, s->index, 0);
    }

    s->eof = 1;
    uapp_post(g_app, s->index, 1);
    // LAST, and it is what lets the slot be reused: nothing after this
    // touches the session, so the main thread may hand it to a new tab.
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

// Move whatever the reader has published into the parser. Returns 1 if
// anything arrived, so the caller repaints only when there is something
// new.
static void release_held(struct session *s);

static int drain(struct session *s) {
    int got = 0;
    for (;;) {
        unsigned tail = __atomic_load_n(&s->tail, __ATOMIC_RELAXED);
        if (tail == __atomic_load_n(&s->head, __ATOMIC_ACQUIRE)) break;
        char c = s->ring[tail];
        __atomic_store_n(&s->tail, (tail + 1) % RING_BYTES, __ATOMIC_RELEASE);
        vt_write(s, &c, 1);
        got = 1;
    }
    if (got && !s->spoke) release_held(s);
    return got;
}

// --- sessions ---------------------------------------------------------

// **THE STRIP'S ORDER IS `g_tab_slot`, AND IT IS THE ORDER TABS WERE
// OPENED IN -- not the order their sessions sit in memory.** This walked
// the slot array instead, and slots are RECYCLED: closing a middle tab
// freed its slot, and the next new tab silently reappeared in that hole
// rather than at the right end. Every terminal and every browser appends.
//
// So this function only DROPS entries (a tab whose shell has gone) and
// refreshes their labels; appending is open_tab()'s job.
static void tabs_refresh(void) {
    // FOLLOW THE SELECTED SLOT, NOT ITS INDEX. Closing a tab to the LEFT
    // of the selected one shifts every index right of it, so clamping an
    // index moves the selection onto a different shell.
    int want = (g_strip.selected >= 0 && g_strip.selected < g_ntabs)
                 ? g_tab_slot[g_strip.selected] : -1;
    int old_index = g_strip.selected;

    int n = 0;
    for (int i = 0; i < g_ntabs; i++) {
        int slot = g_tab_slot[i];
        struct session *s = (slot >= 0 && slot < MAX_TABS) ? g_slot[slot] : 0;
        if (!s || !s->live) continue;
        g_tab_slot[n] = slot;
        g_tablabels[n].label = s->title;
        // EVERY TAB HAS A CLOSE BOX, the lone one included: the strip is
        // drawn at one tab now (chrome_h()), so hiding the box would
        // leave a control-shaped gap rather than no control.
        g_tablabels[n].closable = 1;
        n++;
    }
    g_ntabs = n;
    g_strip.count = n;

    int sel = -1;
    for (int i = 0; i < n; i++) if (g_tab_slot[i] == want) { sel = i; break; }
    // The selected tab itself went. Its INDEX is then the right answer:
    // it lands on whatever took its place, or on the new last tab --
    // which is what Konsole and every browser do.
    if (sel < 0) sel = old_index;
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    g_strip.selected = sel;
}

// `shell` NULL means the configured one; `dir` NULL leaves the shell in
// this process's own directory.
static int session_start(int slot, const char *shell, const char *dir) {
    struct session *s = g_slot[slot];
    if (!s) {
        s = (struct session *)malloc(sizeof *s);
        if (!s) return 0;
        // A FRESH STRUCT HAS NO BUFFERS YET, and malloc does not zero:
        // the pointers carried across the wipe below would otherwise be
        // whatever last lived here -- after grow_caps() has freed the
        // old grids, exactly them -- and the allocation would be skipped.
        s->grid = 0; s->sb = 0; s->saved = 0;
        g_slot[slot] = s;
    }
    // Zeroed whether it is new or reused -- a recycled session must not
    // inherit the last shell's screen. The cell buffers are heap-backed
    // now, so their pointers are carried across the wipe (the CONTENT
    // is cleared by vt_reset_screen below, and scrollback by sb_count
    // going to 0).
    struct cell *keep_grid = s->grid, *keep_sb = s->sb, *keep_saved = s->saved;
    char *raw = (char *)s;
    for (unsigned i = 0; i < sizeof *s; i++) raw[i] = 0;
    s->grid = keep_grid; s->sb = keep_sb; s->saved = keep_saved;
    if (!grow_caps(g_rows, g_cols)) return 0;   // first call sizes the caps
    if (!s->grid) {
        s->grid  = cells_new(g_cap_rows, g_cap_cols);
        s->sb    = cells_new(SB_ROWS, g_cap_cols);
        s->saved = cells_new(g_cap_rows, g_cap_cols);
        if (!s->grid || !s->sb || !s->saved) {
            free(s->grid); free(s->sb); free(s->saved);
            s->grid = 0; s->sb = 0; s->saved = 0;
            return 0;
        }
    }
    s->index = slot;
    s->master = -1;
    s->started_ns = sys_monotonic_ns();
    s->cursor_shown = 1;
    ansi_init(&s->vt, VT_FG, VT_BG);
    vt_reset_screen(s);

    session_default_title(s, s->title);

    int slave = -1;
    if (sys_openpty(&s->master, &slave) < 0) return 0;

    // NO sys_set_nonblock() ANY MORE, and its absence is the change: a
    // reader thread WANTS to block. The flag used to be needed because
    // the drain ran on the window's tick and a blocking read there would
    // have frozen the window rather than slowed it.

    // The child's 0/1/2 are the slave. dup2 around the spawn, exactly as
    // tosh's own redirection does -- SYS_SPAWN inherits the descriptor
    // table, so placing them here is placing them in the child.
    int in0 = sys_dup(0), out1 = sys_dup(1), err2 = sys_dup(2);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    sys_dup2(slave, 2);
    // ITS OWN GROUP, which is what makes it interruptible as a unit --
    // and with tabs it is also what keeps one tab's Ctrl-C out of
    // another's: the group is per session, so the signal reaches the job
    // in the tab you are looking at.
    // A NEW SESSION: this window's pty is its own terminal, and the
    // shell has to own it on behalf of everything it starts -- including
    // a second shell. See abi/syscall_abi.h's SPAWN_SETSID.
    char shbuf[SETTING_ABI_VALUE_MAX];
    if (!shell) shell = g_conf.shell[0] ? g_conf.shell : shell_path(shbuf, sizeof shbuf);
    strlcpy(s->shell, shell, sizeof s->shell);
    // THE CHILD INHERITS OUR CWD, so a new tab's directory is set by
    // standing in it for the spawn -- the same dance as the fds above.
    char back[TITLE_MAX * 2];
    int moved = dir && getcwd(back, sizeof back) && chdir(dir) == 0;
    if (moved) strlcpy(s->cwd, dir, sizeof s->cwd);
    s->child = sys_spawn_flags(s->shell, 0, -1, 0, PGID_NEW, SPAWN_SETSID);
    if (moved) chdir(back);
    if (in0  >= 0) { sys_dup2(in0, 0);  sys_close(in0); }
    if (out1 >= 0) { sys_dup2(out1, 1); sys_close(out1); }
    if (err2 >= 0) { sys_dup2(err2, 2); sys_close(err2); }

    // OUR copy of the slave goes now. The child holds its own through
    // the fds above, and keeping this one would mean the master never
    // sees end-of-file when the shell dies.
    sys_close(slave);

    if (s->child < 0) { sys_close(s->master); s->master = -1; return 0; }

    struct tty_winsize ws = { (uint16_t)g_rows, (uint16_t)g_cols };
    sys_tcsetwinsz(s->master, &ws);

    s->live = 1;
    s->done = 0;
    // DETACHED: nothing joins it. Joining would mean blocking the UI
    // until a shell that may be ignoring its terminal decides to exit,
    // and the slot is recycled on `done` instead -- which is the same
    // information a join would have carried.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&s->reader, &at, reader_main, s) != 0) {
        ulog("uterm: could not start a reader thread\n");
        s->live = 0;
        sys_close(s->master);
        s->master = -1;
        return 0;
    }
    s->reader_started = 1;
    return 1;
}

// END THE SHELL, DO NOT ORPHAN IT. Closing the master alone would give
// it end-of-file and it would exit on its own -- but only when it NEXT
// READS, and a shell waiting on a job does not read for as long as that
// job runs. A tab that has gone must not leave a process on a terminal
// nobody can type at.
//
// SIGTERM rather than SIGKILL: this is the polite one, and the shell is
// not being force-quit -- the person closed its tab.
static void session_stop(struct session *s) {
    if (!s || !s->live) return;
    if (s->child > 0) {
        sys_kill(s->child, SIGTERM);
        int status = 0;
        sys_waitpid(s->child, &status);
        s->child = 0;
    }
    s->live = 0;
    // The master stays OPEN until the reader has finished with it --
    // closing an fd a blocked thread is reading is not something this
    // kernel defines. Killing the shell is what ends that read.
}

static int open_tab_with(const char *shell) {
    int slot = -1;
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s) { slot = i; break; }
        // A slot whose reader has finished is free. One whose reader is
        // still winding down is NOT -- handing it to a new tab would
        // hand two threads one ring.
        if (!s->live && s->done) { slot = i; break; }
    }
    if (slot < 0) return 0;
    if (g_ntabs >= MAX_TABS) return 0;
    // A recycled slot's master is still open: its reader has finished
    // with it, and nothing else would ever close it.
    if (g_slot[slot] && g_slot[slot]->master >= 0) {
        sys_close(g_slot[slot]->master);
        g_slot[slot]->master = -1;
    }
    const char *dir = 0;
    struct session *cur = active();
    if (g_conf.newtab_dir == TERM_NEWTAB_HOME) dir = "/home";
    else if (cur && cur->cwd[0]) dir = cur->cwd;
    if (!session_start(slot, shell, dir)) return 0;
    // APPENDED, so a new tab is always the RIGHT-HAND one whatever slot
    // it got -- see tabs_refresh() on what walking the slots did instead.
    g_tab_slot[g_ntabs] = slot;
    g_ntabs++;
    g_strip.selected = g_ntabs - 1;
    tabs_refresh();
    return 1;
}

static int open_tab(void) { return open_tab_with(0); }

static void mark_cmd(struct session *s) {
    s->is_cmd = 1;
    strlcpy(s->title, k_path_basename(s->shell), sizeof s->title);
    s->title_locked = 1;
}

// The `-e` tab: PATH, standing in its own folder, as a double-click on
// Windows or GNOME runs a program.
static int open_cmd_tab(void) {
    char dir[TITLE_MAX];
    strlcpy(dir, g_cmd, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *(slash == dir ? slash + 1 : slash) = '\0';
    if (!session_start(0, g_cmd, slash ? dir : 0)) return 0;
    mark_cmd(g_slot[0]);
    g_tab_slot[0] = 0;
    g_ntabs = 1;
    g_strip.selected = 0;
    tabs_refresh();
    return 1;
}

static void close_tab(int tab) {
    if (tab < 0 || tab >= g_ntabs) return;
    struct session *s = g_slot[g_tab_slot[tab]];
    session_stop(s);
    tabs_refresh();
    if (g_ntabs == 0 && g_app) uapp_quit(g_app, 0);
}

// --- drawing ----------------------------------------------------------
//
// A ROW AT A TIME, IN RUNS OF ONE COLOUR PAIR. Per-cell drawing would be
// 12,000 calls a frame at this size; a run is one ugfx_draw_string() and
// there is usually one run per row. The background is painted per run
// too, which is what makes reverse video -- a status bar -- look like a
// bar rather than like coloured letters.

static int menubar_h(void) {
    return g_menu_shown ? uui_menubar_height(&g_menu) : 0;
}

// **THE STRIP IS ALWAYS DRAWN NOW, EVEN AT ONE TAB.** It used to hide
// itself below two, which is Konsole's default -- but the strip carries
// the "+" and a control that is only there once you already have what it
// creates is no control at all. The cost is one row of chrome on a
// single-shell window and a geometry that no longer jumps when a second
// tab opens, which is the half worth having.
// The bar row: as tall as the taller of the strip and the toolbar.
// UI face, as everything chrome-shaped here is.
static int tabbar_h(void) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int h = uui_tabs_height(), th = uui_toolbar_height(&g_tb);
    ugfx_set_font(was);
    return th > h ? th : h;
}

static int chrome_h(void) {
    // The same rule as bar_w(): the menu bar and the tab strip are the
    // toolkit's, drawn in the UI face, and this is asked both inside
    // and outside the grid-font bracket.
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int h = menubar_h();
    ugfx_set_font(was);
    return h + tabbar_h();
}

static void draw_run(struct ugfx_surface *s, int x, int y,
                     const char *text, int n, uint8_t fg, uint8_t bg) {
    if (n <= 0 || !g_runbuf) return;
    char *buf = g_runbuf;
    for (int i = 0; i < n; i++) buf[i] = text[i];
    buf[n] = '\0';

    // **THE BACKGROUND IS A RECTANGLE, NOT THE STRING CALL'S `bg`.**
    // ugfx_draw_string() blends the glyph's own pixels against that
    // colour; it does not fill the CELL. For ordinary text the two look
    // identical, and for REVERSE VIDEO they are not: a status bar came
    // out as dark letters on black instead of black letters on a bar --
    // legible, and not what was asked for. Caught by reading pixel
    // values rather than by looking at the screenshot.
    if ((bg & 15) != VT_BG)
        ugfx_fill_rect(s, x, y, n * cell_w(), cell_h(), VGA_RGB[bg & 15]);
    ugfx_draw_string(s, x, y, buf, VGA_RGB[fg & 15], VGA_RGB[bg & 15]);
}

// `sc0`/`sc1` are the selected columns of this row, as a half-open
// range; equal means none. A SELECTED CELL SWAPS ITS OWN COLOURS rather
// than taking a highlight colour, which is what every terminal does and
// what keeps a coloured `ls` legible inside a selection.
static void draw_row(struct ugfx_surface *s, const struct cell *row, int y,
                     int sc0, int sc1) {
    int cw = cell_w();
    int i = 0;
    while (i < g_cols) {
        int sel = (i >= sc0 && i < sc1);
        // A run ends where the colours change OR where the selection
        // starts or stops, because the two halves are drawn differently.
        int j = i;
        while (j < g_cols && row[j].fg == row[i].fg && row[j].bg == row[i].bg &&
               (j >= sc0 && j < sc1) == sel) j++;
        // TRAILING BLANKS IN THE DEFAULT COLOURS ARE NOT DRAWN -- the
        // surface is already that colour, and drawing them would cost a
        // full row of glyphs per line for nothing. A SELECTED blank IS
        // drawn: it is what shows the selection reaching the line end.
        int blank = 1;
        for (int k = i; k < j; k++) if (row[k].ch != ' ') { blank = 0; break; }
        if ((sel || !(blank && row[i].bg == VT_BG)) && g_rowbuf) {
            char *run = g_rowbuf;
            for (int k = i; k < j; k++) run[k - i] = row[k].ch;
            draw_run(s, g_margin + i * cw, y, run, j - i,
                      sel ? row[i].bg : row[i].fg,
                      sel ? row[i].fg : row[i].bg);
        }
        i = j;
    }
}

// --- find ---------------------------------------------------------------
//
// **A HIT IS A LINE IN THE VIRTUAL BUFFER**, the selection's coordinates
// (see struct selpoint), so a hit in the scrollback and one on the screen
// are found, drawn and scrolled to by the same arithmetic. Case is
// ignored, as Konsole's and Windows Terminal's default is. Matches do not
// overlap.
//
// THE HITS GO STALE WHEN OUTPUT SCROLLS THE BUFFER, so they are found
// again after every drain while the bar is open (on_user) -- cheap at
// this buffer's size, and simpler than shifting them.
#define FIND_MAX 512   // past it the count reads as a floor ("512 found")

struct findhit { int line, col; };
static struct findhit g_hits[FIND_MAX];
static int g_nhits;
static int g_hit_cur = -1;   // index into g_hits, -1 for none
static int g_find_len;

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// `keep` holds the current hit where it was when it is still there --
// what new output arriving under an open find bar wants.
static void find_run(struct session *s, int keep) {
    const char *q = uui_findbar_query(&g_find);
    int n = (int)strlen(q);
    struct findhit was = { -1, -1 };
    if (keep && g_hit_cur >= 0) was = g_hits[g_hit_cur];
    g_nhits = 0;
    g_hit_cur = -1;
    g_find_len = n;
    if (s && n > 0 && n <= g_cols) {
        int total = s->sb_count + g_rows;
        for (int line = 0; line < total && g_nhits < FIND_MAX; line++) {
            const struct cell *row = virt_row(s, line);
            if (!row) continue;
            for (int c = 0; c + n <= g_cols && g_nhits < FIND_MAX; c++) {
                int k = 0;
                while (k < n && fold(row[c + k].ch) == fold(q[k])) k++;
                if (k < n) continue;
                g_hits[g_nhits].line = line;
                g_hits[g_nhits].col = c;
                g_nhits++;
                c += n - 1;
            }
        }
    }
    // The NEWEST hit by default: what a person searching a terminal is
    // after is almost always the output they just saw scroll past.
    if (g_nhits) g_hit_cur = g_nhits - 1;
    for (int i = 0; keep && i < g_nhits; i++)
        if (g_hits[i].line == was.line && g_hits[i].col == was.col) { g_hit_cur = i; break; }
    g_find.matches = g_nhits;
    g_find.current = g_hit_cur + 1;
}

// Scroll so the current hit is on screen, centred when it was not.
static void find_reveal(struct session *s) {
    if (!s || g_hit_cur < 0) return;
    int line = g_hits[g_hit_cur].line;
    int top = s->sb_count - s->sb_view;
    if (line >= top && line < top + g_rows) return;
    int want = s->sb_count - (line - g_rows / 2);
    if (want > s->sb_count) want = s->sb_count;
    if (want < 0) want = 0;
    s->sb_view = want;
}

static void find_step(struct session *s, int delta) {
    if (g_nhits <= 0) return;
    g_hit_cur = (g_hit_cur + delta + g_nhits) % g_nhits;   // wraps, as every find does
    g_find.current = g_hit_cur + 1;
    find_reveal(s);
}

static void find_open(void) {
    g_find_open = 1;
    uui_findbar_activate(&g_find);
    find_run(active(), 0);
}

static void find_close(void) {
    g_find_open = 0;
    uui_textbox_set_active(&g_find.field, 0);
    g_nhits = 0;
    g_hit_cur = -1;
}

// The hits on screen, over the text. Yellow for a hit, orange for the
// current one -- the pair every editor uses -- with the text redrawn
// dark, which reads on a light scheme and a dark one alike.
static void draw_hits(struct ugfx_surface *s, struct session *ses, int top) {
    if (!g_find_open || g_find_len <= 0) return;
    int cw = cell_w(), ch = cell_h();
    int first = ses->sb_count - ses->sb_view;
    for (int i = 0; i < g_nhits; i++) {
        int r = g_hits[i].line - first;
        if (r < 0 || r >= g_rows) continue;
        const struct cell *row = virt_row(ses, g_hits[i].line);
        if (!row) continue;
        int c0 = g_hits[i].col, n = g_find_len;
        if (c0 + n > g_cols) n = g_cols - c0;
        int x = g_margin + c0 * cw, y = top + g_margin + r * ch;
        uint32_t hl = i == g_hit_cur ? ugfx_rgb(246, 116, 0) : ugfx_rgb(253, 188, 75);
        ugfx_fill_rect(s, x, y, n * cw, ch, hl);
        char text[TITLE_MAX * 2];
        if (n >= (int)sizeof text) n = (int)sizeof text - 1;
        for (int k = 0; k < n; k++) text[k] = row[c0 + k].ch;
        text[n] = '\0';
        ugfx_draw_string(s, x, y, text, ugfx_rgb(35, 38, 41), hl);
    }
}

// --- the Session panel ---------------------------------------------------

static void draw_panel(struct ugfx_surface *s, struct session *ses) {
    struct term_panel_info in = {
        .shell = ses ? ses->shell : "",
        .cwd = ses ? ses->cwd : "",
        .scheme = g_scheme.label,
        .pid = ses ? ses->child : 0,
        .exited = ses ? ses->exited : 1,
        .cols = g_cols, .rows = g_rows,
        .sb_count = ses ? ses->sb_count : 0, .sb_cap = SB_ROWS,
    };
    term_panel_set_exited(in.exited);
    term_panel_draw(s, &in);
}

// The screen effect over the grid -- text, caret and all -- with the
// scrollbar, find bar and menus drawn after it, crisp, as the window's.
static void draw_effect(struct ugfx_surface *s, int top, int gr) {
    if (!g_effect || !ucrt_look_on(&g_conf.crt)) return;
    g_crt.look = g_conf.crt;
    g_crt.period = (cell_h() + 3) / 6;
    // A neutral frame a little lighter than black, tinted by the page, so
    // the tube's edge reads against a dark scheme.
    g_crt.bezel = ugfx_blend(ugfx_rgb(22, 22, 22), VGA_RGB[VT_BG], 40);
    static int frames;   // since the size or the look last changed
    static struct ucrt_look last;
    int first = g_crt.w != gr || g_crt.h != s->h - top || memcmp(&last, &g_crt.look, sizeof last);
    last = g_crt.look;
    frames = first ? 1 : frames + 1;
    unsigned long long t0 = sys_monotonic_ns();
    if (ucrt_apply(&g_crt, s, 0, top, gr, s->h - top) != 0 && first)
        ulog("uterm: no memory for the screen effect; drawn plain\n");
    // TWO lines per size -- the first frame builds the tables, the second
    // is what every later one costs -- so a test can read both without
    // the log filling at a line per keystroke.
    if (frames <= 2)
        ulogf("uterm: effect %dx%d %s %llu us\n", gr, s->h - top, frames == 1 ? "first" : "frame",
              (sys_monotonic_ns() - t0) / 1000ULL);
    if (ucrt_animates(&g_conf.crt)) uui_anim_request();
}

static void draw_scrollbar(struct ugfx_surface *s, struct session *ses) {
    // The scrollbar. TOTAL is the whole virtual buffer -- scrollback plus
    // the screen -- and the offset is sb_view unconverted, because a
    // vertical bar here already counts from the bottom (uui_scrollbar.h).
    // Drawn only when there is history to scroll: an overlay bar is an
    // indicator of somewhere else to go, and with none it would be a line
    // down the margin saying nothing.
    //
    // Its colours are Plasma's DARK pair (#31363b track, #76797c thumb),
    // not the toolkit theme's: the near-white bar Notepad draws would be
    // the brightest thing on a dark page.
    int bx, by, bw, bh;
    bar_rect(s->w, s->h, &bx, &by, &bw, &bh);
    unsigned long long now = uui_anim_now_ns();
    if (g_sb_collapse && now >= g_sb_collapse && g_bar_grab < 0) {
        g_sb_collapse = 0;
        g_sb_want = 0;
        utween_retarget(&g_sb_tw, 0, SB_GROW_MS, now);
    }
    int e = utween_value(&g_sb_tw, now);
    if (utween_active(&g_sb_tw) || g_sb_collapse) uui_anim_request();
    if (bw > 0 && ses->sb_count > 0) {
        int ty, th;
        uui_scrollbar_thumb_rect(by, bh, ses->sb_count + g_rows, g_rows, ses->sb_view,
                                 &ty, &th, bw, 0);
        uint32_t page = VGA_RGB[VT_BG];
        // Thin: a 3 px thumb on the bar's right edge. Wide: the groove and
        // the inset thumb. Every frame between is the two blended, the
        // groove growing in from the edge -- the Start menu's drawing.
        if (e > 0) {
            int gw = 3 + (bw - 3) * e / SB_FULL;
            uui_fill_round_rect(s, bx + bw - gw, by, gw, bh, UUI_CAPSULE,
                                ugfx_blend(page, ugfx_rgb(49, 54, 59), (uint8_t)(255 * e / SB_FULL)));
        }
        int inset = uui_scrollbar_thumb_inset(bw);
        int tw = 3 + (bw - 2 * inset - 3) * e / SB_FULL;
        int tr = (bx + bw - 1) - (inset - 1) * e / SB_FULL;
        uui_fill_round_rect(s, tr - tw + 1, ty, tw, th, UUI_CAPSULE,
                            g_bar_grab >= 0 ? ugfx_rgb(150, 153, 156) : ugfx_rgb(118, 121, 124));
    }
}

static void draw(struct ugfx_surface *s, int focused) {
    struct session *ses = active();
    int top = chrome_h();
    // The grid is drawn in THIS terminal's font; the menu bar and tab
    // strip are the toolkit's and stay in the desktop's. font_sync()
    // also re-reads the metrics, which is what makes a desktop font-size
    // change reflow a window that is following it.
    font_sync();
    const struct ugfx_font *was_font = ugfx_set_font(grid_font());
    // ONLY THE GRID AREA, not the whole surface: the toolkit paints the
    // tab strip after this runs, and a full-surface fill here would wipe
    // whatever it had already put down (ui/uapp.c's draw order).
    int gr = grid_right(s->w);
    ugfx_fill_rect(s, 0, top, gr, s->h - top, VGA_RGB[VT_BG]);
    // The bar's ground, under the strip and the toolbar: the strip sits
    // at its foot, and the air above it is this.
    ugfx_fill_rect(s, 0, menubar_h(), s->w, tabbar_h(), UTHEME_WINDOW_BG);
    ugfx_fill_rect(s, 0, menubar_h() + tabbar_h() - 1, s->w, 1, UTHEME_BORDER);
    if (g_panel_open) draw_panel(s, ses);
    if (!ses) { ugfx_set_font(was_font); return; }

    int ch = cell_h(), cw = cell_w();

    // Scrolled back: the top rows come from history, the rest from the
    // screen, and they meet without a seam because both are the same
    // grid of cells. That is the payoff of scrollback being made of
    // evicted ROWS rather than of a character stream.
    for (int r = 0; r < g_rows; r++) {
        int line = virt_of_row(ses, r);
        const struct cell *row = virt_row(ses, line);
        if (!row) continue;   // before the oldest line we kept
        int sc0, sc1;
        sel_cols(line, &sc0, &sc1);
        draw_row(s, row, top + g_margin + r * ch, sc0, sc1);
    }
    draw_hits(s, ses, top);

    // The caret, only while FOCUSED and only while the program wants it
    // shown (`ESC[?25l` hides it -- a full-screen program parking the
    // caret somewhere meaningless turns it off rather than moving it).
    // An unfocused window -- or one under a modal -- drawing a caret
    // claims to be taking input that is going somewhere else.
    if (focused && ses->cursor_shown && ses->sb_view == 0 && g_caret_on
            && !modal_up() && !ses->exited) {
        int cx = g_margin + ses->cc * cw, cy = top + g_margin + ses->cr * ch;
        if (g_conf.cursor == TERM_CURSOR_UNDER) {
            int t = ch / 8 + 1;
            ugfx_fill_rect(s, cx, cy + ch - t, cw, t, g_scheme.cursor);
        } else if (g_conf.cursor == TERM_CURSOR_BAR) {
            ugfx_fill_rect(s, cx, cy, cw / 8 + 1, ch, g_scheme.cursor);
        } else {
            // A BLOCK MUST REDRAW ITS CHARACTER, or it hides the one
            // thing the caret is pointing at. In the BACKGROUND colour,
            // which is what every terminal does and what keeps the
            // glyph legible against the caret.
            const struct cell *row = virt_row(ses, virt_of_row(ses, ses->cr));
            ugfx_fill_rect(s, cx, cy, cw, ch, g_scheme.cursor);
            if (row && row[ses->cc].ch != ' ') {
                char one[2] = { row[ses->cc].ch, '\0' };
                ugfx_draw_string(s, cx, cy, one, VGA_RGB[VT_BG], g_scheme.cursor);
            }
        }
    }
    draw_effect(s, top, gr);
    // After the effect: the scrollbar is the window's, not the tube's.
    draw_scrollbar(s, ses);
    ugfx_set_font(was_font);
}

// One line, content-relative, on stderr -- the grammar every GUI test
// tool here asserts on. The CURSOR, as a cell index, because that is the
// thing that would be wrong if a cursor sequence were mishandled and it
// is what a test can predict.
// The ORDER, folded to one number. Part of the signature below because
// closing a middle tab and opening another changes which slot sits
// where without changing the count -- and that order is exactly what a
// test asserting "a new tab went to the right end" has to see.
static int order_sig(void) {
    int v = 0;
    for (int i = 0; i < g_ntabs; i++) v = v * 11 + g_tab_slot[i] + 1;
    return v;
}

// Has anything the rect lines below describe moved since the last
// frame? A signature rather than a comparison of the text: the values
// are small integers and this runs on every draw.
static int chrome_moved(void) {
    static int prev = -1;
    int sig = g_strip.count
            + 7 * order_sig()
            + 17 * g_strip.selected
            + 313 * g_menu_shown
            + 1021 * uui_menubar_depth(&g_menu)
            + 4093 * (g_menu.open_root + 1)
            + 8191 * uui_menubar_depth(&g_ctx)
            + 16381 * g_panel_open
            + 32749 * g_find_open
            + 65537 * g_strip.w;
    if (sig == prev) return 0;
    prev = sig;
    return 1;
}

static int sel_measure(struct session *s, char *out, int cap);

static void log_layout(void) {
    struct session *s = active();
    if (!s) return;
    // **THE FIRST FIELD AFTER `cursor` IS A LINEAR CELL OFFSET, and it
    // has to stay one**: tools/uterm_test.py reads it with
    // `split()[0]`, so a row/column PAIR there parses as neither. Extra
    // fields are safe after it and are what a tabbed window adds.
    char b[256];
    // The CHROME's height, so a pixel check aiming at the grid does not
    // guess where it starts -- the menu bar's arrival moved that edge
    // and a hardcoded band would have gone on comparing the strip.
    //
    // `sbview`/`sbcount` are the SCROLL POSITION and how far back it can
    // go, and `selbytes` how much text is selected. All three so a check
    // can assert the view MOVED and the selection EXISTS without reading
    // pixels -- which for a scrollbar is the assertion that matters
    // (docs/gui-guidelines.md's point 9) and for a selection is the only
    // one that distinguishes "highlighted" from "would be copied".
    snprintf(b, sizeof b,
             "uterm: layout cursor %d rows %d cols %d tabs %d sel %d menu %d "
             "chrome %d rename %d sbview %d sbcount %d selbytes %d "
             "find %d hits %d cur %d panel %d exited %d "
             "effect %d curve %d margin %d cell %d %d\n",
             s->cr * g_cols + s->cc, g_rows, g_cols, g_ntabs,
             g_strip.selected, g_menu_shown, chrome_h(), g_rename_open,
             s->sb_view, s->sb_count, sel_measure(s, 0, 0),
             g_find_open, g_nhits, g_hit_cur + 1, g_panel_open, s->exited,
             g_effect, g_effect ? g_conf.crt.curve : 0, g_margin, cell_w(), cell_h());
    uapp_log_layout_line(b);

    // The chrome's rects, the same grammar Notepad reports -- a test
    // clicking a menu row must be told where it is, not derive it.
    //
    // THROUGH uapp_log_layout_line(), NEVER ulogf(): that call is what
    // the `desktop.layout_log` gate and the per-frame dedupe hang off
    // (docs/conventions/gui.md), and a raw log here would write this
    // whole block on every frame with a window open.
    //
    // **AND ONLY WHEN THE CHROME ACTUALLY MOVED.** The dedupe is per
    // BLOCK, so a cursor that advanced by one cell re-emits everything
    // in the block with it -- which made a scrolling program log ~17
    // geometry lines a frame instead of the one line that changed.
    // These rects move when a tab opens or a menu does, not when a
    // shell prints, so they carry their own guard.
    if (!chrome_moved()) return;

    // The chrome's own rects -- the strip's slots, the menu's titles and
    // rows -- come from the widgets themselves, in the shared vocabulary
    // (ui/uui_describe.h). WHICH SESSION IS AT WHICH POSITION stays the
    // app's: a rect says where a tab is drawn and nothing about which
    // shell it holds, so a test checking that a new tab was APPENDED
    // rather than dropped into a recycled slot's hole needs this.
    uapp_log_layout(g_app, "uterm");
    if (g_app) {
        int bx, by, bw, bh;
        bar_rect(uapp_width(g_app), uapp_height(g_app), &bx, &by, &bw, &bh);
        char r[64];
        snprintf(r, sizeof r, "uterm: layout bar %d %d %d %d\n", bx, by, bw, bh);
        uapp_log_layout_line(r);
    }
    for (int i = 0; i < g_ntabs; i++) {
        char b[64];
        snprintf(b, sizeof b, "uterm: layout tabslot %d %d\n", i, g_tab_slot[i]);
        uapp_log_layout_line(b);
    }
    // The bar's buttons: the toolbar describes them only when it
    // overflows, and a test clicking the ☰ needs to be told where it is.
    for (int i = 0; i < TB_COUNT; i++) {
        int x, y, w, h;
        if (!uui_toolbar_item_rect(&g_tb, i, &x, &y, &w, &h)) continue;
        char b[80];
        snprintf(b, sizeof b, "uterm: layout tbbtn %d %d %d %d %d\n", i, x, y, w, h);
        uapp_log_layout_line(b);
    }
}

static void size_changed(int w, int h);

// --- renaming ---------------------------------------------------------

// The field, centred over the GRID. Derived from the window rather than
// stored, so it survives a resize and a font change with nothing to keep
// in step -- the same reason nothing else here caches a pixel.
static void rename_rect(int w, int h, int *x, int *y, int *fw, int *fh) {
    int width = w / 2;
    if (width < ugfx_char_w() * 16) width = ugfx_char_w() * 16;
    if (width > w - 4 * utheme_pad()) width = w - 4 * utheme_pad();
    *fw = width;
    *fh = utheme_control_h();
    *x = (w - width) / 2;
    *y = chrome_h() + (h - chrome_h() - *fh) / 2;
}

static void rename_begin(void) {
    struct session *s = active();
    if (!s) return;
    g_rename_slot = s->index;
    uui_textbox_init(&g_rename, s->title);
    uui_textbox_set_active(&g_rename, 1);
    g_rename_open = 1;
}

// An EMPTY name hands the tab back to its shell rather than blanking it:
// a tab with no label is unreadable, and "undo the rename" needs somewhere
// to live. That is the same judgement vt_title() already makes about an
// empty OSC.
static void rename_commit(void) {
    g_rename_open = 0;
    struct session *s = (g_rename_slot >= 0 && g_rename_slot < MAX_TABS)
                          ? g_slot[g_rename_slot] : 0;
    g_rename_slot = -1;
    if (!s || !s->live) return;
    const char *want = uui_textbox_text(&g_rename);
    if (!want || !want[0]) {
        s->title_locked = 0;
        session_default_title(s, s->title);
    } else {
        strlcpy(s->title, want, sizeof s->title);
        s->title_locked = 1;
    }
    ulogf("uterm: tab %d title %s\n", s->index, s->title);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    // MEASURED HERE AS WELL AS ON RESIZE, because this is the first
    // moment the real surface exists -- and because the FONT can change
    // under a running client (WIN_EV_FONT), which changes the cell size
    // without changing the window's. It early-outs when nothing moved.
    size_changed(s->w, s->h);
    // The chrome is positioned here rather than by a layout: this window
    // is two strips over a canvas it paints itself, and running a
    // uui_layout for that would be more machinery than arithmetic.
    int mh = menubar_h();
    uui_menubar_set_geometry(&g_menu, 0, 0, s->w, mh);
    // The whole content area, so a menu that will not fit below the bar
    // may flip or slide against the window rather than off it.
    uui_menubar_set_bounds(&g_menu, 0, 0, s->w, s->h);
    uui_menubar_set_bounds(&g_ctx, 0, 0, s->w, s->h);
    uui_toolbar_set_bounds(&g_tb, 0, 0, s->w, s->h);
    uui_dialog_set_bounds(&g_quit_ask, 0, 0, s->w, s->h);

    // The bar: the toolbar at its natural width on the right, the strip
    // in what is left, FLOATING -- centred in the bar, clear of the grid
    // below it; draw() puts the one hairline under the bar.
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int bh = tabbar_h(), th = uui_tabs_height(), tbw = 0, tbh = 0;
    uui_toolbar_natural_size(&g_tb, &tbw, &tbh);
    uui_toolbar_ops.set_geometry(&g_tb, s->w - tbw, mh, tbw, bh);
    uui_tabs_set_geometry(&g_strip, 0, mh + (bh - th) / 2, s->w - tbw, th);

    // Find floats at the grid's top right, clear of the scrollbar.
    int fw = 0, fh = 0;
    uui_findbar_natural_size(&g_find, &fw, &fh);
    int room = grid_right(s->w) - bar_w() - 4 * g_margin;
    if (fw > room) fw = room;
    uui_findbar_set_geometry(&g_find, grid_right(s->w) - bar_w() - 2 * g_margin - fw,
                             chrome_h() + g_margin, fw, fh);
    if (g_panel_open) term_panel_layout(grid_right(s->w), chrome_h(), panel_w(), s->h - chrome_h());
    ugfx_set_font(was);

    g_widgets[W_MENU].hidden = !g_menu_shown;
    g_widgets[W_FIND].hidden = !g_find_open;
    for (int i = 0; i < TERM_PANEL_BUTTONS; i++) g_widgets[W_PANEL0 + i].hidden = !g_panel_open;
    draw(s, uapp_focused(a));
    log_layout();
}

// The rename prompt, over the grid and after everything else -- see the
// note beside g_rename. uapp runs on_draw_over last (ui/uapp.c), which
// is the same z-order rule uui_menubar_draw_popup() states.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    if (!g_rename_open) return;
    struct ugfx_surface *s = uapp_surface(d);
    int fx, fy, fw, fh;
    rename_rect(s->w, s->h, &fx, &fy, &fw, &fh);
    uui_textbox_set_geometry(&g_rename, fx, fy, fw, fh);
    int pad = utheme_pad();
    ugfx_fill_rect(s, fx - pad, fy - pad - ugfx_char_h() - pad,
                    fw + 2 * pad, fh + 3 * pad + ugfx_char_h(),
                    UTHEME_PANEL_BG);
    ugfx_draw_rect(s, fx - pad, fy - pad - ugfx_char_h() - pad,
                    fw + 2 * pad, fh + 3 * pad + ugfx_char_h(),
                    UTHEME_BORDER);
    ugfx_draw_string(s, fx, fy - pad - ugfx_char_h(), "Rename tab:",
                      UTHEME_TEXT, UTHEME_PANEL_BG);
    uui_textbox_draw(s, &g_rename);
}

// HOW BIG THE WINDOW IS, IN CELLS, AND EVERY CHILD IS TOLD. Only this
// process can work that out: the size is pixels and the answer is cells,
// and the conversion needs the font. SYS_TCSETWINSZ is how the shell and
// anything it runs find out -- /bin/edit asks for it before it draws a
// single row.
//
// EVERY SESSION, not just the visible one: a background shell that
// learned the wrong size would draw its next full-screen program wrong
// the moment you switched to it.
static void size_changed(int w, int h) {
    int cw = cell_w(), ch = cell_h();
    if (cw <= 0 || ch <= 0) return;
    int curve_m = g_effect ? ucrt_margin(&g_conf.crt, grid_right(w), h - chrome_h()) : 0;
    g_margin = curve_m > TERM_MARGIN ? curve_m : TERM_MARGIN;
    int rows = (h - chrome_h() - 2 * g_margin) / ch;
    // THE GUTTER COMES OFF THE WIDTH, and default_size() adds it back --
    // the two are inverses and a bar counted in only one of them is a
    // window that opens one column narrower than it asks for.
    int cols = (grid_right(w) - 2 * g_margin) / cw;   // the bar overlays (bar_rect)
    if (rows < 2) rows = 2;
    if (cols < 8) cols = 8;
    // Grow the buffers to fit; on a failed malloc keep the old
    // capacity and clamp, which is the old fixed-grid behaviour.
    if (!grow_caps(rows, cols)) {
        if (rows > g_cap_rows) rows = g_cap_rows;
        if (cols > g_cap_cols) cols = g_cap_cols;
    }
    if (rows == g_rows && cols == g_cols) return;

    int old_rows = g_rows;
    g_rows = rows;
    g_cols = cols;
    struct tty_winsize ws = { (uint16_t)rows, (uint16_t)cols };
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->live || s->master < 0) continue;
        // THE ALTERNATE SCREEN IS NOT SCROLLED, because it has no
        // scrollback to scroll into -- a full-screen program owns every
        // row and redraws them all when it hears the size changed.
        if (!s->alt) {
            if (rows < old_rows)      rows_shrunk(s, old_rows, rows);
            else if (rows > old_rows) rows_grown(s, old_rows, rows);
        }
        clamp_cursor(s);
        // LAST, because it raises SIGWINCH: the program on the other end
        // repaints from where the cursor is, so the grid has to be in
        // its final shape before it is told.
        sys_tcsetwinsz(s->master, &ws);
    }
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    size_changed(w, h);
}

// --- adopting a configuration -----------------------------------------
//
// ONE DOOR for every change, whether it came from the file at startup or
// from the dialog: the palette, the font, the margin and the scrollback
// each feed into the grid geometry, so applying them one at a time from
// two places is how the two paths drift.
static void apply_conf(const struct term_conf *next) {
    int old_fg = VT_FG, old_bg = VT_BG;
    int scheme_moved = strcmp(next->scheme, g_conf.scheme) != 0 || !g_scheme.label[0];
    g_conf = *next;
    if (scheme_moved) {
        term_scheme_load(g_conf.scheme, &g_scheme);
        remap_default_pair(old_fg, old_bg);
    }
    font_sync();
    resize_scrollback(g_conf.scrollback);
    g_caret_on = 1;
    if (g_app) {
        // LAST, and through size_changed(): the grid may have gained or
        // lost rows, and the shell has to be told (SIGWINCH) after the
        // buffers are in their final shape, which is that function's own
        // rule rather than something to repeat here.
        size_changed(uapp_width(g_app), uapp_height(g_app));
        uapp_redraw(g_app);
    }
}

// The cadence exists ONLY for the caret. 500 ms is xterm's and KDE's
// blink interval; two wake-ups a second is the honest cost, against the
// 33 this window paid before its reader threads landed. With blinking
// off it returns 0 every time and nothing repaints.
// THE OPEN PANEL REPAINTS ON THE SAME TICK: its process states change
// with nothing to announce it. Closed, it costs nothing.
static int on_tick(struct uapp *a) {
    (void)a;
    unsigned long long now = sys_monotonic_ns();
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *hs = g_slot[i];
        if (hs && hs->live && !hs->spoke && now - hs->started_ns > 2000000000ull)
            release_held(hs);
    }
    int repaint = uui_toolbar_tick(&g_tb) | g_panel_open;
    if (!g_conf.cursor_blink) {
        if (g_caret_on) return repaint;
        g_caret_on = 1;
        return 1;
    }
    g_caret_on = !g_caret_on;
    return 1;
}

// --- input ------------------------------------------------------------

// THE WHEEL SCROLLS THE SCROLLBACK, and it is the same state Page
// Up/Down moves -- not a second notion of "where the reader is".
//
// Three lines per notch, which is what every desktop terminal does and
// what the toolkit's other scroll consumers use; a notch is a detent,
// not a line. `notches` is positive AWAY from the user (win_proto.h),
// and away means BACK INTO HISTORY.
#define WHEEL_LINES 3

static void scroll_to(struct uapp *a, int want);

static void on_wheel(struct uapp *a, int notches) {
    if (modal_up()) return;
    struct session *s = active();
    if (!s) return;
    scroll_to(a, s->sb_view + notches * WHEEL_LINES);
}

// Move the view and repaint only if it moved. Shared by the wheel, the
// keys and the scrollbar, so the three cannot disagree about where the
// ends are.
//
// CLAMPED AT BOTH ENDS, not wrapped: scrolling past the oldest line must
// stop there, and scrolling forward past the live screen must land
// exactly on it (0) rather than going negative, which would index above
// the top of the buffer.
static void scroll_to(struct uapp *a, int want) {
    struct session *s = active();
    if (!s) return;
    if (want > s->sb_count) want = s->sb_count;
    if (want < 0) want = 0;
    if (want == s->sb_view) return;
    s->sb_view = want;
    uapp_redraw(a);
}

// --- the pointer ------------------------------------------------------
//
// The cell under a pixel. Clamped rather than refused, so a drag that
// runs off the window keeps selecting to the edge -- the pointer grab
// (ui/uui_route.h) keeps delivering motion once the cursor has left.
// The column may come back as g_cols, which is the "past the end of the
// line" position a selection needs to include the break.
static struct selpoint point_at(struct session *s, int x, int y) {
    int cw = cell_w(), ch = cell_h();
    // ON A CURVED SCREEN THE CELL UNDER THE POINTER IS WHERE THE GLASS
    // SHOWS IT, not where it was drawn: ask the effect.
    if (g_effect && g_conf.crt.curve) {
        int sx, sy;
        ucrt_source_point(&g_crt, x, y - chrome_h(), &sx, &sy);
        x = sx;
        y = sy + chrome_h();
    }
    int r = (y - chrome_h() - g_margin) / ch;
    if (r < 0) r = 0;
    if (r >= g_rows) r = g_rows - 1;
    // Snapped to the nearest gap, not to the cell: clicking a glyph's
    // right half puts the caret after it, as uui_textbox_index_at_x()
    // does for a text field.
    int c = (x - g_margin + cw / 2) / cw;
    if (c < 0) c = 0;
    if (c > g_cols) c = g_cols;
    struct selpoint p = { virt_of_row(s, r), c };
    return p;
}

// What counts as one word for a double-click. Konsole's default set
// (`:word_characters`) is letters, digits and `_-.,/`; this is the
// conservative half of it, which is what makes double-clicking a path
// stop at a `/` rather than swallowing the line.
static int word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Grow a point out to its whole word. Returns 0 when there is no word
// under it, which leaves the caret where the click put it.
static int sel_word(struct session *s, struct selpoint p) {
    const struct cell *row = virt_row(s, p.line);
    if (!row) return 0;
    int c = p.col;
    if (c >= g_cols) c = g_cols - 1;
    if (c < 0 || !word_char(row[c].ch)) return 0;
    int a = c, b = c;
    while (a > 0 && word_char(row[a - 1].ch)) a--;
    while (b + 1 < g_cols && word_char(row[b + 1].ch)) b++;
    g_sel_a.line = g_sel_b.line = p.line;
    g_sel_a.col = a;
    g_sel_b.col = b + 1;
    g_sel_on = 1;
    return 1;
}

// The whole line, break included -- so a triple-click copy of two lines
// running together is impossible.
static void sel_line(struct selpoint p) {
    g_sel_a.line = p.line; g_sel_a.col = 0;
    g_sel_b.line = p.line; g_sel_b.col = g_cols;
    g_sel_on = 1;
}

// --- copying ----------------------------------------------------------

// The selected text, laid out the way a person would retype it: trailing
// blanks dropped from every line but the last, and a newline where the
// selection crosses a line break.
//
// TWO PASSES, and the first one is what makes a refusal possible: a copy
// that does not fit is refused rather than truncated (lib/uclip.h), and
// half a command line pasted into a shell is worse than none.
static int sel_measure(struct session *s, char *out, int cap) {
    struct selpoint from, to;
    if (!sel_range(&from, &to)) return 0;
    int n = 0;
    for (int line = from.line; line <= to.line; line++) {
        const struct cell *row = virt_row(s, line);
        int c0, c1;
        sel_cols(line, &c0, &c1);
        int end = c1;
        if (row && line < to.line)          // trailing blanks are padding,
            while (end > c0 && row[end - 1].ch == ' ') end--;   // not text
        for (int c = c0; row && c < end; c++) {
            if (out && n < cap) out[n] = row[c].ch;
            n++;
        }
        if (line < to.line) { if (out && n < cap) out[n] = '\n'; n++; }
    }
    if (out && n < cap) out[n] = '\0';
    return n;
}

// Puts the selection on the clipboard. SAYS SO EITHER WAY -- silence is
// the one outcome a Copy must never have (lib/uclip.h).
static void do_copy(void) {
    struct session *s = active();
    if (!s || !g_sel_on) return;
    int n = sel_measure(s, 0, 0);
    if (n <= 0) return;
    if (n > UCLIP_TEXT_MAX) {
        ulogf("uterm: selection is %d bytes, over the clipboard's %d -- not copied\n",
              n, UCLIP_TEXT_MAX);
        return;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { ulog("uterm: out of memory copying the selection\n"); return; }
    sel_measure(s, buf, n + 1);
    if (!uclip_set_text(buf, n))
        ulog("uterm: the clipboard refused the copy -- is clipboardd running?\n");
    free(buf);
}

// Every ring, not just the visible one: a background tab must keep up,
// and its bytes are already waiting whether or not anyone is looking.
static int drain_all(void) {
    int painted = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s) continue;
        int got = drain(s);
        struct session *cur = active();
        if (got && s == cur) painted = 1;
    }
    return painted;
}

// A tab whose shell has gone closes itself, exactly as the window used
// to. Checked after a drain so the shell's last output is on screen
// before its tab disappears.
static void reap_dead_tabs(void) {
    for (int i = 0; i < MAX_TABS; i++) {
        struct session *s = g_slot[i];
        if (!s || !s->live || !s->eof || s->exited) continue;
        // KEPT: reaped, and the tab stays with what the shell last said
        // -- GNOME Terminal's "hold the terminal open". The note is
        // written THROUGH THE PARSER, so it lands in the grid and the
        // scrollback like any other line.
        if (g_conf.keep_on_exit || s->is_cmd) {
            int status = 0;
            if (s->child > 0) sys_waitpid(s->child, &status);
            s->child = 0;
            s->exited = 1;
            char note[TITLE_MAX + 96];
            if (s->is_cmd) {
                snprintf(note, sizeof note,
                         "\r\n\x1b[0;2m[%s finished, exit status %d -- Enter runs it again, Ctrl+D closes]\x1b[0m\r\n",
                         s->title, status);
                ulogf("uterm: %s finished, status %d\n", s->shell, status);
            } else {
                snprintf(note, sizeof note,
                         "\r\n\x1b[0;2m[shell exited with status %d -- Enter starts a new one]\x1b[0m\r\n",
                         status);
            }
            vt_write(s, note, (int)strlen(note));
            continue;
        }
        session_stop(s);
        tabs_refresh();
    }
    if (g_ntabs == 0 && g_app) uapp_quit(g_app, 0);
}

// Ctrl+Shift+<letter>: the letter has ALREADY been folded to a control
// code by the time it arrives (api/keyboard.h says so plainly), so
// Ctrl+Shift+T is 0x14 with the SHIFT bit still set -- Shift is the one
// modifier that does not fold the key away. Matching the control code
// plus Shift is the only way to spell these here.
#define CTRL_SHIFT_T 0x14
#define CTRL_SHIFT_W 0x17
#define CTRL_SHIFT_V 0x16
#define CTRL_SHIFT_C 0x03
#define CTRL_SHIFT_F 0x06
#define CTRL_SHIFT_E 0x05

// One byte to the shell, the way a keystroke would arrive. THE MENU
// SENDS THE SAME BYTE THE KEY DOES rather than reaching for
// sys_kill(): Ctrl-C is interpreted by kernel/tty/ldisc.c, which is
// what makes it reach the foreground JOB rather than the shell.
// Every keystroke-shaped write to a shell goes through here, so type-ahead
// held before the shell first spoke keeps its order. Past the hold buffer
// a key is written at once: losing it would be worse than an early echo.
static void pty_send(struct session *s, const char *buf, int n) {
    if (!s || s->master < 0 || n <= 0) return;
    if (!s->spoke && s->nheld + n <= (int)sizeof s->held) {
        memcpy(s->held + s->nheld, buf, (size_t)n);
        s->nheld += n;
        return;
    }
    sys_write(s->master, buf, (size_t)n);
}

static void release_held(struct session *s) {
    s->spoke = 1;
    if (s->nheld > 0 && s->master >= 0) sys_write(s->master, s->held, (size_t)s->nheld);
    s->nheld = 0;
}

static void send_byte(char b) {
    pty_send(active(), &b, 1);
}

// PASTING INTO A TERMINAL IS TYPING. The characters go to the pty as
// bytes, so the shell's line editor sees them exactly as it would a
// very fast typist -- there is nothing terminal-specific to do, and
// nothing to interpret on this side.
//
// STATIC: struct uclip embeds the whole 64 KiB payload (lib/uclip.h).
static struct uclip g_clip;
static int g_clip_has_text;

static void clip_refresh(void) {
    uclip_load(&g_clip);
    g_clip_has_text = uclip_text(&g_clip, NULL) != 0;
}

static void do_paste(void) {
    struct session *s = active();
    if (!s || s->master < 0) return;
    clip_refresh();
    int n = 0;
    const char *txt = uclip_text(&g_clip, &n);
    if (!txt || n == 0) return;
    // ONE write, not one per character: a syscall per byte for a
    // pasted paragraph is thousands of kernel entries, and the pty
    // takes the run happily.
    pty_send(s, txt, n);
}

static unsigned menu_item_flags(int code) {
    switch (code) {
    case CMD_MENUBAR:   return g_menu_shown ? UUI_MI_CHECKED : 0;
    case CMD_PANEL:     return g_panel_open ? UUI_MI_CHECKED : 0;
    case CMD_EFFECT:    return g_effect ? UUI_MI_CHECKED : 0;
    case CMD_FIND:      return g_find_open ? UUI_MI_CHECKED : 0;
    case CMD_NEXT_TAB:
    case CMD_PREV_TAB:  return g_ntabs > 1 ? 0 : UUI_MI_DISABLED;
    case CMD_PASTE:     return g_clip_has_text ? 0 : UUI_MI_DISABLED;
    case CMD_COPY:      return g_sel_on ? 0 : UUI_MI_DISABLED;
    default:            return 0;
    }
}

static void step_tab(int delta) {
    if (g_ntabs <= 1) return;
    int next = g_strip.selected + delta;
    if (next < 0) next = g_ntabs - 1;          // wraps, as Konsole does
    if (next >= g_ntabs) next = 0;
    uui_tabs_select(&g_strip, next);
}

// Somebody replaced the clipboard. Paste greys and ungreys with it,
// which is the whole reason the event exists.
static void on_clipboard_cb(struct uapp *a, int op, unsigned serial) {
    (void)op; (void)serial;
    clip_refresh();
    uapp_redraw(a);
}

static void on_prefs_commit(const struct term_conf *next);

// The ▾ list: one row per installed shell, the configured one marked
// with the binding that opens it, then Options.
static void open_newtab_menu(void) {
    g_nshells = term_shells_list(g_shells, TERM_SHELLS_MAX);
    char sys[SETTING_ABI_VALUE_MAX];
    const char *def = g_conf.shell[0] ? g_conf.shell : shell_path(sys, sizeof sys);
    int n = 0;
    for (int i = 0; i < g_nshells; i++) {
        const char *base = strrchr(g_shells[i], '/');
        g_newtab_items[n++] = (struct uui_menu_item){
            .label = base ? base + 1 : g_shells[i],
            .accel = strcmp(g_shells[i], def) == 0 ? "Ctrl+Shift+T" : 0,
            .code = CMD_SHELL_BASE + i, .icon = "tb-new", .tint = UTHEME_ACT_CREATE };
    }
    g_newtab_items[n++] = (struct uui_menu_item)UUI_MENU_SEP;
    g_newtab_items[n++] = (struct uui_menu_item)UUI_MENU_ICON("Options...", CMD_PREFS, 0, "tb-gear", 0);
    int x, y, w, h;
    uui_toolbar_item_rect(&g_tb, TB_NEWTAB, &x, &y, &w, &h);
    uui_menubar_open_below(&g_ctx, g_newtab_items, n, x, y, w, h);
}

static void open_burger_menu(void) {
    int x, y, w, h;
    uui_toolbar_item_rect(&g_tb, TB_MENU, &x, &y, &w, &h);
    // Under the button, its left edge on the button's (KDE's ☰).
    uui_menubar_open_below(&g_ctx, burger_items,
                           (int)(sizeof burger_items / sizeof burger_items[0]), x, y, w, h);
}

// The bar's own commands. Returns 0 for any other code.
static int bar_command(struct uapp *a, int code) {
    if (code >= CMD_SHELL_BASE && code < CMD_SHELL_BASE + g_nshells) {
        open_tab_with(g_shells[code - CMD_SHELL_BASE]);
    } else if (code == CMD_FIND) {
        if (g_find_open) uui_findbar_activate(&g_find);
        else find_open();
    } else if (code == CMD_PANEL) {
        g_panel_open = !g_panel_open;
        size_changed(uapp_width(a), uapp_height(a));
    } else if (code == CMD_MENU_BUTTON) {
        open_burger_menu();
    } else if (code == CMD_NEWTAB_MENU) {
        open_newtab_menu();
    } else {
        return 0;
    }
    uapp_redraw(a);
    return 1;
}

static void do_command(struct uapp *a, int code) {
    struct session *s = active();
    if (bar_command(a, code)) return;
    switch (code) {
    case CMD_NEW_TAB:   open_tab(); break;
    case CMD_CLOSE_TAB: close_tab(g_strip.selected); break;
    case CMD_EXIT:      uapp_quit(a, 0); return;
    case CMD_RENAME:    rename_begin(); break;
    case CMD_CLEAR_SCREEN:
        // THE CURSOR'S LINE SURVIVES, moved to the top: it holds the
        // prompt and whatever is half-typed, and the shell will not
        // redraw them on its own -- Windows Terminal's Clear Buffer. Not
        // on the alternate screen, whose program owns every row.
        if (s && !s->alt) {
            struct cell *keep = malloc((size_t)g_cap_cols * sizeof *keep);
            if (keep) memcpy(keep, grid_row(s, s->cr), (size_t)g_cap_cols * sizeof *keep);
            int cc = s->cc;
            vt_reset_screen(s);
            s->cc = cc;
            if (keep) { memcpy(grid_row(s, 0), keep, (size_t)g_cap_cols * sizeof *keep); free(keep); }
            s->cr = 0;
            s->sb_view = 0;
        }
        break;
    case CMD_CLEAR_SB:
        if (s) { s->sb_count = 0; s->sb_view = 0; }
        break;
    case CMD_RESET:
        // WHAT `reset` DOES: the screen, the parser's colours and modes,
        // and the view. Not the scrollback -- `reset` on a real terminal
        // leaves history alone, which is why Clear Scrollback is its own
        // row rather than part of this one.
        if (s) {
            ansi_init(&s->vt, VT_FG, VT_BG);
            vt_reset_screen(s);
            s->alt = 0;
            s->cursor_shown = 1;
            s->sb_view = 0;
        }
        break;
    case CMD_PASTE:     do_paste(); break;
    case CMD_COPY:      do_copy();  break;
    case CMD_SELECT_ALL: {
        struct session *ses = active();
        if (!ses) break;
        // The whole virtual buffer -- scrollback and screen -- because
        // that is what the window is showing you a part of.
        g_sel_a.line = 0; g_sel_a.col = 0;
        g_sel_b.line = ses->sb_count + g_rows - 1; g_sel_b.col = g_cols;
        g_sel_on = 1;
        do_copy();
        break;
    }
    case CMD_INTR:      send_byte(0x03); break;
    case CMD_EOF:       send_byte(0x04); break;
    case CMD_TOP:       if (s) s->sb_view = s->sb_count; break;
    case CMD_BOTTOM:    if (s) s->sb_view = 0; break;
    case CMD_MENUBAR:   g_menu_shown = !g_menu_shown; size_changed(uapp_width(a), uapp_height(a)); break;
    case CMD_EFFECT:
        // Turned on with a look that does nothing (every slider at off),
        // it would look broken: the toggle then means the default look.
        if (!g_effect && !ucrt_look_on(&g_conf.crt)) g_conf.crt = ucrt_presets[UCRT_PRESET_SUBTLE];
        g_effect = !g_effect;
        ulogf("uterm: effect %s\n", g_effect ? "on" : "off");
        size_changed(uapp_width(a), uapp_height(a));
        break;
    case CMD_PREFS:
        uui_menubar_close(&g_menu);   // a modal owns the input
        uui_menubar_close(&g_ctx);
        term_prefs_open(a, &g_conf, on_prefs_commit);
        break;
    case CMD_NEXT_TAB:  step_tab(+1); break;
    case CMD_PREV_TAB:  step_tab(-1); break;
    default: return;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (modal_up()) return;   // ...and it must not reach the pty
    // --- the rename prompt owns every key while it is up --------------
    if (g_rename_open) {
        if (key == '\n' || key == '\r') { rename_commit(); tabs_refresh(); }
        else if (key == 0x1B)             { g_rename_open = 0; g_rename_slot = -1; }
        else if (!uui_textbox_key_mods(&g_rename, key, mods)) return;
        uapp_redraw(a);
        return;
    }

    // --- an open menu owns it next ------------------------------------
    struct uui_menubar *open_menu = uui_menubar_is_open(&g_ctx) ? &g_ctx
                                  : uui_menubar_is_open(&g_menu) ? &g_menu : 0;
    if (open_menu) {
        int code;
        if (uui_menubar_key(open_menu, key, &code)) {
            if (code >= 0) do_command(a, code);
            uapp_redraw(a);
            return;
        }
    }

    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_F) { do_command(a, CMD_FIND); return; }
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_E) { do_command(a, CMD_EFFECT); uapp_redraw(a); return; }
    if (key == KEY_F9) { do_command(a, CMD_PANEL); return; }

    // --- then an open find bar: typing goes to the query, not the shell
    if (g_find_open && uui_findbar_key(&g_find, key, mods)) {
        struct session *fs = active();
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_CHANGED: find_run(fs, 0); find_reveal(fs); break;
        case UUI_FIND_NEXT:    find_step(fs, +1); break;
        case UUI_FIND_PREV:    find_step(fs, -1); break;
        case UUI_FIND_CLOSE:   find_close(); break;
        default: break;
        }
        uapp_redraw(a);
        return;
    }

    // F10 REVEALS A HIDDEN BAR AS WELL AS OPENING IT, so hiding the menu
    // is never a one-way door. It is intercepted rather than sent to the
    // shell, which is the same trade every app with a menu bar makes.
    if (key == KEY_F10) {
        if (!g_menu_shown) { g_menu_shown = 1; size_changed(uapp_width(a), uapp_height(a)); }
        else { int c = -1; uui_menubar_key(&g_menu, key, &c); }
        uapp_redraw(a);
        return;
    }

    // --- the tab bindings, which are Konsole's ------------------------
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_T) {
        if (open_tab()) uapp_redraw(a);
        return;
    }
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_V) {
        do_paste();
        uapp_redraw(a);
        return;
    }
    // Ctrl+Shift+C, not Ctrl+C: 0x03 IS the INTR byte, which has to keep
    // reaching the shell. Konsole and GNOME Terminal make the same
    // choice for the same reason.
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_C) {
        do_copy();
        return;
    }
    if ((mods & KEY_MOD_SHIFT) && key == CTRL_SHIFT_W) {
        close_tab(g_strip.selected);
        uapp_redraw(a);
        return;
    }
    if ((mods & KEY_MOD_CTRL) && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
        step_tab(key == KEY_PAGE_DOWN ? +1 : -1);
        uapp_redraw(a);
        return;
    }

    struct session *s = active();
    if (!s) return;

    // A KEPT TAB WHOSE SHELL HAS EXITED has nobody to type at: Enter
    // starts a new shell in it, in the same place, and nothing else does
    // anything. Once its reader has let go of the slot.
    if (s->exited) {
        if (s->is_cmd && (key == 0x04 || key == 0x1B)) {   // Ctrl+D, Esc
            close_tab(g_strip.selected);
            uapp_redraw(a);
            return;
        }
        if ((key == '\n' || key == '\r') && s->done) {
            char shell[TERM_SHELL_MAX], dir[TITLE_MAX];
            strlcpy(shell, s->shell, sizeof shell);
            strlcpy(dir, s->cwd, sizeof dir);
            int was_cmd = s->is_cmd;
            if (s->master >= 0) { sys_close(s->master); s->master = -1; }
            if (!session_start(s->index, shell, dir[0] ? dir : 0)) ulog("uterm: could not restart the shell\n");
            else if (was_cmd) mark_cmd(s);
            tabs_refresh();
            uapp_redraw(a);
        }
        return;
    }

    // PAGE UP/DOWN SCROLL AND ARE NOT THE SHELL'S. Everything else --
    // including the arrows, Home, End and every Ctrl combination -- goes
    // to the shell as a byte, because THE SHELL HAS THE LINE EDITOR.
    // This window deciding what Ctrl-A means would be the second
    // implementation kernel/lib/klineedit.c exists to prevent.
    if (key == KEY_PAGE_UP) {
        s->sb_view += g_rows / 2;
        if (s->sb_view > s->sb_count) s->sb_view = s->sb_count;
        uapp_redraw(a);
        return;
    }
    if (key == KEY_PAGE_DOWN) {
        s->sb_view -= g_rows / 2;
        if (s->sb_view < 0) s->sb_view = 0;
        uapp_redraw(a);
        return;
    }

    // **A TERMINAL EMULATOR TRANSLATES A KEYSYM INTO A SEQUENCE, and
    // that is the whole job.** The toolkit delivers KEY_* -- a keysym,
    // exactly what an X11 or Wayland client receives -- and what goes on
    // the wire to the pty is ANSI: Up is `ESC [ A`. This is what Konsole
    // does, and it is why a ported program can read this terminal at
    // all; it used to write the raw 0x91, which only programs written
    // for this system understood.
    //
    // An ordinary character encodes to itself, so typing costs one byte
    // as it always did. A key with no terminal sequence (Super, a bare
    // modifier) encodes to nothing and is correctly not sent.
    //
    // Ctrl and Alt arrive as bits beside the key, so they are encoded
    // here, by the rule tty_input() applies to the console: Ctrl with a
    // character that is not a letter has no terminal code and is not
    // sent, and Alt is readline's meta prefix, ESC then the key. A KEY_*
    // special is never prefixed.
    if (IS_PRINTABLE_KEY(key) && (mods & KEY_MOD_CTRL)) return;
    if ((mods & KEY_MOD_ALT) && !IS_SPECIAL_KEY(key) &&
        s->master >= 0)
        pty_send(s, "\x1b", 1);
    char seq[TERMKEY_MAX];
    int n = termkey_encode(key, seq, sizeof seq);
    if (n > 0) pty_send(s, seq, n);

    // NO DRAIN HERE ANY MORE. The echo comes back through the discipline
    // and the reader thread is already blocked waiting for it, so it
    // arrives as a post within microseconds instead of on the next tick.
    // Draining here used to be what kept typing feeling immediate.
}

// A reader posted. `a0` is the session's slot and `a1` is 1 for EOF --
// neither is trusted for anything except knowing there is work: the
// drain reads every ring regardless, so a post that was dropped because
// the queue was full costs nothing.
static int on_user(struct uapp *a, int a0, int a1) {
    (void)a; (void)a0; (void)a1;
    int painted = drain_all();
    reap_dead_tabs();
    if (g_find_open && painted) find_run(active(), 1);
    return painted;
}

// --- lifecycle --------------------------------------------------------

static void tab_selected(void *ctx, int index) {
    (void)ctx; (void)index;
    // The selection names lines in the session it was made in, and every
    // session has its own scrollback -- so carrying it across a switch
    // would highlight whatever happened to be at those line numbers.
    sel_clear();
    if (g_find_open) find_run(active(), 0);
    if (g_app) uapp_redraw(g_app);
}

static void tab_closed(void *ctx, int index) {
    (void)ctx;
    close_tab(index);
    if (g_app) uapp_redraw(g_app);
}

static void tab_new(void *ctx) {
    (void)ctx;
    open_tab();
    if (g_app) uapp_redraw(g_app);
}

// The router names a widget to the app; the menu bar's commit is parked
// in the widget and taken here (ui/uui_menubar.h). Only the menu reports
// this way -- the strip's own callbacks say what happened directly.
// The Session panel's buttons, whose codes are TERM_PANEL_*.
static void on_action(struct uapp *a, int code) {
    struct session *ps = active();
    if (!ps || ps->exited) return;
    switch (code) {
    case TERM_PANEL_INTR: send_byte(0x03); break;
    case TERM_PANEL_EOF:  send_byte(0x04); break;
    // SIGKILL, the Task Manager's Force Quit: the shell may be
    // ignoring everything politer, which is why the button exists.
    case TERM_PANEL_KILL: if (ps->child > 0) sys_kill(ps->child, SIGKILL); break;
    }
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    if (id == ID_MENU || id == ID_CTX) {
        int code = uui_menubar_take_code(id == ID_MENU ? &g_menu : &g_ctx);
        if (code >= 0) do_command(a, code);
        return;
    }
    if (id == ID_TB) {
        int code = uui_toolbar_take_code(&g_tb);
        if (code >= 0) do_command(a, code);
        return;
    }
    if (id == ID_FIND) {
        struct session *fs = active();
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_NEXT:  find_step(fs, +1); break;
        case UUI_FIND_PREV:  find_step(fs, -1); break;
        case UUI_FIND_CLOSE: find_close(); break;
        default: break;
        }
        uapp_redraw(a);
        return;
    }
    if (id == ID_QUIT_ASK) {
        // Straight to uapp_quit(): on_close_cb() would ask again, and
        // the dialog it would put up is the one that just answered.
        if (uui_dialog_take_code(&g_quit_ask) == QUIT_YES) {
            for (int i = 0; i < MAX_TABS; i++) session_stop(g_slot[i]);
            uapp_quit(a, 0);
        }
        return;
    }
}

// OK in the Options window.
static void on_prefs_commit(const struct term_conf *next) {
    // SAVED BEFORE APPLIED, and the old copy is what says which keys
    // moved -- term_conf_save() writes only those, because uconf_set()
    // rewrites the whole document per call.
    struct term_conf old = g_conf;
    if (term_conf_save(next, &old) < 0)
        ulog("uterm: /etc/terminal.conf could not be written; the change is this session only\n");
    // The dialog's "on" is what new windows do; it reaches this one when
    // it CHANGED, so a window toggled from the menu keeps its state
    // through an unrelated OK.
    if (next->effect != old.effect) g_effect = next->effect;
    else if (memcmp(&next->crt, &old.crt, sizeof next->crt) != 0 && ucrt_look_on(&next->crt))
        g_effect = 1;   // a look just chosen is a look to see
    apply_conf(next);
}

// A press the router did not consume. Its only job is the rename prompt:
// clicking away cancels, and clicking IN it places the caret.
// How close in time and space two presses must be to count as a
// double-click. 400 ms is Windows' default and within a pixel of KDE's;
// the distance gate is what stops two deliberate clicks in different
// places being read as one gesture.
#define MULTICLICK_MS 400
#define MULTICLICK_PX 4

static unsigned long long g_last_click_ns;
static int g_last_click_x, g_last_click_y;
static int g_click_run;    // 1 = single, 2 = double, 3 = triple

static int click_run(int x, int y) {
    unsigned long long now = sys_monotonic_ns();
    int dx = x - g_last_click_x, dy = y - g_last_click_y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int near = dx <= MULTICLICK_PX && dy <= MULTICLICK_PX;
    // The FIRST click has no predecessor, and an unsigned subtraction
    // from a zero g_last_click_ns would be an enormous interval rather
    // than a negative one -- which is the answer this wants anyway.
    int soon = g_last_click_ns &&
               now - g_last_click_ns < (unsigned long long)MULTICLICK_MS * 1000000ull;
    g_click_run = (near && soon) ? g_click_run + 1 : 1;
    if (g_click_run > 3) g_click_run = 1;   // a fourth click starts over
    g_last_click_ns = now;
    g_last_click_x = x;
    g_last_click_y = y;
    return g_click_run;
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (modal_up()) return;
    if (g_rename_open) {
        int fx, fy, fw, fh;
        rename_rect(uapp_width(a), uapp_height(a), &fx, &fy, &fw, &fh);
        uui_textbox_set_geometry(&g_rename, fx, fy, fw, fh);
        if (uui_textbox_hit(&g_rename, x, y))
            g_rename.ed.cursor = uui_textbox_index_at_x(&g_rename, x);
        else
            { g_rename_open = 0; g_rename_slot = -1; }
        uapp_redraw(a);
        return;
    }

    // The PRIMARY button only. A secondary click inside the content area
    // is the client's (docs/conventions/gui.md) and this app has no
    // context menu yet -- it must not clear a selection somebody is
    // about to reach for.
    if (!(WIN_MOUSE_BUTTONS(buttons) & 0x1)) return;

    struct session *ses = active();
    if (!ses) return;
    if (y < chrome_h()) return;   // the strips route themselves
    if (x >= grid_right(uapp_width(a))) return;   // the panel's buttons route themselves

    int bx, by, bw, bh;
    bar_rect(uapp_width(a), uapp_height(a), &bx, &by, &bw, &bh);
    if (bw > 0 && ses->sb_count > 0 && x >= bx && x < bx + bw && y >= by) {
        sb_hover(1);
        int total = ses->sb_count + g_rows;
        switch (uui_scrollbar_hit(bx, by, bw, bh, total, g_rows,
                                   ses->sb_view, x, y, 0)) {
        case UUI_SB_THUMB: {
            // WHERE IN THE THUMB the press landed, subtracted on every
            // motion -- point 1 of the scrollbar spec. Passing 0 makes
            // the thumb leap so its top sits under the cursor.
            int ty, th;
            uui_scrollbar_thumb_rect(by, bh, total, g_rows, ses->sb_view,
                                      &ty, &th, bw, 0);
            g_bar_grab = y - ty;
            break;
        }
        // The trough PAGES; it does not jump to the clicked position.
        // Above the thumb is further back in history, which is up.
        case UUI_SB_ABOVE: scroll_to(a, ses->sb_view + g_rows - 1); break;
        case UUI_SB_BELOW: scroll_to(a, ses->sb_view - g_rows + 1); break;
        default: break;
        }
        return;
    }

    struct selpoint p = point_at(ses, x, y);
    switch (click_run(x, y)) {
    case 2:
        if (!sel_word(ses, p)) sel_clear();
        break;
    case 3:
        sel_line(p);
        break;
    default:
        // Shift EXTENDS an existing selection instead of starting one,
        // which is what every terminal and every list here does.
        if ((WIN_MOUSE_MODS(buttons) & KEY_MOD_SHIFT) && g_sel_on) {
            g_sel_b = p;
        } else {
            g_sel_a = g_sel_b = p;
            g_sel_on = 1;   // armed but empty until the drag moves
        }
        g_selecting = 1;
        break;
    }
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (modal_up()) return;
    struct session *ses = active();
    if (!ses) return;

    // THE I-BEAM OVER THE GRID, named on every motion: uapp sets the
    // widget tree's answer first (the arrow, for the menu bar and the
    // tab strip) and that overwrote the one on_open named. Not while a
    // menu is down -- its rows lie over the grid.
    int sbx, sby, sbw, sbh;
    bar_rect(uapp_width(a), uapp_height(a), &sbx, &sby, &sbw, &sbh);
    int over_bar = sbw > 0 && ses->sb_count > 0 && x >= sbx && x < sbx + sbw
                   && y >= sby && y < sby + sbh;
    if (over_bar != g_sb_want || (over_bar && g_sb_collapse)) {
        if (over_bar || g_bar_grab < 0) sb_hover(over_bar);
    }
    // Not over the floating find bar either: it sits on the grid and
    // names its own cursor (the I-beam over its field only).
    int over_find = g_find_open && uui_hit(g_find.x, g_find.y, g_find.w, g_find.h, x, y);
    if (y >= chrome_h() && x >= 0 && x < sbx && !uui_menubar_is_open(&g_menu)
            && !uui_menubar_is_open(&g_ctx) && !over_find)
        uapp_set_cursor(a, WIN_CURSOR_TEXT);

    if (g_bar_grab >= 0) {
        int bx, by, bw, bh;
        bar_rect(uapp_width(a), uapp_height(a), &bx, &by, &bw, &bh);
        scroll_to(a, uui_scrollbar_offset_for_drag(by, bh,
                        ses->sb_count + g_rows, g_rows, y, g_bar_grab, bw, 0));
        return;
    }

    if (!g_selecting) return;
    // **A MOTION WITH NO BUTTON HELD IS IGNORED, NOT TREATED AS A
    // RELEASE**, and the difference is the whole of why drag-select did
    // not work at first. `docs/conventions/gui.md` says to test the
    // buttons mask, and it is right that a button-up motion must not
    // move anything -- but ENDING the drag on one is wrong here,
    // because the compositor sends exactly such a motion immediately
    // after every press: a leave at (-1,-1) telling the window it is no
    // longer hovered (wm_input.c's wm_update_content_hover, where hover
    // is suppressed the moment something is pressed). Ending on it
    // discarded every real drag motion that followed. on_release is
    // what ends a selection.
    if (!(WIN_MOUSE_BUTTONS(buttons) & 0x1)) return;

    // Dragging past an edge scrolls, one line per motion event. A real
    // terminal autoscrolls on a timer while the pointer is held still at
    // the edge; this window deliberately has no tick (see uapp_desc), so
    // it scrolls while the pointer keeps moving instead.
    if (y < chrome_h() + g_margin)                 scroll_to(a, ses->sb_view + 1);
    else if (y >= chrome_h() + g_margin + g_rows * cell_h())
                                                 scroll_to(a, ses->sb_view - 1);

    g_sel_b = point_at(ses, x, y);
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    if (modal_up()) return;
    (void)buttons;
    if (g_bar_grab >= 0) {
        g_bar_grab = -1;
        int bx, by, bw, bh;
        bar_rect(uapp_width(a), uapp_height(a), &bx, &by, &bw, &bh);
        if (!(x >= bx && x < bx + bw && y >= by && y < by + bh)) sb_hover(0);
    }
    if (!g_selecting) return;
    g_selecting = 0;
    // **COPY ON SELECT**, which X11 calls the PRIMARY selection and
    // Konsole offers as an option. There is one clipboard here, so this
    // does overwrite whatever was last copied -- deliberate, and the
    // reason it is worth having anyway is that a terminal selection is
    // made in order to be pasted, essentially always. A SETTING because
    // the overwrite is the half somebody may not want.
    if (g_conf.copy_on_select) do_copy();
    uapp_redraw(a);
}

static void on_open_cb(struct uapp *a) {
    // The grid is text. Named here for a window that opens under a still
    // pointer; on_motion keeps it named, since the widget tree answers
    // the arrow for the chrome on every move.
    uapp_set_cursor(a, WIN_CURSOR_TEXT);
    g_app = a;

    // The rest of the configuration -- main() already read it, so that
    // default_size() could size the window in the RIGHT cell. This is
    // the half that needs a window to exist.
    apply_conf(&g_conf);
    g_menu_shown = g_conf.menubar;
    uui_dialog_init(&g_quit_ask);
    uui_menubar_init(&g_ctx, 0, 0);
    g_ctx.item_flags = menu_item_flags;
    uui_findbar_init(&g_find);
    g_find.pill = 1;
    term_panel_init();
    for (int i = 0; i < TERM_PANEL_BUTTONS; i++) g_widgets[W_PANEL0 + i] = *term_panel_item(i);

    uui_tabs_init(&g_strip, g_tablabels, 0, 0);
    g_strip.on_select = tab_selected;
    g_strip.on_close  = tab_closed;
    g_strip.on_new    = tab_new;
    g_strip.show_new  = 1;
    g_strip.floating  = 1;

    clip_refresh();   // the broadcast only fires on a CHANGE, so ask once
    uui_menubar_init(&g_menu, menu_bar,
                      (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;

    if (!(g_cmd[0] ? open_cmd_tab() : open_tab())) {
        ulog("uterm: could not open a pty or start the shell\n");
        uapp_quit(a, 1);
    }
}

static int on_close_cb(struct uapp *a) {
    static const struct uui_dialog_button btns[] = {
        { "Close", QUIT_YES, 0 }, { "Cancel", QUIT_NO, 0 },
    };
    static const char *rows[1];
    static char line[64];

    if (uapp_question_open(a)) return 0;   // "Close them all?" -- or another -- decides
    if (g_conf.confirm_close && g_ntabs > 1) {
        snprintf(line, sizeof line, "%d tabs are open. Close them all?", g_ntabs);
        rows[0] = line;
        uui_menubar_close(&g_menu);
        uui_dialog_set_bounds(&g_quit_ask, 0, 0, uapp_width(a), uapp_height(a));
        uui_dialog_open(&g_quit_ask, "Close Terminal", rows, 1, btns, 2, 1, QUIT_NO);
        uapp_redraw(a);
        return 0;   // not yet -- the dialog answers
    }
    for (int i = 0; i < MAX_TABS; i++) session_stop(g_slot[i]);
    return 1;
}


// The exact inverse of size_changed(), which is what keeps the two
// honest: it derives the grid from the window, this derives the window
// from the grid, and both read the same chrome and margin.
//
// It runs BEFORE on_open (userland/ui/uapp.c), so nothing here may touch
// g_menu -- chrome_h() is safe only because both halves of it are pure
// font arithmetic.
static void default_size(int *w, int *h) {
    *w = g_conf.cols * cell_w() + 2 * g_margin + panel_w();
    *h = g_conf.rows * cell_h() + chrome_h() + 2 * g_margin;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "-e") == 0) strlcpy(g_cmd, argv[2], sizeof g_cmd);
    // **BEFORE uapp_run(), because default_size() runs before on_open**
    // and derives the window from cell_w()/cell_h() -- which are this
    // terminal's font, not the desktop's. Read here and the window opens
    // at 120x30 of the configured cell; read in on_open and it opens at
    // the desktop's size and then loses rows to the bigger font.
    term_conf_load(&g_conf);
    term_scheme_load(g_conf.scheme, &g_scheme);
    // Before uapp_run() too: default_size() measures the bar and the
    // panel, and runs before on_open.
    g_panel_open = g_conf.panel;
    ucrt_init(&g_crt);
    g_effect = g_conf.effect;
    uui_toolbar_init(&g_tb, tb_items, TB_COUNT);
    g_tb.item_flags = menu_item_flags;
    g_tb.accent_latch = 1;   // the open panel reads as a latched toggle
    g_tb.bg = UTHEME_WINDOW_BG;
    g_tb.border = UTHEME_BORDER;   // continues the strip's baseline

    struct uapp_desc desc = {
        .title   = "Terminal",
        .app_id  = "terminal",
        .on_size = default_size,
        .x       = 120,
        .y       = 120,
        .flags   = UAPP_RESIZABLE,
        .min_w   = 280,
        .min_h   = 140,
        // **THE CADENCE IS THE CARET'S AND NOTHING ELSE'S.** This
        // window used to poll its one master every 30 ms because there
        // is no poll() to wait on two things at once; a reader thread
        // per session blocks on the pty instead and posts when it has
        // bytes. What is left is a 500 ms blink (on_tick), which costs
        // two wake-ups a second and returns 0 -- repainting nothing --
        // when `cursor_blink` is off.
        .tick_ms = 500,
        .on_tick = on_tick,
        .widgets = g_widgets,
        .widget_count = W_COUNT,
        .on_open = on_open_cb,
        .on_draw = on_draw,
        .on_draw_over = on_draw_over,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_press = on_press,
        .on_motion = on_motion,
        .on_release = on_release,
        .on_key  = on_key,
        .on_clipboard = on_clipboard_cb,
        .on_user = on_user,
        .on_wheel = on_wheel,
        .on_resize = on_resize,
        .on_close = on_close_cb,
    };
    return uapp_run(&desc);
}
