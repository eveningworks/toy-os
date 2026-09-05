// KTESTs for the connection log's ring, its name cache, and the mode
// gate. The hooks that FEED it are the socket layer's and need a device
// with an address, which is tools/net_test.py's job -- what is testable
// without a network is the ring itself, and that is where its two
// silent failures live: a wrap that loses the sequence, and a mode that
// records more than it says.
//
// THESE TESTS WRITE TO THE REAL LOG, because a ring is real state and a
// wrap test must actually wrap one. They use TEST-NET-1 (192.0.2.0/24,
// RFC 5737) so a record one leaves behind is recognisable as synthetic
// rather than as somewhere this machine really connected.
#include "conn_log.h"
#include "net.h"
#include "ktest.h"
#include "string.h"

#define TESTNET(host) NET_IPV4(192, 0, 2, (host))

// The most recent record, or 0 when the log is empty.
static int last_record(struct query_connlog *out) {
    int n = conn_log_count();
    return n ? conn_log_at(n - 1, out) : 0;
}

KTEST("connlog", "a record keeps every field it was given") {
    int saved = conn_log_mode();
    conn_log_set_mode(CONN_LOG_ALL);

    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, TESTNET(10), 443, 51000);

    struct query_connlog r;
    KTEST_ASSERT(last_record(&r));
    KTEST_ASSERT_EQ(r.remote_ip, TESTNET(10));
    KTEST_ASSERT_EQ(r.remote_port, 443);
    KTEST_ASSERT_EQ(r.local_port, 51000);
    KTEST_ASSERT_EQ(r.proto, IP_PROTO_TCP);
    KTEST_ASSERT_EQ(r.direction, QUERY_CONNLOG_OUT);
    KTEST_ASSERT(r.seq != 0);
    KTEST_ASSERT(r.monotonic_ns != 0);

    conn_log_set_mode(saved);
}

KTEST("connlog", "the ring wraps, and the sequence says how much was lost") {
    int saved = conn_log_mode();
    conn_log_set_mode(CONN_LOG_ALL);

    // Overfill it deliberately. The count must STOP at the cap while
    // the sequence keeps climbing -- that difference is the only thing
    // that can tell a reader records were dropped, and a ring that
    // renumbered from zero on wrap would pass a count check and lie.
    for (int i = 0; i < CONN_LOG_MAX + 5; i++)
        conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_UDP, TESTNET(20),
                        (uint16_t)(1000 + i), 40000);

    KTEST_ASSERT_EQ(conn_log_count(), CONN_LOG_MAX);

    struct query_connlog first, last;
    KTEST_ASSERT(conn_log_at(0, &first));
    KTEST_ASSERT(last_record(&last));
    // Oldest first, contiguous, and exactly a ring's worth apart.
    KTEST_ASSERT_EQ(last.seq - first.seq, CONN_LOG_MAX - 1);
    KTEST_ASSERT_EQ(first.remote_port, last.remote_port - (CONN_LOG_MAX - 1));
    KTEST_ASSERT(!conn_log_at(CONN_LOG_MAX, &last));

    conn_log_set_mode(saved);
}

KTEST("connlog", "`off` records nothing and `tcp` refuses a datagram") {
    int saved = conn_log_mode();

    // The pair, not just the positive: "nothing was recorded" is
    // satisfied by a log that records nothing at all, so each refusal
    // is followed by the acceptance that proves the ring still works.
    conn_log_set_mode(CONN_LOG_OFF);
    int before = conn_log_count();
    uint64_t seq_before = 0;
    struct query_connlog r;
    if (last_record(&r)) seq_before = r.seq;

    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, TESTNET(30), 80, 51001);
    KTEST_ASSERT_EQ(conn_log_count(), before);
    if (seq_before) { KTEST_ASSERT(last_record(&r)); KTEST_ASSERT_EQ(r.seq, seq_before); }

    conn_log_set_mode(CONN_LOG_TCP);
    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_UDP, TESTNET(30), 53, 51002);
    if (seq_before) { KTEST_ASSERT(last_record(&r)); KTEST_ASSERT_EQ(r.seq, seq_before); }

    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, TESTNET(30), 80, 51003);
    KTEST_ASSERT(last_record(&r));
    KTEST_ASSERT_EQ(r.remote_port, 80);
    KTEST_ASSERT_EQ(r.proto, IP_PROTO_TCP);

    conn_log_set_mode(saved);
}

KTEST("connlog", "a name reaches the record for its address and no other") {
    int saved = conn_log_mode();
    conn_log_set_mode(CONN_LOG_ALL);

    conn_log_name_hint(TESTNET(40), "named.example");

    struct query_connlog r;
    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, TESTNET(40), 80, 51004);
    KTEST_ASSERT(last_record(&r));
    KTEST_ASSERT_EQ(k_strcmp(r.host, "named.example"), 0);

    // The neighbouring address must stay nameless: a cache that
    // answered for anything would print a plausible wrong hostname
    // beside a correct address, which is worse than none.
    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, TESTNET(41), 80, 51005);
    KTEST_ASSERT(last_record(&r));
    KTEST_ASSERT_EQ(r.host[0], 0);

    // A second name for one address REPLACES the first rather than
    // adding an entry -- a CDN answers for many names and the useful
    // one is whatever was asked for most recently.
    conn_log_name_hint(TESTNET(40), "renamed.example");
    conn_log_record(QUERY_CONNLOG_OUT, IP_PROTO_TCP, TESTNET(40), 80, 51006);
    KTEST_ASSERT(last_record(&r));
    KTEST_ASSERT_EQ(k_strcmp(r.host, "renamed.example"), 0);

    conn_log_set_mode(saved);
}
