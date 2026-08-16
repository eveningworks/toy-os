#include "gui_apps.h"
#include "taskmgr.h"
#include "control_panel.h"

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
    { .name = "Task Manager", .default_size = taskmgr_default_size, .on_open = taskmgr_open,
      .on_draw = taskmgr_draw, .resizable = 1 },
    { .name = "Control Panel", .default_size = control_panel_default_size,
      .on_open = control_panel_open, .on_draw = control_panel_draw,
      .on_hover = control_panel_hover,
      .on_press = control_panel_press, .on_release = control_panel_release,
      .resizable = 1 },
    // --- ring-3 programs (gui_apps.h's `exec_path`) ------------------
    //
    // These are LAUNCHERS, not apps: opening one spawns a process from
    // /bin, and that process builds its own window over the windowing
    // protocol. No callbacks, no default_size -- see `exec_path`.
    //
    // The "(ring 3)" suffixes are gone: Calculator, Notepad and Terminal
    // used to exist BOTH ways, which is what made that migration
    // verifiable, and the kernel-space three retired in Milestone 41's
    // stage 0 (docs/wm-ring3-design.md). There is one of each again, so
    // the disambiguating suffix has nothing left to disambiguate.
    //
    // Appended at the END on purpose, after UI Demo. Registry order is
    // both the Start-menu row order and the desktop icon order, so
    // inserting anywhere above would renumber every row index that
    // tools/ has written down. (Saved desktop icon positions survive
    // either way -- desktop.c keys those by app NAME precisely so a
    // reorder can't scramble them.) Same reasoning as UI Demo's own
    // "last on purpose" note above.
    { .name = "About", .exec_path = "/bin/about" },
    { .name = "Shapes", .exec_path = "/bin/shapes" },
    { .name = "Calculator", .exec_path = "/bin/calculator" },
    { .name = "Notepad", .exec_path = "/bin/notepad" },
    { .name = "Terminal", .exec_path = "/bin/uterm" },
    // Last on purpose: a testing target, not something a user of the OS
    // is looking for, and keeping it at the end means every other app's
    // Start-menu row index stays put.
    { .name = "UI Demo", .exec_path = "/bin/uidemo" },
};
const int gui_app_registry_count = sizeof(gui_app_registry) / sizeof(gui_app_registry[0]);
