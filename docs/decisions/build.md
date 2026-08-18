# Decisions: Build, versioning and project docs

The build, the version scheme, and how this project records what it decided.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## There is no changelog, milestones are named, and nothing carries a target version

Three separate-looking decisions with one cause, so they are answered
together. Anyone arriving at "where is the changelog?", "which milestone
number is this?" or "what version does this land in?" is meeting the
same rot from a different side.

**The cause: each of them made a FILE responsible for keeping a NUMBER
true, and nobody kept it true.**

- The **changelog** indexed changes by build number. Entries pointed at
  it from 272 places. It reached ~12,000 lines across four files, and
  the reasoning in it was already being written twice more -- in a
  comment beside the code and in this file. Closed 2026-08-15, deleted
  2026-08-18.
- **Milestone numbers** encoded position, so inserting a milestone meant
  renumbering. That happened three times; the third moved eight at once
  and left a translation table from the original numbering that nobody
  read. Milestones are named now, and `docs/roadmap.md`'s LAYERS carry
  the ordering that numbers used to pretend to.
- **Target versions** on roadmap items predicted which release the work
  would land in. Before that the convention was one milestone per minor
  release. Both said where work would go and were then wrong. The
  versioning scheme is unchanged -- semver with a `-dev` suffix -- but
  the maintainer picks the number and the contents at release time.

