// kbd -- what the keyboard actually did, at every stage at once.
//
// A keypress is FOUR encodings before anything acts on it: a PS/2
// scancode on the wire, an evdev keycode, whatever the layout file turns
// that into, and the modifiers held at that instant
// (kernel/include/kernel/input.h has the stages). A keyboard bug is
// nearly always one stage disagreeing with the next -- a hole in a
// translation table, a wrong row in /etc/kbs -- and from the outside
// every one of them looks the same: a key does nothing, or the wrong
// thing. This prints all four on one line, so the disagreement is
// visible rather than deduced.
//
// **IT IS NOT A KEY READER.** It never asks for the keystrokes it
// prints; the kernel keeps a rolling log of them whether or not anything
// is reading (kernel/keyboard_tap.h), and this walks that log through
// QUERY_KBDTAP. Two things follow. `kbd --last` can explain a key you
// pressed BEFORE you thought to run it, which is the case that actually
// comes up. And running it inside a Terminal window steals nothing from
// the desktop -- the compositor still gets every key, because this is
// looking at a record rather than at a queue.
//
// THE NEAREST THING ON LINUX is `showkey` (wire bytes or keycodes, by
// switching the console keyboard into a raw mode -- exclusive, and
// console-only) and `evtest` (the input core's own events, read from a
// device node -- non-exclusive, and sees everything). This is evtest's
// shape: toy-os has no device nodes, so the log is a query rather than a
// file, but nothing here takes the keyboard away from anyone.
//
// **THREE WAYS TO QUIT, AND THAT IS DELIBERATE.** This is a tool for a
// keyboard that is misbehaving, so a quit gesture that depends on a
// working key is not enough on its own: Esc twice in a row, Ctrl-C
// (which reaches a `$` prompt and not a `#` one -- see docs/commands/
// tty.md), or simply stop typing and let the idle timeout fire.
// showkey's own answer is the last of those, for the same reason.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <keyboard.h>    // KEY_* and the modifier bits, named below
#include <signal.h>      // SIGINT -- one of the three ways out
#include "query_abi.h"   // QUERY_KBDTAP and its record

// Records fetched in one poll. A burst faster than this is not lost --
// it is still in the kernel's ring and comes out on the next poll -- so
// this bounds one batch, not the tool.
//
// STATIC, NOT A LOCAL: ring-3 frames here are budgeted at 2048 bytes
// (-Wframe-larger-than, see CLAUDE.md), and 64 of these is 4.5 KB.
#define BATCH 64
static struct query_kbdtap g_batch[BATCH];

// --- naming a code ----------------------------------------------------

// The KEY_* family of api/keyboard.h, indexed by its own macros so the
// table cannot drift out of step with the header it is naming. Short
// names on purpose: this is a column, and "SHIFT-LEFT" beside "'a'" in
// the same column is what makes a log readable at a glance.
#define KEY_NAME_BASE KEY_ARROW_UP     // 0x91, the lowest KEY_* code
#define KEY_NAME_TOP  KEY_PRINT_SCREEN // 0xB9, the highest

