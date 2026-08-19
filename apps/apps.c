#include "apps.h"
#include "kapi.h"
#include "shell.h"
#include "gui3.h"
#include "target.h"

// To add a new app: write apps/foo.c with a `void foo_main(void)` entry
// point (and apps/foo.h declaring it, by convention), then add one line
// to this table. That's the whole integration -- nothing else in the
// kernel needs to know foo.c exists. See apps/README.md.
const struct app_info app_registry[] = {
    { "shell", "Command-line shell",     shell_main },
    // One desktop, and it is a ring-3 process: `gui` spawns
    // /bin/wm/system/toywm and waits for it (apps/gui3.c). `gui3` stays
    // as an alias so notes, scripts and muscle memory that ask for the
    // ring-3 desktop by name keep working.
    { "gui",   "Graphical desktop",          gui3_main },
    { "gui3",  "Graphical desktop (alias)",  gui3_main },
};
const int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);

int app_run(const char *name) {
    for (int i = 0; i < app_registry_count; i++) {
        if (k_strcmp(app_registry[i].name, name) == 0) {
            app_registry[i].entry();
            return 1;
        }
    }
    return 0;
}

void app_list(void (*cb)(const char *name, const char *description)) {
    for (int i = 0; i < app_registry_count; i++) {
        cb(app_registry[i].name, app_registry[i].description);
    }
}

// THE PHYSICAL CONSOLE HAS ONE SHELL, AND ON A `text` BOOT IT IS NOT
// THIS ONE.
//
// init starts whatever /etc/services.d names for the boot target, and
// on `text` that is /bin/tosh on the console (docs/init-design.md's
// stage 4). Two shells draining one key ring is exactly the bug
// keyboard_claim_console() exists for -- but that claim is taken by the
// first fd-0 READ, which is tosh reaching its prompt. Measured, that is
// about ten milliseconds after init spawns it, and apps_start() runs
// inside that same window: neither side controls the ordering. A REPL
// started here would print a banner and a prompt onto a console that is
// about to belong to somebody else, and eat whatever was typed in the
// meantime, before the claim silenced it.
//
// So the decision is made from the TARGET, which the kernel knows
// before init is even spawned, instead of being raced against the
// claim. `graphical` is unchanged: the desktop owns the screen and this
// shell sits behind it, which is what *Exit to shell* returns to.
//
// Gated on init ACTUALLY RUNNING, because a boot with no /bin/init is
// supported and quiet (scheduler_init_pid() is 0 there) -- with nobody
// to start a shell, standing down would leave the machine with no
// console at all.
static int console_belongs_to_init(void) {
    return scheduler_init_pid() != 0 &&
           k_strcmp(target_get(), TARGET_TEXT) == 0;
}

// Never returns. The kernel shell is not gone -- every one of its ~60
// commands is still reachable as `sh <cmd>` over the serial debug
// console, which is how the whole test suite drives it anyway; what it
// gives up is the physical keyboard and the screen.
//
// scheduler_idle() is what polls that debug console, so it is the one
// thing this loop MUST keep doing. What it must NOT do is
// vga_cursor_tick()/vga_present(): console upkeep belongs to whoever
// owns the screen, and a suspended reader whose loop BODY kept drawing
// is how a blinking text cursor ended up on top of a desktop icon.
static void console_standby(void) {
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_write("[kernel shell: standing down -- this console belongs to "
              "init's shell]\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_present();
    klog_write("apps: kernel shell standing down -- the console belongs to "
               "init's shell (target=text)\n");

    for (;;) scheduler_idle();
}

void apps_start(void) {
    if (console_belongs_to_init()) console_standby(); // does not return
    app_run("shell");
}
