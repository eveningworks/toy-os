// The application log ring and the fact that exposes it.
//
// These write into the LIVE ring, which is the same one a running system
// is filling, so nothing here asserts an absolute sequence number or
// that a particular record sits at a particular index -- both are true
// only until the next process prints something. What is asserted is the
// RELATIONSHIP between what was written and what comes back, which holds
// whatever else is going on.
#include "ktest.h"
#include "applog.h"
#include "query.h"
#include "string.h"

KTEST("applog", "a write comes back with its tag and text") {
    uint64_t before = applog_total();
    applog_write("ktest", "hello there", 11);
    KTEST_ASSERT_EQ((int)(applog_total() - before), 1);

    struct applog_rec r;
    KTEST_ASSERT(applog_get(before + 1, &r));
    KTEST_ASSERT_EQ(r.len, 11);
    KTEST_ASSERT_EQ(k_strcmp(r.text, "hello there"), 0);
    KTEST_ASSERT_EQ(k_strcmp(r.tag, "ktest"), 0);
    KTEST_ASSERT_EQ((int)(r.seq - before), 1);
}

KTEST("applog", "the writer's newline is not part of what it said") {
    uint64_t before = applog_total();
    applog_write("ktest", "a line\n", 7);
    struct applog_rec r;
    KTEST_ASSERT(applog_get(before + 1, &r));
    // Not merely "shorter": a trailing newline surviving into the record
    // makes logd emit a blank line after every message, which reads as
    // the log being double-spaced rather than as a bug here.
    KTEST_ASSERT_EQ(r.len, 6);
    KTEST_ASSERT_EQ(k_strcmp(r.text, "a line"), 0);
}

KTEST("applog", "a line with nothing but a newline is not a record") {
    uint64_t before = applog_total();
    applog_write("ktest", "\n", 1);
    applog_write("ktest", "", 0);
    KTEST_ASSERT_EQ((int)(applog_total() - before), 0);
}

KTEST("applog", "a fragment says it did not end the line, and the last one does") {
    // The shape cmd_fail_err() writes: five writes, one line. Only the
    // last carries the newline, and a reader with no `eol` could not
    // tell this from five separate messages.
    uint64_t before = applog_total();
    applog_write("ktest", "prog", 4);
    applog_write("ktest", ": ", 2);
    applog_write("ktest", "reason\n", 7);

    struct applog_rec r;
    KTEST_ASSERT(applog_get(before + 1, &r));
    KTEST_ASSERT_EQ(r.eol, 0);
    KTEST_ASSERT(applog_get(before + 2, &r));
    KTEST_ASSERT_EQ(r.eol, 0);
    KTEST_ASSERT(applog_get(before + 3, &r));
    KTEST_ASSERT_EQ(r.eol, 1);
}

KTEST("applog", "a bare newline closes the SAME writer's fragment, not a stranger's") {
    uint64_t before = applog_total();
    applog_write("ktest", "held", 4);
    // Another program's line terminator must not finish this one --
    // two processes interleave freely in this ring.
    applog_write("other", "\n", 1);

    struct applog_rec r;
    KTEST_ASSERT(applog_get(before + 1, &r));
    KTEST_ASSERT_EQ(r.eol, 0);

    // The writer's own does.
    applog_write("ktest", "\n", 1);
    KTEST_ASSERT(applog_get(before + 1, &r));
    KTEST_ASSERT_EQ(r.eol, 1);
    // And it added no record of its own.
    KTEST_ASSERT_EQ((int)(applog_total() - before), 1);
}

KTEST("applog", "an over-long line is truncated and stays NUL-terminated") {
    char big[APPLOG_TEXT_MAX * 2];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = 'x';
    uint64_t before = applog_total();
    applog_write("ktest", big, sizeof big);

    struct applog_rec r;
    KTEST_ASSERT(applog_get(before + 1, &r));
    KTEST_ASSERT_EQ(r.len, APPLOG_TEXT_MAX - 1);
    KTEST_ASSERT_EQ((int)k_strlen(r.text), APPLOG_TEXT_MAX - 1);
}

KTEST("applog", "an aged-out sequence is refused, not answered wrongly") {
    uint64_t start = applog_total();
    // Fill past the ring's capacity so the first of these is certainly
    // gone. The count comes from the oldest the ring reports rather than
    // from a copy of APPLOG_RECS here, which would be a second place to
    // keep the size true.
    for (int i = 0; i < 200; i++) applog_write("ktest", "fill", 4);

    uint64_t oldest = applog_oldest();
    KTEST_ASSERT(oldest > start + 1);

    struct applog_rec r;
    // The record before the oldest is gone. Handing back the slot that
    // now holds a NEWER record wearing an older sequence would be worse
    // than reporting the gap -- a reader would never see it happen.
    KTEST_ASSERT(!applog_get(oldest - 1, &r));
    KTEST_ASSERT(applog_get(oldest, &r));
    KTEST_ASSERT_EQ((int)(r.seq - oldest), 0);
    // Nothing has been written with this sequence yet.
    KTEST_ASSERT(!applog_get(applog_total() + 1, &r));
}

KTEST("applog", "the fact reports the record and where the ring starts") {
    applog_write("ktest", "via the query", 13);

    struct query_applog q;
    KTEST_ASSERT(query_read(QUERY_APPLOG, 0, &q, sizeof q) > 0);
    // Index 0 IS the oldest retained record, which is what lets a reader
    // that has fallen behind ask for something that still exists.
    KTEST_ASSERT_EQ((int)(q.seq - q.oldest), 0);
    KTEST_ASSERT(q.total >= q.seq);

    // The last record is the one just written; found by walking from the
    // oldest rather than by index arithmetic, since something else may
    // write between the two calls.
    int last = (int)(q.total - q.oldest);
    KTEST_ASSERT(query_read(QUERY_APPLOG, last, &q, sizeof q) > 0);
    KTEST_ASSERT_EQ(k_strcmp(q.tag, "ktest"), 0);
    KTEST_ASSERT_EQ(k_strcmp(q.text, "via the query"), 0);
    KTEST_ASSERT_EQ(q.len, 13);
}
