// See editor.h for the split between this file's shared editing core
// (editor_load/editor_save/editor_handle_key -- all just thin wrappers
// around text_scrollback's cursor-aware API, widgets.h) and its two
// renderers: editor_run() below (physical console, vga_*) and
// apps/terminal.c's GUI editor sub-mode (widget_scrollback_draw(),
// gfx_*).
#include "editor.h"
#include "kapi.h"

// Mirrors apps/notepad.c's g_save_buf -- a static scratch buffer for
// flattening tb into a plain string for fs_write(), sized to
// SCROLLBACK_CAP so it never truncates content the buffer itself could
// hold (fs.h's FS_DATA_MAX, 2048 bytes, is the real ceiling on what
// actually gets saved -- see editor_save()'s own comment). Static, not
// a stack local, for the same reason notepad.c's copy is: this can be
// called from editor_run()'s own loop (running on the kernel's boot
// stack, not a process kstack) as well as from terminal.c's non-blocking
// callback, and an 8KB local array is a lot to put on either.
static char g_editor_buf[SCROLLBACK_CAP];

void editor_load(struct text_scrollback *tb, const char *path) {
    uint32_t size = 0;
    const char *data = fs_read(path, &size);
    if (data) {
        for (uint32_t i = 0; i < size && data[i] != '\0'; i++) {
            widget_scrollback_putc(tb, data[i]);
        }
    }
    tb->cursor = 0; // land at the start, not the append point -- see this call's header comment
}

int editor_save(struct text_scrollback *tb, const char *path) {
    int n = 0;
    for (int i = 0; i < tb->count && n < (int)sizeof(g_editor_buf) - 1; i++) {
        g_editor_buf[n++] = tb->buf[(tb->start + i) % SCROLLBACK_CAP].ch;
    }
    g_editor_buf[n] = '\0';
    return fs_write(path, g_editor_buf, 0);
}

void editor_handle_key(struct text_scrollback *tb, const char *path, int key,
                        int *out_should_exit, char *status, unsigned int status_max) {
    // F3 is the real, documented exit key (stands in for nano's
    // Ctrl+X) -- Esc is also accepted as a convenience, but ONLY
    // reaches here from the physical console: apps/wm/wm.c intercepts
    // Esc globally to leave the whole GUI desktop before any window's
    // on_key callback ever sees it (see wm.c's own comment at that
    // check), so a Terminal-embedded editor session can never receive
    // it. Found the hard way while testing this build -- see
    // CHANGELOG.md's build 377 entry.
    if (key == 27 || key == KEY_F3) {
        *out_should_exit = 1;
        return;
    }
    if (key == KEY_F2) { // stands in for nano's Ctrl+O
        int ok = editor_save(tb, path);
        if (status && status_max) {
            k_strcpy(status, ok ? "Saved." : "Save failed.");
        }
        return;
    }

    if (key == '\b') widget_scrollback_backspace_at_cursor(tb);
    else if (key == KEY_DELETE) widget_scrollback_delete_at_cursor(tb);
    else if (key == '\r' || key == '\n') widget_scrollback_insert_at_cursor(tb, '\n');
    else if (key == KEY_ARROW_LEFT) widget_scrollback_cursor_left(tb);
    else if (key == KEY_ARROW_RIGHT) widget_scrollback_cursor_right(tb);
    else if (key == KEY_ARROW_UP) widget_scrollback_cursor_up(tb);
    else if (key == KEY_ARROW_DOWN) widget_scrollback_cursor_down(tb);
    else if (key == KEY_HOME) widget_scrollback_cursor_home(tb);
    else if (key == KEY_END) widget_scrollback_cursor_end(tb);
    else if (key >= 32 && key < 127) widget_scrollback_insert_at_cursor(tb, (char)key);
    // anything else (Page Up/Down, unrecognized codes) -- no-op
}

// Full-screen redraw for the physical console: vga_clear() then walk
// the buffer character-by-character via vga_putc(), which already
// knows how to wrap/scroll/handle '\n' -- so this needs no column-width
// or row-tracking logic of its own at all, unlike a "real" terminal
// editor's usual cursor-addressed redraw. The cursor itself is a single
// reverse-video character, toggled with vga_set_color() around exactly
// one vga_putc() call, rather than a true hardware-cursor placement
// (see editor.h's top comment on why this pass didn't add vga_sink
// cursor-positioning -- this print-everything-in-order approach turned
// out not to need it). Trade-off: on a file taller than the console,
// the status line -- printed last -- scrolls along with the content
// instead of staying pinned to the bottom, unlike real nano.
static void editor_render(struct text_scrollback *tb, const char *path, const char *status) {
    vga_clear();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (int i = 0; i <= tb->count; i++) {
        char c = (i < tb->count) ? tb->buf[(tb->start + i) % SCROLLBACK_CAP].ch : 0;
        if (i == tb->cursor) {
            // A real character gets highlighted in place (no extra
            // column consumed). A '\n' or the very end of the buffer
            // has no glyph of its own to highlight, so a highlighted
            // space stands in for "the cursor is here" instead, printed
            // just before whatever real character (if any) follows.
            char show = (i < tb->count && c != '\n') ? c : ' ';
            vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
            vga_putc(show);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            if (i < tb->count && c == '\n') vga_putc('\n');
            continue;
        }
        if (i == tb->count) break;
        vga_putc(c);
    }

    vga_write("\n-- ");
    vga_write(path);
    vga_write(" -- F2 Save  F3 Exit");
    if (status && status[0]) {
        vga_write("  -- ");
        vga_write(status);
    }
    vga_write(" --\n");
}

// Static, NOT a stack local -- struct text_scrollback embeds an
// 8192-cell buffer (SCROLLBACK_CAP), around 16KB, and editor_run() runs
// on the kernel's own boot stack (16KB total -- see boot.asm), several
// call frames deep (kernel_main -> apps_start -> shell_main -> dispatch
// -> cmd_edit -> here), same reasoning as apps/notepad.c's g_notepad
// and this file's own g_editor_buf. A stack-local copy here blew the
// stack silently in exactly the way that's easy to miss in testing --
// no crash message, just corrupted memory nearby (this cost real
// debugging time before the fix; see CHANGELOG.md's build 377 entry).
// Safe as a single static instance since only one `edit`/`nano` session
// can be running on the physical console at a time.
static struct text_scrollback g_editor_tb;

void editor_run(const char *path) {
    widget_scrollback_init(&g_editor_tb);
    editor_load(&g_editor_tb, path);

    char status[32];
    status[0] = '\0';

    for (;;) {
        editor_render(&g_editor_tb, path, status);
        int key = keyboard_getchar();
        status[0] = '\0'; // see editor_handle_key()'s header comment on this clear-before-call convention
        int should_exit = 0;
        editor_handle_key(&g_editor_tb, path, key, &should_exit, status, sizeof(status));
        if (should_exit) break;
    }

    vga_clear();
}