**The rule that replaced all three: point at things addressed by NAME,
and at commits for anything historical.** A name survives edits, moves
and reordering; a commit SHA cannot change at all. What rots is the
third category -- an index maintained by hand.

  - **Present state** -> a file or symbol, a section in this file, a
    named milestone in the roadmap, a rule in `CLAUDE.md` or the skill.
  - **Past change** -> the commit, paired with what it did ("the
    poison-page fix, 978ebf7"), because a bare hash tells a reader
    nothing.

**What this cost, stated plainly rather than glossed:** commit messages
before the freeze are one-liners ("Build 263 (feature, +10): keyboard
Page Up/Page Down scrolling"), so for pre-2026-08-15 work the changelog
was the only detailed account of a change. Before deleting, all 94
entries in this file that cited it were measured: 89 carried their own
reasoning already, and the 5 that did not were read by hand -- each
stated its decision, and three described code that no longer exists. The
decisions survived; the blow-by-blow of old changes did not.

**And the enforcement, because a rule nobody checks is the thing that
rotted in the first place:** `tools/check_docs.py` fails the build on a
pointer to the deleted changelog, a numbered or versioned milestone
heading, a duplicated roadmap entry, or a link to a doc that does not
exist. It is deliberately narrow -- the duplicated-entry check exists
because two of this repo's own roadmap edits duplicated an entry and a
third silently deleted three, and nothing tests documentation. It does
NOT flag `Milestone N` in prose: those are historical, the roadmap's
details file ends with a legend for resolving them, and the noise would
be what stopped anyone running it.

## The demo ISO is a separate image, and its tour is a file on it

Asked for a way to show the system on a laptop with nobody typing.
Three shapes were possible: a boot flag on the normal ISO, a recorded
input trace, or a scripted tour. The tour won on the same grounds the
`gui` debug commands did -- it drives the real system through the real
paths, so it cannot drift out of sync with the software it is
demonstrating, and when it breaks it breaks visibly.

**It is its own ISO (`make demo-iso`) rather than a runtime toggle**, for
one reason: a demo that can start itself by accident is a demo that
starts during something else. The `demo` keyword is on the kernel
command line in a grub.cfg that only that image carries.

**The script is a FILE on the image** (`/usr/wm/demo.script`, source
`data/wm/demo.script`), not compiled in, so the tour can be edited on a
live USB stick with no toolchain present. Its CLI half runs at the
console; its GUI half is performed **one step per WM iteration** from
inside `wm_run()`'s loop, through the same `wm_debug_dispatch()` path
the 175 GUI checks already use. That is not an implementation detail --
driving a desktop from outside its own event loop is precisely what
makes a scripted demo hang, and the debug console already had the
answer.

**The live ISO stays a separate artifact from the normal one**, which
cost a red CI to learn: a 129 MiB GRUB module took the boot smoke test
from 1.6s to 7.0s locally and blew CI's timeout outright, because GRUB
reads the whole module off the emulated CD-ROM before the kernel starts.
Partial block groups have since brought the image to 24 MiB, so folding
it into the default ISO is now arguable -- measure the boot before doing
it.

## Build-number scheme: fix/feature/major tiers, not dates or semver

**Superseded -- see the next entry below.** This scheme (`tools/
bump_build.sh <fix|feature|major>`, retired) replaced an earlier
date-plus-same-day-counter scheme (`YYYY.MM.DD.N`), which itself
replaced a hand-bumped `0.1.0`-style semver. It was a deliberately
coarse, Windows-build-number-style approximation (+1/+10/+50) chosen
for being consistent and easy to sanity-check later, over a freeform
number that would be more nuanced but less predictable. Kept here for
the historical reasoning -- every existing `Build N` changelog
heading and `build-N` git tag still refers to this scheme. See
the commit for build 110 (the switch itself) and
**Build 121** (the git tag + GitHub Release convention added on top of
it) -- both predate the changelog's split into eras, so they're in the
oldest history now.

## Versioning: semver + `-dev` suffix, not a per-change build number

Replaced the fix/feature/major build-number scheme above. Requested
directly: bumping (and picking a fix/feature/major tier for)
`BUILD_NUMBER` on every single change, however small, had become
ceremony that didn't earn its keep -- a build number that changes
constantly isn't meaningfully more informative than one that doesn't
change until something's actually ready to call a version. `VERSION`
(repo root) now holds a plain semver-ish string, `0.1.0-dev` to start,
read into `TOYOS_VERSION` by `tools/gen_version.sh` exactly the way
`BUILD_NUMBER` was before. It only changes via `tools/set_version.sh
<version>`, and only for one of two reasons: starting a new dev round
(`0.2.0-dev`) or cutting a real release (`0.2.0`, no `-dev` suffix).
the git history follows [Keep a Changelog](https://keepachangelog.com/)
from its `## [Unreleased]` section forward -- every change gets an
entry there, no version/tier attached, until a release is cut; cutting
one renames that heading to `## [<version>] - <date>` and opens a
fresh `## [Unreleased]` above it (`tools/set_version.sh` does both
steps together). See the commit that added it (added the
same day this switch happened) for the change itself.

Day-to-day mechanics: `tools/set_version.sh 0.2.0-dev` starts a new dev
round (rewrites `VERSION` only); `tools/set_version.sh 0.2.0` (no
`-dev`) cuts a release (rewrites `VERSION` AND stamps the git history as
above). Git tags moved from `build-N` per push to `v<version>` at real
releases only, cut by hand after `set_version.sh`:
```
tools/set_version.sh 0.2.0   # rewrites VERSION, stamps the git history
git tag v0.2.0
git push origin main --tags
```
A GitHub Release (title = `v<version>`, body = that release's
the git history, assets attached) is a judgment call per release now
rather than tied to a fixed tier, since there's no tier anymore -- use
one when a release feels milestone-worthy enough that grabbing a
working build without cloning + building is worth it.

**v0.0.9 (first real release, cut ahead of any milestone) established
the actual asset/publish mechanics, corrected below:**

- **Three assets, not just the ISO** -- `toy-os.iso`, `disk.img.gz`,
  and `tools/run_release.sh`. `disk.img` alone isn't optional: it's
  where `/bin/ls`/`/bin/lspci`/the seeded test binaries actually live
  (there's no installer, so the ISO alone boots into a near-empty
  filesystem). `disk.img` is a large SPARSE file (~9GB apparent size,
  ~370KB of real data as of v0.0.9) -- gzip it before attaching
  (`gzip -k -9 disk.img`; shrank to ~9MB) or the raw upload both blows
  past GitHub's 2GB-per-asset limit and wastes bandwidth transferring
  mostly zeros. **Run `tools/tfs2_writer.py trim disk.img` first**:
  sparseness is only ever lost, so an image that has been used at all
  is carrying stale blocks it will happily compress. The dev image had
  reached 8.1 GiB of real data before TRIM existed -- see this file's
  thin-provisioning entry. `tools/run_release.sh` ships as a release asset (not
  just a repo file) because someone with just the ISO/disk image, no
  checkout, otherwise has no easy way to know the correct QEMU device
  config (`if=ide` disk bus separate from `-cdrom`'s, no
  `-device usb-mouse`/`usb-tablet` -- see the Makefile's `run:` target
  comments and `CLAUDE.md`) -- it's a standalone `sh` script that
  gunzips `disk.img.gz` itself if needed, then launches with the same
  flags `make run` uses.
- **Rebuild `disk.img` fresh (`make clean-disk` first) before
  packaging a release** -- otherwise whatever's on the working
  checkout's disk image (test files, session-local state) ships as
  part of the "clean" release.
- **The publish step (`git push --tags` and `gh release create`)
  cannot run from the Cowork cloud sandbox, even with the repo's own
  token in the remote URL.** Confirmed directly (v0.0.9): the push
  failed with `remote: access denied by the git proxy: ... is not in
  this session's authorized repository set` -- the sandbox's outbound
  git egress goes through an allow-list proxy that blocks this
  regardless of credentials embedded in the URL. Read-only git
  (`fetch`, `ls-remote`) works fine through the same proxy -- only
  writes are blocked. The device bridge to the user's real machine has
  no network access at all (by design, see `CLAUDE.md`), so it can't
  publish either. Net effect: the "never push from the session" rule
  in this skill/`CLAUDE.md` isn't just a caution, it's enforced -- tag
  and prep everything locally (both checkouts), then hand the user the
  exact `git push origin main --tags` + `gh release create` commands
  to run from their own machine's terminal, which has real network
  access. `gh` isn't preinstalled in the cloud sandbox either
  (`apt-get install -y gh` if you need it there for anything read-only
  going forward, e.g. checking release state via `gh api`).
- **Version-vs-milestone note:** v0.0.9 was cut *ahead of* Milestone 1
  on purpose (a pre-milestone testing snapshot the user explicitly
  asked for), not part of the `v0.1.0` = Milestone-1-done mapping
  `docs/roadmap.md` otherwise uses. `tools/set_version.sh` doesn't
  care either way -- it'll stamp whatever version string you give it.

See `gh release create v0.2.0 toy-os.iso disk.img.gz run_release.sh
--title "v0.2.0" --notes-file <path>` (or the GitHub web UI) for the
actual invocation shape now.

Alongside this, commit messages going forward list each changed/added
file with a one-line note in the body -- a separate, smaller
convention adopted at the same time, not tied to the versioning switch
itself:
```
kernel/drivers/keyboard.c   - added SE layout remap
apps/shell.c                - fixed signed-char gate in shell_read_line()
docs/decisions.md - only if it answers a "why this way"
```
Subject line stays a short summary as always; this is just the body,
so a commit is skimmable on GitHub without opening the full diff.
See CLAUDE.md's own bullet on this.

## A dev build shows its commit; a release shows only its version

`VERSION` changes about twice a milestone, so for the hundreds of builds
in between, "0.3.0-dev" identified nothing: two ISOs weeks apart carried
the same string. `tools/gen_version.sh` now also embeds
`TOYOS_BUILD_ID` -- `git rev-parse --short HEAD`, plus `-dirty` when the
working tree did not match it -- and `TOYOS_VERSION_FULL`, which is what
the `about` command, the Control Panel and the About window display.

The display rule, and why each half:

- **`0.3.0-dev` shows the commit** (`0.3.0-dev (2034bb1)`). A dev build
  is pinned by nothing else, which is the entire problem being solved.
- **A release shows the bare number** (`0.3.0`), dirty tree or not. It
  is already pinned by its git tag, so the hash is noise on the one
  build where it is not needed.
- **`dirty` appears on a DEV build only.** A build from a tree with
  uncommitted changes came from source that exists nowhere in history,
  so the hash it prints is a lie without the marker -- but that matters
  to whoever is BUILDING, not to whoever is running. "Dirty" is jargon
  about a repository the user does not have.

  The dirty-RELEASE case is therefore a **build-time warning** rather
  than a display string: `gen_version.sh` shouts on stderr when
  `VERSION` has no `-dev` and the tree is dirty, where the person who
  can still act on it will see it, and the shipped string stays clean.
  The first version of this printed `0.3.0 (dirty)` to the user; that
  was simply the wrong audience for the message.

**The trap this had to avoid**, and it is the reason a build TIMESTAMP
is not in there: `gen_version.sh` only rewrites `version.h` when the
content actually changed, because `kapi.h` includes it and an
unconditional rewrite makes nearly every object in the tree look stale
on every build. A commit id changes once per commit and the dirty
marker at most twice per session, so the property holds. A timestamp
would differ every single build and quietly turn every build into a
full rebuild.

`unknown` rather than an empty string when git is unavailable (a release
tarball, a stripped checkout): an empty marker reads as a bug in the
script, and a build that cannot say where it came from should say so.

## Repo is MIT; the baked JetBrains Mono glyph data is separately SIL OFL 1.1

`kernel/drivers/font_ttf.c`/`.h` (generated by `tools/genttf.py`) bake
anti-aliased glyph *bitmaps* rendered from JetBrains Mono directly into
committed C source that ships in the kernel binary -- not just a build-
time dependency, actual redistributed derivative data. JetBrains Mono
itself is licensed under the SIL Open Font License 1.1, not MIT, and
OFL requires its license text travel with the font (or "substantial
rendered derivatives" of it, which baked bitmaps arguably are) --
regardless of what license covers the rest of the software using it.
Relicensing the whole repo to OFL wasn't needed or appropriate (OFL
doesn't require that, and it's a font license, not a general software
one); instead `LICENSE` got a short "Third-party font" section pointing
at `tools/OFL.txt` (already present, already referenced from
`README.md`), so a reader relying on `LICENSE` alone doesn't wrongly
conclude the baked font data is MIT. `tools/gen_kbs.py`'s use of the
system's XKB data is a *different* situation and needed no such
carve-out: it shells out to the locally installed `xkbcli` at build/
seed time and the XKB layout data itself is never embedded or
committed (`seed/sync/` is gitignored, pure regenerable build output)
-- more like depending on `gcc` than bundling third-party data.

## CI is kept for the environment, not the checks -- they duplicate `make verify` exactly

Asked directly, and worth answering once: on a solo hobby project where
`make verify` runs before every push, is a GitHub Actions workflow
earning anything?

Every step `.github/workflows/build.yml` runs -- clean build, iso,
`check_layout.py`, `boot_smoke_test.py`, `ktest_run.py` -- is a step
`make verify` already runs locally. If the value were "run the checks",
it would be pure duplication and should go.

**The value is the machine, not the checks.** CI builds from a fresh
clone on a host that isn't the maintainer's, and that difference catches
a class of bug local verification structurally cannot:

- `tools/gen_kbs.py` needs `xkbcli`, and the `seed` target skips it with
  a message when absent. CI didn't install it, so CI had been building
  images with **no keyboard layouts at all**, silently, for as long as
  that step existed. No amount of care running `make verify` on a
  machine that has `xkbcli` can find that.
- A file that exists locally but was never committed builds fine
  forever locally. `seed/sync/pci.ids` was exactly that (see the
  filesystem-layout entry). Fresh-clone builds catch that class by
  construction -- though not universally: in that specific case nothing
  in the build referenced the file, so CI would have gone green with a
  degraded image anyway. A partial net, not a complete one.
- Ubuntu vs the maintainer's Arch-family host is what the Makefile's
  `grub2-mkrescue` fallback exists for.

**What CI explicitly does NOT cover:** it has no display and no QMP, so
every GUI regression is invisible to it -- a mis-clipped label, a click
handler testing the wrong coordinate space. Those need local screenshot
testing regardless, and a green CI badge must not be read as "the
desktop is fine".

## Repo history scrubbed of the maintainer's real name -- privacy request, not a bug fix

The maintainer's real name and two personal email addresses appeared
in two places: the `LICENSE` copyright line, and as the commit
author/email on every one of 91 commits in git history (both a Gmail
address and a personal domain address). Requested directly, for
privacy reasons -- not something this project would otherwise flag on
its own. Fixed with `git-filter-repo` (not `filter-branch` -- the
maintained, recommended tool), run in an isolated copy of the repo,
never touching the working sandbox clone or the user's real checkout
directly:
- `--mailmap` remapped both old `name <email>` identities to a single
  generic `toy-os <noreply@toy-os.local>` identity, rewriting every
  commit's author AND committer fields.
- `--replace-text` rewrote the literal string in blob content too (the
  `LICENSE` copyright line existed unchanged across its whole history,
  so scrubbing only the latest revision would have left it in every
  earlier commit's tree).
- Verified clean afterward by grepping the *entire* rewritten
  history's authors and full patch text (`git log --all -p`) for the
  name and both emails -- zero hits is the actual proof, not just
  "the current file looks right."

**Delivery mechanic, because this session can't push (see the git-proxy
entry above):** `git bundle create --all` packaged the rewritten
history into one file, delivered via `SendUserFile` +
`device_commit_files` same as any other file. The user applied it
themselves: fresh `git clone` from the bundle, fix the `origin` remote
(cloning from a local bundle auto-sets `origin` to the bundle's own
path, so `git remote add origin <url>` collides -- use `git remote
set-url origin <url>` instead, or remove-then-add), then `git push
--force` both the branch and the tag from their own machine. Verified
afterward from the session by fetching from GitHub (read-only git
still works through the proxy) and re-running the same "grep the whole
history" check against `origin/main` and the tag -- don't trust a
local check alone since the point is what's actually live on GitHub.

**Pitfall hit during verification, worth knowing about:** `git log
--all` inside the *session's own sandbox clone* pulled in the sandbox's
own stale local branch/tag refs (never rewritten, and a leftover local
tag that fetch doesn't force-overwrite by default) alongside the
freshly-fetched `origin/main` -- produced a false-positive "still
finding the name" result on the first check. Check specific refs
(`origin/main`, the actual tag ref) explicitly rather than `--all` when
verifying a remote's real state from a local clone that has its own
unrelated history sitting around.

**One loose end, not urgent:** the rewrite ran against the *session's
sandbox mirror's* commit graph (the copy this session had, not the
real checkout's parallel commit with the same content) -- harmless
content-wise (both had identical diffs), but it means one commit now
public on GitHub is authored `Claude (sandbox mirror) <claude@sandbox>`
rather than the generic `toy-os` identity everything else got. Not
PII, just slightly inconsistent -- worth folding into the identity
`toy-os <noreply@toy-os.local>` if this repo's history ever gets
rewritten again for another reason, not worth a whole rewrite on its
own just for this.

**Standing convention going forward:** every commit in this repo,
whether made by a session or by the maintainer directly, should use
the `toy-os <noreply@toy-os.local>` identity -- never a real name or
personal email. See `CLAUDE.md`'s "Working in the cloud sandbox"
section for the mechanical detail (the device-bridge session has no
git identity configured at all by default, so this has to be passed
explicitly on every commit, not assumed).

## The repo lives in an organization because a personal repo has no read-only collaborator

Wanting to show the code to one person without giving them write access
turns out to be impossible on a personal-account repository. **Every
collaborator on a personal repo gets write.** The Read/Triage/Write/
Maintain/Admin roles exist only for repos owned by an ORGANIZATION, and
paying for GitHub Pro does not change it -- it is a property of personal
repos, not of the plan.

The usual workaround is closed off too: branch protection and rulesets
both return `Upgrade to GitHub Pro or make this repository public` on a
free private repo. Verified by calling the API, before and after the
move -- **a free ORG does not unlock it either**, which contradicts a
guess made mid-session and is why it is written down here.

So on 2026-08-16 the repo moved from a personal account to the
`eveningworks` organization, where an **outside collaborator** can be
added to one repository with the `Read` role. Outside collaborator
rather than org member on purpose: an org member can see the
organization's other repositories and its member list, which defeats the
point once the org holds more than this project.

Two consequences worth knowing:

- **The remote changed** to `git@github.com:eveningworks/toy-os.git`.
  GitHub redirects the old path, but the local remote was repointed
  rather than left depending on a redirect that dies the moment someone
  claims the old name.
- **Release assets survive a transfer**, as do tags, Actions history and
  pending invitations. Only repository *settings* that are plan-gated
  change meaning.

## The account rename, and the second history scrub -- scoped by measuring, not by instinct

The maintainer's GitHub handle changed after the transfer. Two facts
made that safe, and both were checked rather than assumed: the numeric
account ID is unchanged, so GitHub re-resolves every release author and
commit attribution to the new name; and no commit is *authored* by the
GitHub account at all (authors are all `toy-os <noreply@toy-os.local>`,
per the earlier scrub -- see the entry above).

What did carry the old handle was **committer** metadata on nine
web-UI merge commits (`<id>+<handle>@users.noreply.github.com`), one
line in a frozen changelog archive, and one commit message.

**The scope of the fix was decided by measuring, and the first estimate
was wrong by 4x.** Rewriting only the committer fields touches 60
commits and one tag, because the earliest affected commit postdates
`v0.1.0`. Scrubbing the string from historical FILE CONTENTS as well
reaches back to a commit that predates `v0.0.9` -- 264 commits and all
three tags. That difference was found by asking `git log -S` where the
string was introduced, *after* the smaller number had already been
quoted and a decision made on it. The decision was re-put with the real
number, and the narrower scope chosen.

So: committer metadata and the current tree are clean; old revisions of
the git history still contain the handle if someone checks out a
months-old commit. That was judged acceptable because a GitHub handle is
public by nature -- unlike the real name the first scrub removed, where
the wider blast radius was worth paying.

The mechanics, if this comes up again: `git filter-branch` in a FRESH
CLONE (never the working checkout, and never a repo with worktrees
attached), `--tag-name-filter cat` so tags follow, then verify with
`git diff <old-tip> <new-tip> --stat` -- which must show only the
intended content change and nothing else. `git filter-repo` is the
modern tool but is not installed here; filter-branch is built in and was
sufficient. Take a backup first (`tools/backup_repo.sh`), because a
force-push over a rewritten history is the one operation where the old
objects stop being reachable.

## A backup of this repo is not a `git clone`

`tools/backup_repo.sh` exists because the obvious backup is incomplete
in a way that only shows up when you need it. A mirror clone captures
every commit, branch and tag -- and none of the **release assets**,
which are ~130 MB of ISOs and pre-seeded disk images that live only on
GitHub. Rebuilding a historical release's assets by hand means checking
out that tag and reproducing the exact build, which is precisely the
situation a backup is supposed to avoid.

It also captures what a clone cannot: the repo's own settings, the pull
request bodies, and a bundle of LOCAL refs, since a mirror of the remote
cannot know about a branch that was never pushed.

Two things it deliberately does not do. It does not capture
collaborators, webhooks, secrets or Actions history -- those are
GitHub-side state with no export, and pretending otherwise would be
worse than saying so. And it does not run the restore test, because that
costs a full build: clone the mirror and run `make all` by hand before
relying on a backup for anything irreversible. That test is worth the
minutes -- matching hashes prove the bytes survived, but only a build
proves it restores to a working project.

## Socket fds: scaffolding ahead of the driver, not a working transport

`SYS_SOCKET`/`SYS_SEND`/`SYS_RECV` (build 420) exist and are reachable,
but `SYS_SEND`/`SYS_RECV` always return -1 -- there's no NIC driver or
protocol stack for them to move bytes through yet (see README.md's
"Basic TCP/IP networking": PCI enumeration, IRQ registration, and
contiguous memory are done; the driver itself isn't). Building even a
minimal in-kernel loopback transport (two processes exchanging bytes
through a shared buffer, no real network) was considered and
deliberately not taken -- it would prove the syscalls can move data,
but not the actual thing this scaffolding needs to prove: that the fd
namespace, the syscall ABI, and the tagged fd table (`FD_KIND_FILE`/
`FD_KIND_SOCKET` in `syscall.c`) are right, ahead of a real driver
existing. `sockettest` verifies exactly that surface -- fd allocation,
kind separation (`SYS_WRITE`/`SYS_READ` correctly reject a socket fd),
and cleanup -- without pretending a transport exists. See
`syscall_abi.h`'s `SYS_SOCKET` doc comment and the commit for build 420 for the full writeup.

## Header dependency tracking is a `find`, and a test proves it works

The Makefile's `-include` for the `-MMD -MP` dependency files used to
name six directories by hand. When source discovery went recursive,
kernel objects moved from `build/core/` to `build/kernel/core/`, the
glob stopped matching, and **88 of 161 `.d` files silently stopped
being read** -- `touch kernel/include/kernel/process.h && make all`
rebuilt nothing at all. It is a `$(shell find $(BUILD) -name '*.d')`
now, which cannot drift when a directory moves.

Two things are worth remembering beyond the fix. First, the failure was
invisible in every way this project normally looks: nothing warns, a
clean build is unaffected, and the symptom appears later as a stale
`.o` compiled against an old struct layout -- which is not a compile
error but an array indexed with the wrong stride at runtime. This repo
has paid for that twice already (the Start menu drawing function
prologues as text; a filesystem honesty check reading garbage and
refusing a good backend). Second, that invisibility is why the fix
comes with `tools/check_deps.py` rather than standing alone: it touches
one header per build directory, asks `make -n` what it would rebuild,
and fails if the answer is "nothing". It runs in `preflight.sh` and CI,
and was validated by putting the old glob back and watching it report
exactly the ten directories that had been uncovered. A one-line fix
with no guard would have left the next directory move free to do this
again. See the git history.

## Userland programs link one archive, and name nothing

Each ring-3 GUI client used to need a hand-written `FOO_OBJS = ...`
block plus its own link rule in the Makefile -- four of them, ~50 lines,
identical apart from the object list. That became one
`EXTRA_OBJS_<name>` line per binary, and then, once the toolkit was
about to be split one-file-per-widget, an archive: `libuapp.a` holds
`userland/ui/`, `userland/lib/` and the sources shared with the kernel,
every program links it, and **a program names nothing at all** -- the
linker pulls only the members it references. A new GUI app is a `.c`
file in `userland/gui/` and no Makefile edit.

The archive was deliberately deferred at design time and then brought
forward, which is the rule working rather than a reversal: the bar is a
second real caller, and splitting `uwidgets.c` into ten per-widget
objects was what created one. Without an archive that split would have
turned each app's object list from three entries into eight.

Three mechanics that are load-bearing together, and useless apart:

- `-ffunction-sections -fdata-sections` plus `--gc-sections`. Archive
  granularity alone still links the whole of a member: one checkbox
  would pull in the listbox, dropdown and text field sharing its `.c`.
- **`userland/rt/link.ld` must match `.text.*`, not just `.text`.** With
  function-sections on, a script matching only `*(.text)` places none of
  the code. The link SUCCEEDS and the ELF is nearly empty; the symptom
  is a fault at the entry point, not a linker error.
- **The archive goes last on the link line.** A linker resolves archive
  members against the undefined symbols accumulated so far, so an
  archive ahead of its callers contributes nothing.

Measured on the 29 userland binaries: 171 KB smaller in total, Terminal
-32%, Calculator -27%, `uiclient` -41%, and `hello` gained nothing (it
references no toolkit symbol, so it pulls no member). See
the git history.

**A second trap, found when the first source file was deleted:** `ar
rcs` UPDATES an archive rather than rebuilding it, so a member whose
source has gone stays inside indefinitely. Splitting `uwidgets.c` into
per-widget files left `uwidgets.o` in `libuapp.a` for three commits, and
nothing failed -- a linker pulls the first member that satisfies a
symbol and only errors when two ALREADY-pulled members collide, so the
link kept working while being free to use the deleted file's code. It
surfaced only when `ui_checkbox` became an object and the two versions
of `uui_checkbox_draw` finally differed. The rule `rm -f $@` first, so
the archive is a function of its declared objects rather than of every
object that has ever existed. Note `make clean` hides this class of bug
rather than revealing it: a full `preflight.sh` would have built a
correct archive and said nothing, which is why `--skip-clean` runs are
worth keeping in the loop.

**The trap this exposed, which is general:** the Makefile tracks HEADER
dependencies (`-MMD`/`-MP`), not compiler FLAGS. Adding
`-ffunction-sections` to `USERLAND_CFLAGS` invalidated nothing, so the
first build linked stale objects compiled without it -- and the result
looked like `--gc-sections` half-working (binaries shrank, because
unreferenced archive members were still skipped, but unused functions
inside a pulled member survived). `make clean` after a CFLAGS change,
and be suspicious of a flag that appears to work partially.

## The GUI stack has names: TWP, TWS and Toykit

Three things had no names, which made every sentence about them a
description: "the windowing protocol", "the window manager acting as a
server", "the ring-3 widgets". They are now:

| Name | What | Where | Analogy |
|---|---|---|---|
| **TWP** -- Toy Window Protocol | the client<->server message contract | `kernel/include/abi/win_proto.h` | Wayland, the X11 protocol |
| **TWS** -- Toy Window Server | the compositor implementing it | `kernel/proc/win_server.c` + `userland/wm/wm_client.c` | Mutter, Weston, Xorg |
| **Toykit** | the client toolkit an app programs against | `userland/ui/` | GTK, Qt, Win32 |

**Why three and not one.** The protocol is deliberately meant to outlive
this particular server -- M41 moves TWS to ring 3, and the whole bet in
`win_proto.h` is that this is a transport swap rather than a rewrite.
Naming the protocol separately is what makes that sentence sayable, and
what makes "TWP v2" a thing you could version. One name for all three
would blur exactly the line the design leans on.

**TWP and TWS follow TFS2/TFS3's style** -- short, plain, project-initial
-- because they are the same kind of thing: a format or service with a
contract worth versioning. **Toykit deliberately breaks that pattern**,
because the plain version would be "TUI", which universally means *text*
user interface and would mislead every reader arriving without context.
It is also the name said out loud most often, which is worth a real word.

**The symbol prefixes do NOT change.** Toykit's are `uui_`, `ugfx_`,
`uapp_`; TWP's are `WIN_REQ_*`/`WIN_EV_*`. A toolkit's name and its
prefix need not match -- GNOME's toolkit is GTK -- and renaming several
hundred symbols to spell a name out would be churn with no reader
benefit. The names are for docs, comments and conversation, which is
where the ambiguity actually was.

## Ring-3 GUI apps are callbacks and a layout, not a loop

Every TWP client used to hand-write the same three things: the
create/title/present/destroy handshake, a `for(;;)` around a
`switch (ev.type)`, and a `draw(); present();` pair at every state
change -- about 55 of `winclient.c`'s 141 lines before it did anything
of its own. Then it computed every rectangle by hand as well.

Toykit's `uapp` (`userland/ui/uapp.h`) owns the loop; `uui_layout`
(`userland/ui/uui_layout.h`) owns the geometry. An app is a
`struct uapp_desc` and some callbacks.

**The property that justifies it, and the one to preserve:** every
callback is optional and the library has a defined default for every
event, so TWS can gain a feature without any app being edited. That was
tested rather than asserted -- shipping resize (stage 3) touched ZERO
lines in the clients that did not opt in, and they all kept passing. An
unknown event type is ignored on purpose.

Four pieces of API were written and deleted before landing, each for
having no caller: `gfx_text_index_at_x()`, `uapp_text()`, the
`uapp_open()`/`uapp_pump()` escape hatch, and `WIN_REQ_MOVE`. The hatch
is the instructive one -- the design predicted for three stages that
Terminal would need it because Terminal BLOCKS inside a command, and
porting it showed the requirement was to PAINT at a chosen moment
(`uapp_flush()`, one line), not to own the loop. "This app blocks" and
"this app needs the loop" are not the same requirement.

Full design and staging: `docs/uapp-design.md`.

## Parallel test VMs lease a slot, they don't derive one from their position

`tools/gui_regress.py` runs the GUI test tools concurrently, each
against its own VM. Everything that could collide -- pidfile, serial
socket, QMP port, VNC display -- is derived from one slot number
(`vm.py --instance N`), and slot 0 keeps the original unsuffixed names
so every existing caller is unaffected.

The subtle part is how a tool GETS its slot. Deriving it from the
tool's index in the list (`idx % jobs`) looks equivalent to leasing one
and isn't: with `-j4` and seven tools, task 4 also maps to slot 0, but
it starts as soon as any worker frees up, which is routinely while task
0 is still running there. Written that way first, and the symptom was
actively misleading -- the fifth tool's slot-0 `vm.py stop` killed the
FIRST tool's VM, so the first tool died on a broken pipe and the fifth
died on a screenshot that was never written, with neither traceback
pointing anywhere near the scheduling. Slots come from a
`queue.Queue` lease held for exactly as long as the VM exists.

Ports are derived, not probed for. A free-port probe has a bind/close
race and gives a different port every run, which makes re-driving a
failed tool by hand harder than it needs to be; `-j4` always means
slots 0-3.

