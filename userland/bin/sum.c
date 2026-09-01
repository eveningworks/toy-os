// sum -- checksums and digests of files.
//
// ONE PROGRAM WITH AN ALGORITHM TABLE, not a binary per algorithm.
// coreutils shipped md5sum/sha1sum/sha256sum for two decades and then
// consolidated them into `cksum -a` in 9.0; this is that shape under
// the name POSIX already reserves for the job. The table is
// <uhash.h>, and the code is /lib/libhash.so -- a third algorithm is
// a row there, and every program that links it gains the row.
//
// THE OUTPUT SHAPE IS THE ALGORITHM'S, so the host is an oracle. crc32
// prints cksum(1)'s "<decimal> <bytes> <name>"; sha256 prints
// sha256sum(1)'s "<hex>  <name>", two spaces and all, so a manifest
// written here is one `sha256sum -c` can check on Linux and vice versa.
//
// THE crc32 IS THE ZLIB/IEEE ONE, NOT POSIX cksum's. They are different
// polynomials over different bit orders and agree on nothing; this is
// the one zip, gzip, PNG and a GPT header use. The host equivalent is
// `cksum -a crc32b` (coreutils 9.6+) or python3's zlib.crc32 -- plain
// `cksum` will disagree, correctly.
//
// A missing file does not stop the remaining ones, as in cat: the
// failure is reported, the exit code goes non-zero, and the rest are
// still hashed.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <uhash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define USAGE "sum [-a ALGORITHM] [-c LISTFILE] [FILE...]"

// One block per read. .bss, not the stack -- the ring-3 frame budget is
// 2 KiB.
#define CHUNK 8192
static unsigned char g_buf[CHUNK];

// A manifest line. Long enough for a sha512 hex plus this OS's 64-byte
// paths, with room to see that a longer line was truncated.
#define LINE_MAX 256

struct result {
    unsigned char digest[UHASH_DIGEST_MAX];
    uint64_t bytes;
};

// Hashes an open fd to the end. 0 on success, -1 on a read error.
static int hash_fd(const struct uhash_alg *alg, int fd, struct result *out) {
    union uhash_ctx ctx;
    alg->init(&ctx);
    out->bytes = 0;
    for (;;) {
        int64_t n = read(fd, g_buf, sizeof g_buf);
        if (n == 0) break;
        if (n < 0) return -1;
        alg->update(&ctx, g_buf, (size_t)n);
        out->bytes += (uint64_t)n;
    }
    alg->final(&ctx, out->digest);
    return 0;
}

// "-" is stdin, here and in every tool that takes file arguments.
static int hash_named(const struct uhash_alg *alg, const char *name,
                      struct result *out) {
    if (strcmp(name, "-") == 0) return hash_fd(alg, 0, out);
    int fd = open(name, O_RDONLY);
    if (fd < 0) return -1;
    int rc = hash_fd(alg, fd, out);
    close(fd);
    return rc;
}

static void print_line(const struct uhash_alg *alg, const struct result *r,
                       const char *name) {
    char line[LINE_MAX];
    if (alg->cksum_style) {
        // The digest read back as one number, which for a 4-byte CRC is
        // what cksum prints.
        uint64_t v = 0;
        for (unsigned i = 0; i < alg->digest_len; i++)
            v = (v << 8) | r->digest[i];
        snprintf(line, sizeof line, "%llu %llu %s\n", (unsigned long long)v,
                 (unsigned long long)r->bytes, name);
    } else {
        char hex[UHASH_DIGEST_MAX * 2 + 1];
        uhash_hex(r->digest, alg->digest_len, hex);
        snprintf(line, sizeof line, "%s  %s\n", hex, name);
    }
    sys_print(line);
}

// --- verify ----------------------------------------------------------

