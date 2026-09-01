// guictl -- ask the window manager what it is doing, from ring 3.
//
// The `gui` diagnostics have always existed and have always been
// reachable from ONE place: the serial debug console. A machine with no
// serial console attached -- the bare-metal laptop -- could not be asked
// anything about its desktop at all. This is the same channel, from a
// program, so `guictl state` over telnet answers what `sh gui state`
// answers over COM1.
//
// The MODEL IS swaymsg / hyprctl: a small client that speaks the
// compositor's own diagnostic protocol and prints what comes back. It
// parses nothing -- the vocabulary is the window manager's
// (userland/wm/wm_debug.c) and stays there, so a subcommand added to the
// WM works here the day it lands, with no edit to this file. `guictl
// help` is the list.
//
// NOT NAMED `gui`: the kernel shell's `gui` STARTS a desktop, and a
// /bin program of that name would shadow a builtin that does something
// else entirely (CLAUDE.md).
//
// THE CHANNEL IS ONE SLOT, so this can be refused. The serial console
// and this program are two clients of one reply buffer; the kernel
// hands the second one -EBUSY rather than letting it read the first
// one's bytes (kernel/proc/win_server.c). That is a real answer, not a
// failure to try again around.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "win_proto.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define USAGE "guictl <command> [args...]   (try `guictl help`)"

// The console's own bound, and for the same reason: a transport that
// answered with WIN_DEBUG_F_MORE permanently set would otherwise spin
// forever, and this is a diagnostic tool -- it must fail rather than
// hang on a wedged desktop. 64 chunks is 32 KB, far past any real reply.
#define MAX_CHUNKS 64

// THE WAIT IS OURS, and that is the design rather than an accident. The
// window manager is a process too, so the kernel POSTS our command to
// it and hands back WIN_DEBUG_F_PENDING immediately -- it may not park a
// syscall in place (api/scheduler.h), and it faulted when it tried. So
// we poll. 2 ms a turn for up to 2 s: a `gui state` answers on the
// compositor's next frame, which is at most WM_IDLE_WAIT_MS away.
#define POLL_MS      2
#define POLL_TRIES   1000

// .bss, not the stack: the struct is 528 bytes and the ring-3 frame
// budget is 2 KiB.
static struct win_debug_msg g_msg;

static void emit(const struct win_debug_msg *m) {
    if (m->len) write(1, m->text, m->len);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage(USAGE);
        return 1;
    }

    // argv joined with spaces -- the WM parses its own line, so a
    // subcommand this program has never heard of passes through intact.
    char line[WIN_DEBUG_CMD_LEN];
    size_t n = 0;
    for (int i = 1; i < argc && n < sizeof line - 1; i++) {
        if (i > 1) line[n++] = ' ';
        for (const char *p = argv[i]; *p && n < sizeof line - 1; p++)
            line[n++] = *p;
    }
    line[n] = '\0';

    memset(&g_msg, 0, sizeof g_msg);
    g_msg.type = WIN_REQ_DEBUG_CMD;
    strncpy(g_msg.text, line, WIN_DEBUG_CMD_LEN - 1);

    int rc = sys_win_debug(&g_msg);
    if (rc == -EBUSY) {
        sys_print("guictl: busy -- another diagnostic is in flight\n");
        return 1;
    }
    if (rc <= 0) {
        // No window manager, as distinct from a command it did not know.
        sys_print("guictl: no window manager is running\n");
        return 1;
    }

    // Wait out the post, if there was one.
    int tries = 0;
    while (g_msg.flags & WIN_DEBUG_F_PENDING) {
        if (++tries > POLL_TRIES) {
            sys_print("guictl: the window manager did not answer\n");
            return 1;
        }
        usleep(POLL_MS * 1000);
        memset(&g_msg, 0, sizeof g_msg);
        g_msg.type = WIN_REQ_DEBUG_MORE;
        if (sys_win_debug(&g_msg) <= 0) {
            sys_print("guictl: the window manager stopped answering\n");
            return 1;
        }
    }

    if (g_msg.flags & WIN_DEBUG_F_UNKNOWN) {
        sys_print("guictl: unknown command -- try `guictl help`\n");
        return 1;
    }

    emit(&g_msg);
    for (int guard = 0; guard < MAX_CHUNKS; guard++) {
        if (!(g_msg.flags & WIN_DEBUG_F_MORE)) break;
        memset(&g_msg, 0, sizeof g_msg);
        g_msg.type = WIN_REQ_DEBUG_MORE;
        if (sys_win_debug(&g_msg) <= 0) break;
        emit(&g_msg);
    }
    return 0;
}
