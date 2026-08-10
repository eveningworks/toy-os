#include "gui_apps.h"
#include "notepad.h"
#include "about.h"
#include "calculator.h"
#include "terminal.h"

// To add a new windowed app: write apps/foo.c + apps/foo.h implementing
// the gui_app callbacks (see apps/notepad.c for the simplest example),
// then add one line here. That's the whole integration -- it'll show up
// in the Start menu automatically.
//
// Window sizes are computed by each app's own default_size() from
// whatever font is active when the window opens (see gui_apps.h's
// comment on default_size) -- not fixed pixel constants, so a window is
// always right-sized for its font instead of cramped at large fonts or
// full of wasted space at small ones.
//
// `resizable` (see gui_apps.h) is 1 for everything except Calculator --
// its button grid has no sensible way to fill extra window space, so
// it's fixed at whatever default_size() computed for the active font.
//
// Designated initializers (rather than positional, which this table
// used until on_drag_start/on_drag were added to gui_apps.h) so adding
// another optional callback in the future doesn't require touching
// every existing entry's field order again -- only apps that actually
// use a given callback need to mention it.
const struct gui_app gui_app_registry[] = {
    { .name = "Notepad", .default_size = notepad_default_size, .on_open = notepad_open,
      .on_draw = notepad_draw, .on_key = notepad_key, .on_click = notepad_click, .resizable = 1 },
    { .name = "About", .default_size = about_default_size, .on_open = about_open,
      .on_draw = about_draw, .resizable = 1 },
    { .name = "Calculator", .default_size = calculator_default_size, .on_open = calculator_open,
      .on_draw = calculator_draw, .on_key = calculator_key, .on_click = calculator_click, .resizable = 0 },
    { .name = "Terminal", .default_size = terminal_default_size, .on_open = terminal_open,
      .on_draw = terminal_draw, .on_key = terminal_key, .on_click = terminal_click,
      .on_drag_start = terminal_drag_start, .on_drag = terminal_drag,
      .on_wheel = terminal_wheel, .resizable = 1 },
};
const int gui_app_registry_count = sizeof(gui_app_registry) / sizeof(gui_app_registry[0]);
