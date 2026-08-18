#include "gui.h"
#include "kapi.h"
#include "multiboot.h"
#include "wm/wm.h"

void gui_main(void) {
    // WHICH DESKTOP. The RING-3 one by default since 2026-08-18, when
    // the last three GUI tools that assumed the desktop is not a
    // process learned to ask who holds the compositor role -- the whole
    // suite, 23 of 23 tools, now passes against it.
    //
    // `gui0` on the kernel command line (`make iso KCMDLINE="gui0"`)
    // still starts the ring-0 one. That is not hedging: `apps/wm/` is
    // still in the tree until stage 4c deletes it, and this repo's
    // standing rule is that a path nothing can reach is a guess -- the
    // same reasoning behind nokaslr, nopat, notsc and `ata nodma`. The
    // flag goes when the code it selects does.
    //
    // The pair is deliberately not symmetrical: `gui3` is also still
    // accepted, so every existing script and note that asks for the
    // ring-3 desktop by name keeps working rather than silently
    // selecting something else.
    const char *cmdline = multiboot_cmdline();
    if (cmdline && k_strstr(cmdline, "gui0")) {
        wm_run();
        return;
    }
    gui3_main();
}
