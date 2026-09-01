#!/usr/bin/env python3
"""/bin/sum -- the command, in a guest, against the HOST's own digests.

WHAT ONLY THIS CAN CHECK. tools/hash_hostcheck.py judges the algorithms
against hashlib and zlib over ~2000 vectors, and /tests/hash_test proves
they link and run in ring 3. Neither runs the PROGRAM: the argument
parsing, the streaming read of a real file through the filesystem, the
two line shapes, and -c.

THE LOAD-BEARING CHECK IS THE CROSS-RING ONE. This hashes a file that
exists on both sides -- /bin/hello is build/userland/bin/hello.elf, byte
for byte -- and requires the guest's answer to equal what coreutils and
zlib say on the host. A guest-only check would pass just as happily with
a consistently wrong implementation on both ends of its own comparison.

The -c half is asserted BOTH ways, because a verifier that says OK to
everything and one that works are indistinguishable from the OK alone:
a manifest sum itself wrote must verify, and one with a deliberately
wrong size must FAIL with a non-zero exit.

It writes only under /tmp on the guest and touches no host file.

    python3 tools/vm.py start
    python3 tools/sum_test.py
"""
import argparse
import hashlib
import os
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files that exist identically on both sides. /bin/<name> in the guest is
# build/userland/bin/<name>.elf on the host -- `make iso` copies it.
SUBJECTS = ["hello", "echo"]

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}"
          + (f"   -- {detail}" if detail and not ok else ""))


def host(path):
    data = open(path, "rb").read()
    return (zlib.crc32(data) & 0xFFFFFFFF, len(data),
            hashlib.sha256(data).hexdigest())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instance", default=None)
    args = ap.parse_args()

    # vm.py driven as a subprocess, ATTACHING to a guest somebody else
    # started -- the same shape grep_test.py and stdin_test.py use, and
    # why ondemand_sweep.py marks this one wants_vm.
    def run(cmd):
        argv = [sys.executable, os.path.join(REPO, "tools", "vm.py")]
        if args.instance:
            argv += ["--instance", str(args.instance)]
        argv += ["exec", cmd]
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
        return r.stdout + r.stderr

    print("sum_test: /bin/sum against the host's own digests")

    # --- 1. the two line shapes, cross-checked on the host ------------
    for name in SUBJECTS:
        want_crc, want_len, want_sha = host(
            os.path.join(REPO, "build/userland/bin", name + ".elf"))

        out = run(f"sum /bin/{name}")
        line = f"{want_crc} {want_len} /bin/{name}"
        check(f"crc32 of /bin/{name} matches zlib and the size",
              line in out, f"want {line!r}")

        out = run(f"sum -a sha256 /bin/{name}")
        line = f"{want_sha}  /bin/{name}"
        check(f"sha256 of /bin/{name} matches coreutils",
              line in out, f"want {line!r}")

    # --- 2. refusals ---------------------------------------------------
    out = run("sum -a md5 /bin/hello")
    check("an unknown algorithm is refused and the known ones listed",
          "usage:" in out and "crc32 sha256" in out and "exit 1" in out)

    out = run("sum /nosuchfile")
    check("a missing file fails with a reason and a non-zero exit",
          "no such file" in out and "exit 1" in out)

    # --- 3. -c, both ways ----------------------------------------------
    # WAIT ON THE ARTIFACT, never on a sleep. The debug console's `spawn`
    # returns as soon as the child is started, so a verify issued behind
    # it reads a file that is not there yet -- which fails as "no usable
    # lines" and reads exactly like a broken parser. Two checks failed
    # that way before this poll existed, on a sum that was correct.
    #
    # AND WAIT FOR THE WHOLE LINE, not for any content at all. A poll
    # that stops at the first non-empty read catches the file MID-WRITE
    # -- one read returned "3637643924 " with the rest still to come,
    # and the verify behind it then reported a malformed manifest, which
    # looks exactly like a broken parser. `tail` is the last token of
    # the line, so it can only appear once the write is complete.
    def write_fixture(path, command, tail):
        run(f"spawn /bin/tosh -c {command} > {path}")
        for _ in range(40):
            if tail in run(f"cat {path}"):
                return True
        return False

    ok = write_fixture("/tmp/sum_list", "sum /bin/hello", "/bin/hello")
    check("the manifest was written", ok, "nothing appeared")
    out = run("sum -c /tmp/sum_list")
    check("a manifest sum wrote verifies", "/bin/hello: OK" in out
          and "exit 1" not in out)

    write_fixture("/tmp/sum_sha", "sum -a sha256 /bin/echo", "/bin/echo")
    out = run("sum -a sha256 -c /tmp/sum_sha")
    check("and so does a sha256 one", "/bin/echo: OK" in out
          and "exit 1" not in out)

    # A right CRC with a wrong SIZE. This is the check that distinguishes
    # cksum's two-field line from a bare digest -- a verifier ignoring
    # the size passes it.
    want_crc = host(os.path.join(REPO, "build/userland/bin/hello.elf"))[0]
    write_fixture("/tmp/sum_bad", f"echo {want_crc} 5 /bin/hello", "/bin/hello")
    out = run("sum -c /tmp/sum_bad")
    check("a right crc with a wrong size FAILS",
          "/bin/hello: FAILED" in out and "exit 1" in out)

    write_fixture("/tmp/sum_gone", "echo 12345 9 /nosuchfile", "/nosuchfile")
    out = run("sum -c /tmp/sum_gone")
    check("a listed file that cannot be read is named, not skipped",
          "/nosuchfile: FAILED open or read" in out and "1 unreadable" in out)

    write_fixture("/tmp/sum_junk", "echo not-a-manifest-line", "manifest-line")
    out = run("sum -c /tmp/sum_junk")
    check("a manifest with no usable line is refused, not called clean",
          "no usable lines" in out and "exit 1" in out)

    # --- 4. the library is SHARED ---------------------------------------
    out = run("ls /lib")
    check("libhash.so is installed in /lib", "libhash.so" in out)

    passed = sum(1 for _, ok in results if ok)
    print(f"sum_test: {passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
