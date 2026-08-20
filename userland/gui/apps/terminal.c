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
// (userland/tosh.c), and shows whatever comes back. Nothing it does
// requires privilege.
//
// WHAT IS DELIBERATELY DIFFERENT from the kernel Terminal, and why:
//
//   * The shell is `tosh`, not the kernel's. The kernel shell's builtins
//     (fsck, ktest, fsformat, timezone...) reach into subsystems no
//     syscall exposes, and wrapping each one would be re-exporting the
//     kernel's internals under a new name. tosh has the builtins a shell
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
#include "lib/tosh.h"
#include "lib/uhistory.h"
#include "keyboard.h"
#include "klineedit.h"

#define WIN_W 640
#define WIN_H 400
#define MARGIN 6

// Defined below, beside the prompt it paints.
static const char *prompt_cwd(void);

static struct utext g_out;      // the scrollback
static struct tosh g_shell;
static int g_running;  // 1 while a command is executing

// Where the prompt was last drawn, content-relative. Reported through
// the log so tools/uterm_test.py can assert it follows the transcript
// rather than re-deriving it in Python -- the trap every other GUI test
// tool here documents.
static int g_prompt_y;

// The line being typed. Not utext's cursor: the prompt line is a
// separate, editable thing from the transcript above it, exactly as in
// a real terminal.
//
// EDITED BY THE KERNEL'S OWN EDITOR. kernel/lib/klineedit.c is compiled
// a second time into libuapp.a (the shared-source rule), so this
// window, /bin/tosh and the physical shell have ONE definition of what
// Ctrl-A, Alt-B and Up mean. This front end used to carry its own
// append-only loop -- printables and Backspace -- which is exactly the
// drift klineedit.h's header warns about: the copies diverge, and the
// divergence only shows when someone types the same keystroke in the
// other window.
//
// File scope rather than local: the editor is ~1.2 KiB and the history
// ~4 KiB, against -Wframe-larger-than=2048 and one guard page.
static struct kline_edit g_ed;
static struct uhistory g_hist;

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
    ugfx_draw_string(s, MARGIN, py, prompt_cwd(), ugfx_rgb(120, 200, 120), ugfx_rgb(0, 0, 0));
    int px = MARGIN + ugfx_text_width(prompt_cwd());
    ugfx_draw_string(s, px, py, "> ", ugfx_rgb(120, 200, 120), ugfx_rgb(0, 0, 0));
    px += 2 * ugfx_char_w();
    ugfx_draw_string_clipped(s, px, py, s->w - px - MARGIN, g_ed.buf,
                              ugfx_rgb(240, 240, 240), ugfx_rgb(0, 0, 0));
    // Only while FOCUSED, and only while not running a command. An
    // unfocused window drawing a caret claims to be taking input that
    // is actually going to whatever window is in front of it -- which
    // is the whole reason TWP has a focus event.
    if (!g_running && focused) {
        // At the CURSOR, not at the end of the line. With a real
        // editor the two differ the moment anything moves left, and a
        // caret pinned to the end makes Ctrl-A look like it did
        // nothing while the next keystroke lands somewhere else.
        char pre[KLINE_MAX];
        int c = g_ed.cursor;
        for (int i = 0; i < c; i++) pre[i] = g_ed.buf[i];
        pre[c] = '\0';
        ugfx_fill_rect(s, px + ugfx_text_width(pre), py, 2, ugfx_char_h(),
                        ugfx_rgb(240, 240, 240));
    }
}

// The prompt's directory. Asked of the KERNEL rather than kept here:
// the cwd belongs to this process (SYS_CHDIR), and a copy in the app
// could only ever disagree with the one `cd` actually moved -- which
// would show as a prompt naming a directory the shell is not in.
static const char *prompt_cwd(void) {
    static char here[64]; // TOSH_PATH_MAX
    if (sys_getcwd(here, sizeof here) < 0) here[0] = '/', here[1] = '\0';
    return here;
}

