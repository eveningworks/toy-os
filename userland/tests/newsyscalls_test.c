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
#include "rt/sys.h"
#include "timer.h" // struct rtc_time, shared with the kernel's SYS_GETTIME handler

// This test links no libc (it is one of the static /tests), so the one
// string comparison it needs is here.
static int same_name(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

























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

// Signed, for a syscall return that may be a negative errno.
static void put_dec(int64_t v) {
    if (v < 0) { put("-"); v = -v; }
    put_udec((uint32_t)v, 0);
}

#define TESTFILE "/newsyscalls_test.txt"

// Which phase failed first, for the SUMMARY line.
//
// The summary carries it because that line is often the ONLY one a
// reader gets: a test harness that truncates its capture keeps the tail,
// and the tail is the summary -- so "at least one phase FAILED" with the
// detail thousands of characters earlier is a diagnosis that has to be
// fetched in a second round trip. It cost exactly that on a CI failure.
static const char *g_failed_phase;

static void phase_failed(const char *name) {
    if (!g_failed_phase) g_failed_phase = name;
}

int main(void) {
    int all_ok = 1;

    // --- Phase 1: SYS_UNLINK -----------------------------------------
    put("newsyscalls_test: unlink phase\n");
    {
        int64_t fd = sys_open(TESTFILE, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
        int ok = (fd >= 0);
        if (ok) sys_close((int)fd);

        int64_t unlinked = ok ? sys_unlink(TESTFILE) : -1;
        ok = ok && (unlinked == 0);

        // Reading it back should now fail (open without O_CREAT on a
        // gone file) -- proves the delete was real, not just a
        // truthy-looking return value.
        int64_t reopened = sys_open(TESTFILE, 0);
        ok = ok && (reopened < 0);

        if (ok) put("  OK: created, deleted, and confirmed gone\n");
        else { put("  FAIL: unlink round-trip didn't check out\n");
               all_ok = 0; phase_failed("unlink"); }
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
        // static: SYS_LISTDIR_MAX dirents overflow the ring-3 frame
        // budget, and a big local array here steps toward the guard page.
        static struct sys_dirent entries[SYS_LISTDIR_MAX];
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
            all_ok = 0; phase_failed("listdir");
        }
    }

    // --- Phase 2a: SYS_LISTDIR_AT pages -------------------------------
    //
    // The offset is what makes SYS_LISTDIR_MAX a batch size rather than
    // a ceiling: without it, a directory bigger than the cap could not
    // be read at all, which is what stopped the installer copying GRUB's
    // 305-file module directory. Driven with a batch of ONE rather than
    // 256 files, because the property is the offset and a fixture that
    // needs 257 files to prove it is a fixture nobody runs.
    put("newsyscalls_test: listdir paging phase\n");
    {
        static struct sys_dirent one[1];
        static struct sys_dirent all[SYS_LISTDIR_MAX];
        int64_t total = sys_listdir("/etc", all, SYS_LISTDIR_MAX);
        int ok = (total > 1);   // /etc has several files by boot time
        if (!ok) put("  SKIP: /etc has too few entries to page\n");

        // One at a time must visit every entry, in the same order, and
        // then stop -- a page past the end is 0, not an error, or the
        // end of a listing would read as a missing directory.
        for (int64_t i = 0; ok && i < total; i++) {
            int64_t n = sys_listdir_at("/etc", one, 1, (int)i);
            if (n != 1 || !same_name(one[0].name, all[i].name)) {
                put("  FAIL: page ");
                put_udec((uint32_t)i, 0);
                put(" did not match the whole listing\n");
                ok = 0;
            }
        }
        if (ok && sys_listdir_at("/etc", one, 1, (int)total) != 0) {
            put("  FAIL: a page past the end was not empty\n");
            ok = 0;
        }
        if (ok) {
            put("  OK: paged /etc one entry at a time, ");
            put_udec((uint32_t)total, 0);
            put(" of them, in order\n");
        } else if (total > 1) {
            all_ok = 0; phase_failed("listdir paging");
        }
    }

    // --- Phase 2b: SYS_LISTDIR tells EMPTY apart from MISSING ----------
    //
    // The three outcomes have to be distinguishable, and they were not:
    // a missing directory and an empty one both came back as 0, so `ls`
    // could not tell them apart (docs/bugs.md, fixed). An empty
    // directory has to be MADE here rather than assumed -- /etc is
    // seeded, and a test that inherits its fixture from whatever ran
    // before it is the shape this repo has been bitten by.
    put("newsyscalls_test: listdir empty-vs-missing phase\n");
    {
        static struct sys_dirent entries[8];
        const char *empty_dir = "/tmp_listdir_empty";
        const char *missing   = "/tmp_listdir_missing";
        const char *a_file    = "/tmp_listdir_file";

        sys_unlink(empty_dir);  // in case a previous run left them behind
        sys_unlink(a_file);
        int made_dir = sys_mkdir(empty_dir) == 0; // SYS_MKDIR returns 0 on SUCCESS
        int fd = sys_open(a_file, SYS_O_WRITE | SYS_O_CREAT);
        int made_file = fd >= 0;
        if (fd >= 0) sys_close(fd);

        // The WRAPPER's contract, not the kernel's: rt/sys.c's err()
        // collapses every -errno to -1 and parks the reason in
        // sys_errno(). So the three cases are distinguished by (return,
        // errno) pairs, which is exactly what /bin/ls reads.
        int64_t r_empty = sys_listdir(empty_dir, entries, 8);
        int e_empty = sys_errno();
        int64_t r_missing = sys_listdir(missing, entries, 8);
        int e_missing = sys_errno();
        int64_t r_file = sys_listdir(a_file, entries, 8);
        int e_file = sys_errno();

        int ok = made_dir && made_file &&
                  r_empty == 0 &&
                  r_missing == -1 && e_missing == ENOENT &&
                  r_file == -1 && e_file == ENOTDIR;
        if (ok) {
            put("  OK: empty=0, missing=-1/ENOENT, file=-1/ENOTDIR\n");
        } else {
            put("  FAIL: listdir() empty/missing/file were ");
            put_dec(r_empty); put("("); put_dec(e_empty); put(")/");
            put_dec(r_missing); put("("); put_dec(e_missing); put(")/");
            put_dec(r_file); put("("); put_dec(e_file); put(")");
            put(" (wanted 0/-1(2)/-1(20))\n");
            all_ok = 0; phase_failed("listdir-empty-vs-missing");
        }

        sys_unlink(empty_dir);
        sys_unlink(a_file);
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
        int ok = (got == 0) && t.hour < 24 && t.minute < 60 && t.second < 60 &&
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
            all_ok = 0; phase_failed("gettime");
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
        if (!ok) { all_ok = 0; phase_failed("yield"); }
    }

    if (all_ok) {
        put("newsyscalls_test: all phases passed\n");
        sys_exit(0);
    } else {
        put("newsyscalls_test: at least one phase FAILED -- first was ");
        put(g_failed_phase ? g_failed_phase : "unknown");
        put("\n");
        sys_exit(1);
    }
}
