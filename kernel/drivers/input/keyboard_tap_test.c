// KTESTs for the keyboard tap. See kernel/include/kernel/keyboard_tap.h.
//
// WHAT THESE COVER THAT tools/kbd_test.py CANNOT: the ring WRAPPING.
// That tool types real keys through QMP and reads them back, which is
// the only way to prove the four columns mean what they say -- but it
// cannot press 300 keys, and wrap-around is exactly where an off-by-one
// in "the oldest retained record is `count` back from the newest" would
// live. Driving kbdtap_key() directly is what makes it reachable.
//
// **THEY WRITE INTO THE LIVE LOG, AND THEY TURN THE TAP ON TO DO IT**,
// which is not a mistake to fix: the tap has exactly one ring and there
// is no second one to test against. Each one saves the tap's real state
// and restores it, so a run leaves the machine as it found it -- and
// since disabling WIPES, a machine whose tap was off ends the suite with
// an empty ring rather than with these fixtures in it. On a machine
// where the tap was deliberately left on, `make test` costs that
// session's captured history; that is the honest price and it is stated
// here rather than discovered.
//
// Preemption is disabled around each one, for the reason input_test.c
// states beside it: a real keystroke landing between the writes and the
// reads would be indistinguishable from a wrap bug.
#include "keyboard_tap.h"
#include "query.h"
#include "scheduler.h"
#include "ktest.h"

// Comfortably more than the ring holds, so the wrap is forced rather
// than hoped for. Not derived from TAP_MAX: that is private to
// keyboard_tap.c, and a test that imported it would agree with an
// off-by-one in the implementation instead of catching it.
#define BEYOND_ANY_RING 600

// Arm the tap for a test, returning what it was so the test can put it
// back. Enabling wipes, so every test starts from an empty ring whatever
// ran before it -- which is the precondition each one would otherwise
// have to establish for itself.
static int tap_arm(void) {
    int was = kbdtap_enabled();
    kbdtap_set_enabled(1);
    return was;
}

static void tap_restore(int was) { kbdtap_set_enabled(was); }

// The tap's own count, the way any reader gets it -- by asking for
// records until one is refused. Bounded well above any plausible ring.
static int tap_records(void) {
    struct query_kbdtap r;
    int n = 0;
    while (n < 4096 && query_read(QUERY_KBDTAP, n, &r, sizeof r) == (int)sizeof r)
        n++;
    return n;
}

KTEST("kbdtap", "a key event is recorded with every stage it passed") {
    scheduler_preempt_disable();
    int was = tap_arm();
    kbdtap_key(0x1E, 0, 30, 1, 0x01 /* KEY_MOD_SHIFT */);
    kbdtap_produced('A');
    int n = tap_records();

    struct query_kbdtap r;
    int got = query_read(QUERY_KBDTAP, n - 1, &r, sizeof r);
    tap_restore(was);
    scheduler_preempt_enable();

    KTEST_ASSERT(got == (int)sizeof r);
    KTEST_ASSERT(r.wire == 0x1E);
    KTEST_ASSERT(r.keycode == 30);
    KTEST_ASSERT(r.mods == 0x01);
    KTEST_ASSERT(r.flags & QUERY_KBDTAP_DOWN);
    KTEST_ASSERT(!(r.flags & QUERY_KBDTAP_EXTENDED));
    KTEST_ASSERT(r.produced == 1);
    KTEST_ASSERT(r.produced_code[0] == 'A');
}

KTEST("kbdtap", "a key with no scancode reports no scancode, not a zero-ish one") {
    scheduler_preempt_disable();
    int was = tap_arm();
    kbdtap_key(0, 0, 30, 1, 0);          // what input_report_key() does
    int n = tap_records();
    struct query_kbdtap r;
    int got = query_read(QUERY_KBDTAP, n - 1, &r, sizeof r);
    tap_restore(was);
    scheduler_preempt_enable();

    KTEST_ASSERT(got == (int)sizeof r);
    // The distinction the whole scancode column exists to make: the
    // keycode arrived and the wire did not.
    KTEST_ASSERT(r.keycode == 30);
    KTEST_ASSERT(r.wire == 0);
    KTEST_ASSERT(r.produced == 0);
}

KTEST("kbdtap", "an extended key carries the prefix as a flag, not as a record") {
    scheduler_preempt_disable();
    int was = tap_arm();
    int before = tap_records();
    kbdtap_key(0x48, 1, 103, 1, 0);      // Up: e0 48 -> INPUT_KEY_UP
    kbdtap_produced(0x91);               // KEY_ARROW_UP
    int after = tap_records();
    struct query_kbdtap r;
    query_read(QUERY_KBDTAP, after - 1, &r, sizeof r);
    tap_restore(was);
    scheduler_preempt_enable();

    // ONE record for a two-byte sequence. Nothing happened when the
    // prefix arrived, so a reader counting keypresses must not see two.
    KTEST_ASSERT(after == before + 1 || after == before);  // `before` may be capped
    KTEST_ASSERT(r.flags & QUERY_KBDTAP_EXTENDED);
    KTEST_ASSERT(r.wire == 0x48);
    KTEST_ASSERT(r.keycode == 103);
}

