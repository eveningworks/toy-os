#include "gui_apps.h"
#include "notepad.h"
#include "about.h"
#include "calculator.h"

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
const struct gui_app gui_app_registry[] = {
    { "Notepad",    notepad_default_size,    notepad_open,    notepad_draw,    notepad_key,    notepad_click,    1 },
    { "About",      about_default_size,      about_open,      about_draw,      0,              0,                1 },
    { "Calculator", calculator_default_size, calculator_open, calculator_draw, calculator_key, calculator_click, 0 },
};
const int gui_app_registry_count = sizeof(gui_app_registry) / sizeof(gui_app_registry[0]);
