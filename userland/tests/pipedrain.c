// Reads fd 0 to EOF and verifies the byte pattern pipefull_test wrote.
//
// A separate process on purpose: the point of pipefull_test is that the
// WRITER blocks when the buffer fills, which only happens if the reader
// is somewhere else and slower.
//
// Exits 0 when it saw exactly TOTAL bytes and every one matched, 1 on a
// mismatch (with the offset on stderr), 2 on a short stream. Distinct
// codes so a failure says WHICH way it went wrong.
#include <stdint.h>
#include "rt/sys.h"
#include <stdio.h>

#define TOTAL (16 * 1024)

static char byte_at(int i) { return (char)((i * 7 + (i >> 8)) & 0x7f); }

int main(void) {
    static char buf[256];
    char msg[96];
    int seen = 0;

    for (;;) {
        int64_t n = sys_read(0, buf, sizeof buf);
        if (n <= 0) break; // 0 = EOF once the writer closes
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] != byte_at(seen + (int)i)) {
                snprintf(msg, sizeof msg, "pipedrain: mismatch at %d\n",
                         seen + (int)i);
                sys_eprint(msg);
                return 1;
            }
        }
        seen += (int)n;
    }

    if (seen != TOTAL) {
        snprintf(msg, sizeof msg, "pipedrain: got %d of %d bytes\n", seen, TOTAL);
        sys_eprint(msg);
        return 2;
    }
    return 0;
}
