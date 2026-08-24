// KTESTs for the keyboard tap. See kernel/include/kernel/keyboard_tap.h.
//
// WHAT THESE COVER THAT tools/kbd_test.py CANNOT: the ring WRAPPING.
// That tool types real keys through QMP and reads them back, which is
// the only way to prove the four columns mean what they say -- but it
// cannot press 300 keys, and wrap-around is exactly where an off-by-one
// in "the oldest retained record is `count` back from the newest" would
// live. Driving kbdtap_key() directly is what makes it reachable.
//
// **THEY WRITE INTO THE LIVE LOG**, which is not a mistake to fix: the
// tap has exactly one ring and there is no second one to test against.
// The cost is stated instead -- after `make test`, the first screenful
// of `kbd --last` is these fixtures rather than anything anyone typed.
// The sequence numbers stay monotonic across it, so nothing is
// corrupted; the log is simply older than it looks.
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
    kbdtap_key(0x1E, 0, 30, 1, 0x01 /* KEY_MOD_SHIFT */);
    kbdtap_produced('A');
    int n = tap_records();

    struct query_kbdtap r;
    int got = query_read(QUERY_KBDTAP, n - 1, &r, sizeof r);
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
    kbdtap_key(0, 0, 30, 1, 0);          // what input_report_key() does
    int n = tap_records();
    struct query_kbdtap r;
    int got = query_read(QUERY_KBDTAP, n - 1, &r, sizeof r);
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
    int before = tap_records();
    kbdtap_key(0x48, 1, 103, 1, 0);      // Up: e0 48 -> INPUT_KEY_UP
    kbdtap_produced(0x91);               // KEY_ARROW_UP
    int after = tap_records();
    struct query_kbdtap r;
    query_read(QUERY_KBDTAP, after - 1, &r, sizeof r);
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
    kbdtap_key(0x30, 0, 48, 1, 0x04 /* KEY_MOD_ALT */);
    kbdtap_produced(0x1B);               // ESC, then...
    kbdtap_produced('b');                // ...the key -- readline's meta prefix
    kbdtap_produced('!');                // one past the cap: must be dropped
    int n = tap_records();
    struct query_kbdtap r;
    query_read(QUERY_KBDTAP, n - 1, &r, sizeof r);
    scheduler_preempt_enable();

    KTEST_ASSERT(r.produced == QUERY_KBDTAP_PRODUCED_MAX);
    KTEST_ASSERT(r.produced_code[0] == 0x1B);
    KTEST_ASSERT(r.produced_code[1] == 'b');
}

KTEST("kbdtap", "the ring wraps: the count saturates and the oldest record moves") {
    scheduler_preempt_disable();

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
