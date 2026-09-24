// `gui` -- start the desktop, which is a ring-3 process.
//
// It spawns `/bin/wm/system/toywm` and waits for it: the shell blocks
// exactly as it did when the window manager was kernel code, but what
// it is blocking on is a process. That is R7's "spawn-and-wait", and it
// is the whole of what the kernel knows about the desktop now.
//
// The waiting is what makes the death path work as a user sees it: when
// the desktop exits -- normally, killed (`ps` for its pid, then
// `kill <pid>`), or faulting -- this
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
#include "win_role.h" // win_server_compositor_pid() -- is a desktop already up?

#define TOYWM_PATH "/bin/wm/system/toywm"

void gui3_main(void) {
    // THE ROLE IS SINGLE. Since init started supervising the desktop
    // (docs/init-design.md stage 2) a `graphical` boot already has one,
    // and a second toywm would come up, be refused the compositor role
    // by win_server.c, and exit -- silently, from the user's side, which
    // is the exact shape of bug this repo keeps writing up. Ask first.
    //
    // This is not a lock and does not need to be: the only caller is a
    // person typing `gui`. What it buys is a sentence instead of a
    // mystery.
    int held = win_server_compositor_pid();
    if (held > 0) {
        vga_printf("gui: the desktop is already running (pid %d)\n", held);
        vga_write("gui: `ps` to see it; `kill <pid>` to stop it -- init "
                  "will restart it\n");
        return;
    }

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
    // A halt rather than a spin: the desktop we are waiting for runs
    // when this context idles.
    int code = -1;
    while (scheduler_poll(pid, &code) == SCHED_POLL_RUNNING) {
        scheduler_idle();
        scheduler_idle_halt();
    }

    // The console is restored by the kernel when the compositor role is
    // dropped -- cleanly, by a kill, or by a fault, all one path (R7,
    // win_server.c's compositor_gone()). Nothing to do here but report,
    // which is the point: a desktop that died is survivable, and the
    // shell it returns to is the kernel's doing rather than the WM's.
    klog_printf("gui3: ring-3 desktop exited (code %d)\n", code);
}
