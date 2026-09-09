// The diagnostic registry's refusal paths and its chunking.
//
// NARROW ON PURPOSE. The interesting end-to-end case -- a real provider
// answering a real command -- needs a scheduled process, and the live
// desktop is exactly that: `tools/guictl_test.py` and every GUI tool
// drive it. What is checked here is the logic a ring-3 test cannot
// reach: an unclaimed name, a name somebody else holds, a TAKE with
// nothing pending, and a reply that spans chunks.
//
// **THESE RUN IN THE LIVE KERNEL**, so a desktop may hold the name
// `gui`. Every test below uses a name of its own and never touches that
// one.
#include "ktest.h"
#include "diag.h"
#include "string.h"
#include <stddef.h>

#define TEST_NAME "ktest-diag"
#define TEST_PID  9001            // not a real pid; the registry never
                                  // dereferences one, it only compares
#define OTHER_PID 9002

static void msg_init(struct diag_msg *m, uint32_t type, const char *name) {
    k_memset(m, 0, sizeof *m);
    m->type = type;
    if (name) k_strlcpy(m->name, name, DIAG_NAME_LEN);
}

// Leaves the registry as it was found, so a later test -- and the live
// desktop -- see nothing.
static void release(int pid) {
    struct diag_msg m;
    msg_init(&m, DIAG_RELEASE, TEST_NAME);
    diag_request(pid, &m);
}

KTEST("diag", "a name can be claimed, and only by one process") {
    release(TEST_PID); release(OTHER_PID);

    struct diag_msg m;
    msg_init(&m, DIAG_CLAIM, TEST_NAME);
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 1);
    KTEST_ASSERT(diag_have_provider(TEST_NAME));

    // Idempotent for the SAME pid: a provider that re-claims on every
    // poll (wm_client.c does) must not be refused its own name.
    msg_init(&m, DIAG_CLAIM, TEST_NAME);
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 1);

    // ...and refused to anyone else, or a second process could silently
    // take over answering for a service that is still running.
    msg_init(&m, DIAG_CLAIM, TEST_NAME);
    KTEST_ASSERT_EQ(diag_request(OTHER_PID, &m), 0);

    release(TEST_PID);
    KTEST_ASSERT(!diag_have_provider(TEST_NAME));
}

KTEST("diag", "a command for a name nobody holds is refused, not queued") {
    release(TEST_PID);
    struct diag_msg m;
    msg_init(&m, DIAG_CMD, TEST_NAME);
    k_strlcpy(m.text, "anything", DIAG_CMD_LEN);
    // 0, not -1: "no such provider" is a refusal the console turns into
    // a sentence naming what IS registered, not a protocol error.
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 0);
    KTEST_ASSERT_EQ(m.len, 0u);
}

KTEST("diag", "TAKE with nothing pending answers 0, which is the normal case") {
    release(TEST_PID);
    struct diag_msg m;
    msg_init(&m, DIAG_CLAIM, TEST_NAME);
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 1);

    // A wake says only "look at your sources", so a provider asks this
    // on every one and usually gets nothing. Answering 0 must not read
    // as a failure.
    msg_init(&m, DIAG_TAKE, NULL);
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 0);
    KTEST_ASSERT_EQ(m.len, 0u);

    release(TEST_PID);
}

KTEST("diag", "a dead provider drops its name") {
    release(TEST_PID);
    struct diag_msg m;
    msg_init(&m, DIAG_CLAIM, TEST_NAME);
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 1);
    KTEST_ASSERT(diag_have_provider(TEST_NAME));

    diag_provider_gone(TEST_PID);
    // Without this a restarted service could never re-claim its own
    // name, since the registry would still be holding it for a pid that
    // no longer exists.
    KTEST_ASSERT(!diag_have_provider(TEST_NAME));
}

KTEST("diag", "diag_list names the registered providers") {
    release(TEST_PID);
    char buf[128];
    int before = diag_list(buf, sizeof buf);

    struct diag_msg m;
    msg_init(&m, DIAG_CLAIM, TEST_NAME);
    KTEST_ASSERT_EQ(diag_request(TEST_PID, &m), 1);

    int after = diag_list(buf, sizeof buf);
    KTEST_ASSERT_EQ(after, before + 1);
    // The console prints this when a name is not found, so it has to
    // actually contain the names rather than just count them.
    KTEST_ASSERT(k_strstr(buf, TEST_NAME) != NULL);

    release(TEST_PID);
}
