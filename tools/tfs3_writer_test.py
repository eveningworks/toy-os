#!/usr/bin/env python3
"""Host-side: does tfs3_writer.py's OVERWRITE hand every old block back?

Rewriting a file is delete-then-write (`write_file()`), so the blocks
the old copy held must all return to the bitmap. If any do not, the
image gains an allocated block nothing references -- a LEAK, which the
kernel's own `fsck` reports and which accumulates one pair per rewrite.

WHY THE SIZES ARE THE TEST. TFS3's block map is twelve direct pointers,
then single-, double- and triple-indirect tables, and a free path can be
correct for one level and wrong for the next. A fixture that only ever
writes a small file cannot see that: the seed tree has exactly ONE file
over the single-indirect ceiling (/install/kernel.bin, ~4.8 MB), which
is why a real leak sat in the seeding path with every existing test
green. So each size below is chosen to reach one more level than the
one before it, and the double-indirect case is the one that was broken.

Triple-indirect is NOT covered: it starts past 4 GiB, and a test image
that large is not worth the disk. `tfs3_writer.py`'s own write path
marks that level unexercised for the same reason.

No VM: this reads the allocation bitmap directly, so it runs in about a
second and needs nothing built.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tfs3_writer import Tfs3Image, BLOCK  # noqa: E402

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tfs3_writer.py")
IMG_MB = 96

# (label, bytes) -- each one reaches a level the previous did not.
# 12 direct blocks = 48 KiB; + 1024 single-indirect = 4,243,456 bytes.
CASES = [
    ("direct only",      40 * 1024),
    ("single-indirect",  1024 * 1024),
    ("double-indirect",  4_814_512),   # the size of the real kernel.bin
]

g_fail = 0


def check(ok, what):
    global g_fail
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        g_fail += 1


def run(*args):
    subprocess.run([sys.executable, TOOL, *args], check=True,
                   stdout=subprocess.DEVNULL)


def allocated(path):
    """How many blocks the bitmap says are in use."""
    img = Tfs3Image(path)
    n = 0
    for g in range(img.sb["gc"]):
        span = img.group_span(g)
        if not span:
            continue
        bm = img.read_block(img.group_base(g))
        n += sum((bm[i >> 3] >> (i & 7)) & 1 for i in range(span))
    img.close()
    return n


def main():
    print("tfs3_writer_test: an overwrite returns every block it held")
    with tempfile.TemporaryDirectory() as tmp:
        img = os.path.join(tmp, "t.img")
        run("format", img, "--size", str(IMG_MB * 1024 * 1024), "--force")

        for label, size in CASES:
            a = os.path.join(tmp, "a.bin")
            b = os.path.join(tmp, "b.bin")
            with open(a, "wb") as f:
                f.write(b"A" * size)
            with open(b, "wb") as f:
                f.write(b"B" * size)   # same size, different content
            dst = "/f_%d" % size

            run("write", img, a, dst)
            before = allocated(img)
            run("write", img, b, dst)
            after = allocated(img)

            blocks = (size + BLOCK - 1) // BLOCK
            check(after == before,
                  f"{label} ({blocks} blocks): rewrite leaks nothing "
                  f"-- allocated {before} -> {after}")

    print("tfs3_writer_test: " + ("all checks passed" if not g_fail else "FAILURES"))
    return g_fail


sys.exit(main())
