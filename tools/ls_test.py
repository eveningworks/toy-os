#!/usr/bin/env python3
"""/bin/ls: format flags, ordering, colour, and the truncation limit.

WHAT IS UNDER TEST
------------------
`ls` had no test at all, and that is how it shipped a silent bug: the
kernel filled at most SYS_LISTDIR_MAX entries and said nothing about
what it left behind, so a directory of 40 files listed 32 and stopped
with no message. Nothing was wrong on screen -- the output simply ended.

Four properties, each with a failure the others would not catch:

1. **Ordering is reproducible.** SYS_LISTDIR returns whatever order the
   filesystem walked, so an unsorted `ls` can list the same directory
   differently on two machines. Asserted by comparing against Python's
   own sort, not by eyeballing a listing.

2. **The default is ONE PER LINE.** Real ls columnises to a terminal and
   prints one per line to a pipe; there is no isatty() here, so the
   default has to be the parseable one. Making columns the default broke
   Notepad's dialog test the moment it was tried, because that test reads
   `ls /` a line at a time.

3. **Colour is escape sequences, and they never arrive as TEXT.** The
   console parses ESC[...m (kernel/lib/ansi.c). If that parser were not
   wired in, the bytes would print as a literal "[1;36m" beside every
   directory name -- which is precisely what this checks for, since the
   captured console output cannot show colour itself.

4. **A truncated listing SAYS SO.** The fixture is staged from the HOST
   (tfs3_writer.py) rather than by typing `touch` hundreds of times, so
   this reaches the branch at all -- a 40-file directory does not, now
   that the cap is 256, and a test that only creates a handful would be
   green with the whole limit removed.

RUN IT ON DEMAND. It builds its own >256-entry directory in a THROWAWAY
copy of disk.img, so it never touches the real image.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.path.join(REPO, "tools", "vm.py")
WRITER = os.path.join(REPO, "tools", "tfs3_writer.py")

# Comfortably past SYS_LISTDIR_MAX (256), so the cap is reached whatever
# else the directory happens to hold.
BIG_DIR = "/tmp/lsbig"
BIG_COUNT = 300

_fail = 0


def check(name, ok, detail=""):
    global _fail
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail and not ok else ""))
    if not ok:
        _fail += 1


class VM:  # noqa: F811 -- shadows the path constant on purpose, see below
    """vm.py, driven as a subprocess.

    The same shape init_test.py uses. Kept local rather than shared
    because the two want different lifetimes and factoring it out would
    be a module with two callers and no agreement between them.
    """

    def __init__(self, disk, instance):
        self.disk, self.instance = disk, instance

    def _cmd(self, *argv):
        cmd = [sys.executable, os.path.join(REPO, "tools", "vm.py"), "--disk", self.disk]
        if self.instance:
            cmd += ["--instance", str(self.instance)]
        return cmd + list(argv)

    def run(self, *argv):
        r = subprocess.run(self._cmd(*argv), cwd=REPO, capture_output=True, text=True)
        return r.stdout + r.stderr

    def sh(self, cmd):
        """One shell command's output, with the kernel's own log lines
        dropped -- they share this console (see notepad_client_test.py's
        note on exactly this)."""
        out = self.run("exec", cmd)
        return [l for l in out.splitlines()
                if not l.startswith(("elf_run:", "syscall:", "vm:"))]


def stage_fixture(disk):
    """A directory with BIG_COUNT files in it, written from the host."""
    tmp = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
    tmp.write("x\n")
    tmp.close()
    # The directory first -- `write` does not create parents.
    r = subprocess.run([sys.executable, WRITER, "mkdir", disk, BIG_DIR],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        os.unlink(tmp.name)
        return f"mkdir {BIG_DIR} failed:\n{r.stdout}{r.stderr}"
    for i in range(BIG_COUNT):
        # Zero-padded so the guest's sort and Python's agree -- an
        # unpadded f10 sorts before f9 as a STRING, which would make this
        # tool's own expectation wrong rather than the shell's.
        r = subprocess.run([sys.executable, WRITER, "write", disk, tmp.name,
                            f"{BIG_DIR}/f{i:04d}"],
                           cwd=REPO, capture_output=True, text=True)
        if r.returncode != 0:
            os.unlink(tmp.name)
            return f"staging {BIG_DIR}/f{i:04d} failed:\n{r.stdout}{r.stderr}"
    os.unlink(tmp.name)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--disk", default=None)
    ap.add_argument("--instance", type=int, default=0)
    args = ap.parse_args()

    tmp_disk = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("ls_test: no disk.img -- run `make iso` first")
            return 2
        t = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        t.close()
        # --sparse=always: disk.img is a few MB of data in a 9 GB sparse
        # file, and a hole-filling copy costs the whole 9 GB.
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, t.name], check=True)
        args.disk = tmp_disk = t.name
        print(f"ls_test: staging {BIG_COUNT} files in {BIG_DIR}")
        err = stage_fixture(args.disk)
        if err:
            print("ls_test: " + err)
            os.unlink(tmp_disk)
            return 2

    vm = VM(args.disk, args.instance)
    try:
        if "ready" not in vm.run("start"):
            print("ls_test: could not start the VM")
            return 2

        print("ordering and format")
        names = vm.sh("ls /bin")
        entries = [n.strip().rstrip("/") for n in names if n.strip()]
        check("the default prints one entry per line",
              all(" " not in n.strip() for n in names if n.strip()),
              f"got {names}")
        check("entries come back sorted by name",
              entries == sorted(entries), f"{entries}")

        cols = [l for l in vm.sh("ls -C /bin") if l.strip()]
        check("-C puts several entries on one line",
              any(len(l.split()) > 1 for l in cols), f"{cols}")

        long_rows = [l for l in vm.sh("ls -l /bin") if l.strip().startswith(("-", "d"))]
        check("-l gives a type, a size and a timestamp",
              long_rows and all(re.search(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", r)
                                for r in long_rows),
              f"{long_rows[:2]}")

        human = [l for l in vm.sh("ls -lh /bin") if l.strip().startswith("-")]
        check("-h prints human-readable sizes",
              any(re.search(r"\d+(\.\d)?[KMG]\b", r) for r in human), f"{human[:2]}")

        rev = [n.strip().rstrip("/") for n in vm.sh("ls -r /bin") if n.strip()]
        check("-r reverses the order", rev == sorted(entries, reverse=True), f"{rev}")

        big_first = [l for l in vm.sh("ls -lS /bin") if l.strip().startswith("-")]
        sizes = [int(r.split()[1]) for r in big_first if r.split()[1].isdigit()]
        check("-S lists the largest first", sizes == sorted(sizes, reverse=True), f"{sizes}")

        print("colour")
        # The console consumes ESC[..m, so colour cannot be SEEN here --
        # what is asserted is that the escapes were consumed rather than
        # printed. A missing parser shows up as literal "[1;36m" text.
        root = "\n".join(vm.sh("ls /"))
        check("escape sequences never arrive as literal text",
              "[1;36m" not in root and "\x1b" not in root, repr(root[:120]))
        check("a directory still carries its trailing slash with colour off",
              any(l.strip().endswith("/") for l in vm.sh("ls --color=never /")))

        print("recursion, and the limit")
        rec = vm.sh("ls -R /etc")
        check("-R prints a header per subdirectory",
              any(l.strip().endswith(":") for l in rec), f"{rec[:4]}")

        big = vm.sh(f"ls -1 {BIG_DIR}")
        listed = [l for l in big if l.strip().startswith("f")]
        check(f"a directory of {BIG_COUNT} files is truncated at the cap",
              len(listed) < BIG_COUNT, f"listed {len(listed)}")
        check("...and the truncation is REPORTED, not silent",
              any("truncated" in l for l in big),
              "listing simply stopped -- the silent bug is back")

        bad = "\n".join(vm.sh("ls --nonsense /"))
        check("an unknown option is refused rather than guessed at",
              "unknown option" in bad, repr(bad[:120]))
    finally:
        vm.run("stop")
        if tmp_disk:
            os.unlink(tmp_disk)

    print(f"\nls_test: {'all checks passed' if _fail == 0 else f'{_fail} FAILED'}")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
