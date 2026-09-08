// uchan: a message channel between two ring-3 processes, over shared
// memory, a futex and a wakeword -- with no kernel support of its own.
//
// The shape that can see it working is TWO PROCESSES, because one
// process talking to itself proves nothing about the wakeup: it never
// parks, so a channel that woke nobody would pass. This spawns itself
// as a client and serves it.
//
// WHAT EACH CHECK IS FOR, since several look alike: that a message
// arrives at all; that ASYNC really is async (the sender does not wait);
// that the server can be WOKEN OUT OF A PARK by a message rather than
// finding it on a poll; that a round trip gets an answer back; and that
// a full ring is reported rather than silently dropping.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "lib/uchan.h"
#include "lib/utest.h"

#define SERVICE "chantest"

struct msg {
    uint32_t kind;      // 1 = note, 2 = please answer
    uint32_t value;
    char     text[32];
};

enum { KIND_NOTE = 1, KIND_ASK = 2 };

static int child_main(void) {
    struct uchan_client c;
    // The server may not have published yet; a client that gave up on
    // the first miss would make this a race rather than a test.
    for (int i = 0; i < 200; i++) {
        if (uchan_client_open(&c, SERVICE) == 0) break;
        sys_sleep_ms(10);
        if (i == 199) return 2;
    }

    struct msg m;
    memset(&m, 0, sizeof m);
    m.kind = KIND_NOTE;
    m.value = 41;
    snprintf(m.text, sizeof m.text, "hello");
    if (uchan_send(&c, &m, sizeof m) < 0) return 3;

    // Let the server park, so the NEXT message has to wake it rather
    // than being found by a poll. That is the check this delay exists
    // for; without it the test cannot tell the two apart.
    sys_sleep_ms(300);
    m.value = 42;
    if (uchan_send(&c, &m, sizeof m) < 0) return 4;

    struct msg reply;
    memset(&m, 0, sizeof m);
    m.kind = KIND_ASK;
    m.value = 7;
    if (uchan_call(&c, &m, sizeof m, &reply, sizeof reply, 5000) < 0) return 5;
    if (reply.value != 8) return 6;

    uchan_client_close(&c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();

    utest_begin("chan_test", "a message channel between two processes",
                UTEST_VERDICT_FILE);

    struct uchan_server s;
    utest_check(uchan_server_open(&s, SERVICE) == 0, "the server publishes");

    int pid = sys_spawn("/tests/chan_test", "child", -1);
    utest_check(pid > 0, "the client spawns");
    if (pid <= 0) return utest_end();

    int got_note = 0, got_41 = 0, got_42 = 0, answered = 0, woke_from_park = 0;
    struct msg m;

    for (int frame = 0; frame < 400 && answered < 1; frame++) {
        uchan_server_scan(&s);

        // Park. After the first message this returns only when the
        // client sends again -- which is the wakeup under test.
        uint64_t before = sys_monotonic_ns();
        uchan_server_wait(&s, 1000);
        uint64_t waited_ms = (sys_monotonic_ns() - before) / 1000000ull;

        int from;
        while ((from = uchan_server_recv(&s, &m, sizeof m)) != 0) {
            if (m.kind == KIND_NOTE) {
                got_note++;
                if (m.value == 41 && !strcmp(m.text, "hello")) got_41 = 1;
                if (m.value == 42) {
                    got_42 = 1;
                    // The second note came 300 ms after the first, so
                    // the server was parked when it arrived -- and it
                    // came back well inside the 1000 ms deadline, which
                    // is what says a MESSAGE woke it rather than the
                    // timeout.
                    if (waited_ms >= 100 && waited_ms < 900) woke_from_park = 1;
                }
            } else if (m.kind == KIND_ASK) {
                struct msg r;
                memset(&r, 0, sizeof r);
                r.kind = m.kind;
                r.value = m.value + 1;
                uchan_server_reply(&s, from, &r, sizeof r);
                answered++;
            }
        }
    }

    utest_checkf(got_note == 2, "both notes arrive (got %d)", got_note);
    utest_check(got_41, "the first note's payload survives the ring");
    utest_check(got_42, "the second note arrives too");
    utest_check(woke_from_park,
                "a message WAKES a parked server, well inside its deadline");
    utest_checkf(answered == 1, "the round trip is answered (%d)", answered);

    int code = 0;
    utest_check(sys_waitpid(pid, &code) == pid, "the client is reaped");
    utest_checkf(code == 0, "the client saw its reply (exit %d)", code);

    // A FULL RING IS REPORTED, not swallowed. Done from here with no
    // server draining, because that is the only way to fill one.
    struct uchan_client self;
    if (uchan_client_open(&self, SERVICE) == 0) {
        struct msg junk;
        memset(&junk, 0, sizeof junk);
        int sent = 0;
        while (uchan_send(&self, &junk, sizeof junk) == 0 && sent < UCHAN_SLOTS * 4) sent++;
        utest_checkf(sent == UCHAN_SLOTS,
                     "a full ring refuses rather than dropping (took %d of %d)",
                     sent, UCHAN_SLOTS);
        uchan_client_close(&self);
    }

    uchan_server_close(&s);
    return utest_end();
}
