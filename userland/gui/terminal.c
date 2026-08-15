// Terminal, as a RING-3 PROCESS.
//
// The kernel-space original (apps/terminal.c) is not really an app: it
// is a front-end to the kernel's own shell. It calls shell_dispatch()
// directly, installs a vga_sink so kernel vga_write() output lands in
// its scrollback, and uses scheduler_spawn() for /bin binaries. All
// three are kernel-only facilities, which is why this migration needed
// new kernel machinery rather than just a port:
//
//   SYS_PIPE + SYS_SPAWN + SYS_WAITPID (see abi/syscall_abi.h)
//
// With those, a ring-3 terminal is an ordinary program: it reads keys
// from its window, hands complete lines to a shell it links against
// (userland/ush.c), and shows whatever comes back. Nothing it does
// requires privilege.
//
// WHAT IS DELIBERATELY DIFFERENT from the kernel Terminal, and why:
//
//   * The shell is `ush`, not the kernel's. The kernel shell's builtins
//     (fsck, ktest, fsformat, timezone...) reach into subsystems no
//     syscall exposes, and wrapping each one would be re-exporting the
//     kernel's internals under a new name. ush has the builtins a shell
//     can honestly implement over the file API, and spawns everything
//     else. `Esc` still drops to the physical shell for the rest.
//
//   * It BLOCKS while a command runs. The kernel Terminal cannot -- it
//     lives inside wm_run() -- which is why it needs an async spawn, a
//     WM-global pending_proc slot and a per-frame poll. Here the
//     desktop keeps running because this is a separate process, so the
//     command loop is just a function call.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utext.h"
#include "ui/utheme.h"
#include "lib/ush.h"
#include "keyboard.h"

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

static struct utext g_out;      // the scrollback
static struct ush g_shell;
static int g_running;  // 1 while a command is executing

// Where the prompt was last drawn, content-relative. Reported through
// the log so tools/uterm_test.py can assert it follows the transcript
// rather than re-deriving it in Python -- the trap every other GUI test
// tool here documents.
static int g_prompt_y;

// The line being typed. Not utext's cursor: the prompt line is a
// separate, editable thing from the transcript above it, exactly as in
// a real terminal.
#define LINE_MAX 128
static char g_line[LINE_MAX];
static int g_line_len;

static void out_sink(void *ctx, const char *text, int len) {
    (void)ctx;
    for (int i = 0; i < len; i++) utext_putc(&g_out, text[i]);
}

static void put(const char *s) { while (*s) utext_putc(&g_out, *s++); }


static void text_rect(struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    *x = MARGIN;
    *y = MARGIN;
    *w = s->w - 2 * MARGIN;
    // The bottom row is the live prompt, drawn separately so it never
    // scrolls away with the transcript.
    *h = s->h - 2 * MARGIN - ugfx_char_h() - 4;
}

static void draw(struct ugfx_surface *s, int focused) {
    ugfx_fill(s, ugfx_rgb(0, 0, 0));

    int tx, ty, tw, th;
    text_rect(s, &tx, &ty, &tw, &th);
    utext_draw(&g_out, s, tx, ty, tw, th,
                ugfx_rgb(220, 220, 220), ugfx_rgb(0, 0, 0),
                ugfx_rgb(60, 80, 120), 0);

    // The prompt follows the TRANSCRIPT, clamped to the bottom row.
    //
    // It used to be pinned to the bottom unconditionally, which is
    // wrong whenever the transcript is short: the output sat at the top
    // and the prompt was stranded at the foot of the window with a band
    // of empty black between them. A real terminal puts the prompt
    // immediately after the last line and only reaches the bottom once
    // the screen has filled. Clamping gives both behaviours with one
    // expression.
    int total_lines = 0, visible_rows = 0;
    utext_metrics(&g_out, tw, th, &total_lines, &visible_rows);
    int py = ty + total_lines * ugfx_char_h();
    int bottom = s->h - MARGIN - ugfx_char_h();
    if (py > bottom) py = bottom;
    g_prompt_y = py;
    ugfx_draw_string(s, MARGIN, py, g_shell.cwd, ugfx_rgb(120, 200, 120), ugfx_rgb(0, 0, 0));
    int px = MARGIN + ugfx_text_width(g_shell.cwd);
    ugfx_draw_string(s, px, py, "> ", ugfx_rgb(120, 200, 120), ugfx_rgb(0, 0, 0));
    px += 2 * ugfx_char_w();
    ugfx_draw_string_clipped(s, px, py, s->w - px - MARGIN, g_line,
                              ugfx_rgb(240, 240, 240), ugfx_rgb(0, 0, 0));
    // Only while FOCUSED, and only while not running a command. An
    // unfocused window drawing a caret claims to be taking input that
    // is actually going to whatever window is in front of it -- which
    // is the whole reason TWP has a focus event.
    if (!g_running && focused) {
        ugfx_fill_rect(s, px + ugfx_text_width(g_line), py, 2, ugfx_char_h(),
                        ugfx_rgb(240, 240, 240));
    }
}

