// A freestanding userland test program that exercises the four newest
// syscalls (SYS_UNLINK, SYS_LISTDIR, SYS_GETTIME, SYS_YIELD -- see
// syscall_abi.h) against the in-memory filesystem, the RTC/timezone
// code, and the scheduler. Each phase prints what it's doing and
// whether it checked out, then exits 0 if every phase passed, or 1 if
// any phase failed -- same pass/fail-by-exit-code convention as
// file_test.c.
//
// Deliberately runs OUTSIDE the preemptive scheduler (loaded via
// process_run_ring3(), same as file_test.c/syscall_test.c -- see
// kernel/core/newsyscalls_test.c), so the SYS_YIELD phase is checking
// the "no-op when there's nothing to yield to" path, not an actual
// context switch -- see that phase's comment for why that's still a
// meaningful thing to verify.
#include <stdint.h>
#include "sys.h"
#include "timer.h" // struct rtc_time, shared with the kernel's SYS_GETTIME handler

























static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys_write(1, s, my_strlen(s));
}

// Minimal unsigned-decimal printer -- every earlier userland test that
// needed numbers (counter_a.c/counter_b.c) either wrote raw digits or
// used SYS_WRITE with a fixed string; this is the first one that needs
// small two/four-digit numbers (clock fields, entry counts), so it gets
// its own tiny helper rather than pulling in a real printf.
static void put_udec(uint32_t v, int min_digits) {
    char buf[10];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n < min_digits) { buf[n++] = '0'; } // pad AFTER digits, reversed below
    // buf currently holds digits least-significant-first (plus zero
    // padding at the end, which is really the most-significant end once
    // reversed) -- print back to front, so e.g. put_udec(5, 2) prints
    // "05" not "50".
    for (int i = n - 1; i >= 0; i--) {
        char c = buf[i];
        sys_write(1, &c, 1);
    }
}

#define TESTFILE "/newsyscalls_test.txt"

int main(void) {
    int all_ok = 1;

    // --- Phase 1: SYS_UNLINK -----------------------------------------
    put("newsyscalls_test: unlink phase\n");
    {
        int64_t fd = sys_open(TESTFILE, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
        int ok = (fd >= 0);
        if (ok) sys_close((int)fd);

        int64_t unlinked = ok ? sys_unlink(TESTFILE) : 0;
        ok = ok && (unlinked == 1);

        // Reading it back should now fail (open without O_CREAT on a
        // gone file) -- proves the delete was real, not just a
        // truthy-looking return value.
        int64_t reopened = sys_open(TESTFILE, 0);
        ok = ok && (reopened < 0);

        if (ok) put("  OK: created, deleted, and confirmed gone\n");
        else { put("  FAIL: unlink round-trip didn't check out\n"); all_ok = 0; }
    }

    // --- Phase 2: SYS_LISTDIR -----------------------------------------
    put("newsyscalls_test: listdir phase\n");
    {
        // /etc always exists by boot time (kernel_main creates it right
        // after fs_init(), before font_config_init()/tz_init() write
        // their own files into it -- see kernel.c) and has at least one
        // file in it on any system that's ever set a timezone or font
        // size, but don't depend on that -- an empty result is also a
        // legitimate pass as long as the call itself succeeds (RAX >= 0).
        struct dirent entries[SYS_LISTDIR_MAX];
        int64_t count = sys_listdir("/etc", entries, SYS_LISTDIR_MAX);
        int ok = (count >= 0);
        if (ok) {
            put("  OK: /etc has ");
            put_udec((uint32_t)count, 0);
            put(" entries: ");
            for (int64_t i = 0; i < count; i++) {
                if (i > 0) put(", ");
                put(entries[i].name);
            }
            put("\n");
        } else {
            put("  FAIL: listdir() returned an error\n");
            all_ok = 0;
        }
    }

    // --- Phase 3: SYS_GETTIME ------------------------------------------
    put("newsyscalls_test: gettime phase\n");
    {
        struct rtc_time t;
        int64_t got = sys_gettime(&t);
        // Sanity-range the fields rather than checking an exact value
        // (there's no way to know what time it'll be when this runs) --
        // same "structurally plausible" bar file_test.c's byte-compare
        // holds real data to, just shaped for a clock instead of a file.
        int ok = (got == 1) && t.hour < 24 && t.minute < 60 && t.second < 60 &&
                  t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31;
        if (ok) {
            put("  OK: ");
            put_udec(t.year, 4); put("-");
            put_udec(t.month, 2); put("-");
            put_udec(t.day, 2); put(" ");
            put_udec(t.hour, 2); put(":");
            put_udec(t.minute, 2); put(":");
            put_udec(t.second, 2); put("\n");
        } else {
            put("  FAIL: gettime() returned implausible fields\n");
            all_ok = 0;
        }
    }

    // --- Phase 4: SYS_YIELD --------------------------------------------
    put("newsyscalls_test: yield phase\n");
    {
        // This process was loaded via process_run_ring3(), NOT the
        // preemptive scheduler (see kernel/core/newsyscalls_test.c) --
        // so there's no sibling process for the kernel to switch to, and
        // this is really testing the no-op path: that calling
        // SYS_YIELD with nothing else runnable returns cleanly (0) and
        // control comes straight back here, rather than hanging or
        // corrupting this process's own state. A real "does it actually
        // switch" test lives at the scheduler layer (counter_a.c /
        // counter_b.c's preemptive demo, see scheduler.c) -- this is the
        // syscall-ABI-level check that a yield with no other process
        // ready behaves exactly like scheduler_tick() already promises.
        // The raw hatch, not sys_yield(): this test asserts on the
        // syscall's RETURN VALUE, which the typed wrapper discards
        // because no ordinary caller has any use for it. Poking the raw
        // ABI is what this binary is for -- see sys.h.
        int64_t ret = sys_call(SYS_YIELD, 0, 0, 0);
        int ok = (ret == 0);
        put("  after yield, still running -- return value: ");
        put_udec(ret < 0 ? (uint32_t)(-ret) : (uint32_t)ret, 0);
        put(ok ? " (OK)\n" : " (FAIL, expected 0)\n");
        if (!ok) all_ok = 0;
    }

    if (all_ok) {
        put("newsyscalls_test: all phases passed\n");
        sys_exit(0);
    } else {
        put("newsyscalls_test: at least one phase FAILED\n");
        sys_exit(1);
    }
}
