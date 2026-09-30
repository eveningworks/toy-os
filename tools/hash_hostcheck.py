#!/usr/bin/env python3
"""Check /lib/libhash.so's algorithms against hashlib and zlib, on the HOST.

WHY A HOST HARNESS EXISTS BESIDE THE GUEST TEST. `/tests/hash_test`
runs the algorithms where they live and proves they work in toy-os, but
a guest test can only carry the handful of vectors somebody committed --
and a hash's bugs hide at the BOUNDARIES: the block that is exactly 64
bytes, the one that lands the length field in the second pad block, the
chunking that splits a block across two update() calls. That is a sweep
of a few hundred sizes, not nine.

The oracle shares no code with what it judges: SHA-256 comes from
Python's hashlib (OpenSSL) and CRC-32 from zlib. Both are in the
standard library, so this needs nothing but gcc.

  python3 tools/hash_hostcheck.py                  # the standard sweep
  python3 tools/hash_hostcheck.py --max 100000     # further out
  python3 tools/hash_hostcheck.py --positive-control

WHAT IT DOES NOT COVER, said plainly: anything about ring 3, the file
syscalls, or /bin/sum's argument handling and -c mode. That is
tools/sum_test.py, in a guest.

Exit status is non-zero if any vector disagrees.
"""
import argparse
import hashlib
import os
import random
import subprocess
import sys
import tempfile
import zlib
import hostcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Feeds the input through in CHUNKS taken from argv, so the sweep can
# vary where update() boundaries fall -- which is the half of this that
# a one-shot call cannot test.
DRIVER = r"""
#include <uhash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const struct uhash_alg *alg = uhash_find(argv[1]);
    if (!alg) return 2;
    long chunk = strtol(argv[2], NULL, 10);
    if (chunk <= 0) chunk = 4096;

    static unsigned char buf[65536];
    union uhash_ctx ctx;
    alg->init(&ctx);
    for (;;) {
        size_t want = (size_t)chunk < sizeof buf ? (size_t)chunk : sizeof buf;
        size_t n = fread(buf, 1, want, stdin);
        if (n == 0) break;
        alg->update(&ctx, buf, n);
    }
    unsigned char out[UHASH_DIGEST_MAX];
    alg->final(&ctx, out);
    char hex[UHASH_DIGEST_MAX * 2 + 1];
    uhash_hex(out, alg->digest_len, hex);
    printf("%s\n", hex);
    return 0;
}
"""


def build(tmp, poison=False):
    """Compile uhash.c + kcrc.c + ksha256.c + the driver with the host gcc.

    The sources are COPIED into the temp directory, which is what makes
    --positive-control possible without leaving a test hook in shipped
    code: the copy is edited, the original never is.

    kcrc.h is copied rather than reached with -Ikernel/include/api,
    because that directory also holds the toolkit's own string.h --
    which would shadow the host's and take memcpy/strcmp away from a
    compile that has no toy-os libc under it.
    """
    for h in ("kernel/include/api/kcrc.h", "kernel/include/api/ksha256.h",
              "userland/include/uhash.h"):
        hostcheck.stage(tmp, h)
    srcs = [hostcheck.stage(tmp, src, edits=edits, apply=poison, tool="hash_hostcheck")
            for src, edits in (("userland/dynlib/uhash.c", []),
                               ("kernel/lib/ksha256.c", [("ror(e, 6) ^", "ror(e, 7) ^")]),
                               ("kernel/lib/kcrc.c", [("0xEDB88320u", "0xEDB88321u")]))]
    srcs.append(hostcheck.write(tmp, "driver.c", DRIVER))
    return hostcheck.compile(tmp, "uhash_host", srcs, includes=[tmp], tool="hash_hostcheck")


def run(exe, alg, chunk, data):
    r = subprocess.run([exe, alg, str(chunk)], input=data,
                       capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("driver exited %d" % r.returncode)
    return r.stdout.decode().strip()


def expected(alg, data):
    if alg == "sha256":
        return hashlib.sha256(data).hexdigest()
    if alg == "crc32":
        return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
    raise ValueError(alg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=200000,
                    help="largest random input to try (bytes)")
    ap.add_argument("--seed", type=int, default=20260901)
    ap.add_argument("--positive-control", action="store_true",
                    help="break SHA-256 on purpose; the sweep MUST go red")
    args = ap.parse_args()

    rng = random.Random(args.seed)

    # The sizes that matter are around the 64-byte block and the 56-byte
    # point where the length field stops fitting in the last block.
    sizes = list(range(0, 130))
    sizes += [255, 256, 257, 511, 512, 513, 1023, 1024, 1025,
              8191, 8192, 8193, 65535, 65536, 65537]
    sizes += [rng.randrange(0, args.max) for _ in range(40)]
    sizes = sorted({s for s in sizes if s <= args.max})

    # Chunk sizes deliberately NOT multiples of 64, so a block boundary
    # lands inside an update() call rather than between two.
    chunks = [1, 3, 63, 64, 65, 100, 4096]

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, poison=args.positive_control)

        checks = 0
        bad = []
        for size in sizes:
            data = bytes(rng.randrange(256) for _ in range(size)) if size else b""
            for alg in ("crc32", "sha256"):
                for chunk in chunks:
                    # Only sweep every chunking on the small sizes; a
                    # 200 KB input at chunk=1 is 200k update() calls and
                    # proves nothing the small ones did not.
                    if size > 4096 and chunk not in (64, 4096):
                        continue
                    got = run(exe, alg, chunk, data)
                    want = expected(alg, data)
                    checks += 1
                    if got != want:
                        bad.append((alg, size, chunk, got, want))

        print("hash_hostcheck: %d vectors, %d disagreed" % (checks, len(bad)))
        for alg, size, chunk, got, want in bad[:10]:
            print("  %-6s size=%-7d chunk=%-5d got %s want %s"
                  % (alg, size, chunk, got, want))
        if len(bad) > 10:
            print("  ... and %d more" % (len(bad) - 10))

        if args.positive_control:
            if bad:
                print("hash_hostcheck: positive control OK -- the sweep can fail")
                return 0
            print("hash_hostcheck: POSITIVE CONTROL DID NOT FAIL -- "
                  "this suite is measuring nothing")
            return 1
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
