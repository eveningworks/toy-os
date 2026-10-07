// The in-kernel test runner -- see kernel/include/kernel/ktest.h for
// the API and for what this replaced.
//
// Output goes through vga_write() rather than klog_write() on purpose:
// vga_write() honours the output sink (vga_set_sink(), see
// docs/decisions.md), so `ktest` typed in the GUI Terminal renders into
// that window's scrollback, and `ktest` typed at the serial debug
// console goes down the wire -- which is what tools/ktest_run.py reads
// to decide whether `make test` passed. A klog_write() report would
// always go to the serial log and never to the window the user typed in.
#include "ktest.h"
#include "scheduler.h" // ktest_spare_pids()
#include "block.h"
#include "kapi.h"
#include "fault_inject.h"

struct ktest_ctx {
    int failed;
    int skipped;
    const char *skip_reason;
};

// Provided by linker.ld's .ktests section -- the array bounds. Declared
// as arrays rather than pointers deliberately: a linker-defined symbol
// IS the address, so `&__ktests_start` is what's wanted and an
// `extern struct ktest_case *__ktests_start` would read whatever bytes
// live there as a pointer instead.
extern const struct ktest_case __ktests_start[];
extern const struct ktest_case __ktests_end[];

void ktest_fail(struct ktest_ctx *ctx, const char *expr, const char *file, int line) {
    ctx->failed = 1;
    vga_write("\n    FAIL: ");
    vga_write(expr);
    vga_write("\n      at ");
    vga_write(file);
    vga_write(":");
    vga_write_dec((uint32_t)line);
    vga_putc('\n');
}

void ktest_fail_eq(struct ktest_ctx *ctx, const char *expr, int64_t got, int64_t expected,
                    const char *file, int line) {
    ctx->failed = 1;
    vga_write("\n    FAIL: ");
    vga_write(expr);
    vga_write("\n      expected ");
    vga_write_dec((uint32_t)expected);
    vga_write(", got ");
    vga_write_dec((uint32_t)got);
    vga_write("\n      at ");
    vga_write(file);
    vga_write(":");
    vga_write_dec((uint32_t)line);
    vga_putc('\n');
}

void ktest_skip(struct ktest_ctx *ctx, const char *reason) {
    ctx->skipped = 1;
    ctx->skip_reason = reason;
}

static int streq(const char *a, const char *b) {
    return k_strcmp(a, b) == 0;
}

int ktest_run_all(const char *suite_filter) {
    int filter = (suite_filter && suite_filter[0] != '\0');
    // Count in BYTES then divide: subtracting the two typed pointers
    // directly is undefined behaviour unless the byte distance is an
    // exact multiple of the element size, and relying on that is how the
    // first version of this produced a count of 0xAAAAAAAD. char*
    // arithmetic is always well-defined; the aligned(32) on struct
    // ktest_case (ktest.h) is what makes the division exact.
    uint32_t span = (uint32_t)((const char *)__ktests_end - (const char *)__ktests_start);
    uint32_t total = span / (uint32_t)sizeof(struct ktest_case);

    uint32_t ran = 0, passed = 0, failed = 0, skipped = 0;
    uint64_t start_ticks = coarse_ticks();

    vga_write("ktest: ");
    vga_write_dec(total);
    vga_write(" test(s) registered");
    if (filter) {
        vga_write(", filtering on suite '");
        vga_write(suite_filter);
        vga_write("'");
    }
    vga_write("\n");

    // THE ENVIRONMENT THE SUITE IS RUNNING IN, stated rather than
    // inferred. Which block device carries the filesystem changes what
    // several suites can even test -- the ATA cache tests need the
    // filesystem on ATA, and a fault injector reaches one layer and not
    // another -- so a report that does not say which one it had leaves
    // every result ambiguous.
    //
    // This exists because a CI failure was misdiagnosed twice for want
    // of exactly this line: the run's own output could not answer "was
    // the filesystem on virtio or ATA?", and two different wrong
    // conclusions were drawn from greps that measured nothing.
    vga_write("ktest: block device = ");
    vga_write(blk_root_present() ? blk_root_name() : "none");
    vga_write(blk_root_present() && blk_root_persistent() ? " (persistent)" : " (RAM-only)");
    vga_write("\n");

    // One pass per test, printing the suite header whenever it changes.
    // Tests land in .ktests in link order, which groups a file's tests
    // together but doesn't sort suites -- so a suite whose tests live in
    // two files prints two headers. Accepted: sorting would need a
    // scratch array sized to the test count, and the report is for
    // humans and a line-matching script, not a data structure.
    const char *current_suite = 0;
    for (uint32_t i = 0; i < total; i++) {
        const struct ktest_case *t = &__ktests_start[i];
        // Defensive: a zeroed entry would be linker padding, not a test.
        // Shouldn't happen with aligned(32), but reading a NULL suite
        // pointer would panic rather than report, and a test runner that
        // panics is worse than useless.
        if (!t->fn || !t->suite || !t->name) continue;
        if (filter && !streq(t->suite, suite_filter)) continue;

        if (!current_suite || !streq(current_suite, t->suite)) {
            current_suite = t->suite;
            vga_write("  [");
            vga_write(t->suite);
            vga_write("]\n");
        }

        vga_write("    ");
        vga_write(t->name);
        vga_write(" ... ");

        struct ktest_ctx ctx = { .failed = 0, .skipped = 0, .skip_reason = 0 };
        t->fn(&ctx);
        ran++;

        // A test that arms a fault injector and returns early (an
        // assertion fires) leaves it armed, and every later test then
        // fails for reasons that have nothing to do with what it's
        // testing. Catch it here, name the test that did it, and
        // disarm -- one confusing cascade is enough.
        if (fault_any_armed()) {
            fault_fail_next_ata_writes(0);
            fault_fail_next_ata_reads(0);
            fault_fail_next_allocs(0);
            vga_write("\n    WARNING: this test left a fault injector armed; disarmed it\n");
        }

        if (ctx.failed) {
            failed++;
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_write("    FAILED\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else if (ctx.skipped) {
            skipped++;
            vga_write("skipped");
            if (ctx.skip_reason) {
                vga_write(" (");
                vga_write(ctx.skip_reason);
                vga_write(")");
            }
            vga_putc('\n');
        } else {
            passed++;
            vga_write("ok\n");
        }
    }

    uint64_t elapsed = coarse_ticks() - start_ticks; // 100Hz PIT -- see timer.h

    // The summary line is what tools/ktest_run.py matches on, so its
    // shape is load-bearing: "ktest: PASSED" / "ktest: FAILED" first,
    // counts after. Don't reword it without updating that script.
    if (failed) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_write("ktest: FAILED -- ");
    } else {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write("ktest: PASSED -- ");
    }
    vga_write_dec(passed); vga_write(" passed, ");
    vga_write_dec(failed); vga_write(" failed, ");
    vga_write_dec(skipped); vga_write(" skipped, ");
    vga_write_dec(ran); vga_write(" run in ");
    vga_write_dec((uint32_t)(elapsed / 100)); vga_write(".");
    vga_write_dec((uint32_t)((elapsed % 100) / 10));
    vga_write("s\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    if (filter && ran == 0) {
        vga_write("ktest: no tests matched that suite name -- `ktest` alone lists them all\n");
    }
    return (int)failed;
}

int ktest_spare_pids(int *out, int n) {
    int found = 0;
    for (int p = SCHED_PID_MAX - 1; p > 0 && found < n; p--)
        if (!scheduler_pid_valid(p)) out[found++] = p;
    return found == n;
}
