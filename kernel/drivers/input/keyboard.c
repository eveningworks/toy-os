#include "keyboard.h"
#include "keyboard_layout.h"
#include "io.h"
#include "vga.h"
#include "klog.h"
#include "scheduler.h" // scheduler_idle(), and the fd-0 reader wake below
#include "syscall_abi.h" // SYS_RETRY -- the wake value a parked fd-0 read gets
#include "string.h" // k_tolower() -- the Ctrl-key fold
#include "tty.h" // tty_intr() -- what Ctrl-C means, see its own header

#define KBD_DATA_PORT 0x60

// Each slot is (mods << 16) | key -- see keyboard.h's "Modifier bits"
// comment. The KEY is unchanged from what this driver has always
// pushed (terminal-encoded: Ctrl-A is 0x01, Alt-B is ESC then 'b'), so
// every existing consumer that calls keyboard_getchar() and gets the
// low half back behaves exactly as before. The mods half is additional
// information for callers that need to tell Shift-Tab from Tab, which
// the terminal encoding genuinely cannot express.
static volatile uint32_t ring_buf[256];
static volatile unsigned int ring_head = 0;
static volatile unsigned int ring_tail = 0;
static int shift_pressed = 0;
static int altgr_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;   // LEFT Alt only -- right Alt is AltGr, see below
static int extended_prefix = 0;

#define LEFT_SHIFT_PRESS   0x2A
#define LEFT_SHIFT_RELEASE 0xAA
#define RIGHT_SHIFT_PRESS  0x36
#define RIGHT_SHIFT_RELEASE 0xB6

// Right Alt = AltGr on a PS/2 keyboard, sent as an 0xE0-prefixed
// (extended) scancode. Left Alt is the SAME 0x38/0xB8 byte pair
// without the prefix, which is what makes telling them apart free:
// the extended block below sees only AltGr, the plain path below sees
// only left Alt. That split matters here -- AltGr is a layout modifier
// (it picks a third character from the keyboard layout tables), while
// left Alt is readline's Meta. Conflating them would make `AltGr-b`
// try to be Meta-b on a Nordic layout.
#define RIGHT_ALT_PRESS    0x38
#define RIGHT_ALT_RELEASE  0xB8
#define LEFT_ALT_PRESS     0x38
#define LEFT_ALT_RELEASE   0xB8

// Left Ctrl is plain 0x1D/0x9D; right Ctrl is the same pair with an
// 0xE0 prefix. Both set the same state -- nothing here distinguishes
// them, same as the two Shift keys.
#define CTRL_PRESS         0x1D
#define CTRL_RELEASE       0x9D

// The modifiers physically held RIGHT NOW. Sampled by ring_push() at
// the moment a key is pushed -- i.e. at scancode-processing time, the
// same instant the layout table decides between 'a' and 'A'.
//
// That timing is the whole point, and is why this is captured here
// rather than exposed as a "what is held now?" query an app polls
// later: a modifier release racing a keypress then resolves the same
// way for the mods as it already does for the character itself. See
// docs/decisions.md -- the Shift+arrow family was given discrete codes
// for exactly this reason, and this generalises that decision rather
// than reversing it.
// Exposed as keyboard_mods_now() below. Static here because everything
// in this file wants the live value; the accessor exists for callers
// that have no KEY event to read modifiers off -- see keyboard.h.
static uint8_t current_mods(void) {
    uint8_t m = 0;
    if (shift_pressed) m |= KEY_MOD_SHIFT;
    if (ctrl_pressed) m |= KEY_MOD_CTRL;
    if (alt_pressed) m |= KEY_MOD_ALT;
    if (altgr_pressed) m |= KEY_MOD_ALTGR;
    return m;
}

static void ring_push(uint16_t c) {
    unsigned int next = (ring_head + 1) % 256;
    if (next == ring_tail) return; // full, drop
    ring_buf[ring_head] = ((uint32_t)current_mods() << 16) | c;
    ring_head = next;
    // Release anything parked in a ring-3 read of fd 0 (syscall_fd.c).
    // This runs in the IRQ1 handler, so it may only flip scheduler state
    // and write an already-saved trapframe -- which is all
    // scheduler_wake() does; see its own comment on that restraint. The
    // woken reader pops the key at the next ordinary rotation, not here.
    //
    // Waking unconditionally is deliberate: the ring is shared with the
    // ring-0 blocking reader and with win_input.c, so a woken reader may
    // find the key already gone. SYS_RETRY is the wake value, so that
    // case simply parks again rather than reporting a spurious EOF.
    scheduler_wake(SCHED_CHAN_KEY, SYS_RETRY);
}

static int ring_pop(uint32_t *out) {
    if (ring_tail == ring_head) return 0; // empty
    *out = ring_buf[ring_tail];
    ring_tail = (ring_tail + 1) % 256;
    return 1;
}

