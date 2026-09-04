// The checksum table (userland/lib/uhash.c), in RING 3.
//
// WHY THIS EXISTS RATHER THAN ONLY THE HOST SWEEP -- and rather than
// only kcrc's KTESTs. tools/hash_hostcheck.py judges the same .c against
// hashlib and zlib over ~2000 vectors, which is far more than this
// carries, and it compiles with the HOST gcc: it cannot see whether a
// byte of this links into libuapp.a. kernel/lib/kcrc_test.c runs in the
// kernel and cannot see it either. This is the link, in the ring the
// only caller runs in.
//
// The vectors are the published ones. The chunked case is the property
// /bin/sum depends on: it hashes an 8 KiB block at a time, so a digest
// that is only correct when the whole input arrives in one update() is
// a digest that is wrong on every file over 8 KiB.
//
// Prints one line per case and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <uhash.h>
#include <string.h>
#include <stdio.h>
#include "syscall_abi.h"

// A SPAWNED test's verdict has to reach a FILE as well as the console.
// This binary is dynamic, so the legacy `run` loader refuses it and
// tools/usertest_run.py spawns it instead -- and a spawned child's
// output arrives on the shared serial console across several reads,
// with no exit code the harness can see. It reads /tmp/<name>.out,
// which UTEST_VERDICT_FILE writes.
#include "lib/utest.h"

// The call sites here read `check(what, ok, detail)`; the harness takes
// the boolean first. One adapter rather than transposing a hundred call
// sites: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

// Hashes `data` in `chunk`-byte pieces and returns the hex digest.
static void digest(const char *alg_name, const void *data, size_t len,
                   size_t chunk, char *hex) {
    const struct uhash_alg *alg = uhash_find(alg_name);
    hex[0] = '\0';
    if (!alg) return;
    union uhash_ctx ctx;
    alg->init(&ctx);
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        size_t n = len < chunk ? len : chunk;
        alg->update(&ctx, p, n);
        p += n;
        len -= n;
    }
    unsigned char out[UHASH_DIGEST_MAX];
    alg->final(&ctx, out);
    uhash_hex(out, alg->digest_len, hex);
}

int main(void) {
    utest_begin("hash_test", "uhash: md5, sha1, sha256, crc32", UTEST_VERDICT_FILE);

    char hex[UHASH_DIGEST_MAX * 2 + 1];

    check("the table has crc32 and sha256",
          uhash_find("crc32") && uhash_find("sha256"), "");
    check("and refuses a name it does not have",
          uhash_find("md5") == NULL, "");

    digest("crc32", "123456789", 9, 64, hex);
    check("crc32 of the standard check vector", strcmp(hex, "cbf43926") == 0, hex);

    digest("crc32", "", 0, 64, hex);
    check("crc32 of nothing is zero", strcmp(hex, "00000000") == 0, hex);

    digest("sha256", "", 0, 64, hex);
    check("sha256 of nothing",
          strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca4"
                      "95991b7852b855") == 0, hex);

    digest("sha256", "abc", 3, 64, hex);
    check("sha256 of \"abc\" (FIPS 180-4)",
          strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb4"
                      "10ff61f20015ad") == 0, hex);

    // 56 bytes: the length field no longer fits in the final block, so
    // the pad spills into a second one. An implementation that pads
    // wrongly is correct for "abc" and wrong here.
    digest("sha256", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           56, 64, hex);
    check("sha256 across the pad boundary",
          strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6e"
                      "cedd419db06c1") == 0, hex);

    // The same input in every chunking must give the same digest.
    static unsigned char big[9000];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 31u + 7u);
    char one[UHASH_DIGEST_MAX * 2 + 1];
    digest("sha256", big, sizeof big, sizeof big, one);
    int same = 1;
    for (size_t c = 1; c <= 4096; c *= 7) {
        digest("sha256", big, sizeof big, c, hex);
        if (strcmp(hex, one) != 0) same = 0;
    }
    check("sha256 is chunk-independent", same, one);

    digest("crc32", big, sizeof big, sizeof big, one);
    same = 1;
    for (size_t c = 1; c <= 4096; c *= 7) {
        digest("crc32", big, sizeof big, c, hex);
        if (strcmp(hex, one) != 0) same = 0;
    }
    check("crc32 is chunk-independent", same, one);

    return utest_end();
}
