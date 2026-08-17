// `gui3` -- start the RING-3 desktop (Milestone 41's switchover).
//
// The ring-0 `gui` calls wm_run() directly and the desktop IS the kernel
// for as long as it is up. This spawns `/bin/wm/system/toywm` instead
// and waits for it, which is what R7 calls "spawn-and-wait" -- the shell
// blocks exactly as it did before, but what it is blocking on is a
// process.
//
// **A SEPARATE COMMAND, deliberately, and only for as long as it takes
// to trust it.** Every GUI test tool reaches the WM over the serial
// debug console, and the ring-3 leg of that channel could not be
// exercised before this existed (see docs/wm-ring3-design.md) -- so
// flipping `gui` outright would have turned all 315 checks red at once
// with no way left to ask the desktop what went wrong. With both
// commands present, the ring-0 desktop stays the one under test while
// the ring-3 one is brought up beside it.
//
// This file goes away with `apps/wm/`: once `gui` spawns the process,
// there is nothing for a second command to mean.
#include "gui.h"
#include "kapi.h"
#include "apps.h"

#define TOYWM_PATH "/bin/wm/system/toywm"

void gui3_main(void) {
    int pid = scheduler_spawn(TOYWM_PATH, 0);
    if (pid <= 0) {
        vga_write("gui3: could not spawn " TOYWM_PATH "\n");
        vga_write("gui3: is it seeded? `ls /bin/wm/system`\n");
        return;
    }

    klog_printf("gui3: ring-3 desktop started as pid %d\n", pid);

    // Wait for it, keeping the kernel's idle work going meanwhile.
    //
    // scheduler_idle() is what drains the serial debug console, and
    // while a ring-3 desktop is up it is the ONLY thing doing so -- the
    // ring-0 WM's event loop used to. That is precisely the deletion
    // stage 4a's R5 was built to make safe: the capability moved into
    // the kernel first, so the WM's departure costs a call rather than
    // the console.
    //
    // hlt rather than a spin: the timer is what schedules the desktop we
    // are waiting for.
    int code = -1;
    while (scheduler_poll(pid, &code) == SCHED_POLL_RUNNING) {
        scheduler_idle();
        __asm__ volatile ("hlt");
    }

    // The console is restored by the kernel when the compositor role is
    // dropped -- cleanly, by a kill, or by a fault, all one path (R7,
    // win_server.c's compositor_gone()). Nothing to do here but report,
    // which is the point: a desktop that died is survivable, and the
    // shell it returns to is the kernel's doing rather than the WM's.
    klog_printf("gui3: ring-3 desktop exited (code %d)\n", code);
}
