#include "gui.h"
#include "wm/wm.h"

void gui_main(void) {
    // Still the RING-0 desktop. The flip is one line (`gui3_main()`) and
    // the ring-3 desktop now WORKS -- measured, with the flip in place:
    // 18 of 23 tools and every widget, window, menu and dialog check
    // pass against it.
    //
    // The five that do not are not WM bugs, which is why this is a
    // deliberate pause rather than a retreat:
    //
    //   * compositor/screen/compdeath register a SECOND compositor
    //     (compclient, screenclient) alongside the desktop. That was
    //     free when the WM was ring 0 and is a contradiction now -- the
    //     role is single, so those tools evict the desktop and then ask
    //     it questions.
    //   * taskmgr/forcequit end a process chosen from the process
    //     table, and the desktop is IN that table now. They kill pid 1
    //     and then report that no window manager is running, which is
    //     true and their own doing.
    //
    // Both need the tools rethought for a world where the desktop is a
    // process. Flipping before that would turn five tools red for
    // reasons that have nothing to do with the code under test.
    wm_run();
}