KTEST("kbdtap", "one event carries both codes of an Alt-<key> sequence") {
    scheduler_preempt_disable();
    int was = tap_arm();
    kbdtap_key(0x30, 0, 48, 1, 0x04 /* KEY_MOD_ALT */);
    kbdtap_produced(0x1B);               // ESC, then...
    kbdtap_produced('b');                // ...the key -- readline's meta prefix
    kbdtap_produced('!');                // one past the cap: must be dropped
    int n = tap_records();
    struct query_kbdtap r;
    query_read(QUERY_KBDTAP, n - 1, &r, sizeof r);
    tap_restore(was);
    scheduler_preempt_enable();

    KTEST_ASSERT(r.produced == QUERY_KBDTAP_PRODUCED_MAX);
    KTEST_ASSERT(r.produced_code[0] == 0x1B);
    KTEST_ASSERT(r.produced_code[1] == 'b');
}

KTEST("kbdtap", "the ring wraps: the count saturates and the oldest record moves") {
    scheduler_preempt_disable();
    int was = tap_arm();

    // Fill well past any ring size, with the keycode carrying the
    // iteration number. AN ADDRESS-DERIVED PATTERN, the same reasoning
    // /tests/memtest uses: a constant fill cannot tell a correct wrap
    // from one that returns the wrong slot, because every slot holds
    // the same value and both look perfect.
    for (int i = 0; i < BEYOND_ANY_RING; i++)
        kbdtap_key(0, 0, (uint16_t)(i & 0x7FF), 1, 0);

    int n = tap_records();
    struct query_kbdtap oldest, newest;
    int a = query_read(QUERY_KBDTAP, 0, &oldest, sizeof oldest);
    int b = query_read(QUERY_KBDTAP, n - 1, &newest, sizeof newest);

    // Every record in order, checked as one walk rather than at the two
    // ends: a wrap that returned the right first and last record and
    // scrambled the middle would pass an endpoints-only check.
    int contiguous = 1;
    struct query_kbdtap prev = oldest, cur;
    for (int i = 1; i < n; i++) {
        if (query_read(QUERY_KBDTAP, i, &cur, sizeof cur) != (int)sizeof cur) {
            contiguous = 0;
            break;
        }
        if (cur.seq != prev.seq + 1) { contiguous = 0; break; }
        prev = cur;
    }
    tap_restore(was);
    scheduler_preempt_enable();

    KTEST_ASSERT(a == (int)sizeof oldest && b == (int)sizeof newest);
    // SATURATED, not grown: the ring is bounded, so writing 600 records
    // must leave fewer than 600 readable.
    KTEST_ASSERT(n < BEYOND_ANY_RING);
    KTEST_ASSERT(n > 1);
    // The oldest retained is exactly `n` back from the newest -- the
    // arithmetic the whole indexing rests on.
    KTEST_ASSERT(newest.seq == oldest.seq + (uint64_t)(n - 1));
    KTEST_ASSERT(contiguous);
    // And the newest really is the last thing written, not a stale slot
    // the wrap walked past.
    KTEST_ASSERT(newest.keycode == ((BEYOND_ANY_RING - 1) & 0x7FF));
}

// **THE CHECK THE PRIVACY DEFAULT RESTS ON.** Everything above turns the
// tap on first, so nothing above it can tell a working gate from an
// absent one -- and "off" is the state the machine ships in. If this
// ever passes vacuously, a public build is recording keystrokes.
KTEST("kbdtap", "while the tap is off it records NOTHING") {
    scheduler_preempt_disable();
    int was = kbdtap_enabled();

    // On first, and written to, so the ring demonstrably CAN hold
    // something -- a check that starts from an empty ring cannot tell
    // "the gate works" from "nothing was written either way".
    kbdtap_set_enabled(1);
    kbdtap_key(0x1E, 0, 30, 1, 0);
    kbdtap_produced('a');
    int while_on = tap_records();

    kbdtap_set_enabled(0);
    int after_off = tap_records();          // the WIPE
    kbdtap_key(0x1E, 0, 30, 1, 0);          // and the GATE
    kbdtap_produced('a');
    int while_off = tap_records();

    kbdtap_set_enabled(was);
    scheduler_preempt_enable();

    KTEST_ASSERT(while_on > 0);     // the positive control, inline
    KTEST_ASSERT(after_off == 0);   // disabling wiped what was there
    KTEST_ASSERT(while_off == 0);   // and nothing is recorded while off
}

// Turning it off has to erase, not merely stop -- a switch that leaves
// the last hundred keystrokes readable is decorative. Checked against
// the RECORD's own bytes rather than only against the count, since a
// count of zero would be satisfied by a bookkeeping reset that left the
// data in place for the next wrap to expose.
KTEST("kbdtap", "disabling wipes the ring rather than just stopping it") {
    scheduler_preempt_disable();
    int was = kbdtap_enabled();

    kbdtap_set_enabled(1);
    for (int i = 0; i < 8; i++) {
        kbdtap_key(0x1E, 0, 30, 1, 0);
        kbdtap_produced((uint16_t)('a' + i));
    }
    int filled = tap_records();

    kbdtap_set_enabled(0);
    kbdtap_set_enabled(1);   // back on: the ring must be empty, not stale
    int after = tap_records();

    // And whatever the ring does hand out now must not be the old data.
    struct query_kbdtap r;
    int leaked = 0;
    kbdtap_key(0, 0, 30, 1, 0);              // one fresh record to read
    if (query_read(QUERY_KBDTAP, 0, &r, sizeof r) == (int)sizeof r)
        leaked = (r.produced != 0);

    kbdtap_set_enabled(was);
    scheduler_preempt_enable();

    KTEST_ASSERT(filled == 8);
    KTEST_ASSERT(after == 0);
    KTEST_ASSERT(!leaked);
}