static const char *const g_key_names[KEY_NAME_TOP - KEY_NAME_BASE + 1] = {
    [KEY_ARROW_UP        - KEY_NAME_BASE] = "UP",
    [KEY_ARROW_DOWN      - KEY_NAME_BASE] = "DOWN",
    [KEY_PAGE_UP         - KEY_NAME_BASE] = "PGUP",
    [KEY_PAGE_DOWN       - KEY_NAME_BASE] = "PGDN",
    [KEY_ARROW_LEFT      - KEY_NAME_BASE] = "LEFT",
    [KEY_ARROW_RIGHT     - KEY_NAME_BASE] = "RIGHT",
    [KEY_HOME            - KEY_NAME_BASE] = "HOME",
    [KEY_END             - KEY_NAME_BASE] = "END",
    [KEY_DELETE          - KEY_NAME_BASE] = "DEL",
    [KEY_F2              - KEY_NAME_BASE] = "F2",
    [KEY_F3              - KEY_NAME_BASE] = "F3",
    [KEY_SHIFT_ARROW_LEFT  - KEY_NAME_BASE] = "S-LEFT",
    [KEY_SHIFT_ARROW_RIGHT - KEY_NAME_BASE] = "S-RIGHT",
    [KEY_SHIFT_ARROW_UP    - KEY_NAME_BASE] = "S-UP",
    [KEY_SHIFT_ARROW_DOWN  - KEY_NAME_BASE] = "S-DOWN",
    [KEY_SHIFT_HOME        - KEY_NAME_BASE] = "S-HOME",
    [KEY_SHIFT_END         - KEY_NAME_BASE] = "S-END",
    [KEY_CTRL_ARROW_LEFT   - KEY_NAME_BASE] = "C-LEFT",
    [KEY_CTRL_ARROW_RIGHT  - KEY_NAME_BASE] = "C-RIGHT",
    [KEY_F10             - KEY_NAME_BASE] = "F10",
    [KEY_F4              - KEY_NAME_BASE] = "F4",
    [KEY_SUPER           - KEY_NAME_BASE] = "SUPER",
    // The four modifier keys never reach the byte stream at all (they
    // ride the transition queue instead, see api/keyboard.h), so they
    // cannot appear in this column. Named anyway: the table is indexed,
    // so a hole here would be a NULL entry to guard rather than a name
    // to print, and the gap would read as an oversight.
    [KEY_SHIFT           - KEY_NAME_BASE] = "SHIFT",
    [KEY_CTRL            - KEY_NAME_BASE] = "CTRL",
    [KEY_ALT             - KEY_NAME_BASE] = "ALT",
    [KEY_ALTGR           - KEY_NAME_BASE] = "ALTGR",
    [KEY_F1              - KEY_NAME_BASE] = "F1",
    [KEY_F5              - KEY_NAME_BASE] = "F5",
    [KEY_F6              - KEY_NAME_BASE] = "F6",
    [KEY_F7              - KEY_NAME_BASE] = "F7",
    [KEY_F8              - KEY_NAME_BASE] = "F8",
    [KEY_F9              - KEY_NAME_BASE] = "F9",
    [KEY_F11             - KEY_NAME_BASE] = "F11",
    [KEY_F12             - KEY_NAME_BASE] = "F12",
    [KEY_INSERT          - KEY_NAME_BASE] = "INS",
    [KEY_MENU            - KEY_NAME_BASE] = "MENU",
    [KEY_CAPS_LOCK       - KEY_NAME_BASE] = "CAPS",
    [KEY_NUM_LOCK        - KEY_NAME_BASE] = "NUMLK",
    [KEY_SCROLL_LOCK     - KEY_NAME_BASE] = "SCRLK",
    [KEY_PAUSE           - KEY_NAME_BASE] = "PAUSE",
    [KEY_PRINT_SCREEN    - KEY_NAME_BASE] = "PRTSC",
};

// One code, as a person would say it. The control codes get their
// terminal names rather than ^-forms where they have one, because
// Ctrl-I genuinely IS Tab in this encoding (api/keyboard.h) and calling
// it "^I" would suggest a distinction the wire does not carry.
static void code_name(unsigned code, char *out, int cap) {
    if (code >= 32 && code < 127) { snprintf(out, cap, "'%c'", (char)code); return; }
    if (IS_NORDIC_CHAR(code))     { snprintf(out, cap, "'%c'", (char)code); return; }
    switch (code) {
    case 0x08: snprintf(out, cap, "BS");  return;
    case 0x09: snprintf(out, cap, "TAB"); return;
    case 0x0A: snprintf(out, cap, "LF");  return;
    case 0x0D: snprintf(out, cap, "CR");  return;
    case 0x1B: snprintf(out, cap, "ESC"); return;
    case 0x7F: snprintf(out, cap, "DEL"); return;
    default: break;
    }
    if (code >= 1 && code <= 26) { snprintf(out, cap, "^%c", (char)('A' + code - 1)); return; }
    if (code >= KEY_NAME_BASE && code <= KEY_NAME_TOP) {
        const char *n = g_key_names[code - KEY_NAME_BASE];
        if (n) { snprintf(out, cap, "%s", n); return; }
    }
    snprintf(out, cap, "0x%02x", code);
}