#define SC_ARROW_UP    0x48
#define SC_ARROW_DOWN  0x50
#define SC_PAGE_UP     0x49
#define SC_PAGE_DOWN   0x51
#define SC_ARROW_LEFT  0x4B
#define SC_ARROW_RIGHT 0x4D
#define SC_HOME        0x47
#define SC_END         0x4F
#define SC_DELETE      0x53

// The function keys, unlike the keys above, aren't 0xE0-prefixed
// extended scancodes -- they're plain scancodes like any letter key,
// just ones no layout table maps to anything (0x3C-0x3E and 0x44 are
// unmapped -- 0, same "nothing happens" as any other unmapped slot).
// Checked explicitly, before the layout translation, same as the shift
// keys below them.
//
// Only the four with callers exist: F2/F3 (the file manager), F10 (focus
// the menu bar) and F4 (Alt+F4 closes a window). Pushing them here and
// not through the layout translation is also what keeps Alt+F4 whole --
// it returns before the Alt-prefixes-with-ESC path below, so the key
// arrives once, with KEY_MOD_ALT set, rather than as ESC + something.
#define SC_F2 0x3C
#define SC_F3 0x3D
#define SC_F4  0x3E
#define SC_F10 0x44

// The Windows/Super/Meta keys, both 0xE0-prefixed. Their RELEASE
// codes (0xDB/0xDC) have bit 7 set, so the press-only guard below
// already excludes them -- the desktop wants the keypress, not a
// held-modifier state, since this key ACTS rather than modifies.
#define SC_SUPER_L 0x5B
#define SC_SUPER_R 0x5C

