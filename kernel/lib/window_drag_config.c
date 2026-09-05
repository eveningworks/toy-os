// What a drag SHOWS: the window, or an outline. Two registry
// descriptors and nothing else.
//
// PERSIST-ONLY, like the wallpaper and the week's first day beside them
// in /etc/desktop.conf: the dragging is done by a RING-3 process, and
// `setting_register()` takes function pointers a process cannot supply.
// The registry validates the value and writes the file; the window
// manager reads it when a drag begins (userland/wm/wm_input.c), which
// is why neither needs a poll.
//
// WHY THE SETTING EXISTS AT ALL. Windows has had exactly this switch
// since XP (`SPI_SETDRAGFULLWINDOWS`, "Show window contents while
// dragging") and KDE DELETED its version in Plasma 5, on the grounds
// that a compositor makes live resize always affordable. KWin is right
// about KWin: its clients redraw in milliseconds. It is not right here,
// because a resize is a round trip to a client that may be rescaling a
// JPEG or drawing a Doom frame, and a window that lags the pointer by a
// tenth of a second is worse than an outline that does not.
//
// WHICH IS WHY `auto` IS THE DEFAULT AND THE THIRD CHOICE. Neither
// system offers it: both make the user decide in advance, for every
// app at once. The window manager already times each resize proposal
// against its acknowledgement, so it can simply watch -- live until the
// client falls behind, an outline for the rest of that drag.
//
// MOVING HAS NO `auto`, and that asymmetry is deliberate: the WM owns
// a window's position and moves it with no client involved, so a move
// cannot fall behind and a third choice would be one that never
// happens. An outline move is offered anyway because some people want
// it, not because anything here is slow.
#include "setting.h"
#include "etc_config.h"
#include "string.h"
#include "window_drag_config.h"

#define DESKTOP_CONFIG_FILE "/etc/desktop.conf"

static const char *const g_resize_modes[] = { "auto", "live", "outline" };
static const char *const g_move_modes[]   = { "live", "outline" };

static int resize_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_resize_modes / sizeof g_resize_modes[0]))
        return 0;
    k_strlcpy(out, g_resize_modes[index], out_size);
    return 1;
}

static int move_choice(int index, char *out, uint32_t out_size) {
    if (index < 0 || index >= (int)(sizeof g_move_modes / sizeof g_move_modes[0]))
        return 0;
    k_strlcpy(out, g_move_modes[index], out_size);
    return 1;
}

// Reads the FILE, not what the compositor last adopted -- the note
// wallpaper_config.c's and week_start_config.c's getters carry, and for
// the same reason: the two differ only between a write and the next
// drag, and the file is what the registry means by "what is this set
// to".
static void resize_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "resize_mode", out, out_size))
        k_strlcpy(out, g_resize_modes[0], out_size);
}

static void move_get(char *out, uint32_t out_size) {
    if (!etc_config_get(DESKTOP_CONFIG_FILE, "move_mode", out, out_size))
        k_strlcpy(out, g_move_modes[0], out_size);
}

static const struct setting g_resize_setting = {
    .name  = "resize_mode",
    .label = "While resizing",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Windows",
    .choice = resize_choice,
    .get    = resize_get,
    .apply  = 0, // persist-only -- the next drag reads it
};

static const struct setting g_move_setting = {
    .name  = "move_mode",
    .label = "While moving",
    .type  = SETTING_TYPE_ENUM,
    .file  = DESKTOP_CONFIG_FILE,
    .category = "Appearance",
    .group    = "Windows",
    .choice = move_choice,
    .get    = move_get,
    .apply  = 0,
};

void window_drag_setting_register(void) {
    setting_register(&g_resize_setting);
    setting_register(&g_move_setting);
}
