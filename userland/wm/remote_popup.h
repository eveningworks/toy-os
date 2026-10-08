#ifndef WM_REMOTE_POPUP_H
#define WM_REMOTE_POPUP_H

// THE TRAY'S REMOTE-ACTIVITY ITEM: an indicator while somebody is
// reaching this machine over the network, and a flyout listing what
// they have done.
//
// **THE INDICATOR IS THE POINT, AND IT IS NOT DECORATION.** KDE's krfb
// raises a tray icon for as long as a remote party is connected, and
// every Wayland screen-share portal does the same, because the one
// thing the machine's owner must never have to guess is whether
// somebody else is acting on it. `auto` therefore means "while a
// session is open" -- the hardware question for this item.
//
// The records come from QUERY_REMOTELOG and the live sessions from
// QUERY_REMOTESESS, both the kernel's (kernel/include/kernel/remote_log.h).
// The flyout can END a session and make a remote desktop VIEW ONLY
// (lib/uremote.h's signals) -- the two things the person at the screen
// must always be able to do -- but cannot start or hide one. While a
// remote DESKTOP session is open the icon is lit (tray_set_accent()).

// How many records the flyout lists. The ring holds far more; this is
// what fits beside a taskbar without becoming a window.
#define REMOTE_ROWS 10

struct remote_geom {
    int x, y, w, h;                  // the flyout
    int tray_x, tray_y, tray_w, tray_h;
    int pad, row_h;
    int list_y, list_h;              // the live sessions' rows
    int log_y;                       // the first record row
    int link_y;                      // the settings link
    int log_rows;                    // how many records are shown
};

extern int remote_open;

void remote_tray_init(void);
void remote_poll(void);
void remote_geometry(struct remote_geom *g);
void remote_draw(int mx, int my);
int  remote_handle_click(int mx, int my);
int  remote_hover_at(int mx, int my);
void remote_damage(void);
// Where it is, for wm_overlay.h's automatic damage. 0 when it has
// no rect to report.
int remote_rect(int *x, int *y, int *w, int *h);
void remote_close(void);
int  remote_tray_hidden(void);

// For `gui remote --json` (wm_debug.c), which is how a test reads this
// without interpreting pixels.
int remote_session_count(void);
void remote_peer(char *out, unsigned cap);
int remote_row_count(void);
void remote_row_text(int i, char *out, unsigned cap);
// The live sessions (`gui remote --json`), and a button's rect, so a test
// presses Disconnect by asking where it is. 0 when there is no such button.
int remote_live_count(void);
int remote_desktop_live(void);   // the icon is lit
int remote_live_button(int row, int btn, int r[4]);

#endif // WM_REMOTE_POPUP_H