// Processes one byte already read from the 8042 by i8042_poll(). This
// must NOT read port 0x60 itself -- see i8042.h for why.
void keyboard_feed_byte(uint8_t sc) {

    if (sc == 0xE0) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        if (sc == RIGHT_ALT_PRESS) { altgr_pressed = 1; return; }
        if (sc == RIGHT_ALT_RELEASE) { altgr_pressed = 0; return; }
        if (sc == CTRL_PRESS) { ctrl_pressed = 1; return; }   // right Ctrl
        if (sc == CTRL_RELEASE) { ctrl_pressed = 0; return; }
        if (!(sc & 0x80)) { // key press, not release
            // Ctrl+Left/Right are word motion in every readline-ish
            // line editor, so they get their own codes -- exactly the
            // KEY_SHIFT_ARROW_* precedent right below, resolved here
            // from live modifier state at keypress time for the same
            // reason (see docs/decisions.md).
            if (ctrl_pressed && sc == SC_ARROW_LEFT) { ring_push(KEY_CTRL_ARROW_LEFT); return; }
            if (ctrl_pressed && sc == SC_ARROW_RIGHT) { ring_push(KEY_CTRL_ARROW_RIGHT); return; }
            // Shift+arrow/Home/End get their own codes, decided right
            // here from the live `shift_pressed` state -- same timing
            // as the ASCII table swap below for ordinary letter keys,
            // so a shift release racing the arrow keypress resolves the
            // same way either family of key already does.
            if (sc == SC_ARROW_UP) ring_push(shift_pressed ? KEY_SHIFT_ARROW_UP : KEY_ARROW_UP);
            else if (sc == SC_ARROW_DOWN) ring_push(shift_pressed ? KEY_SHIFT_ARROW_DOWN : KEY_ARROW_DOWN);
            else if (sc == SC_PAGE_UP) ring_push(KEY_PAGE_UP);
            else if (sc == SC_PAGE_DOWN) ring_push(KEY_PAGE_DOWN);
            else if (sc == SC_ARROW_LEFT) ring_push(shift_pressed ? KEY_SHIFT_ARROW_LEFT : KEY_ARROW_LEFT);
            else if (sc == SC_ARROW_RIGHT) ring_push(shift_pressed ? KEY_SHIFT_ARROW_RIGHT : KEY_ARROW_RIGHT);
            else if (sc == SC_HOME) ring_push(shift_pressed ? KEY_SHIFT_HOME : KEY_HOME);
            else if (sc == SC_END) ring_push(shift_pressed ? KEY_SHIFT_END : KEY_END);
            else if (sc == SC_DELETE) ring_push(KEY_DELETE);
            // Super/Win opens the Start menu, the way it does on
            // Windows and KDE. Both sides send the same code: no
            // desktop distinguishes them, and nothing here should
            // invent a distinction (same call the driver already
            // makes for left/right Ctrl).
            else if (sc == SC_SUPER_L || sc == SC_SUPER_R) ring_push(KEY_SUPER);
        }
        return;
    }

    if (sc == LEFT_SHIFT_PRESS || sc == RIGHT_SHIFT_PRESS) {
        shift_pressed = 1;
        return;
    }
    if (sc == LEFT_SHIFT_RELEASE || sc == RIGHT_SHIFT_RELEASE) {
        shift_pressed = 0;
        return;
    }
    if (sc == CTRL_PRESS) { ctrl_pressed = 1; return; }       // left Ctrl
    if (sc == CTRL_RELEASE) { ctrl_pressed = 0; return; }
    if (sc == LEFT_ALT_PRESS) { alt_pressed = 1; return; }    // Meta -- not AltGr, see above
    if (sc == LEFT_ALT_RELEASE) { alt_pressed = 0; return; }
    if (sc & 0x80) return; // other key releases ignored

    if (sc == SC_F2) { ring_push(KEY_F2); return; }
    if (sc == SC_F3) { ring_push(KEY_F3); return; }
    if (sc == SC_F4) { ring_push(KEY_F4); return; }
    if (sc == SC_F10) { ring_push(KEY_F10); return; }

    if (sc >= 128) return;
    char c = keyboard_layout_translate(sc, shift_pressed, altgr_pressed);
    if (!c) return;

    // Ctrl and Alt are encoded the way a real terminal encodes them --
    // see keyboard.h's "Ctrl and Alt" comment for the full reasoning.
    //
    // Ctrl folds a letter to its control code (Ctrl-A -> 0x01), which
    // is why Ctrl-H/I/J/M come out as backspace/tab/newline/return with
    // no special cases: in this encoding they ARE those keys, exactly as
    // in bash. Ctrl with anything that isn't a letter is dropped rather
    // than guessed at -- Ctrl-[ really is Esc on a physical terminal,
    // but nothing here wants that, and inventing codes for the rest
    // would be making up an encoding instead of following one.
    if (ctrl_pressed) {
        int lower = k_tolower((unsigned char)c);
        if (lower < 'a' || lower > 'z') return;
        uint16_t code = (uint16_t)(lower - 'a' + 1);
        // THE INTR KEY IS THE ONE CONTROL CODE THAT IS NOT JUST A BYTE.
        // With a job in the foreground of the console it interrupts that
        // job's process GROUP and is swallowed; with nothing running it
        // falls through and reaches the line editor as 0x03, which is
        // what abandons the line today. tty.h has both cases and says
        // why this recognition lives in the driver for now -- it belongs
        // to a line discipline, and there is not one yet.
        //
        // A SIGNAL, NOT A KILL, and that is forced rather than
        // stylistic: this runs in the keyboard IRQ, so tearing an
        // address space down here would call the heap underneath
        // whatever the CPU was doing. Sending sets a bit; the kernel
        // acts on it on the way back to ring 3 (kernel/signal.h).
        if (code == 0x03 && tty_intr()) return;
        ring_push(code);
        return;
    }

    // Alt (Meta) prefixes the key with ESC, so Alt-B arrives as the two
    // bytes 0x1B 'b'. Two pushes rather than one combined code: this is
    // what every terminal emulator sends, so the line editor's decoder
    // is the same one it would need for a real serial terminal anyway.
    if (alt_pressed) {
        ring_push(0x1B);
    }
    ring_push((uint8_t)c);
}

// See keyboard.h. Not static state the shell can reach around: the
// blocking readers below are the only consumers.
// TWO INDEPENDENT REASONS THE RING-0 BLOCKING READER STANDS DOWN, and
// they must not share one flag: a compositor holds the screen
// (win_server.c), or a ring-3 process is reading the console through
// fd 0 (syscall_fd.c). Both can be true at once, and with a single
// boolean whichever released second would hand the keyboard back while
// the other still owned it -- a shell executing keys typed at somebody
// else's prompt, which is the exact bug keyboard_suspend_blocking() was
// added to fix in the first place.
//
// So: two setters, one predicate. Everything that asks "may I take a
// key?" asks the predicate.
static int g_blocking_suspended;
static int g_console_claimed;

void keyboard_suspend_blocking(int on) { g_blocking_suspended = on ? 1 : 0; }
void keyboard_claim_console(int on)    { g_console_claimed = on ? 1 : 0; }
int  keyboard_console_claimed(void)    { return g_console_claimed; }
int  keyboard_compositor_owns(void)    { return g_blocking_suspended; }
int  keyboard_blocking_suspended(void) { return g_blocking_suspended || g_console_claimed; }

int keyboard_getchar(void) { return keyboard_getchar_mods(0); }

