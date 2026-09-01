// Tests for the TWP transport seam and its diagnostic channel --
// Milestone 41's stage 3 (docs/wm-ring3-design.md).
//
// WHAT IS ACTUALLY WORTH TESTING HERE
// -----------------------------------
// The transport indirection itself is three function pointers; a test
// that it dispatches is a test that C works. What has a real failure
// mode is the CHUNKING: a reply longer than one message has to come back
// in pieces, and the two ways that goes wrong are both silent.
//
//   - A chunk that exactly fills the buffer is indistinguishable from a
//     truncated one unless the reply is self-delimiting. That is what
//     WIN_DEBUG_F_MORE is for, and a "read until a short chunk" client
//     would pass every test written against a reply whose length is not
//     an exact multiple of WIN_DEBUG_CHUNK. So one test below uses
//     exactly that length.
//   - Reassembly that drops or duplicates a byte at a chunk boundary
//     produces output that still LOOKS like a transcript. So the checks
//     compare the reassembled bytes against the original, rather than
//     just counting them.
//
// These run without a desktop, which is the other reason they are
// KTESTs: the real `gui` commands need a WM, and none of the properties
// above are about what any particular command prints. A stub
// presentation layer that emits a known pattern isolates the transport
// from the WM entirely.
//
// POSITIVE CONTROL, run when these were written: in dbg_take_chunk()
// (win_server.c), change the MORE flag's condition to `>` instead of
// `<`, i.e. never set it. Measured result -- exactly TWO checks go red,
// the two that span more than one chunk, each reporting 512 bytes where
// it wanted 1543 and 1024.
//
// The four that stayed green are the informative half. "A reply shorter
// than one chunk" cannot catch this and never could: it fits in one
// message, so a broken MORE flag is invisible to it. Neither can the
// unknown-subcommand, no-debug_command or missing-slot checks, none of
// which carry a payload at all. So the chunking rests on exactly two
// checks -- worth knowing before adding a third that looks like
// coverage and is not.

#include "ktest.h"
#include "win_transport.h"
#include "win_server.h"
#include "string.h"
#include "kerrno.h" // EBUSY -- the diagnostic channel is one slot

// A reply of known length and known content. Byte i is derived from i,
// so a dropped or duplicated byte at a chunk boundary shifts everything
// after it and cannot cancel out.
static int g_stub_len = 0;

static int stub_pattern(char *out, int cap, int want) {
    if (want > cap - 1) want = cap - 1;
    for (int i = 0; i < want; i++) out[i] = (char)('a' + (i % 26));
    out[want] = '\0';
    return want;
}

static int stub_debug_command(const char *line, char *out, int cap) {
    if (k_strcmp(line, "unknown-thing") == 0) return -1;
    return stub_pattern(out, cap, g_stub_len);
}

static const struct win_server_ops STUB_OPS = {
    .debug_command = stub_debug_command,
};

// Drains a whole reply through the transport, reassembling it into `buf`.
// Returns the total length, or -1 if the reply came back UNKNOWN.
static int drain(const char *cmd, char *buf, int cap) {
    struct win_debug_msg msg;
    k_memset(&msg, 0, sizeof msg);
    msg.type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(msg.text, cmd, WIN_DEBUG_CMD_LEN);

    if (!win_transport_debug(WIN_PID_KERNEL, &msg)) return -1;
    if (msg.flags & WIN_DEBUG_F_UNKNOWN) return -1;

    int total = 0;
    for (int guard = 0; guard < 64; guard++) {
        for (uint32_t i = 0; i < msg.len && total < cap - 1; i++)
            buf[total++] = msg.text[i];
        if (!(msg.flags & WIN_DEBUG_F_MORE)) break;

        k_memset(&msg, 0, sizeof msg);
        msg.type = WIN_REQ_DEBUG_MORE;
        if (!win_transport_debug(WIN_PID_KERNEL, &msg)) break;
    }
    buf[total] = '\0';
    return total;
}

// Every test here registers the stub and must put the real WM back, or
// it takes the desktop's diagnostics down for the rest of the boot --
// `ktest` runs inside the LIVE kernel (see CLAUDE.md).
static const struct win_server_ops *save_ops(void) {
    return win_server_ops_current();
}