static void run_current_line(struct uapp *a) {
    put(prompt_cwd());
    put("> ");
    put(g_ed.buf);
    put("\n");

    g_running = 1;
    // Show the echoed command BEFORE blocking on it -- tosh_run_line()
    // below can take seconds, and the loop would not otherwise paint
    // until this handler returned, i.e. after the command it echoed.
    // This is what uapp_flush() is for; see ui/uapp.h.
    uapp_redraw(a);
    uapp_flush(a);

    tosh_run_line(&g_shell, g_ed.buf);

    g_running = 0;
    kline_init(&g_ed);
    uhist_reset(&g_hist);
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

// Alt-. -- the last whitespace-delimited word of the previous command.
static void insert_last_arg(void) {
    const char *last = uhist_last(&g_hist);
    if (!last) return;
    int n = 0;
    while (last[n]) n++;
    kline_insert_str(&g_ed, last + kline_ws_word_start(last, n, n));
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // Esc belongs to the shell's line editor, not to closing the
    // window: Alt+F4 does that now. See docs/decisions.md.

    // PAGE UP/DOWN SCROLL THE TRANSCRIPT AND ARE NOT THE EDITOR'S.
    // Taken before kline_key() rather than after, because the editor
    // answers KLINE_IGNORED for them and "ignored" is indistinguishable
    // from "not mine" once it has been asked.
    if (key == KEY_PAGE_UP)   { utext_scroll(&g_out, 5);  uapp_redraw(a); return; }
    if (key == KEY_PAGE_DOWN) { utext_scroll(&g_out, -5); uapp_redraw(a); return; }

    switch (kline_key(&g_ed, key)) {
    case KLINE_ACCEPT:
        if (g_ed.len > 0) uhist_add(&g_hist, g_ed.buf);
        run_current_line(a); // echoes the line, runs it, clears the editor
        break;

    case KLINE_CANCEL:
        put(prompt_cwd());
        put("> ");
        put(g_ed.buf);
        put("^C\n");
        kline_init(&g_ed);
        uhist_reset(&g_hist);
        break;

    case KLINE_EOF:
        // A window is not a console: Ctrl-D on an empty line would
        // close it, and closing goes through the WM so that on_close
        // can refuse (docs/gui-guidelines.md). Nothing here.
        return;

    case KLINE_HISTORY_PREV: {
        const char *e = uhist_prev(&g_hist, g_ed.buf);
        if (!e) return;
        kline_set(&g_ed, e);
        break;
    }

    case KLINE_HISTORY_NEXT: {
        const char *e = uhist_next(&g_hist);
        if (!e) return;
        kline_set(&g_ed, e);
        break;
    }

    case KLINE_LAST_ARG:
        insert_last_arg();
        break;

    case KLINE_CLEAR_SCREEN:
        utext_init(&g_out);
        break;

    case KLINE_REDRAW:
        break;

    // Tab completion and reverse search are the front end's to
    // implement and neither exists in ring 3 yet -- completion lives in
    // apps/completion.c, which is kernel-side. Named roadmap items
    // rather than half-built here; see /bin/tosh's matching comment.
    case KLINE_COMPLETE:
    case KLINE_SEARCH:
    case KLINE_IGNORED:
        return; // nothing changed
    }
    uapp_redraw(a);
}

static void on_open_cb(struct uapp *a) {
    (void)a;
    utext_init(&g_out);
    tosh_init(&g_shell, out_sink, 0);
    kline_init(&g_ed);
    uhist_init(&g_hist);

    put("tosh -- the toy-os shell, running in ring 3.\n");
    put("Type `help`. Alt+F4 closes this window.\n\n");
}

int main(void) {
    struct uapp_desc desc = {
        // Plain "Terminal" since Milestone 41's stage 0 retired the
        // kernel-space one -- the suffix existed only to tell two
        // identically-named Terminals apart, and there is one now.
        .title   = "Terminal",
        .app_id  = "terminal",
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
