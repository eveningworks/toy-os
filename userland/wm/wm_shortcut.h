#ifndef WM_SHORTCUT_H
#define WM_SHORTCUT_H

// Global keyboard shortcuts. See wm_shortcut.c for why the compositor
// is the only thing that matches them.

// The compositor's own bound on the action table, which the kernel's
// side (api/shortcuts_config.h) may grow without this file changing --
// anything past it is simply not bound, which is a visible shortfall
// rather than memory corruption.
#define SHORTCUT_ACTION_MAX 16

// Re-read every binding from the settings registry. Called at start-up
// and whenever the filesystem generation moves.
void wm_shortcut_reload(void);

// Has anything changed on disk? One integer compare per frame; reloads
// when it has. Called once per compositor frame.
void wm_shortcut_poll(void);

// The command bound to this key, or NULL. Exposed for the debug console
// so a test can ask what WOULD fire without launching anything.
const char *wm_shortcut_match(int key, unsigned mods);

// Match and launch. 1 if the key was a shortcut and was consumed -- the
// caller must NOT then route it to a window.
int wm_shortcut_fire(int key, unsigned mods);

// A client asking to receive shortcut keys itself (WIN_REQ_INHIBIT_
// SHORTCUTS -- abi/win_proto.h says why one would). `window` is the
// slot; `on` arms or releases.
void wm_shortcut_inhibit(int window, int on);

// Is the shortcut matcher currently held off for the FOCUSED window?
// The compositor asks before matching.
int wm_shortcut_inhibited(int focused_window);

// The focus moved, or a window went away: drop an inhibitor that is no
// longer the focused window's. Called from the compositor, because an
// inhibitor that outlived its window would leave the desktop with no
// shortcuts and no way to get them back.
void wm_shortcut_focus_changed(int focused_window);

#endif