static void run_current_line(struct uapp *a) {
    put(g_shell.cwd);
    put("> ");
    put(g_line);
    put("\n");

    g_running = 1;
    // Show the echoed command BEFORE blocking on it -- ush_run_line()
    // below can take seconds, and the loop would not otherwise paint
    // until this handler returned, i.e. after the command it echoed.
    // This is what uapp_flush() is for; see ui/uapp.h.
    uapp_redraw(a);
    uapp_flush(a);

    ush_run_line(&g_shell, g_line);

    g_running = 0;
    g_line_len = 0;
    g_line[0] = '\0';
}

// --- Toykit callbacks -------------------------------------------------

// One line per widget, content-relative, on stderr -- the same grammar
// UI Demo, Shapes and Calculator use.
static void log_layout(void) {
    char b[64];
    int n = 0;
    const char *pre = "uterm: layout prompt ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    int v = g_prompt_y;
    char d[12];
    int c = 0;
    if (v <= 0) d[c++] = '0';
    while (v > 0) { d[c++] = (char)('0' + v % 10); v /= 10; }
    while (c > 0) b[n++] = d[--c];
    b[n++] = '\n';
    b[n] = '\0';
    sys_eprint(b);
}


static void on_draw(struct uapp *a, struct uapp_draw *d) {
    draw(uapp_surface(d), uapp_focused(a));
    log_layout(); // after the draw: g_prompt_y is where it actually went
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 0x1B) { uapp_quit(a, 0); return; }

    if (key == '\n' || key == '\r') {
        run_current_line(a);
    } else if (key == '\b') {
        if (g_line_len > 0) g_line[--g_line_len] = '\0';
    } else if (key == KEY_PAGE_UP) {
        utext_scroll(&g_out, 5);
    } else if (key == KEY_PAGE_DOWN) {
        utext_scroll(&g_out, -5);
    } else if (key >= 32 && key < 127 && g_line_len < LINE_MAX - 1) {
        g_line[g_line_len++] = (char)key;
        g_line[g_line_len] = '\0';
    } else {
        return; // nothing changed
    }
    uapp_redraw(a);
}

static void on_open_cb(struct uapp *a) {
    (void)a;
    utext_init(&g_out);
    ush_init(&g_shell, out_sink, 0);
    g_line[0] = '\0';
    g_line_len = 0;

    put("toy-os terminal, running in ring 3.\n");
    put("Type `help`. Esc closes this window.\n\n");
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "Terminal (ring 3)",
        .w       = WIN_W,
        .h       = WIN_H,
        .x       = 120,
        .y       = 120,
        // A terminal is the other app that obviously wants resizing --
        // and needs no resize code, because draw() derives its text area
        // from the surface. The minimum keeps a usable number of
        // columns and rows.
        .flags   = UAPP_RESIZABLE,
        .min_w   = 280,
        .min_h   = 140,
        .on_open = on_open_cb,
        .on_draw = on_draw,
        .on_key  = on_key,
    };
    return uapp_run(&desc);
}
