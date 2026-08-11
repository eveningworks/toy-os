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
// hold. fs_write() itself has no separate size ceiling to worry about
// here anymore -- fs.h's FS_DATA_MAX stopped being a real per-file
// limit once TFS2 v2's block-addressed on-disk format landed (fs_write()
// goes through the same storage as fs_write_range(), limited only by
// available RAM/disk space) -- SCROLLBACK_CAP (this buffer's own size)
// is the actual ceiling on what a save can hold today. Static, not
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
    else if (IS_PRINTABLE_KEY(key)) widget_scrollback_insert_at_cursor(tb, (char)key);
    // anything else (Page Up/Down, unrecognized codes) -- no-op
}

// Full-screen redraw for the physical console. Unlike the first pass at
// this (see CHANGELOG.md's build 377/378 entries), this now does its
// own line-wrapping pass rather than just dumping the whole buffer via
// vga_putc() and letting the console's own wrap/scroll handle it --
// two things that approach got wrong once actually tested against a
// real console (see the build-378 screenshots that caught this):
//   1. The status line, printed last, scrolled along with the content
//      on anything taller than the console instead of staying pinned
//      to the bottom row like real nano (or this editor's own GUI
//      Terminal renderer, which already pins its status bar).
//   2. The console's own blinking cursor (see vga.c's
//      cursor_show_and_reset_blink()) sits wherever the last vga_putc()
//      left it -- which, after printing the status line and its
//      trailing '\n', was an extra blank row below the status bar. A
//      second, unrelated-looking cursor block, right under the
//      editor's own reverse-video one.
// The fix: reserve the console's last row for the status bar, only
// print a window of content lines that fits above it (scrolling that
// window to keep the buffer's cursor inside it -- same windowing
// widget_scrollback_draw() (widgets.c) already does for the GUI
// editor, just against text rows/vga_cols() instead of pixels/
// gfx_char_w()), pad with blank lines when the content doesn't fill
// the window, and print the status line with NO trailing '\n' so the
// console's own cursor lands right after it on the last row instead of
// a row below -- the same place it would sit after any ordinary
// vga_write() that doesn't end in '\n', not a special case.
static void editor_render(struct text_scrollback *tb, const char *path, const char *status) {
    uint32_t rows = vga_rows();
    int visible_rows = (rows > 1) ? (int)rows - 1 : 1; // last row reserved for the status bar
    int max_cols = (int)vga_cols();
    if (max_cols < 1) max_cols = 1;

    // Pass 1: replicate vga_putc()'s own wrap rule (wrap at max_cols)
    // to find which wrapped line/column tb->cursor lands on and how
    // many wrapped lines the buffer has in total -- mirrors
    // widgets.c's scrollback_measure(). i reaches tb->count (one past
    // the last real character) so a cursor sitting at the very end is
    // still captured.
    int cur_line = 0, cur_col = 0;
    int cursor_line = 0, cursor_col = 0;
    int captured = 0;
    for (int i = 0; i <= tb->count; i++) {
        if (i == tb->cursor && !captured) {
            cursor_line = cur_line;
            cursor_col = cur_col;
            captured = 1;
        }
        if (i == tb->count) break;
        char c = tb->buf[(tb->start + i) % SCROLLBACK_CAP].ch;
        if (c == '\n') {
            cur_line++;
            cur_col = 0;
            continue;
        }
        if (cur_col >= max_cols) {
            cur_line++;
            cur_col = 0;
        }
        cur_col++;
    }
    int total_lines = cur_line + 1;
    (void)cursor_col;

    // Scroll just enough to keep the cursor inside the visible window.
    // Deliberately stateless -- recomputed from tb->cursor on every
    // redraw, unlike widget_scrollback_draw()'s persistent
    // tb->scroll_offset (which exists for the GUI Terminal's mouse
    // wheel/scrollbar) -- the CLI editor has no such input, so there's
    // nothing for a remembered offset to do here.
    int first_line = 0;
    if (cursor_line >= visible_rows) first_line = cursor_line - visible_rows + 1;
    if (first_line > total_lines - visible_rows) first_line = total_lines - visible_rows;
    if (first_line < 0) first_line = 0;

    vga_clear();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    // Pass 2: walk the buffer again, only actually emitting characters
    // whose wrapped line falls inside [first_line, first_line +
    // visible_rows) -- everything outside that window is walked (to
    // keep line/col tracking correct) but not printed. Every '\n'
    // actually printed here also physically advances the console's own
    // row (via vga_putc()), so `line` stays in lockstep with the real
    // console cursor row for as long as we're inside the window -- see
    // the padding step below, which relies on that.
    int line = 0, col = 0;
    for (int i = 0; i <= tb->count; i++) {
        int in_window = (line >= first_line && line < first_line + visible_rows);
        int is_end = (i == tb->count);
        char c = is_end ? 0 : tb->buf[(tb->start + i) % SCROLLBACK_CAP].ch;
        int is_newline = (!is_end && c == '\n');

        if (in_window) {
            if (i == tb->cursor) {
                // A real character gets highlighted in place (no extra
                // column consumed). A '\n' or the very end of the
                // buffer has no glyph of its own to highlight, so a
                // highlighted space stands in for "the cursor is here"
                // instead.
                char show = (!is_end && !is_newline) ? c : ' ';
                vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
                vga_putc(show);
                vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
                if (is_newline) vga_putc('\n');
            } else if (!is_end) {
                vga_putc(c);
            }
        }

        if (is_end) break;
        if (is_newline) {
            line++;
            col = 0;
            if (line >= first_line + visible_rows) break;
            continue;
        }
        col++;
        if (col >= max_cols) {
            line++;
            col = 0;
            if (line >= first_line + visible_rows) break;
        }
    }

    // `line - first_line` is the console's actual current row within
    // the window, whether the loop above stopped because it ran out of
    // content (mid-row or freshly wrapped, either way `line` is exactly
    // right) or because it hit the window limit (in which case the '\n'
    // that crossed the limit was itself printed while still in-window,
    // so the console already sits at the start of that boundary row --
    // `line` reflects that too). One '\n' always advances to the start
    // of the next row regardless of which of those two cases it is, so
    // a single formula covers both: pad with exactly enough newlines to
    // reach the status row, no more.
    int console_row = line - first_line;
    if (console_row < 0) console_row = 0;
    if (console_row > visible_rows) console_row = visible_rows;
    for (int i = 0; i < visible_rows - console_row; i++) vga_putc('\n');

    // No trailing '\n' here on purpose -- see this function's top
    // comment (point 2). The console's own blinking cursor lands right
    // after this text on the last row instead of on a spurious blank
    // row below it.
    vga_write("-- ");
    vga_write(path);
    vga_write(" -- F2 Save  F3 Exit");
    if (status && status[0]) {
        vga_write("  -- ");
        vga_write(status);
    }
    vga_write(" --");
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
