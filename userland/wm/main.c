// toywm -- the window manager as a ring-3 process (Milestone 41 stage 4b).
//
// This file is only the entry point. Everything the desktop actually is
// lives in `userland/wm/`, which is the port of `apps/wm/` and is linked
// in through the Makefile's EXTRA_OBJS_toywm.
//
// WHY THE SOURCES ARE A SECOND COPY, AND WHEN THAT ENDS
// -----------------------------------------------------
// `apps/wm/` still exists and is still the live desktop. It has to: all
// 22 GUI test tools and their 305 checks drive it, and every stage of
// this migration has been shaped so the suite passes at the stage's end
// rather than going red for the duration. Porting the call sites in
// place would break the kernel build the moment the first one changed.
//
// So the port lives beside the original until it can replace it, and
// **stage 4c deletes `apps/wm/` outright** -- not merges it, not
// reconciles it. Two copies of an 8,000-line component is exactly the
// drift this repo has paid for before, so treat the duplication as a
// countdown rather than a state: any fix made to one during 4b has to be
// made to the other, and the sooner 4c lands the shorter that window is.
//
// WHAT THIS IS NOT, YET
// ---------------------
// Stage 4b's scope is that this COMPILES AND LINKS as a ring-3 binary
// with the ring-0 WM still driving the screen. Spawning it, letting it
// composite, and cutting input over to it are 4c. Until then this
// program is built and seeded but never launched by anything.
#include "rt/sys.h"
#include "wm/wm.h"

int main(void) {
    // wm_run() owns the whole desktop lifetime and returns only when the
    // session ends. In ring 0 that return landed back in the shell's
    // `gui` command, which then resumed the text console; in ring 3 the
    // kernel does that when it sees the compositor deregister, so there
    // is nothing to do here but hand back an exit code (R7).
    wm_run();
    return 0;
}
