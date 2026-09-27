// KTESTs for SHA-256 against FIPS 180-4's own examples. The ring-3 copy
// is checked against hashlib by tools/hash_hostcheck.py.
#include "ktest.h"
#include "ksha256.h"
#include "string.h"

static int digest_is(const char *msg, size_t len, const char *want) {
    static const char hex[] = "0123456789abcdef";
    struct ksha256 c;
    unsigned char out[KSHA256_LEN];
    char got[KSHA256_LEN * 2 + 1];
    ksha256_init(&c);
    ksha256_update(&c, msg, len);
    ksha256_final(&c, out);
    for (int i = 0; i < KSHA256_LEN; i++) {
        got[i * 2] = hex[out[i] >> 4];
        got[i * 2 + 1] = hex[out[i] & 15];
    }
    got[KSHA256_LEN * 2] = 0;
    return k_strcmp(got, want) == 0;
}

KTEST("ksha256", "the FIPS 180-4 examples") {
    KTEST_ASSERT(digest_is("", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    KTEST_ASSERT(digest_is("abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // 56 bytes: the length no longer fits the first block's pad.
    static const char two[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    KTEST_ASSERT(digest_is(two, sizeof two - 1,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}
