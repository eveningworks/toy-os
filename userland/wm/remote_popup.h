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
// It READS ONLY. The records come from QUERY_REMOTELOG, which the
// kernel fills (see kernel/include/kernel/remote_log.h); nothing here
// can start, stop or hide a session.

// How many records the flyout lists. The ring holds far more; this is
// what fits beside a taskbar without becoming a window.
#define REMOTE_ROWS 10

struct remote_geom {
    int x, y, w, h;                  // the flyout
    int tray_x, tray_y, tray_w, tray_h;
    int pad, row_h;
};

extern int remote_open;

void remote_tray_init(void);
void remote_poll(void);
void remote_geometry(struct remote_geom *g);
void remote_draw(int mx, int my);
int  remote_handle_click(int mx, int my);
int  remote_hover_at(int mx, int my);
void remote_damage(void);
void remote_close(void);
int  remote_tray_hidden(void);

// For `gui remote --json` (wm_debug.c), which is how a test reads this
// without interpreting pixels.
int remote_session_count(void);
void remote_peer(char *out, unsigned cap);
int remote_row_count(void);
void remote_row_text(int i, char *out, unsigned cap);

#endif // WM_REMOTE_POPUP_H
