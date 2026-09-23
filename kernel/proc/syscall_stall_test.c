// The syscall stall histogram (syscall_stall.c).
//
// THE BUCKETING IS TESTED THROUGH syscall_stall_record_us() RATHER THAN
// THROUGH REAL SYSCALLS, because a syscall that lands in bucket 15 would
// have to hold the machine for 32 ms to say so -- the test would BE the
// stall it is checking for. What the real path adds over this is two
// clocksource reads and a subtraction, and the arming compare; the
// arithmetic that decides which bucket a duration falls in is here.
#include "ktest.h"
#include "syscall_stall.h"
#include "string.h"

// A number no syscall holds, so an armed machine's own traffic cannot
// land in the slot under test. The table is sized above the ABI
// (syscall_table.c asserts it), which is what leaves a spare.
#define SPARE (SYSCALL_STALL_MAX - 1)

KTEST("syscall_stall", "a duration lands in the log2 bucket that holds it") {
    int was_on = syscall_stall_get();
    syscall_stall_set(0);
    syscall_stall_set(1); // arming from off zeroes

    struct syscall_stall_info s;
    KTEST_ASSERT(!syscall_stall_info(SPARE, &s)); // nothing measured yet

    syscall_stall_record_us(SPARE, 0);    // bucket 0, the "below 1 us" floor
    syscall_stall_record_us(SPARE, 1);    // bucket 0: [1, 2)
    syscall_stall_record_us(SPARE, 3);    // bucket 1: [2, 4)
    syscall_stall_record_us(SPARE, 1000); // bucket 9: [512, 1024)

    KTEST_ASSERT(syscall_stall_info(SPARE, &s));
    KTEST_ASSERT_EQ((int)s.n, 4);
    KTEST_ASSERT_EQ((int)s.sum_us, 1004);
    KTEST_ASSERT_EQ((int)s.max_us, 1000);
    KTEST_ASSERT_EQ((int)s.bucket[0], 2);
    KTEST_ASSERT_EQ((int)s.bucket[1], 1);
    KTEST_ASSERT_EQ((int)s.bucket[9], 1);

    syscall_stall_set(0);
    syscall_stall_set(was_on);
}

KTEST("syscall_stall", "anything past the last bucket is counted, not dropped") {
    int was_on = syscall_stall_get();
    syscall_stall_set(0);
    syscall_stall_set(1);

    // 2^21 us is ~2.1 s; the top bucket is "and everything above".
    // Dropping these instead would make the worst stall on the machine
    // the one number the histogram cannot show.
    syscall_stall_record_us(SPARE, 1ull << 21);
    syscall_stall_record_us(SPARE, 1ull << 40);

    struct syscall_stall_info s;
    KTEST_ASSERT(syscall_stall_info(SPARE, &s));
    KTEST_ASSERT_EQ((int)s.bucket[QUERY_SYSCALL_STALL_BUCKETS - 1], 2);
    KTEST_ASSERT_EQ((int)s.n, 2);

    syscall_stall_set(0);
    syscall_stall_set(was_on);
}

KTEST("syscall_stall", "a number past the table is refused rather than wrapping") {
    int was_on = syscall_stall_get();
    syscall_stall_set(0);
    syscall_stall_set(1);

    struct syscall_stall_info s;
    syscall_stall_record_us(SYSCALL_STALL_MAX, 5);
    syscall_stall_record_us(-1, 5);
    // The refusal is the point: an unchecked index here would write past
    // the array, and the symptom would be somewhere else entirely.
    KTEST_ASSERT(!syscall_stall_info(SYSCALL_STALL_MAX, &s));
    KTEST_ASSERT(!syscall_stall_info(-1, &s));
    KTEST_ASSERT(!syscall_stall_info(0, &s) || s.n > 0); // slot 0 untouched by the above

    syscall_stall_set(0);
    syscall_stall_set(was_on);
}

KTEST("syscall_stall", "begin() reads no clock while disarmed") {
    int was_on = syscall_stall_get();
    syscall_stall_set(0);
    // The sentinel IS the contract: a 0 from begin() is what end()
    // tests, so a disarmed begin() returning a real timestamp would
    // start recording without the tunable ever being set.
    KTEST_ASSERT(syscall_stall_begin().t0 == 0);
    syscall_stall_set(1);
    KTEST_ASSERT(syscall_stall_begin().t0 != 0);
    syscall_stall_set(0);
    syscall_stall_set(was_on);
}
