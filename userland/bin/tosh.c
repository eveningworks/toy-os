// /bin/tosh -- the standalone toy-os shell.
//
// The thin main() userland/lib/tosh.h has promised since it was
// written: tosh_init() plus a read loop. Everything a shell DOES lives
// in the library, which the GUI Terminal drives from its own event
// loop -- so this file is only the half a terminal already owns, the
// part that turns keystrokes into a line.
//
// What made it possible is a blocking stdin. Until fd 0 could be read
// (kernel/proc/syscall_fd.c's sys_do_read_console), a ring-3 program
// had only the non-blocking SYS_READ_KEY and would have had to
// spin-poll the keyboard for its entire idle life -- which is why
// docs/decisions.md said there was no /bin/tosh yet rather than that
// nobody had got round to it.
//
// LINE EDITING IS DELIBERATELY MINIMAL HERE: printable characters,
// Backspace, Enter, Ctrl-C to abandon a line and Ctrl-D to exit. The
// kernel's front ends share a real readline-style editor
// (kernel/lib/klineedit.c) and this should too, but that file reaches
// for kernel headers today; making it freestanding is its own change
// (docs/roadmap.md). A worse editor that is honestly the whole editor
// beats a second copy of a good one that drifts from the first.
//
// THERE IS NO ECHO FROM THE KERNEL. fd 0 is raw -- bytes as typed, no
// line discipline -- so what the user sees is what this loop prints.
// That is the shape a program with its own editor wants anyway.
#include "rt/sys.h"
#include "lib/tosh.h"
#include "lib/string.h"

#define LINE_MAX 256

// The shell's output sink. `struct tosh` streams through this rather
// than returning text, so a long `cat` appears as it is read.
static void out_fd1(void *ctx, const char *text, int len) {
    (void)ctx;
    sys_write(1, text, (size_t)len);
}

static void put(const char *s) { sys_write(1, s, (size_t)strlen(s)); }

// Key codes this shell acts on. The printable range and the control
// codes are ASCII; keyboard.h's specials (arrows, Home, F-keys) arrive
// as 0x91-0xA6 and are simply not handled yet, which is the same thing
// as saying this editor has no history or cursor movement.
#define K_BACKSPACE 8
#define K_TAB       9
#define K_ENTER    '\n'
#define K_RETURN   '\r'
#define K_CTRL_C    3
#define K_CTRL_D    4

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tosh sh;
    tosh_init(&sh, out_fd1, 0);

    put("tosh -- toy-os shell in ring 3. Ctrl-D to exit.\n");

    char line[LINE_MAX];
    int len = 0;
    put("$ ");

    for (;;) {
        char buf[32];
        // Blocks. The kernel parks this process on SCHED_WAIT_KEY and
        // the keyboard IRQ releases it, so an idle shell costs nothing
        // -- `ps` shows it blocked, not ready.
        int64_t n = sys_read(0, buf, sizeof buf);
        if (n <= 0) break; // a console has no EOF; this is an error

        for (int64_t i = 0; i < n; i++) {
            unsigned char k = (unsigned char)buf[i];

            if (k == K_ENTER || k == K_RETURN) {
                line[len] = '\0';
                put("\n");
                if (len > 0) tosh_run_line(&sh, line);
                len = 0;
                put("$ ");
                continue;
            }
            if (k == K_BACKSPACE) {
                // Erase on the screen too: with no echo from the kernel
                // there is nothing else to un-draw the character.
                if (len > 0) { len--; put("\b \b"); }
                continue;
            }
            if (k == K_CTRL_C) {
                put("^C\n$ ");
                len = 0;
                continue;
            }
            if (k == K_CTRL_D) {
                put("\n");
                return 0;
            }
            if (k == K_TAB) continue; // no completion here yet
            if (k < 32 || k > 126) continue; // specials and unmapped keys

            if (len < LINE_MAX - 1) {
                line[len++] = (char)k;
                sys_write(1, (const char *)&k, 1);
            }
        }
    }
    return 0;
}
