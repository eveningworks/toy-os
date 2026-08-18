// `gui` -- start the desktop, which is a ring-3 process.
//
// It spawns `/bin/wm/system/toywm` and waits for it: the shell blocks
// exactly as it did when the window manager was kernel code, but what
// it is blocking on is a process. That is R7's "spawn-and-wait", and it
// is the whole of what the kernel knows about the desktop now.
//
// The waiting is what makes the death path work as a user sees it: when
// the desktop exits -- normally, killed (`kill 1`), or faulting -- this
// returns and the caller redraws the text console. The kernel has
// already revoked the framebuffer grant and asked any client windows to
// close by then (kernel/proc/win_server.c's compositor_gone()).
//
// The file is still named gui3.c because the command it used to
// implement was `gui3`, kept while the ring-0 desktop existed beside
// it. `apps/wm/` is gone; `gui3` survives only as an alias in
// apps/apps.c so notes and scripts that ask for the ring-3 desktop by
// name keep selecting what they meant.
#include "gui3.h"
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
