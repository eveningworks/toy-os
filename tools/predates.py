#!/usr/bin/env python3
"""Did this failure exist before my changes? Measure it, don't guess.

CLAUDE.md's rule is that "it predates me" is a MEASUREMENT. The way to
take it is to stash the working tree, rebuild at HEAD, run the failing
thing, and restore -- and that is a five-step manual procedure with a
footgun in it (`git stash pop` on a conflict can lose work, which is
why the documented form applies BY SHA and never pops blindly).

A procedure with a footgun that is written down in prose is a script
waiting to happen. This is the script.

    python3 tools/predates.py "python3 tools/init_test.py"
    python3 tools/predates.py --build "make iso" "make test"

WHAT IT GUARANTEES ABOUT YOUR WORKING TREE, because that is the part
worth trusting:

  - The stash is created with a NAMED message and recovered by its
    SHA, never by `pop` and never by index -- an index moves when
    anything else stashes, and a bare pop is what loses a tree.
  - `-u` is passed, so UNTRACKED files (a brand-new .c file, which is
    most of what a feature adds) are stashed too. Without it the new
    files stay in the tree, the "HEAD" build is not HEAD, and the
    comparison is meaningless -- it silently measures HEAD-plus-your-
    new-files.
  - The restore runs in a `finally`, so a Ctrl-C or a crashing command
    still puts the tree back.
  - It REFUSES to run mid-rebase/merge, where stashing is not safe.
  - It REFUSES a SECOND concurrent run, which is the one way these
    guarantees can still lose a tree: the second process stashes a tree
    the first has already emptied, so its "stash" is identical to HEAD,
    and restoring that empty stash wipes the work the first one was
    holding. (Measured: two runs five seconds apart left the tree at
    HEAD with 36 files' work reachable only as a dangling commit.)

WHAT IT CANNOT TELL YOU. That a failure predates you is not that it is
unrelated -- a change can make a latent bug reachable without being
its cause. It answers "was this already red", which is the question
worth answering first, and no more than that.
"""
import argparse
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args, check=True):
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f"predates: git {' '.join(args)} failed:\n{r.stderr.strip()}")
    return r.stdout.strip()


