#include "gui.h"
#include "kapi.h"
#include "multiboot.h"
#include "wm/wm.h"

void gui_main(void) {
    // WHICH DESKTOP. The ring-0 one by default; the ring-3 one when
    // `gui3` is on the kernel command line
    // (`make iso KCMDLINE="gui3"`), which is how the GUI suite is run
    // against it without editing this file -- the step that otherwise
    // gets edited in and forgotten on the way out.
    //
    // Same switch pattern as nokaslr/nopat/notsc/faultinject, and the
    // same reasoning: a path nothing can reach is a guess.
    //
    // The ring-3 desktop WORKS -- it composites, opens client windows,
    // and passes 19 of 23 GUI tools. What it is waiting on is four
    // tools that assume the desktop is not a process
    // (docs/roadmap.md). When those are fixed this whole function
    // becomes `gui3_main()` and apps/wm/ is deleted.
    const char *cmdline = multiboot_cmdline();
    if (cmdline && k_strstr(cmdline, "gui3")) {
        gui3_main();
        return;
    }
    wm_run();
}
