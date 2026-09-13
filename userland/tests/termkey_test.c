// termkey in RING 3, over the same table ring 0 runs.
//
// The encoder and decoder are shared source compiled twice, so the
// KTESTs in kernel/lib/termkey_test.c would pass whether or not ring 3
// could link a byte of it. This is the half that proves it can -- the
// arrangement /tests/klineedit_test already has.
#include <string.h>
#include "termkey.h"
#include "termkey_cases.h"
#include "lib/utest.h"

int main(void) {
    utest_begin("termkey_test", "terminal key sequences, in ring 3",
                UTEST_VERDICT_FILE);

    int enc_ok = 1, dec_ok = 1;
    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        char buf[TERMKEY_MAX + 1];
        int n = termkey_encode(TERMKEY_CASES[i].key, buf, TERMKEY_MAX);
        buf[n] = 0;
        if (strcmp(buf, TERMKEY_CASES[i].seq) != 0) {
            utest_notef("note: %s encoded wrong", TERMKEY_CASES[i].why);
            enc_ok = 0;
        }

        struct termkey_state st;
        memset(&st, 0, sizeof st);
        const char *s = TERMKEY_CASES[i].seq;
        int got = TERMKEY_MORE;
        for (int j = 0; s[j]; j++) got = termkey_feed(&st, (unsigned char)s[j]);
        if (got != TERMKEY_CASES[i].key) {
            utest_notef("note: %s decoded wrong", TERMKEY_CASES[i].why);
            dec_ok = 0;
        }
    }
    utest_check(enc_ok, "every key encodes to its terminal sequence");
    utest_check(dec_ok, "and decodes back to the key it came from");

    struct termkey_state st;
    memset(&st, 0, sizeof st);
    utest_check(termkey_feed(&st, 'a') == 'a', "an ordinary byte passes through");
    utest_check(termkey_feed(&st, 0x01) == 0x01, "and so does a control code");

    // The ESC ambiguity, which is what makes reading a terminal hard.
    memset(&st, 0, sizeof st);
    int esc = termkey_feed(&st, 0x1B);
    int back = termkey_feed(&st, 'b');
    utest_check(esc == TERMKEY_MORE && back == 0x1B,
                "a lone ESC is given back, so Alt-<key> survives");

    return utest_end();
}
