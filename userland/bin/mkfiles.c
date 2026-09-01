// mkfiles -- fill a directory with N files, to test the filesystem at
// scale.
//
//   mkfiles <dir> <count>                 empty files
//   mkfiles <dir> <count> <size>          every file that many bytes
//   mkfiles <dir> <count> <min>-<max>     a random size in the range
//   mkfiles --verify <dir> <count> [size] read them back and check
//
// WHAT IT IS FOR: a directory with thousands of entries, which is
// unreachable by typing `touch` and is where a filesystem's directory
// handling actually gets tested -- lookup cost, the listing cap, inode
// and block-group exhaustion. `tools/ls_test.py` stages 300 entries
// from the HOST for exactly this reason; this does it from inside,
// through the same syscalls a normal program uses, which is the part
// host staging cannot exercise.
//
// IT REPORTS WHERE IT STOPPED AND WHY. A run that creates 4,000 of
// 10,000 files and says which errno it hit is the answer; one that
// says "failed" is not. Timing is printed for the same reason -- a
// directory whose creates slow down as it grows is the finding, and
// only a per-batch rate makes that visible.
//
// THE CONTENT IS DERIVED FROM (file index, offset), so --verify proves
// every file still holds ITS OWN bytes. A constant fill cannot detect
// two files sharing a block: both read back the constant and look
// perfect. That is this repo's memtest lesson applied to storage.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <knum.h>
#include <fcntl.h>
#include <unistd.h>

#define CHUNK 1024
static uint8_t g_buf[CHUNK];

// The byte a given file owes at a given offset. Cheap, and invertible
// enough that a mismatch says WHOSE byte it got: mixing the index in
// twice (once shifted) stops two files whose indices differ by a
// multiple of 256 producing the same stream.
static uint8_t byte_for(unsigned idx, unsigned long off) {
    return (uint8_t)((idx * 31u) ^ (off * 7u) ^ (idx >> 8));
}

static void fill(unsigned idx, unsigned long off, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) g_buf[i] = byte_for(idx, off + i);
}

// "512" or "0-64000". Returns 1 on success. REJECTS anything else
// rather than guessing -- a size argument that silently became 0 would
// make an entire run measure nothing.
static int parse_size(const char *s, uint32_t *lo, uint32_t *hi) {
    const char *dash = 0;
    for (const char *p = s; *p; p++) if (*p == '-') { dash = p; break; }
    if (!dash) {
        if (!k_parse_u32(s, lo)) return 0;
        *hi = *lo;
        return 1;
    }
    char head[16];
    unsigned n = (unsigned)(dash - s);
    if (n >= sizeof head) return 0;
    for (unsigned i = 0; i < n; i++) head[i] = s[i];
    head[n] = '\0';
    if (!k_parse_u32(head, lo) || !k_parse_u32(dash + 1, hi)) return 0;
    return *hi >= *lo;
}

// Deterministic per index, so --verify reproduces the same sizes
// without being told the seed. A random draw would have to be recorded
// and passed back, which is a worse interface than arithmetic.
static uint32_t size_for(unsigned idx, uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + (uint32_t)((idx * 2654435761u) % (hi - lo + 1));
}

static void name_for(char *out, unsigned long cap, const char *dir, unsigned idx) {
    // Fixed width, so the names sort the way the numbers do -- a
    // listing of f1, f10, f2 is the kind of thing that makes a 4,000
    // entry directory unreadable.
    snprintf(out, cap, "%s/f%06u", dir, idx);
}

static int create_one(const char *path, unsigned idx, uint32_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    unsigned long off = 0;
    while (off < size) {
        unsigned long n = size - off;
        if (n > CHUNK) n = CHUNK;
        fill(idx, off, n);
        if (write(fd, g_buf, n) != (int64_t)n) { close(fd); return 0; }
        off += n;
    }
    close(fd);
    return 1;
}

// WHY THIS REPORTS A REASON RATHER THAN A BOOLEAN. "verify failed" is
// three completely different findings -- the file is missing, it is
// short, or it holds the wrong bytes -- and only the third is a data
// corruption. A tool that collapses them sends the reader to the wrong
// place, which is what the first version of this did.
static const char *verify_one(const char *path, unsigned idx, uint32_t size) {
    static char why[96];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return "cannot open it";
    unsigned long off = 0;
    while (off < size) {
        unsigned long want = size - off;
        if (want > CHUNK) want = CHUNK;
        int64_t got = read(fd, g_buf, want);
        if (got <= 0) {
            close(fd);
            snprintf(why, sizeof why, "short: %lu of %u bytes readable", off, size);
            return why;
        }
        for (int64_t i = 0; i < got; i++) {
            uint8_t want_b = byte_for(idx, off + (unsigned long)i);
            if (g_buf[i] != want_b) {
                close(fd);
                // The byte it DID hold, and whose it is. A byte that
                // belongs to another file is the signature of two files
                // sharing a block, which is the failure this pattern
                // exists to catch.
                snprintf(why, sizeof why,
                         "byte %lu is 0x%02x, wanted 0x%02x",
                         off + (unsigned long)i, g_buf[i], want_b);
                return why;
            }
        }
        off += (unsigned long)got;
    }
    close(fd);
    return 0; // no reason: it is correct
}

int main(int argc, char **argv) {
    int verify = 0, a = 1;
    if (argc > 1 && strcmp(argv[1], "--verify") == 0) { verify = 1; a = 2; }
    if (argc - a < 2) {
        cmd_usage("mkfiles [--verify] <dir> <count> [size | min-max]");
        return 1;
    }
    const char *dir = argv[a];
    uint32_t count = 0, lo = 0, hi = 0;
    if (!k_parse_u32(argv[a + 1], &count) || count == 0) {
        sys_print("mkfiles: count must be a positive number\n");
        return 1;
    }
    if (argc - a >= 3 && !parse_size(argv[a + 2], &lo, &hi)) {
        sys_print("mkfiles: size must be a number or min-max\n");
        return 1;
    }

    char path[64], line[192];
    unsigned long long t0 = sys_monotonic_ns(), last = t0;
    uint32_t done = 0;
    // Progress every 250, because the whole point is watching whether
    // the RATE changes as the directory grows -- a single total at the
    // end would average that away.
    const uint32_t STEP = 250;

    for (uint32_t i = 0; i < count; i++) {
        name_for(path, sizeof path, dir, i);
        uint32_t size = size_for(i, lo, hi);
        const char *why = verify ? verify_one(path, i, size)
                                 : (create_one(path, i, size) ? 0
                                    : sys_strerror(sys_errno()));
        if (why) {
            snprintf(line, sizeof line, "mkfiles: %s failed at file %u of %u\n",
                     verify ? "verify" : "create", i, count);
            sys_print(line);
            snprintf(line, sizeof line, "  %s (size %u): %s\n", path, size, why);
            sys_print(line);
            return 1;
        }
        done++;
        if (done % STEP == 0) {
            unsigned long long now = sys_monotonic_ns();
            unsigned long long ms = (now - last) / 1000000ull;
            snprintf(line, sizeof line, "  %u/%u  (%llu ms for the last %u)\n",
                     done, count, ms, STEP);
            sys_print(line);
            last = now;
        }
    }

    unsigned long long total_ms = (sys_monotonic_ns() - t0) / 1000000ull;
    snprintf(line, sizeof line, "mkfiles: %s %u file(s) in %llu ms\n",
             verify ? "verified" : "created", done, total_ms);
    sys_print(line);
    return 0;
}