KTEST("wintransport", "a reply shorter than one chunk arrives in one message") {
    const struct win_server_ops *prev = save_ops();
    win_server_register(&STUB_OPS);
    g_stub_len = 10;

    struct win_debug_msg msg;
    k_memset(&msg, 0, sizeof msg);
    msg.type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(msg.text, "pattern", WIN_DEBUG_CMD_LEN);

    int ok = win_transport_debug(WIN_PID_KERNEL, &msg);
    win_server_register(prev);

    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(msg.type, (uint32_t)WIN_EV_DEBUG_OUT);
    KTEST_ASSERT_EQ(msg.len, (uint32_t)10);
    KTEST_ASSERT(!(msg.flags & WIN_DEBUG_F_MORE));
}

KTEST("wintransport", "a long reply reassembles byte for byte across chunks") {
    const struct win_server_ops *prev = save_ops();
    win_server_register(&STUB_OPS);
    g_stub_len = WIN_DEBUG_CHUNK * 3 + 7; // deliberately not a multiple

    static char got[4096], want[4096];
    int n = drain("pattern", got, sizeof got);
    win_server_register(prev);

    KTEST_ASSERT_EQ(n, g_stub_len);
    stub_pattern(want, sizeof want, g_stub_len);
    KTEST_ASSERT_EQ(k_strcmp(got, want), 0);
}

KTEST("wintransport", "a reply that exactly fills a chunk still terminates") {
    // The case a "read until a short chunk" client gets wrong: the last
    // full chunk is followed by an empty one, and only the MORE flag
    // says which is the end.
    const struct win_server_ops *prev = save_ops();
    win_server_register(&STUB_OPS);
    g_stub_len = WIN_DEBUG_CHUNK * 2;

    static char got[4096], want[4096];
    int n = drain("pattern", got, sizeof got);
    win_server_register(prev);

    KTEST_ASSERT_EQ(n, g_stub_len);
    stub_pattern(want, sizeof want, g_stub_len);
    KTEST_ASSERT_EQ(k_strcmp(got, want), 0);
}

KTEST("wintransport", "an unrecognised subcommand is distinct from an empty reply") {
    const struct win_server_ops *prev = save_ops();
    win_server_register(&STUB_OPS);

    struct win_debug_msg unknown, empty;
    k_memset(&unknown, 0, sizeof unknown);
    unknown.type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(unknown.text, "unknown-thing", WIN_DEBUG_CMD_LEN);
    win_transport_debug(WIN_PID_KERNEL, &unknown);

    g_stub_len = 0;
    k_memset(&empty, 0, sizeof empty);
    empty.type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(empty.text, "pattern", WIN_DEBUG_CMD_LEN);
    win_transport_debug(WIN_PID_KERNEL, &empty);

    win_server_register(prev);

    // Both carry zero bytes; only the flag tells them apart. A console
    // that conflated them would print "unknown subcommand" for a command
    // that ran fine and simply had nothing to say.
    KTEST_ASSERT(unknown.flags & WIN_DEBUG_F_UNKNOWN);
    KTEST_ASSERT_EQ(unknown.len, (uint32_t)0);
    KTEST_ASSERT(!(empty.flags & WIN_DEBUG_F_UNKNOWN));
    KTEST_ASSERT_EQ(empty.len, (uint32_t)0);
}

KTEST("wintransport", "a presentation layer with no debug_command refuses cleanly") {
    // A REGISTERED COMPOSITOR also answers a debug command
    // (win_server_debug()'s `else if (g_comp_pid)`), so with the desktop
    // up this stub layer is not the only thing that could reply and the
    // refusal under test cannot happen. Same stale-precondition family
    // as the two skips in win_events_test.c and win_server_test.c --
    // win_server_any() is the predicate that covers both kinds.
    if (win_server_any()) KTEST_SKIP("a compositor would answer instead");

    static const struct win_server_ops NO_DEBUG_OPS = { 0 };
    const struct win_server_ops *prev = save_ops();
    win_server_register(&NO_DEBUG_OPS);

    struct win_debug_msg msg;
    k_memset(&msg, 0, sizeof msg);
    msg.type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(msg.text, "pattern", WIN_DEBUG_CMD_LEN);
    int ok = win_transport_debug(WIN_PID_KERNEL, &msg);

    win_server_register(prev);

    KTEST_ASSERT(!ok); // "no window manager to ask", not a crash
    KTEST_ASSERT_EQ(msg.len, (uint32_t)0);
}

KTEST("wintransport", "registering a transport with a missing slot is refused") {
    // The display_driver honesty rule: a half-installed carriage must
    // fail at REGISTRATION, not at the first message, which would be far
    // from the mistake that caused it.
    static const struct win_transport BROKEN = { .name = "broken" };
    win_transport_register(&BROKEN);
    KTEST_ASSERT_EQ(k_strcmp(win_transport_name(), "direct"), 0);
}

