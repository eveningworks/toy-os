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
#include "sys.h"
#include "ugfx.h"
#include "uui.h"
#include "utext.h"
#include "utheme.h"
#include "ush.h"
#include "keyboard.h"

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

static struct utext g_out;      // the scrollback
static struct ush g_shell;
static uint32_t g_win;
static int g_running;           // 1 while a command is executing

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

static void present(void) {
    struct win_request_msg req = {0};
    req.type = WIN_REQ_PRESENT;
    req.window = g_win;
    sys_win_request(&req);
}

static void text_rect(struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    *x = MARGIN;
    *y = MARGIN;
    *w = s->w - 2 * MARGIN;
    // The bottom row is the live prompt, drawn separately so it never
    // scrolls away with the transcript.
    *h = s->h - 2 * MARGIN - ugfx_char_h() - 4;
}

static void draw(struct ugfx_surface *s) {
    ugfx_fill(s, ugfx_rgb(0, 0, 0));

    int tx, ty, tw, th;
    text_rect(s, &tx, &ty, &tw, &th);
    utext_draw(&g_out, s, tx, ty, tw, th,
                ugfx_rgb(220, 220, 220), ugfx_rgb(0, 0, 0),
                ugfx_rgb(60, 80, 120), 0);

    // Prompt line.
    int py = s->h - MARGIN - ugfx_char_h();
    ugfx_draw_string(s, MARGIN, py, g_shell.cwd, ugfx_rgb(120, 200, 120), ugfx_rgb(0, 0, 0));
    int px = MARGIN + ugfx_text_width(g_shell.cwd);
    ugfx_draw_string(s, px, py, "> ", ugfx_rgb(120, 200, 120), ugfx_rgb(0, 0, 0));
    px += 2 * ugfx_char_w();
    ugfx_draw_string_clipped(s, px, py, s->w - px - MARGIN, g_line,
                              ugfx_rgb(240, 240, 240), ugfx_rgb(0, 0, 0));
    if (!g_running) {
        ugfx_fill_rect(s, px + ugfx_text_width(g_line), py, 2, ugfx_char_h(),
                        ugfx_rgb(240, 240, 240));
    }
}

static void run_current_line(struct ugfx_surface *s) {
    put(g_shell.cwd);
    put("> ");
    put(g_line);
    put("\n");

    g_running = 1;
    draw(s);
    present(); // show the echoed command BEFORE blocking on it

    ush_run_line(&g_shell, g_line);

    g_running = 0;
    g_line_len = 0;
    g_line[0] = '\0';
}

int main(void) {
    if (!ugfx_font_init()) return 2;

    struct win_request_msg req = {0};
    req.type = WIN_REQ_CREATE;
    req.a = WIN_W;
    req.b = WIN_H;
    req.c = 120;
    req.d = 120;
    if (sys_win_request(&req) != 1) return 1;
    g_win = req.window;

    req.type = WIN_REQ_TITLE;
    req.window = g_win;
    const char *title = "Terminal (ring 3)";
    int t = 0;
    for (; title[t] && t < WIN_TITLE_LEN - 1; t++) req.text[t] = title[t];
    req.text[t] = '\0';
    sys_win_request(&req);

    utext_init(&g_out);
    ush_init(&g_shell, out_sink, 0);
    g_line[0] = '\0';
    g_line_len = 0;

    put("toy-os terminal, running in ring 3.\n");
    put("Type `help`. Esc closes this window.\n\n");

    struct ugfx_surface s = ugfx_surface_for_window(g_win, WIN_W, WIN_H);
    draw(&s);
    present();

    for (;;) {
        struct win_event ev;
        if (sys_wait_event(&ev) != 1) break;

        int quit = 0, dirty = 0;

        if (ev.type == WIN_EV_CLOSE) {
            quit = 1;
        } else if (ev.type == WIN_EV_KEY) {
            int k = ev.a;
            if (k == 0x1B) {
                quit = 1;
            } else if (k == '\n' || k == '\r') {
                run_current_line(&s);
                dirty = 1;
            } else if (k == '\b') {
                if (g_line_len > 0) g_line[--g_line_len] = '\0';
                dirty = 1;
            } else if (k == KEY_PAGE_UP) {
                utext_scroll(&g_out, 5);
                dirty = 1;
            } else if (k == KEY_PAGE_DOWN) {
                utext_scroll(&g_out, -5);
                dirty = 1;
            } else if (k >= 32 && k < 127 && g_line_len < LINE_MAX - 1) {
                g_line[g_line_len++] = (char)k;
                g_line[g_line_len] = '\0';
                dirty = 1;
            }
        }

        if (quit) break;
        if (dirty) { draw(&s); present(); }
    }

    struct win_request_msg d = {0};
    d.type = WIN_REQ_DESTROY;
    d.window = g_win;
    sys_win_request(&d);
    return 0;
}
