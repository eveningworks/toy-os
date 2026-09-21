#ifndef CALENDAR_POPUP_H
#define CALENDAR_POPUP_H

// The taskbar's calendar: a month grid in a panel anchored above the
// clock, opened by clicking the clock and dismissed by clicking
// anywhere outside it. Same peer-file pattern as start_menu.h and
// context_menu.h -- state, drawing and hit-testing for one popup,
// sharing the window manager's globals through wm_internal.h rather
// than being an independently-reasoned module.
//
// WHY THE PANEL OWNS IT RATHER THAN AN APP. Windows 11, GNOME Shell,
// KDE Plasma's digital-clock applet and XFCE's clock plugin all put the
// calendar in a popup the PANEL draws, anchored to the clock -- none of
// them open an application window for it. A glance at the date should
// not cost a process spawn, a title bar and a taskbar button. A
// standalone Calendar app is still a reasonable thing to want later
// (events, a month-at-a-time view); it is not what clicking the clock
// should do.
//
// VIEW-ONLY, WITH NAVIGATION. `<` and `>` page the viewed month and the
// title snaps back to today, which is Plasma's and XFCE's behaviour. A
// day cell is not clickable: there is nothing to select it FOR until
// something stores events, and a highlight that does nothing reads as a
// broken control.
//
// The week's first column comes from `desktop.week_start`
// (kernel/lib/week_start_config.c), polled the same way the taskbar
// polls `desktop.start_button`.

// Whether the popup is currently open -- read by wm_render.c (draw or
// not, and to force the full-screen overlay repaint) and wm_input.c.
extern int calendar_open;

// Opens the popup at TODAY's month, closing the Start and context menus
// -- the three are mutually exclusive, as on every desktop. Called from
// wm_input.c when the click lands on the tray clock.
void calendar_open_now(void);

// Closes it with no action. Safe to call when it is already closed.
void calendar_close(void);

// Re-reads `desktop.week_start` if anything on the filesystem has
// changed -- called once per frame from wm.c beside taskbar_poll_config()
// and for the same reason: there is no inotify here, so a generation
// counter is what says "ask again". The idle cost is one compare.
void calendar_poll_config(void);

// Draws the panel, using the live cursor for the `<`/`>` hover -- a
// no-op when closed, same "caller may still check, this just draws"
// contract as start_menu_draw()/context_menu_draw().
// The overlay registry's hover and damage ops -- see wm_overlay.h.
// These are what retired this popup's full-screen repaint per mouse
// move: 1 = `<`, 2 = `>`, 3 = the title, 0 = none.
int calendar_hover_at(int mx, int my);
void calendar_damage(void);
// Where it is, for wm_overlay.h's automatic damage. 0 when it has
// no rect to report.
int calendar_rect(int *x, int *y, int *w, int *h);

void calendar_draw(int mx, int my);

// Handles a left click at (mx, my) while the popup is open: pages the
// month, snaps back to today, or closes when the click lands outside.
// Returns 1 when wm_input.c should stop there, 0 when it should carry
// on with its ordinary handling -- which is not simply "was it open".
// A dismissing click on the TASKBAR falls through, so the Start button
// acts on the same click that closed the popup (see the .c file for the
// three cases); a dismissing click anywhere else is swallowed, as it is
// for any open menu.
int calendar_handle_click(int mx, int my);

// The popup's live geometry, for the debug console (`gui calendar`) and
// therefore for tests -- the SAME numbers drawing and hit-testing use,
// so a test cannot be told a different position from the one a click
// would land on (the lesson `gui taskbar` was rewritten for). Valid
// whether or not the popup is open, which is what lets a test read the
// clock's own rect and the panel's expected position before opening it.
struct calendar_geom {
    int x, y, w, h;            // the panel
    int header_h;              // the title/arrow row, measured from y
    int prev_x, prev_y, prev_w, prev_h;  // the `<` button
    int next_x, next_y, next_w, next_h;  // the `>` button
    int title_x, title_y, title_w, title_h; // the month/year label -- click = today
    int grid_x, grid_y;        // top-left of the WEEKDAY header row
    int cell_w, cell_h;        // one grid cell; the day rows start at grid_y + cell_h
    int view_year, view_month; // what is on screen (month 1-12), not today
    int first_col;             // which column the 1st of the viewed month lands in
    int days;                  // days in the viewed month
    int today_col, today_row;  // today's cell, or -1/-1 when a different month is shown
    int week_start_monday;     // 1 = Monday first (the default), 0 = Sunday first
};
void calendar_geometry(struct calendar_geom *out);

// Today's date, as the popup sees it (local time, per the `timezone`
// setting). Reported by the debug console so a test can compute which
// cell must be highlighted without a second clock of its own.
void calendar_today(int *out_year, int *out_month, int *out_day);

// The viewed month's name, for the debug console -- "August", never a
// number, because that is what the header actually draws.
const char *calendar_month_name(int month);

#endif
