#include "gui_apps.h"
#include "notepad.h"
#include "about.h"
#include "calculator.h"
#include "terminal.h"
#include "taskmgr.h"
#include "control_panel.h"
#include "uidemo.h"

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
      .on_draw = notepad_draw, .on_key = notepad_key, .on_click = notepad_click,
      .on_press = notepad_press, .on_release = notepad_release,
      .on_hover = notepad_hover,
      .on_drag_start = notepad_drag_start, .on_drag = notepad_drag,
      .on_wheel = notepad_wheel, .on_write_complete = notepad_write_complete,
      .on_read_complete = notepad_read_complete, .resizable = 1 },
    { .name = "About", .default_size = about_default_size, .on_open = about_open,
      .on_draw = about_draw, .resizable = 1 },
    { .name = "Calculator", .default_size = calculator_default_size, .on_open = calculator_open,
      .on_close = calculator_close,
      .on_draw = calculator_draw, .on_key = calculator_key,
      .on_press = calculator_press, .on_release = calculator_release,
      .on_hover = calculator_hover, .resizable = 0,
      .multi_instance = 1 },
    { .name = "Terminal", .default_size = terminal_default_size, .on_open = terminal_open,
      .on_draw = terminal_draw, .on_key = terminal_key, .on_click = terminal_click,
      .on_drag_start = terminal_drag_start, .on_drag = terminal_drag,
      .on_wheel = terminal_wheel, .on_process_exit = terminal_process_exit, .resizable = 1 },
    { .name = "Task Manager", .default_size = taskmgr_default_size, .on_open = taskmgr_open,
      .on_draw = taskmgr_draw, .resizable = 1 },
    { .name = "Control Panel", .default_size = control_panel_default_size,
      .on_open = control_panel_open, .on_draw = control_panel_draw,
      .on_hover = control_panel_hover,
      .on_press = control_panel_press, .on_release = control_panel_release,
      .resizable = 1 },
    // Last on purpose: it's a testing target, not something a user of
    // the OS is looking for, and keeping it at the end means every
    // other app's Start-menu row index stays put.
    { .name = "UI Demo", .default_size = uidemo_default_size, .on_open = uidemo_open,
      .on_draw = uidemo_draw, .on_key = uidemo_key, .on_click = uidemo_click,
      .on_press = uidemo_press, .on_release = uidemo_release,
      .on_hover = uidemo_hover, .on_wheel = uidemo_wheel,
      .on_drag_start = uidemo_drag_start, .on_drag = uidemo_drag,
      .resizable = 1 },

    // --- ring-3 programs (gui_apps.h's `exec_path`) ------------------
    //
    // These are LAUNCHERS, not apps: opening one spawns a process from
    // /bin, and that process builds its own window over the windowing
    // protocol. No callbacks, no default_size -- see `exec_path`.
    //
    // Why the "(ring 3)" suffix on three of them: Calculator, Notepad
    // and Terminal exist BOTH ways right now. The kernel-space ones are
    // deliberately still here (keeping both is what made the migration
    // verifiable -- Calculator's arithmetic engine is compiled into
    // both, so the two cannot disagree), which leaves two menu entries
    // that would otherwise be identically labelled and open completely
    // different programs. The suffix goes away when the kernel-space
    // versions retire; the Task Manager makes the same distinction with
    // its [r0]/[r3] column. Shapes needs no suffix -- it only ever
    // existed in ring 3.
    //
    // Appended at the END on purpose, after UI Demo. Registry order is
    // both the Start-menu row order and the desktop icon order, so
    // inserting anywhere above would renumber every row index that
    // tools/ has written down. (Saved desktop icon positions survive
    // either way -- desktop.c keys those by app NAME precisely so a
    // reorder can't scramble them.) Same reasoning as UI Demo's own
    // "last on purpose" note above.
    { .name = "Shapes", .exec_path = "/bin/shapes" },
    { .name = "Calculator (ring 3)", .exec_path = "/bin/calculator" },
    { .name = "Notepad (ring 3)", .exec_path = "/bin/notepad" },
    { .name = "Terminal (ring 3)", .exec_path = "/bin/uterm" },
};
const int gui_app_registry_count = sizeof(gui_app_registry) / sizeof(gui_app_registry[0]);