def run(cmd, label):
    print(f"predates: [{label}] {cmd}", flush=True)
    t = time.time()
    r = subprocess.run(cmd, shell=True, cwd=REPO, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    print(f"predates: [{label}] exit {r.returncode} in {time.time() - t:.0f}s", flush=True)
    return r.returncode, out


def tail(out, n):
    lines = [ln for ln in out.splitlines() if ln.strip()]
    return "\n".join("      " + ln for ln in lines[-n:])


def _stash_ref_for(sha):
    """The stash@{n} ref whose commit is `sha`, or None."""
    out = git("stash", "list", "--format=%gd %H", check=False) or ""
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == sha:
            return parts[0]
    return None


def _never_ran(out):
    """Did this run die before the thing under test did any work? The
    signature is a connection failure -- a tool that could not reach a
    guest measured nothing, whatever its exit code says."""
    if not out:
        return False
    markers = ("could not connect to QMP", "Connection refused",
               "ConnectionRefusedError", "could not connect to the serial")
    return any(m in out for m in markers)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("command", help="the command whose result is in question")
    ap.add_argument("--build", default="make iso",
                    help="how to build before running it (default: make iso; "
                         "'' to skip, e.g. for a host-only check)")
    ap.add_argument("--tail", type=int, default=6,
                    help="lines of each run's output to show (default 6)")
    args = ap.parse_args()

    # Refuse where stashing is not safe. A rebase or merge in progress
    # has its own stash-like state and mixing the two is how a tree
    # gets genuinely lost.
    gitdir = git("rev-parse", "--git-dir")
    for marker in ("rebase-merge", "rebase-apply", "MERGE_HEAD", "CHERRY_PICK_HEAD"):
        if os.path.exists(os.path.join(REPO, gitdir, marker)):
            sys.exit(f"predates: refusing -- a {marker} is in progress. "
                     f"Finish or abort it first.")

    # **ONE AT A TIME, OR THE SECOND RUN RESTORES AN EMPTY STASH OVER
    # THE FIRST ONE'S WORK.** An flock, so a run killed with -9 frees it;
    # the pid in the file is for the message.
    import fcntl
    lock_path = os.path.join(REPO, "build", ".predates.lock")
    os.makedirs(os.path.dirname(lock_path), exist_ok=True)
    lock_fh = open(lock_path, "a+")
    try:
        fcntl.flock(lock_fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock_fh.seek(0)
        holder = lock_fh.read().strip() or "unknown pid"
        sys.exit(f"predates: refusing -- another run holds the tree "
                 f"({holder}). Two at once is how a working tree is lost: "
                 f"the second stashes what the first already stashed away, "
                 f"then restores that nothing over it.")
    lock_fh.seek(0)
    lock_fh.truncate()
    lock_fh.write(f"{os.getpid()}\n")
    lock_fh.flush()

    head = git("rev-parse", "--short", "HEAD")
    dirty = git("status", "--porcelain")
    if not dirty:
        sys.exit("predates: the working tree is clean, so there is nothing to "
                 "compare against HEAD. Nothing to measure.")
    n_files = len(dirty.splitlines())

    print(f"predates: HEAD is {head}; {n_files} file(s) differ from it")

    # --- the working tree, first: if it passes there is nothing to ask.
    if args.build:
        rc, _ = run(args.build, "working")
        if rc != 0:
            sys.exit("predates: the working tree does not even BUILD -- fix that "
                     "before asking whether a test failure predates you.")
    work_rc, work_out = run(args.command, "working")

    # --- HEAD.
    # UNTRACKED FILES GO INTO THE STASH TOO (`-u`), so anything created
    # but not yet committed -- a tool written this session, most often --
    # DOES NOT EXIST during the HEAD run. That is correct (HEAD really
    # does not have it) and it is confusing, because the failure looks
    # like the tool being broken rather than absent: an argparse "
    # "unrecognized arguments" or a "No such file". Say so up front when
    # the command names one.
    untracked = [f[3:] for f in dirty.splitlines() if f.startswith("?? ")]
    named = [u for u in untracked if u and u in args.command]
    if named:
        print("predates: NOTE -- the command names untracked file(s) that HEAD "
              "does not have:")
        for u in named:
            print(f"predates:   {u}")
        print("predates: the HEAD run will fail to find them. That is not a "
              "measurement -- commit them first, or compare a command that "
              "exists on both sides.")

    tag = f"predates-{int(time.time())}"
    git("stash", "push", "-u", "-m", tag)
    # Recover the SHA NOW, while it is unambiguous. Looking it up later
    # by index or by message is what breaks when something else stashes.
    stash_sha = git("rev-parse", "stash@{0}")
    print(f"predates: stashed as {tag} ({stash_sha[:10]})")

    head_rc, head_out = None, ""
    restore_failed = False
    try:
        if args.build:
            rc, _ = run(args.build, "HEAD")
            if rc != 0:
                print("predates: HEAD does not build -- cannot compare. "
                      "That is itself worth knowing.")
            else:
                head_rc, head_out = run(args.command, "HEAD")
        else:
            head_rc, head_out = run(args.command, "HEAD")
    finally:
        # ALWAYS, including on Ctrl-C or a crash above.
        git("checkout", "--", ".", check=False)
        git("clean", "-fd", check=False)
        r = subprocess.run(["git", "stash", "apply", stash_sha],
                           cwd=REPO, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"\npredates: COULD NOT RESTORE automatically.\n{r.stderr.strip()}\n"
                  f"Your work is safe in the stash. Recover it with:\n"
                  f"    git stash apply {stash_sha}", file=sys.stderr)
            restore_failed = True   # returned below: a `return` inside finally swallows exceptions
        else:
            # Dropped only after a CLEAN apply, and never on the failure
            # path above -- which leaves the stash intact with the
            # recovery command printed. A conflicted apply is exactly
            # when the stash is the only copy of something. `git stash
            # drop` wants a stash@{n} REF, not a SHA: it accepted the SHA
            # silently and dropped nothing, which is how four predates
            # stashes accumulated over three days.
            ref = _stash_ref_for(stash_sha)
            if ref:
                git("stash", "drop", ref, check=False)
                print(f"predates: working tree restored ({n_files} file(s)), stash dropped")
            else:
                print(f"predates: working tree restored ({n_files} file(s)); "
                      f"stash {stash_sha[:10]} not found in the list, left as is")

        # AND REBUILT, because restoring the SOURCE does not restore the
        # ARTIFACTS: build/ and any boot image still hold what HEAD
        # produced. The next thing anyone runs then tests the other
        # kernel while every file on disk says otherwise -- which is a
        # wrong answer with nothing to suggest it is wrong, and it cost
        # this tool's own author a bogus bug report. Leaving the tree
        # where it was found includes what was built from it.
        if args.build:
            print("predates: rebuilding the working tree "
                  "(HEAD's build artifacts are still in place)")
            run(args.build, "restore")

    if restore_failed:
        return 2

    print()
    print(f"  HEAD ({head}):  exit {head_rc}")
    print(tail(head_out, args.tail))
    print(f"  working tree:   exit {work_rc}")
    print(tail(work_out, args.tail))
    print()

    if head_rc is None:
        print("predates: INCONCLUSIVE -- HEAD did not build.")
        return 1
    if head_rc == 0 and work_rc == 0:
        print("predates: BOTH PASS -- there is no failure to attribute.")
        return 0
    # **A RUN THAT NEVER REACHED THE CODE IS NOT EVIDENCE OF ANYTHING.**
    # A GUI tool that ATTACHES to a guest it does not launch dies in
    # seconds with "Connection refused" on both sides, and the two
    # identical failures then read as a confident PRE-EXISTING -- which
    # is worse than no answer, because it looks like a measurement. Ask
    # for the tool to be driven through a runner that launches a guest
    # (gui_regress.py -k <name>) instead.
    both_unrun = (head_rc == work_rc
                  and _never_ran(head_out) and _never_ran(work_out))
    if both_unrun:
        print("predates: INVALID -- neither run reached the code under test "
              "(both failed to connect, in seconds).")
        print("predates: this is NOT 'pre-existing'. A tool that attaches to a "
              "guest it does not launch needs one started for it --")
        print("predates: re-run it through a launcher, e.g. "
              "predates.py \"python3 tools/gui_regress.py -k <tool>\".")
        return 2

    if head_rc == work_rc:
        print(f"predates: PRE-EXISTING -- HEAD fails the same way (exit {head_rc}).")
        print("predates: note that pre-existing is not the same as unrelated: a "
              "change can make a latent bug reachable without causing it.")
        return 0
    if head_rc == 0 and work_rc != 0:
        print("predates: YOURS -- HEAD passes and the working tree does not.")
        return 1
    print("predates: DIFFERENT -- HEAD fails and the working tree does not. "
          "You may have fixed something.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
