#!/usr/bin/env python3
"""Check userland/lib/udes.c against OpenSSL's DES, on the HOST.

udes exists for one caller -- /bin/remoted's VNC password check, where a
wrong cipher looks exactly like a wrong password -- so it is checked
against an implementation that shares nothing with it: `openssl enc
-des-ecb` (OpenSSL 3 keeps DES in its legacy provider, loaded here). The
classic FIPS 81 vector is checked as well, and every block is decrypted
back, so encrypt and decrypt are each held to the other.

  python3 tools/des_hostcheck.py                  # 300 random key/blocks
  python3 tools/des_hostcheck.py --count 2000
  python3 tools/des_hostcheck.py --positive-control

Exit status is non-zero if any block disagrees.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

DRIVER = r"""
#include "lib/udes.h"
#include <stdio.h>

static int hex(const char *s, uint8_t out[8]) {
    for (int i = 0; i < 8; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

int main(void) {
    char k[32], b[32];
    while (scanf("%31s %31s", k, b) == 2) {
        uint8_t key[8], in[8], enc[8], dec[8];
        if (!hex(k, key) || !hex(b, in)) return 2;
        struct udes_key ks;
        udes_set_key(&ks, key);
        udes_encrypt(&ks, in, enc);
        udes_decrypt(&ks, enc, dec);
        for (int i = 0; i < 8; i++) printf("%02x", enc[i]);
        printf(" ");
        for (int i = 0; i < 8; i++) printf("%02x", dec[i]);
        printf("\n");
    }
    return 0;
}
"""

# A wrong shift schedule still produces a permutation of the block, which
# is exactly the bug a round-trip alone would pass.
CONTROL = ("static const uint8_t SHIFTS[16] = { 1, 1, 2,",
           "static const uint8_t SHIFTS[16] = { 1, 2, 2,")


def openssl_des(key, block):
    r = subprocess.run(["openssl", "enc", "-des-ecb", "-nopad", "-K", key.hex(),
                        "-provider", "legacy", "-provider", "default"],
                       input=block, capture_output=True)
    if r.returncode != 0:
        sys.exit("des_hostcheck: openssl has no DES here: " + r.stderr.decode().strip())
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--positive-control", action="store_true",
                    help="break the key schedule in a copy; the check must fail")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    # FIPS 81's worked example, then random pairs.
    cases = [(bytes.fromhex("0123456789abcdef"), b"Now is t")]
    cases += [(rng.randbytes(8), rng.randbytes(8)) for _ in range(args.count)]

    with tempfile.TemporaryDirectory() as tmp:
        hostcheck.stage(tmp, "userland/lib/udes.h", dest="lib/udes.h")
        src = hostcheck.stage(tmp, "userland/lib/udes.c", edits=[CONTROL],
                              apply=args.positive_control, tool="des_hostcheck")
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        exe = hostcheck.compile(tmp, "udes_host", [src, drv], includes=[tmp],
                                tool="des_hostcheck")
        feed = "".join(f"{k.hex()} {b.hex()}\n" for k, b in cases)
        out = subprocess.run([exe], input=feed, capture_output=True, text=True,
                             check=True).stdout.split("\n")

    bad = 0
    for i, ((k, b), line) in enumerate(zip(cases, out)):
        enc, dec = line.split()
        # The first is FIPS 81's, whose answer is printed in the standard.
        want = "3fa40e8a984d4815" if i == 0 else openssl_des(k, b).hex()
        if enc != want or dec != b.hex():
            bad += 1
            if bad <= 5:
                print(f"MISMATCH key {k.hex()} block {b.hex()}: udes {enc} (back {dec}), "
                      f"openssl {want}")
    print(f"des_hostcheck: {len(cases) - bad}/{len(cases)} blocks agree with OpenSSL")
    if args.positive_control:
        if bad:
            print("des_hostcheck: positive control FAILED the check, as it must")
            return 0
        print("des_hostcheck: positive control PASSED -- the check cannot see a broken schedule")
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
