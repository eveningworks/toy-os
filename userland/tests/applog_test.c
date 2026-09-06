// A service's stdout reaching the application log, tagged with its name.
//
// THE POINT IS THAT IT IS A DIFFERENT PROCESS. The KTESTs beside
// kernel/lib/applog.c cover the ring -- what a write stores, what ages
// out, what the fact reports. None of that touches the half this exists
// for: that `SPAWN_FD_LOG` in a spawn makes the CHILD's fd 1 land in the
// ring, tagged with the child's own name and not the parent's.
//
// The control is the second spawn. A test that only checks the log
// contains the child's output would pass just as well if every write in
// the system went there, so the same program is run again with an
// ordinary stdout and its marker must be ABSENT.
//
// Prints one line per check and exits with the number of failures.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "query_abi.h"
#include "applog.h"

// Two markers, distinct per run: the log is persistent and a marker
// reused across runs would let a record from a PREVIOUS boot satisfy
// this run's assertion.
static char g_logged[64];
static char g_console[64];

static int run_echo(const char *text, int stdout_fd) {
    int pid = sys_spawn("/bin/echo", text, stdout_fd);
    if (pid <= 0) return -1;
    int status = 0;
    sys_waitpid(pid, &status);
    return status;
}

// Walks the ring from the oldest record still held. Returns the tag it
// was found under, or NULL.
static const char *find(const char *text, char *tag_out, unsigned cap) {
    struct query_applog q;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_APPLOG, i, &q, sizeof q) <= 0) return 0;
        if (strcmp(q.text, text) == 0) {
            snprintf(tag_out, cap, "%s", q.tag);
            return tag_out;
        }
    }
}

int main(void) {
    utest_begin("applog_test", "a spawned child's stdout, tagged",
                UTEST_VERDICT_FILE);

    unsigned long long stamp = sys_monotonic_ns();
    snprintf(g_logged, sizeof g_logged, "applog-logged-%llu", stamp);
    snprintf(g_console, sizeof g_console, "applog-console-%llu", stamp);

    utest_check(run_echo(g_logged, SPAWN_FD_LOG) == 0,
                "a child spawned onto the log runs and exits");
    utest_check(run_echo(g_console, -1) == 0,
                "the same child spawned onto the console runs and exits");

    char tag[APPLOG_TAG_MAX];
    const char *found = find(g_logged, tag, sizeof tag);
    utest_check(found != 0, "what it printed is in the application log");
    // The tag is the CHILD's name. This process is applog_test, so a tag
    // of "echo" is the only thing that proves the kernel took it from
    // the writer rather than from whoever asked for the spawn.
    utest_check(found && strcmp(found, "echo") == 0,
                "the record is tagged with the child's own name");

    char other[APPLOG_TAG_MAX];
    utest_check(find(g_console, other, sizeof other) == 0,
                "a child with an ordinary stdout leaves no record");

    return utest_end();
}