// The `produced` column: what this event put into the console byte
// stream. Empty for a modifier, for a release, and for a key this layout
// maps nothing to -- three different things that all legitimately type
// nothing, which is why the column says "--" rather than being blank.
static void produced_text(const struct query_kbdtap *r, char *out, int cap) {
    if (r->produced == 0) { snprintf(out, cap, "--"); return; }
    char a[16], b[16];
    code_name((unsigned)r->produced_code[0], a, sizeof a);
    if (r->produced < 2) { snprintf(out, cap, "%s", a); return; }
    code_name((unsigned)r->produced_code[1], b, sizeof b);
    snprintf(out, cap, "%s %s", a, b);
}

static void print_header(void) {
    sys_print("  seq   +ms  scan   code  produced       mods  edge\n");
}

static void print_record(const struct query_kbdtap *r, unsigned long prev_ticks) {
    char scan[8], prod[24], line[128], gap[8];

    // THE WIRE COLUMN IS BLANK FOR A REASON WORTH READING, not because
    // something failed: a key that did not arrive over PS/2 has no
    // scancode to report, and this is how you tell a virtio-input or USB
    // keypress from an 8042 one at a glance.
    if (r->wire == 0)
        snprintf(scan, sizeof scan, "--");
    else if (r->flags & QUERY_KBDTAP_EXTENDED)
        snprintf(scan, sizeof scan, "e0 %02x", (unsigned)r->wire);
    else
        snprintf(scan, sizeof scan, "%02x", (unsigned)r->wire);

    // Milliseconds since the PREVIOUS event, which is how autorepeat
    // reads as autorepeat rather than as a very fast typist. The clock
    // is the 100 Hz PIT, so the resolution is 10ms and nothing finer is
    // being claimed.
    if (prev_ticks == 0 || r->ticks < prev_ticks)
        snprintf(gap, sizeof gap, "-");
    else
        snprintf(gap, sizeof gap, "%lu", (unsigned long)(r->ticks - prev_ticks) * 10ul);

    produced_text(r, prod, sizeof prod);

    snprintf(line, sizeof line, "%5lu %5s  %-5s  %4lu  %-13s  %c%c%c%c  %s\n",
             (unsigned long)r->seq, gap, scan, (unsigned long)r->keycode, prod,
             (r->mods & KEY_MOD_SHIFT) ? 'S' : '-',
             (r->mods & KEY_MOD_CTRL)  ? 'C' : '-',
             (r->mods & KEY_MOD_ALT)   ? 'A' : '-',
             (r->mods & KEY_MOD_ALTGR) ? 'G' : '-',
             (r->flags & QUERY_KBDTAP_DOWN) ? "down" : "up");
    sys_print(line);
}

// --- reading the kernel's ring ---------------------------------------

static int record_at(int index, struct query_kbdtap *r) {
    return sys_query_record(QUERY_KBDTAP, index, r, sizeof *r) >= (int)sizeof *r;
}

// How many records the tap is holding RIGHT NOW: the first index that is
// refused, found by doubling and then bisecting.
//
// **NOT THROUGH QUERY_PROVIDERS, WHICH IS THE OBVIOUS WAY AND IS A
// TRAP.** Class 0 reports every class's `count`, so one read of it looks
// like exactly the right answer for one syscall -- but filling a
// provider-info record CALLS that provider's count(), and some of those
// do real work (QUERY_HEAPCHECK scans the kernel heap, QUERY_MMAUDIT
// walks live page tables). At the 50 Hz this loop polls at, asking the
// registry a question costs a heap scan fifty times a second, which is
// what the first version of this did: `kbd` did not appear to hang so
// much as make the machine stop answering.
//
// ~16 reads of a ring buffer instead, and no baked-in ring size -- the
// upper bound is discovered, so growing the kernel's ring needs no edit
// here.
static int tap_count(void) {
    struct query_kbdtap r;
    if (!record_at(0, &r)) return 0;
    int lo = 0, hi = 1;
    while (record_at(hi, &r)) {
        lo = hi;
        hi *= 2;
        if (hi > 65536) return hi;   // a ring this size is not a thing
    }
    while (hi - lo > 1) {
        int mid = lo + (hi - lo) / 2;
        if (record_at(mid, &r)) lo = mid; else hi = mid;
    }
    return lo + 1;
}

