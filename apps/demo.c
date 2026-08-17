// The scripted tour -- see demo.h for the format and why it is a file
// rather than a hardcoded sequence.
#include "demo.h"
#include "shell.h"
#include "wm/wm_debug.h"
#include "kapi.h"
#include "multiboot.h"

#define DEMO_MAX_STEPS 64
#define DEMO_LINE_MAX  96

struct demo_step {
    char line[DEMO_LINE_MAX];
};

static struct demo_step g_steps[DEMO_MAX_STEPS];
static int g_count;
static int g_next;          // the step to perform
static int g_gui_started;   // has the `gui` step been reached?
static uint64_t g_wait_until; // pit_ticks() value to resume at

int demo_requested(void) {
    const char *cmdline = multiboot_cmdline();
    return cmdline && k_strstr(cmdline, "demo") != 0;
}

// A whole file into a fixed table. No allocator, and a demo script that
// does not fit in 64 steps is a demo nobody will sit through.
int demo_load(const char *path) {
    g_count = g_next = g_gui_started = 0;
    g_wait_until = 0;

    // fs_read() hands back the backend's own staging buffer, valid only
    // until the next filesystem call -- and this parser makes none, so
    // reading straight out of it is safe. Copying 4 KiB to be "careful"
    // would need somewhere to copy it TO, which is the thing this
    // kernel does not hand out casually.
    uint32_t size = 0;
    const char *buf = fs_read(path, &size);
    if (!buf || size == 0) return 0;
    int n = (int)size;

    int i = 0;
    while (i < n && g_count < DEMO_MAX_STEPS) {
        int start = i;
        while (i < n && buf[i] != '\n') i++;
        int len = i - start;
        if (i < n) i++; // step over the newline

        while (len > 0 && k_isblank(buf[start])) { start++; len--; }
        while (len > 0 && (buf[start + len - 1] == '\r' || buf[start + len - 1] == ' ')) len--;
        if (len <= 0 || buf[start] == '#') continue;   // blank or comment
        if (len >= DEMO_LINE_MAX) len = DEMO_LINE_MAX - 1;

        k_memcpy(g_steps[g_count].line, buf + start, (size_t)len);
        g_steps[g_count].line[len] = '\0';
        g_count++;
    }
    return g_count;
}

// `verb rest` -- returns the rest, and NUL-terminates the verb in place.
static char *split(char *line) {
    char *p = line;
    while (*p && *p != ' ') p++;
    if (!*p) return p; // no argument: point at the terminator
    *p++ = '\0';
    while (*p == ' ') p++;
    return p;
}

static void banner(const char *text) {
    // Deliberately loud and unlike ordinary output: somebody is WATCHING
    // this, probably from across a room, and the narration is the only
    // thing tying the commands together.
    vga_write("\n=== ");
    vga_write(text);
    vga_write(" ===\n");
}

static void spin_ms(uint32_t ms) {
    uint64_t start = pit_ticks();
    uint64_t want = (uint64_t)ms / 10; // the PIT runs at 100Hz
    while (pit_ticks() - start < want) {
        __asm__ volatile ("hlt");
        scheduler_idle(); // stay answerable while the demo waits
    }
}

int demo_run_cli(void) {
    while (g_next < g_count) {
        char line[DEMO_LINE_MAX];
        k_strlcpy(line, g_steps[g_next].line, sizeof line);
        char *arg = split(line);

        if (k_strcmp(line, "gui") == 0) {
            g_next++;             // consumed; the rest belongs to the WM
            g_gui_started = 1;
            return 1;
        }
        g_next++;

        if (k_strcmp(line, "say") == 0) {
            banner(arg);
        } else if (k_strcmp(line, "sh") == 0) {
            vga_write("$ ");
            vga_write(arg);
            vga_write("\n");
            char cmd[DEMO_LINE_MAX];
            k_strlcpy(cmd, arg, sizeof cmd);
            shell_dispatch(cmd, 0);   // 0 = the physical console
        } else if (k_strcmp(line, "wait") == 0) {
            uint32_t ms = 0;
            k_parse_u32(arg, &ms);
            spin_ms(ms);
        } else if (k_strcmp(line, "end") == 0) {
            g_next = g_count;
            return 0;
        }
        // An unknown verb is skipped rather than fatal: a typo should
        // cost one step of a demo, not the demo.
    }
    return 0;
}

void demo_gui_tick(void) {
    if (!g_gui_started || g_next >= g_count) return;
    if (g_wait_until && pit_ticks() < g_wait_until) return;
    g_wait_until = 0;

    // ONE step per call, and the WM's own injected-input queue drains
    // one event per iteration -- so a click issued here is delivered
    // over the following frames exactly as a test tool's would be.
    // Waiting for that to finish is what `wait` steps are for.
    char line[DEMO_LINE_MAX];
    k_strlcpy(line, g_steps[g_next].line, sizeof line);
    char *arg = split(line);
    g_next++;

    if (k_strcmp(line, "wait") == 0) {
        uint32_t ms = 0;
        k_parse_u32(arg, &ms);
        g_wait_until = pit_ticks() + ms / 10;
        return;
    }
    if (k_strcmp(line, "end") == 0) {
        g_next = g_count;
        return;
    }

    // Everything else is a `gui` subcommand, dispatched through exactly
    // the path the GUI test tools use (apps/wm/wm_debug.c). Reusing it
    // means the demo drives the desktop the same way the GUI suite already
    // do, rather than through a second injection mechanism that could
    // behave differently.
    char cmd[DEMO_LINE_MAX];
    if (*arg) k_snprintf(cmd, sizeof cmd, "%s %s", line, arg);
    else      k_strlcpy(cmd, line, sizeof cmd);
    wm_debug_dispatch(cmd);
}
