// A pipe whose buffer fills must BLOCK its writer, not truncate.
//
// THE BUG THIS IS FOR. pipe_write() used to take what fitted and report
// a short count. That is a correct-looking answer nothing in ring 3
// acts on -- no program here loops on a short write -- so a producer
// faster than its reader silently lost the remainder. It was latent
// while the only reader was a shell draining continuously, and became
// unavoidable with `|`, where the reader is another process that may
// not have been scheduled yet.
//
// The check writes MORE THAN THE BUFFER (PIPE_BUF_SIZE is 4096) through
// a pipe and requires every byte back. A short write shows up as a
// count that is too low; a truncating kernel cannot pass it.
//
// The data is ADDRESS-DERIVED rather than a constant fill: a constant
// cannot tell "all the bytes arrived" from "the same byte arrived many
// times", which is exactly the failure being tested for.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>

#define TOTAL (16 * 1024)   // four times the pipe buffer
#define CHUNK 256

#include "lib/utest.h"

static char byte_at(int i) { return (char)((i * 7 + (i >> 8)) & 0x7f); }

int main(void) {
    utest_begin("pipefull_test", "a full pipe blocks its writer", 0);

    int fds[2];
    if (sys_pipe(fds) != 0) { utest_check(0, "create a pipe"); return utest_end(); }

    // The CHILD drains; this process writes. That way the writer is the
    // one that must block, which is the property under test -- and the
    // child is slow to start, so the pipe really does fill first.
    // Point OUR fd 0 at the pipe's read end so the child inherits it,
    // then put ours back -- the same dance a shell does for `<`, and
    // the reason no fork() is needed to set a child's stdin.
    int saved_in = sys_dup(0);
    sys_dup2(fds[0], 0);
    int pid = sys_spawn("/tests/pipedrain", 0, -1);
    sys_dup2(saved_in, 0);
    sys_close(saved_in);
    if (pid < 0) { utest_check(0, "spawn /tests/pipedrain"); return utest_end(); }
    // Drop OUR read end: the child holds its own. Leaving it open would
    // not break this test, but it would stop the pipe ever reporting
    // that the reader had gone.
    sys_close(fds[0]);

    static char buf[CHUNK];
    int64_t sent = 0;
    int short_write = 0;
    for (int off = 0; off < TOTAL; off += CHUNK) {
        for (int i = 0; i < CHUNK; i++) buf[i] = byte_at(off + i);
        int64_t n = sys_write(fds[1], buf, CHUNK);
        if (n != CHUNK) { short_write = 1; break; }
        sent += n;
    }
    sys_close(fds[1]);

    utest_check(!short_write, "every write took the WHOLE buffer");
    utest_check(sent == TOTAL, "all of it was written");
    if (sent != TOTAL) {
        utest_notef("(sent %d of %d)", (int)sent, TOTAL);
    }

    int code = -1;
    sys_waitpid(pid, &code);
    // pipedrain exits with 0 when it read TOTAL correct bytes.
    utest_check(code == 0, "the reader got every byte, in order");
    if (code != 0) {
        utest_notef("(reader exited %d)", code);
    }

    return utest_end();
}
