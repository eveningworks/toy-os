# Shipping a verified change

> **Verification, as of 2026-08-14:** `tools/preflight.sh` runs the
> full gate -- build, iso, `check_layout.py`, `boot_smoke_test.py` and
> the in-kernel test suite (`tools/ktest_run.py`; 81 tests at time of
> writing -- don't assert on the count, it grows), and `make verify`
> is the same gate without the `git status`
> summary. For checking behaviour while iterating, `python3 tools/vm.py
> exec "<shell command>"` returns the command's output as text and is
> usually a better check than a screenshot -- screenshots are still
> required as proof for GUI work.
>
> **For a GUI change, `make verify` is NOT sufficient on its own.** Also
> run, against a `--disk` copy:
> ```
> python3 tools/gui_regress.py             # every GUI test tool, ~82 checks, ~2 min
> python3 tools/damage_sweep.py            # damage invariant, exit != 0 on violation
> python3 tools/dialog_test.py             # confirm dialog, by pixel value
> ```
> `gui_regress.py` is the one to reach for first -- it runs
> `uidemo_test`, `uiclient_test`, `winclient_test`, `gfxdemo_test`,
> `calculator_client_test`, `notepad_client_test` and `uterm_test`, each
> against its own fresh disk copy and its own VM (both matter: several
> write files, and all of them expect an empty desktop). `-k NAME` for a
> subset, `--logs DIR` to keep full output. `damage_sweep.py` stays
> separate because it is much slower under `gui damage verify on` and
> has its own `--positive-control` protocol.
> `damage_sweep.py --random 60 --seed N` finds what the fixed sequence
> doesn't -- three of the five damage bugs it caught came from the random
> walk. And check `du disk.img` (not `ls -l`): 9 GiB apparent is normal,
> but allocated should be ~3 MiB; the matching writer tool's trim
> (`tools/tfs3_writer.py trim disk.img` for a fresh-built image --
> TFS3 is the default format since 2026-08-14 -- `tfs2_writer.py` for
> an old TFS2 one) reclaims it if not. After touching `kernel/fs/`,
> also run `python3 tools/fs_switch_test.py`.


Everything below assumes you've already built and tested the change
(step 3-4 of the main workflow) and it's clean. This is the mechanical
part: getting it into git history correctly.

**Versioning here is semver + a `-dev` suffix (`VERSION`, repo root),
not a per-change build number** -- this replaced an earlier
`BUILD_NUMBER`/`tools/bump_build.sh <fix|feature|major>` scheme (see
`docs/decisions.md`'s versioning entry for the full history and why).
If you see `BUILD_NUMBER` or `tools/bump_build.sh` referenced anywhere
including in this skill's older text, that's stale -- `VERSION` and
`tools/set_version.sh` are current. Re-check `CLAUDE.md` if in doubt;
conventions here have changed before without every doc catching up
immediately.

## 1. Most changes: no version bump at all

**THERE IS NO CHANGELOG -- it was closed on 2026-08-15 and DELETED on
2026-08-18 (see CLAUDE.md). Do not let older text anywhere in this file
talk you into looking for one.** The
default path for a routine change is now: a commit message that lists
every changed file with a one-line note, a comment beside the code for
any mechanism or trap, a `docs/decisions.md` entry (self-contained, not
a pointer) for a why-this-way question, and `docs/roadmap.md` for
anything still broken. `VERSION` stays exactly where it is. The four
changelog files stay in the tree, frozen, because ~800 references
across the repo point into them. There's no per-change ceremony to run here anymore; that
was the entire point of the switch away from the build-number scheme.

Only touch `VERSION` when actually starting a new dev round or cutting
a real release (rare, and usually an explicit decision, not something
a routine feature/fix triggers on its own):
```
tools/set_version.sh 0.2.0-dev   # starts a new dev round -- rewrites VERSION only
tools/set_version.sh 0.2.0       # cuts a release -- rewrites VERSION AND stamps
                                  # CHANGELOG.md's [Unreleased] as "## [0.2.0] - <date>",
                                  # opening a fresh empty [Unreleased] above it
```
Rebuild after either (`make all && make iso`) so `version.h` picks up
the change, and re-run the boot smoke test.

## 1b. At a release: run the QEMU matrix

`python3 tools/qemu_matrix.py` -- the suite against QEMU 6.2, 7.2 and
8.2 in Docker, ~15s per version once the images are cached. Run it
BEFORE cutting the tag, alongside `preflight.sh`.

Why at a release specifically: the ISOs you are about to publish will
be booted by people on whatever QEMU their distro ships, which is
routinely years behind the developer's. A defect that depends on the
host -- and there has been at least one, a virtio-blk poll budget that
was invisible on 11.1 and fired every time on 8.2.2 -- ships silently
otherwise.

**A failure here is a CONVERSATION, not a blocker.** An older QEMU
disagreeing may be an emulator quirk rather than a bug in this OS.
Bring the user: which versions differ, what the failure is, and whether
it reproduces on the current QEMU. They decide whether it is worth
fixing before the release, worth filing in `docs/bugs.md`, or worth
noting in the release notes as a known limitation. Do not delay a
release on it by yourself, and do not quietly ignore it either.

GitHub CI also runs on a release tag (and only then, plus on demand),
so the clean-checkout build is covered without you doing anything.

## 2. List what changed

Standing project instruction: compact list of every file added or
edited, in your final response to the user. `git status --short` in
the sandbox clone is the fastest way to get the exact list right
before you start delivering -- don't reconstruct it from memory.

## 3. Commit, tag and publish

- **Commit:** the files are already on the real checkout --
  `git add <the file list from step 2>` then commit with plain `git`
  (identity is already configured as `toy-os <noreply@toy-os.local>`,
  confirmed via `git config user.name`/`user.email`, so no `-c
  user.name=...` flags needed). Short subject line, then a per-file
  body:
  ```
  git commit -m "$(cat <<'EOF'
  <short summary as the subject line>

  <file path>   - <one-line note on what changed in it>
  <file path>   - <one-line note on what changed in it>
  docs/decisions.md - only when the change answers a "why this way"
  EOF
  )"
  ```
- **Tag, if cutting a release:** `git tag -a v<version> <commit> -m
  "..."` directly.
- **Publish:** `git push origin main` (`--tags` if a tag was cut) and
  `gh release create`/`gh release upload` both work directly from the
  session -- confirmed by actually doing both (ordinary pushes, and
  cutting the `v0.1.0` release end-to-end, including a same-day
  follow-up `gh release upload` to fix a missing asset). **Pushing
  ordinary commits needs no confirmation** -- the user's standing
  grant (2026-08-14) is push-to-main-without-asking for clear-cut,
  tested changes. Tags and `gh release` publishing still get
  confirmed first.
- **Release assets, if cutting one:** three of them --
  `toy-os.iso`, gzipped `disk.img.gz` (`make clean-disk && make iso`
  first so it's not carrying session-local test data, then `gzip -k -9
  disk.img`), and `tools/run_release.sh` -- `gh release create v<version> toy-os.iso disk.img.gz
  tools/run_release.sh --title ... --notes ...` attaches them directly.

## Direct local checkout, but as a background job (worktree-isolated)

Confirmed directly across four separate changes in one session
(2026-08-12, the tray feature, its bug fix, NX/W^X, and
`shell_flow.py`): a *background* Claude Code session runs against the
user's real local checkout, but the harness enforces its own isolation
on top, which changes the mechanics above:

- **File edits are rejected against the shared checkout** until the
  session calls `EnterWorktree` -- do this before the first `Write`/
  `Edit` of the change, not after hitting the rejection. It creates a
  fresh worktree on a new branch (`.claude/worktrees/<name>`,
  `worktree-<name>`) and switches the session into it; all normal file
  tools then work against that branch's copy.
- **Build/test/commit happens in the worktree, on its own branch** --
  same `preflight.sh`/`make test`/QMP-testing steps as any other change,
  just
  running from the worktree's path. Commit there with plain `git`
  (same identity, same per-file-body-line message shape as above).
- **Publishing is a fast-forward push of that branch onto `main`, not
  a plain `git push origin main`** -- the worktree's branch isn't
  `main`, so there's nothing to literally push `main` from. Instead:
  `git push -u origin worktree-<name>` (ships the work somewhere
  durable immediately, in case the worktree gets discarded before
  merging), then -- once the user confirms they want it merged --
  `git push origin worktree-<name>:main` (a real fast-forward, no
  merge commit, works as long as `origin/main` hasn't moved since the
  branch was cut; `git fetch origin main && git merge-base
  --is-ancestor origin/main HEAD` confirms that first). Clean up after:
  `git push origin --delete worktree-<name>`.
- **Merging to `main` needs no fresh ask anymore** (standing grant,
  2026-08-14): `git push origin HEAD:main` directly for a clear-cut,
  tested change -- a whole multi-stage feature shipped that way, one
  fast-forward per stage, and the user's only correction was in the
  MORE-pushing direction. Pushing the `worktree-<name>` branch first
  is still worth it for anything long-running (durability if the
  worktree is discarded), but delete it (`git push origin --delete
  worktree-<name>`) once its commits are on `main` -- a merged
  leftover branch shows up as a confusing "Compare & pull request"
  banner on GitHub. Tags/releases/force-pushes still get confirmed.
- **Leaving the worktree:** `ExitWorktree` with `action: "remove"` once
  the branch is safely on `origin/main` (pass `discard_changes: true`
  if it complains about the branch having commits -- that's expected,
  they're already merged upstream, not lost) returns the session to
  the original checkout directory. Follow with `git pull origin main`
  there so the shared checkout actually reflects what just shipped --
  merging on GitHub doesn't update a different local directory on its
  own.

## The recurring stop-hook warning

An automated stop-hook may warn about "unpushed commits" after nearly
every turn. It means what it says -- real unpushed work on the real
checkout -- so surface it rather than explaining it away.

## Ring-3 apps (added after the Milestone 41 work)

toy-os now runs real apps as ring-3 processes, and a change touching
them has its own checks:

- Which side is this? `apps/` is kernel-space (compiled into
  kernel.bin, draws via `gfx_*`, gets WM callbacks). `userland/` is a
  separate process (draws via `ugfx_*` into its own buffer, gets
  `win_event` messages, links `crt0.asm` + `sys.c`). Calculator,
  Notepad and Terminal exist on BOTH sides deliberately, so name which
  one you changed.
- **Never hand-roll an `int $0x80` stub** in a new userland program --
  `userland/sys.h` has a typed wrapper per syscall. `sys_call()` is the
  raw hatch and is only for the `/tests` diagnostics that poke the ABI
  on purpose.
- A file needed by both sides is COMPILED TWICE via
  `build/userland/shared/`, never copied -- different code models mean
  the objects can't be shared, but the source can. Only freestanding
  files qualify.
- Run the client tests for whatever you touched: `winclient_test.py`
  (protocol), `uiclient_test.py` (text), `calculator_client_test.py`
  (widgets + commit-on-release), `notepad_client_test.py` (text widget
  + file round trip), `uterm_test.py` (spawn + pipes). Plus
  `uidemo_test.py` and `damage_sweep.py` for anything kernel-side that
  draws.
- Adding a syscall means: the number and its contract in
  `abi/syscall_abi.h`, a handler, a typed wrapper in `sys.h`/`sys.c`,
  a `strace.c` table entry, and a KTEST. Missing the strace entry is
  the one people forget.