int keyboard_getchar_mods(uint8_t *out_mods) {
    uint32_t ev;
    for (;;) {
    // The suspend check comes FIRST and short-circuits, so a suspended
    // reader never pops -- it must not consume a key the compositor is
    // about to be given (win_input.c drains the same ring, from the
    // scheduler_idle() call below).
    while (keyboard_blocking_suspended() || !ring_pop(&ev)) {
        // hlt wakes on every interrupt, not just a real keypress -- most
        // commonly the 100Hz PIT tick -- so this is a convenient, cheap
        // place to drive the framebuffer console's blinking cursor while
        // otherwise idle waiting for input. vga_cursor_tick() gates its
        // own actual work internally, so calling it this often costs
        // nothing on the ticks where it doesn't toggle. debug_console_poll()
        // rides the same wakeup for the same reason -- this is the
        // physical shell's main idle point, so a serial debug session
        // stays responsive whenever nobody's actively typing at the
        // physical console (see docs/decisions.md for the honest
        // limitation: it does NOT get polled while a blocking command,
        // the GUI's own event loop, or a ring-3 process is running --
        // userland/wm/wm.c's loop covers the GUI case separately).
        // ...but ONLY WHEN THE CONSOLE OWNS THE SCREEN. A suspended
        // reader is one whose screen belongs to a compositor, and the
        // console's cursor tick and present both write to the
        // framebuffer -- so doing them anyway paints a blinking text
        // cursor on top of the desktop, at whatever cell the shell's
        // prompt left it. Reported from a screenshot: a blinking block
        // sitting on a desktop icon.
        //
        // This is the invariant scheduler_idle() already states -- that
        // console upkeep belongs to whoever owns the screen, which is
        // why the tick and the present are deliberately NOT part of it.
        // Suspending the READ was not enough; the loop body had to stop
        // drawing too.
        if (!keyboard_blocking_suspended()) {
            vga_cursor_tick();
        }
        // The kernel's idle work (scheduler.h) -- the serial debug
        // console, today. Runs either way: it is the one thing here that
        // is not the console's, and a suspended shell must still drain
        // the debug console and feed raw input to the compositor.
        scheduler_idle();
        // The physical console's flush point: it draws into a back
        // buffer and this is where "output is finished, we are waiting
        // for a human" is true, so it is where the screen catches up.
        // Cheap when nothing changed. See vga.h's vga_present().
        if (!keyboard_blocking_suspended()) {
            vga_present();
        }
        __asm__ volatile ("hlt");
    }

    // PageUp/PageDown scroll the console's history rather than reaching
    // the caller (see vga.h's scrollback section). Handled here, in the
    // BLOCKING reader, so it works at the shell prompt, in the CLI
    // editor, anywhere the kernel waits for a key -- and deliberately
    // NOT in keyboard_try_getchar(), which is what the window manager
    // polls: the GUI Terminal and Notepad have their own PageUp/PageDown
    // scrolling of their own widgets, and swallowing the keys here would
    // break both.
    int c = (int)(ev & 0xFFFF);
    if (c == KEY_PAGE_UP || c == KEY_PAGE_DOWN) {
        uint32_t page = vga_rows() > 2 ? vga_rows() - 2 : 1; // keep two lines of overlap
        if (c == KEY_PAGE_UP) vga_scroll_back((int)page);
        else vga_scroll_forward((int)page);
        continue; // keep waiting for a key the caller actually wants
    }
    if (out_mods) *out_mods = (uint8_t)(ev >> 16);
    return c;
    }
}

int keyboard_try_getchar(void) { return keyboard_try_getchar_mods(0); }

int keyboard_try_getchar_mods(uint8_t *out_mods) {
    uint32_t ev;
    if (!ring_pop(&ev)) return -1;
    if (out_mods) *out_mods = (uint8_t)(ev >> 16);
    return (int)(ev & 0xFFFF);
}

void keyboard_read_line(char *buf, unsigned int len) {
    unsigned int pos = 0;
    for (;;) {
        int c = keyboard_getchar();
        // Ignore special keys (arrows, F2/F3, ...) in this simple reader,
        // but let Nordic letters through -- they also live at codepoints
        // >= 128, just not in the KEY_* range those special keys use.
        if (c >= 128 && !IS_NORDIC_CHAR(c)) continue;

        if (c == '\n') {
            vga_putc('\n');
            break;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                vga_backspace();
            }
        } else if (pos < len - 1) {
            buf[pos++] = c;
            vga_putc(c);
        }
    }
    buf[pos] = '\0';
}

// The modifiers held RIGHT NOW, for a caller with no key event to read
// them off -- a mouse click, which carries no modifier state of its own.
// The desktop's Ctrl/Shift-drag is the first caller (rubber-band
// selection, userland/wm/desktop.c).
//
// Deliberately a live sample, not a latched value: it answers "what is
// held at this instant", which is the question a click has. That makes
// it wrong for keyboard input, where the modifiers that matter are the
// ones held when the KEY was pressed -- which is why key events carry
// their own mods (keyboard_try_getchar_mods()) rather than calling this.
uint8_t keyboard_mods_now(void) { return current_mods(); }