// --- one diagnostic at a time ----------------------------------------
//
// The reply buffer and its chunk cursor are one slot. That was safe
// while the serial console was the only client; `/bin/guictl` is a
// second, so a command arriving mid-drain has to be refused rather than
// served from the same buffer. These drive win_server_debug() directly
// -- the transport's entry point passes WIN_PID_KERNEL, and the whole
// point here is two DIFFERENT callers.

// STATIC, not on the stack: a struct win_debug_msg is 528 bytes and the
// kernel frame budget is 1024, so the two these tests need overflow it.
// Safe here for the reason the WM's own static dirent array is -- a
// KTEST is one call on one thread and does not recurse.
static struct win_debug_msg g_a, g_b;

KTEST("wintransport", "a second caller mid-drain is refused, not served") {
    if (win_server_any()) KTEST_SKIP("a compositor would answer instead");

    const struct win_server_ops *prev = save_ops();
    win_server_register(&STUB_OPS);
    g_stub_len = 1200;   // three chunks, so the first caller is still draining

    struct win_debug_msg *a = &g_a, *b = &g_b;
    k_memset(a, 0, sizeof *a);
    a->type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(a->text, "pattern", WIN_DEBUG_CMD_LEN);
    KTEST_ASSERT_EQ(win_server_debug(100, a), 1);
    KTEST_ASSERT(a->flags & WIN_DEBUG_F_MORE);   // the claim is live

    k_memset(b, 0, sizeof *b);
    b->type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(b->text, "pattern", WIN_DEBUG_CMD_LEN);
    KTEST_ASSERT_EQ(win_server_debug(200, b), -EBUSY);

    // ...and the stranger cannot steal a chunk either: it gets the empty
    // final chunk, and the owner's next chunk is still the owner's.
    k_memset(b, 0, sizeof *b);
    b->type = WIN_REQ_DEBUG_MORE;
    KTEST_ASSERT_EQ(win_server_debug(200, b), 1);
    KTEST_ASSERT_EQ(b->len, (uint32_t)0);

    k_memset(a, 0, sizeof *a);
    a->type = WIN_REQ_DEBUG_MORE;
    KTEST_ASSERT_EQ(win_server_debug(100, a), 1);
    KTEST_ASSERT_EQ(a->len, (uint32_t)WIN_DEBUG_CHUNK);

    // Drain the rest; the claim lapses with the last chunk.
    for (int guard = 0; guard < 8 && (a->flags & WIN_DEBUG_F_MORE); guard++) {
        k_memset(a, 0, sizeof *a);
        a->type = WIN_REQ_DEBUG_MORE;
        win_server_debug(100, a);
    }

    k_memset(b, 0, sizeof *b);
    b->type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(b->text, "pattern", WIN_DEBUG_CMD_LEN);
    KTEST_ASSERT_EQ(win_server_debug(200, b), 1);

    // Leave the channel free for whatever runs next.
    for (int guard = 0; guard < 8 && (b->flags & WIN_DEBUG_F_MORE); guard++) {
        k_memset(b, 0, sizeof *b);
        b->type = WIN_REQ_DEBUG_MORE;
        win_server_debug(200, b);
    }
    g_stub_len = 0;
    win_server_register(prev);
}

KTEST("wintransport", "the same caller may issue a second command") {
    if (win_server_any()) KTEST_SKIP("a compositor would answer instead");

    const struct win_server_ops *prev = save_ops();
    win_server_register(&STUB_OPS);
    g_stub_len = 1200;

    struct win_debug_msg *m = &g_a;
    k_memset(m, 0, sizeof *m);
    m->type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(m->text, "pattern", WIN_DEBUG_CMD_LEN);
    KTEST_ASSERT_EQ(win_server_debug(100, m), 1);

    // Abandoning a drain and asking again must NOT lock the caller out
    // of its own channel -- that is the shape that would wedge guictl
    // after one interrupted command.
    k_memset(m, 0, sizeof *m);
    m->type = WIN_REQ_DEBUG_CMD;
    k_strlcpy(m->text, "pattern", WIN_DEBUG_CMD_LEN);
    KTEST_ASSERT_EQ(win_server_debug(100, m), 1);

    for (int guard = 0; guard < 8 && (m->flags & WIN_DEBUG_F_MORE); guard++) {
        k_memset(m, 0, sizeof *m);
        m->type = WIN_REQ_DEBUG_MORE;
        win_server_debug(100, m);
    }
    g_stub_len = 0;
    win_server_register(prev);
}