// Splits one manifest line into the fields the algorithm prints. Returns
// the file name, or NULL if the line is not this algorithm's shape.
// `want` gets the expected digest, `want_bytes` the expected size (0
// when the shape carries none).
static const char *parse_line(const struct uhash_alg *alg, char *line,
                              unsigned char *want, uint64_t *want_bytes) {
    *want_bytes = 0;

    if (alg->cksum_style) {
        // "<decimal> <bytes> <name>". strtoul twice, then the rest of
        // the line -- a name may contain spaces, so it is never split.
        char *end = NULL;
        unsigned long v = strtoul(line, &end, 10);
        if (end == line || *end != ' ') return NULL;
        char *p = end + 1;
        unsigned long b = strtoul(p, &end, 10);
        if (end == p || *end != ' ') return NULL;
        for (unsigned i = 0; i < alg->digest_len; i++)
            want[alg->digest_len - 1 - i] = (unsigned char)(v >> (i * 8));
        *want_bytes = (uint64_t)b;
        return end + 1;
    }

    // "<hex>  <name>", GNU's two spaces. Anything else -- one space, a
    // BSD "SHA256 (f) = h" line -- is REJECTED rather than guessed at,
    // because a manifest half-understood is worse than one refused.
    unsigned need = alg->digest_len * 2;
    for (unsigned i = 0; i < need; i++) {
        char ch = line[i];
        int hi = (ch >= '0' && ch <= '9') ? ch - '0'
               : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
               : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (hi < 0) return NULL;
        if (i % 2 == 0) want[i / 2] = (unsigned char)(hi << 4);
        else want[i / 2] |= (unsigned char)hi;
    }
    if (line[need] != ' ' || line[need + 1] != ' ') return NULL;
    return line + need + 2;
}

static int verify(const struct uhash_alg *alg, const char *listfile) {
    FILE *f = fopen(listfile, "r");
    if (!f) {
        cmd_fail("sum", listfile);
        return 1;
    }

    char line[LINE_MAX];
    unsigned checked = 0, bad = 0, unreadable = 0, malformed = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        unsigned char want[UHASH_DIGEST_MAX];
        uint64_t want_bytes = 0;
        const char *name = parse_line(alg, line, want, &want_bytes);
        if (!name || !name[0]) {
            malformed++;
            continue;
        }

        checked++;
        struct result got;
        char out[LINE_MAX];
        if (hash_named(alg, name, &got) < 0) {
            unreadable++;
            snprintf(out, sizeof out, "%s: FAILED open or read\n", name);
            sys_print(out);
            continue;
        }
        // The SIZE is half the check in cksum's shape, and a file that
        // grew by a multiple of the polynomial is exactly what it is
        // there to catch.
        int ok = memcmp(got.digest, want, alg->digest_len) == 0 &&
                 (!alg->cksum_style || got.bytes == want_bytes);
        if (!ok) bad++;
        snprintf(out, sizeof out, "%s: %s\n", name, ok ? "OK" : "FAILED");
        sys_print(out);
    }
    fclose(f);

    char msg[LINE_MAX];
    if (malformed) {
        snprintf(msg, sizeof msg, "sum: %u line(s) are not %s %s\n", malformed,
                 alg->name, alg->cksum_style ? "cksum lines" : "digest lines");
        sys_print(msg);
    }
    if (checked == 0) {
        // Distinguished from "everything matched", which is what an
        // empty or wrong-format manifest would otherwise look like.
        sys_print("sum: no usable lines in the list file\n");
        return 1;
    }
    if (bad || unreadable) {
        snprintf(msg, sizeof msg, "sum: %u of %u did not match, %u unreadable\n",
                 bad, checked, unreadable);
        sys_print(msg);
        return 1;
    }
    return malformed ? 1 : 0;
}

// --- entry -----------------------------------------------------------

static void list_algorithms(void) {
    char line[LINE_MAX];
    sys_print("sum: algorithms:");
    for (unsigned i = 0;; i++) {
        const struct uhash_alg *a = uhash_nth(i);
        if (!a) break;
        snprintf(line, sizeof line, " %s", a->name);
        sys_print(line);
    }
    sys_print("\n");
}

int main(int argc, char **argv) {
    const struct uhash_alg *alg = uhash_find("crc32");
    const char *listfile = NULL;
    int first_file = argc;

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            alg = uhash_find(argv[++i]);
            if (!alg) {
                cmd_usage(USAGE);
                list_algorithms();
                return 1;
            }
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            listfile = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            cmd_usage(USAGE);
            list_algorithms();
            return 1;
        } else {
            break;
        }
    }
    first_file = i;

    if (listfile) {
        if (first_file != argc) {
            cmd_usage(USAGE);
            return 1;
        }
        return verify(alg, listfile);
    }

    // No file arguments: stdin, named "-" so the line round-trips
    // through -c the way `sum f > list` does.
    if (first_file == argc) {
        struct result r;
        if (hash_fd(alg, 0, &r) < 0) {
            cmd_fail("sum", "-");
            return 1;
        }
        print_line(alg, &r, "-");
        return 0;
    }

    int failed = 0;
    for (; first_file < argc; first_file++) {
        struct result r;
        if (hash_named(alg, argv[first_file], &r) < 0) {
            cmd_fail("sum", argv[first_file]);
            failed = 1;
            continue;
        }
        print_line(alg, &r, argv[first_file]);
    }
    return failed;
}