// Everything with a sequence number above `after`, newest-first into
// g_batch. Returns how many were collected.
//
// WALKED BACKWARDS FROM THE END, and that is not an optimisation. The
// ring is fed from an interrupt handler, so it can move between two of
// these reads; walking forward from a remembered index would drift, and
// walking forward from 0 would re-read the whole ring to find the two
// new records at the end of it. Backwards, the stop condition is the
// data itself -- the first record we have already seen.
static int fetch_since(unsigned long long after, int count) {
    int n = 0;
    for (int i = count - 1; i >= 0 && n < BATCH; i--) {
        struct query_kbdtap r;
        if (!record_at(i, &r)) break;
        if (r.seq <= after) break;
        g_batch[n++] = r;
    }
    return n;
}

// Print g_batch[0..n-1] in the order the keys were pressed, and return
// the highest sequence number printed. `prev` carries the tick count of
// the last line printed so the gap column spans a poll boundary.
static unsigned long long print_batch(int n, unsigned long *prev,
                                      unsigned long long last_seq) {
    // A GAP IN THE SEQUENCE IS SAID OUT LOUD. The ring holds 256 events;
    // anything that overran it while we were not looking is gone, and a
    // log that silently skipped from 40 to 300 would be worse than one
    // that admits it -- the whole value of this tool is being trusted
    // about what did and did not happen.
    if (n > 0 && last_seq != 0) {
        unsigned long long oldest = g_batch[n - 1].seq;
        if (oldest > last_seq + 1) {
            char msg[64];
            snprintf(msg, sizeof msg, "  ... %lu events lost\n",
                     (unsigned long)(oldest - last_seq - 1));
            sys_print(msg);
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        print_record(&g_batch[i], *prev);
        *prev = (unsigned long)g_batch[i].ticks;
        last_seq = g_batch[i].seq;
    }
    return last_seq;
}

// --- live mode --------------------------------------------------------

static volatile int g_interrupted;
static void on_intr(int sig) { (void)sig; g_interrupted = 1; }

// **NOTHING HERE EVER READS THE KEYBOARD, INCLUDING ON THE WAY OUT.**
// So keys pressed during a live session are still queued for whoever
// reads fd 0 next, and the shell gets them when its prompt comes back --
// exactly as it would for keys typed during any other command, and
// exactly what evtest does. A version of this drained fd 0 at exit to
// tidy that up, and it was wrong twice over: fd 0's non-blocking flag
// lives on the DESCRIPTION and so is shared with the shell (rt/sys.h),
// and a console read PARKS while a compositor owns the keyboard
// (api/keyboard.h) whether or not the descriptor says non-blocking -- so
// the tidy-up hung the tool on precisely the ordinary graphical boot it
// is most useful on. Not touching input is not a limitation here; it is
// the property that lets this run inside a Terminal window without
// stealing a key from the desktop.

static int live(int timeout_s) {
    // **LIVE MODE NEEDS A SCHEDULER SLOT, AND SAYS SO INSTEAD OF HANGING
    // THE MACHINE.** Started through the legacy `run` loader -- which is
    // what a bare name at the kernel's `#` prompt uses -- there is no
    // slot to park, so SYS_SLEEP is refused (-EPERM) AND the monotonic
    // clock stands still, because that context never reaches a timer
    // tick. A poll loop then spins at full speed against a deadline that
    // can never arrive and takes the whole machine with it: measured,
    // three times, before this guard existed. /bin/less carries the same
    // warning in prose; prose is not enough when the failure mode is a
    // dead machine rather than a slow pager. `--last` needs none of this
    // and works anywhere.
    if (sys_sleep_ms(0) < 0) {
        sys_print("kbd: live mode needs a scheduler slot -- use `spawn /bin/kbd`,\n"
                  "     or run it from a $ prompt. `kbd --last` works anywhere.\n");
        return 1;
    }
    sys_signal(SIGINT, on_intr);
    char intro[128];
    snprintf(intro, sizeof intro,
             "Press keys. Esc twice, Ctrl-C, or %ds without a keypress to quit.\n",
             timeout_s);
    sys_print(intro);
    print_header();

    int count = tap_count();

    // START AT THE END: live mode shows what happens NEXT, and the
    // history is what --last is for. Without this, the first screenful
    // is always the command you just typed.
    unsigned long long last_seq = 0;
    if (count > 0) {
        struct query_kbdtap r;
        if (record_at(count - 1, &r)) last_seq = r.seq;
    }

    unsigned long prev_ticks = 0;
    int esc_run = 0;                 // consecutive bare-Esc presses

    // THE IDLE TIMEOUT IS MEASURED AGAINST A CLOCK, not counted in
    // polls. Counting polls quietly assumes the sleep between them
    // works, and under the legacy `run` loader it does NOT -- SYS_SLEEP
    // refuses a caller with no scheduler slot (-EPERM), so `run kbd`
    // would spin and call its ten seconds up in a few milliseconds while
    // `spawn kbd` waited the ten seconds. One number meaning two very
    // different durations depending on how the program was started is
    // the kind of thing nobody debugs twice.
    unsigned long long deadline = sys_monotonic_ns()
                                + (unsigned long long)timeout_s * 1000000000ull;

    while (!g_interrupted) {
        count = tap_count();
        int n = count > 0 ? fetch_since(last_seq, count) : 0;
        if (n == 0) {
            if (sys_monotonic_ns() >= deadline) break;
            sys_sleep_ms(20);
            continue;
        }
        deadline = sys_monotonic_ns()
                 + (unsigned long long)timeout_s * 1000000000ull;

        // Esc twice in a row, counted from the LOG rather than from a
        // read of fd 0 -- which is what lets this work identically at a
        // `#` prompt, at a `$` prompt and inside a Terminal window,
        // none of which agree about who owns the keyboard. A bare Esc
        // only: Alt-<key> is encoded as ESC then the key and arrives as
        // one event producing two codes, so it cannot be mistaken for
        // one here.
        for (int i = n - 1; i >= 0; i--) {
            const struct query_kbdtap *r = &g_batch[i];
            if (!(r->flags & QUERY_KBDTAP_DOWN)) continue;
            if (r->produced == 1 && r->produced_code[0] == 0x1B) esc_run++;
            else if (r->produced > 0) esc_run = 0;
        }
        last_seq = print_batch(n, &prev_ticks, last_seq);
        if (esc_run >= 2) break;
    }

    sys_print("\n");
    return 0;
}

// --- history mode -----------------------------------------------------

static int last(int want) {
    int count = tap_count();
    if (count == 0) {
        sys_print("kbd: no key events recorded yet\n");
        return 0;
    }
    if (want > count) want = count;

    print_header();
    unsigned long prev_ticks = 0;
    for (int i = count - want; i < count; i++) {
        struct query_kbdtap r;
        if (!record_at(i, &r)) break;
        print_record(&r, prev_ticks);
        prev_ticks = (unsigned long)r.ticks;
    }
    return 0;
}

int main(int argc, char **argv) {
    int want_last = 0, n = 20, timeout_s = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--last") == 0) {
            want_last = 1;
            // An optional count, so `kbd --last` alone means the
            // default rather than an error.
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else {
            cmd_usage("kbd [--last [count]] [--timeout <seconds>]");
            return 1;
        }
    }
    if (n <= 0) n = 20;
    if (timeout_s <= 0) timeout_s = 10;
    if (n > BATCH * 4) n = BATCH * 4;

    return want_last ? last(n) : live(timeout_s);
}
