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
- **Publishing happens from this checkout**, which has real network
  access: `git push origin main --tags` then `gh release create`. (It
  used to have to be handed to the maintainer to run by hand, because
  the retired Cowork sandbox's outbound git egress went through an
  allow-list proxy that blocked writes regardless of credentials.)
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

**The body's VOICE changed in 2026-08-24, at the maintainer's request:
problem, then change, then files.** It had drifted into an essay --
capitalised lede sentences stating the rule the session had learned, the
story of how a bug presented, forty-plus lines of it. The reasoning
itself is worth keeping and this repo has three better homes for it
(`docs/decisions.md` for why-this-way, a comment beside the code for the
trap in it, `docs/conventions/` for the rule), so the commit was saying
a third time what two other files already said. The shape now:

```
settings: show timezone display names, fix list hover and type-ahead

One or two short paragraphs: what was wrong, and what actually caused
it. Present tense, no shouting, no forensics.

- One bullet per change, saying what it does and why.
- ...

Files:
  kernel/lib/tz.c            display column, parser, seed table
  userland/ui/uui_route.c    recursive motion delivery
```

Imperative subject under ~72 characters, prefixed with the area
(`settings:`, `wm:`, `kernel:`) as it already was. What did NOT change:
every changed file is still listed with a one-line note, because that is
what makes a commit skimmable without the diff.

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

**Delivery mechanic, because that session could not push (see the
publishing entry above):** `git bundle create --all` packaged the
rewritten history into one file and handed it over. The user applied it
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
personal email. It is configured in this checkout, so an ordinary
`git commit` already does the right thing; the thing to avoid is
overriding it.

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


## Why there is exactly one `run` target

`make run-kvm`, `run-virtio`, `run-virtio-kvm`, `run-vmware`,
`run-audio`, `run-nographic`, `run-menu`, `run-live` and `run-demo` are
gone (2026-08-19). There is `make run`, with every way to boot expressed
as a variable on it, and `make debug`.

This is the second half of a fix whose first half was already recorded
here. Twelve near-identical `qemu-system-x86_64` lines had grown by
MULTIPLICATION rather than addition -- adding `run-virtio` immediately
forced `run-virtio-kvm`, and `run-virtio-audio` and `run-vmware-kvm` did
not exist only because nobody had asked. Collapsing them into one recipe
with `$(if ...)` axes fixed the duplicated COMMAND LINE and left the
NAMES behind as thin aliases, on the grounds of muscle memory.

That was the same problem one level up. A name per combination
multiplies exactly as fast as a recipe per combination: the aliases
covered five of the useful combinations and none of the rest, so
`make run KVM=1 DISK=virtio` -- the fast configuration, and the one worth
typing most -- had no name and looked second-class beside targets that
did. The aliases were also what the docs taught, so the flags stayed
invisible.

**`debug` survives because it is not one of the axes.** It freezes the
CPU for a debugger; that is a different thing to do with the same
command line, not a different way to configure the machine.

**`LIVE=1` and `DEMO=1` were the interesting ones to fold in.** They are
not pure command-line axes -- each selects a different ISO and has to
BUILD it first -- so they change the prerequisite list as well. That
works because a command-line variable is set before the Makefile is
parsed, so `$(if $(LIVE),live-iso,iso)` expands correctly in a
prerequisite. A target-specific variable would not have; this is
precisely why they were targets in the first place.

**`MENU=1` is the odd axis**, since GRUB's timeout is baked into
`grub.cfg` at BUILD time rather than passed to QEMU. It derives
`GRUB_TIMEOUT` (`?= $(if $(MENU),5,0)`), so the media are rebuilt with
the menu and `GRUB_TIMEOUT=` still overrides both. "Media" plural since
the kernel moved onto the disk: one repo-root `grub.cfg` is generated
into the ISO tree AND into `disk.img`'s FAT32 `/boot/grub`, in the same
`make iso`, so a timeout (or a `KCMDLINE=`) cannot reach one medium and
miss the other.

Verified with `make -n run <FLAGS>` across every axis, which is the
check worth repeating: it prints the command line without running it,
and it is what previously caught a `run-virtio-kvm` that shipped with no
`-enable-kvm` at all.

## The seed rename table is keyed by PATH, not by basename

`make seed` copies each built ELF to its on-disk name, with a small
override table for the three that are not just their own basename
(`gui/apps/terminal` -> `uterm`, `gui/demos/gfxdemo` -> `shapes`,
`tests/echo` -> `echo_test`). That table used to be keyed by BASENAME:
`SEED_NAME_echo = echo_test`.

It broke the day `/bin/echo` was written. The rename meant for the
syscall exercise in `userland/tests/` matched the real `echo(1)` in
`userland/bin/` as well, so the new program was seeded to
`/bin/echo_test` -- on top of the entry `tests/echo` was already
producing there in the same build.

**The failure mode is the reason this is written down.** Nothing went
red. The compile succeeded, the link succeeded, `make iso` succeeded,
`check_layout.py` passed (it checks directories, not names), and
`ls /bin` listed a plausible set of programs. The only symptom was
`echo` reporting `Unknown command` at a prompt where `cat` and
`uptime`, added in the same commit, both worked -- which reads as a
bug in the new program, not in the build.

Keying by the ELF's path under `build/userland/` (`tests/echo`) makes
each override name exactly the one thing it was meant to rename. The
general form, and the reason this is a decision rather than a fix: **a
rename table addressed less specifically than the thing it renames will
eventually rename something else**, and it will do it silently, because
a rename cannot fail.

## The C library's headers get their own root, and the shared sources have it taken away

`docs/libc-design.md`'s Stage 1. The C library's public headers live in
`userland/include/` and are reached with angle brackets; `userland/lib/`
keeps the toy-os-internal headers (`cmd.h`, `tosh.h`, `human.h`,
`dirsort.h`) and the implementation.

**Why a separate directory rather than putting `userland/lib/` on the
angle-bracket path**, which would have been one `-I` and no churn: the
libc's public surface would then be "whatever file happens to be in that
directory", so `<cmd.h>` and `<tosh.h>` would be part of the C library
as far as any including program could tell. That is the audience
question `kernel/include/api|abi|kernel` already answered here, and the
answer is a directory per audience with the build enforcing it.

**ORDER IS LOAD-BEARING, and it created the one real problem.**
`kernel/include/api/string.h` -- the `k_*` toolkit -- already owned the
name `string.h`. The C library's has to win it, because a program
written elsewhere asking for `<string.h>` means the C library's, so
`-Iuserland/include` goes ahead of `-Ikernel/include/api`. That leaves
`userland/include/string.h` unable to name the header it is a renaming
of: `<string.h>` and `"string.h"` both come back to itself, where the
include guard turns the reference into a silent no-op and every `k_*`
prototype disappears -- a confusing failure for a line that looks
obviously correct.

`kernel/include/api/kstring.h` exists for exactly that and contains
nothing else: a quoted `#include "string.h"`, which searches its own
directory first and therefore reaches the toolkit's header and cannot
reach the libc's. A header whose whole content is a name.

**AND THE SHARED SOURCES NEEDED THE FLAG REMOVED AGAIN.**
`kernel/lib/klineedit.c`, `kfmt.c` and `heap_core.c` are compiled into
both rings from one source, and all three include `"string.h"`. With
`-Iuserland/include` in front, that single line resolves to the C
library's header in the ring-3 pass and the toolkit's in the kernel
pass: the same source line meaning two different files depending on
which compilation it is. It would have compiled and linked either way,
which is what makes it worth writing down -- nothing would have
reported it.

So `SHARED_CFLAGS` is `USERLAND_CFLAGS` with `$(LIBC_INCLUDES)`
substituted out, and `LIBC_INCLUDES` is a separate variable only so that
subtraction can be written. The Makefile already had the pattern
(`APPS_CFLAGS` subtracts `-Ikernel/include/kernel` from `CFLAGS`), and
the effect is the same in both places: a rule that says what a class of
file is NOT allowed to reach.

The gain beyond correctness is that "everything on the shared list must
be freestanding and toolkit-only" stopped being a comment asking nicely
and became something the build enforces. Verified with `gcc -M` on
`kernel/lib/kfmt.c` both ways -- with the flag it pulls
`userland/include/string.h`, without it `kernel/include/api/string.h` --
rather than reasoned about.

## `printf` is built on a SINK, not on a scratch buffer -- and there is still one formatter

`docs/libc-design.md`'s Stage 2. Giving ring 3 a `printf` meant deciding
where the bytes go while a conversion is being produced, and the obvious
answer is the one the kernel already uses: `vga_printf()` and
`klog_printf()` format into a fixed `KFMT_LINE_MAX` buffer and then
write it.

That is right for a diagnostic line and wrong for a C library. It caps
everything a program can ever print at a number `kfmt_print.c` happened
to choose, and the cap is silent -- `k_vsnprintf` reports the length the
result WOULD have had, so a `printf` built this way could detect its own
truncation and still have nowhere to put the rest.

The alternative that a real libc uses is a formatter that emits as it
goes, which `kfmt.c` was already one small change away from: every
conversion funnels through a single `put()`. So `struct out` gained an
optional sink, `k_vsnprintf` and the new `k_vcbprintf` became two entry
points onto one `vformat()`, and the conversions do not know which kind
they are feeding. **There is still exactly one formatter in the tree**,
which was the constraint that mattered -- a private `vfprintf` beside
`kfmt.c` is precisely the duplication the shared-source rule exists to
prevent, and every real libc has one because none of them share a
formatter with a kernel.

Two things about the shape. The sink is called with **one byte at a
time**, deliberately: batching would need a scratch buffer here, which
is the thing being avoided, and the sink this exists for is a stream's
own buffer where a byte costs a bounds check and a store. And `printf`'s
sink is the ordinary `fputc()` path rather than a fast one, so
`printf("a"); putchar('b');` cannot come out in the wrong order.

It also puts Stage 4's `%f` in the right place: a float conversion
becomes a hook inside `vformat()`, compiled only into the ring-3 build,
rather than a second formatter that would have to be kept in step.

## `libc.a` is a second archive beside `libuapp.a`

Same audience question `userland/include/` answered for the headers, one
layer down. A `/bin` program links the C library; a GUI app links the C
library plus Toykit. One merged archive would have made the libc's
audience "toy-os apps" rather than "any C program", which is the
opposite of what the port-capable target is for.

`kernel/lib/string.c`, `knum.c`, `kfmt.c` and `heap_core.c` are in
`libc.a` rather than `libuapp.a`, because they ARE the C library's
implementation: `<string.h>`'s inlines call the first two, `snprintf`
and `printf` call the third, and `malloc` is the fourth.

**Link order is `libuapp.a` then `libc.a`, and it is not arbitrary.**
Toykit calls `strlen` and `snprintf`; the C library calls nothing in
Toykit. A linker resolves an archive against what is still undefined
when it reaches it, so the dependency has to come first. Reversing them
fails at link time with undefined `k_*` references from widgets -- loud,
which is the one good thing about getting this wrong.

The split is cheap now and awkward later: every program that comes to
depend on the merged shape makes it harder to separate, which is why it
was done while the libc was three files.

## The C library takes the plain header name; the kernel header gets a `k` alias

Stated once because it has now happened twice and will happen again.

`userland/include/` comes first on the ring-3 include path, because a
program written elsewhere asking for `<string.h>` or `<errno.h>` means
the C library's. Two kernel headers already owned those names:
`kernel/include/api/string.h` (the `k_*` toolkit) and
`kernel/include/abi/errno.h` (the shared error codes). Neither can be
reached from the libc header that displaced it -- both `<name.h>` and
`"name.h"` resolve back to the libc's own file, where the include guard
turns the reference into a silent no-op and every declaration
disappears.

**The rule: the C library keeps the plain name, and the kernel header
gains a k-prefixed forwarding alias** -- `api/kstring.h`, `abi/kerrno.h`
-- whose entire content is a quoted `#include` of its neighbour. Quoted
is load-bearing: a quoted search starts in the including file's own
directory, so the alias reaches the kernel header and cannot reach the
libc's.

The alternative considered and rejected was putting `-Ikernel/include`
on the ring-3 path so both could be spelled `<api/string.h>` and
`<abi/errno.h>`. That resolves the collision and reopens a boundary:
`<kernel/vmm.h>` would resolve too, and "ring-3 code cannot include
kernel internals" is currently enforced by that directory simply not
being on the path. Two forwarding headers are cheaper than an enforced
boundary.

Only those two collide today. `api/` and `abi/` between them also own
`fs.h`, `heap.h`, `timer.h`, `pipe.h` and `query.h`, none of which the
C library wants -- and `time.h` is NOT among them, which is worth
knowing before Stage 5 adds one.

## `struct dirent` became `struct sys_dirent`

POSIX's `<dirent.h>` declares a `struct dirent` with `d_name`, and two
structs cannot share a tag in one translation unit. The syscall ABI's
directory entry had that name; it is `struct sys_dirent` now, across
fifteen files.

The rename is not a concession -- it is the ABI's own convention
arriving late. Every other struct crossing that boundary is `sys_*`
(`struct sys_stat` being the one this most resembles), so `dirent` was
the outlier, and it only looked correct while nothing needed the C name.

The POSIX struct is deliberately NOT a copy of the ABI one with fields
renamed: it carries `d_type` and `d_name` and no `d_ino`, because
`SYS_LISTDIR` does not report an inode and a field that was always zero
would invite somebody to believe it. `SYS_STAT` reports one, and a
caller that needs it has the name to ask with.

## `%f` is a LINKED SPLIT, not an `#ifdef` and not a runtime hook

`kfmt.c` is compiled into the kernel and into ring 3 from one source,
and the kernel is built `-mno-sse`. That is not a stylistic constraint
on floating point -- `va_arg(ap, double)` on x86-64 reads the varargs FP
save area, so the ONE LINE that fetches the argument is SSE, in a kernel
that does not save SSE state on the interrupt path
(`kernel/include/kernel/fpu.h` has why that split is worth copying).

So the conversion sits behind `k_fmt_float()`, and each build links a
different implementation: `kernel/lib/kfmt_nofloat.c` returns 0,
`userland/libc/printf_float.c` formats. **This is not a new pattern in
this module** -- it is exactly the one-header-two-files split
`kfmt_print.c` already uses for the sinks, which is what makes it the
obvious answer once you see it.

Two alternatives were weighed. An `#ifdef` on a `-D` flag added to
`USERLAND_CFLAGS` works and makes the two compilations of one file
differ in behaviour, which is the thing the shared-source rule exists to
prevent -- the flag would be a second, invisible input to what
`kfmt.c` means. A registered function pointer works too and needs
something to call the registration before the first `printf`, which in a
libc with no constructors means either an initialiser check on every
call or a rule nobody can see.

**The kernel returning 0 is a designed failure, not a stub.** It falls
through to the formatter's emit-it-literally path, so `%f` in a kernel
format string appears in the output as `%f`: visible, consumes no
argument, cannot desynchronise the rest of the line. The two silent
alternatives -- printing a garbage number read out of an integer
register, or eating an argument that was never passed -- both look like
data. A KTEST asserts it, and it has to be a KTEST rather than a ring-3
test, because in ring 3 `%f` works.

The same split is where `%f`'s accuracy limit is confined: digits come
from repeated scaling rather than exact arithmetic over the mantissa, so
`printf_float.c` is the entire surface to replace if correctly-rounded
output ever matters. That is the payoff for putting the conversion
behind one function rather than spreading it through the formatter.

## Doom is a vendored port linked into one binary, and it found a printf bug

`userland/ports/doom/` is doomgeneric, byte for byte, beside
`userland/ports/cjson/` and for the same stated reason one size up: **a
real program nobody working on this repo wrote, built against this OS.**
cJSON (~3,000 lines) proved the C library; Doom (~36,000) exercises the
allocator, stdio over a real 4 MB file, the ELF loader, floating point,
the window protocol and both edges of the keyboard, all at once and for
minutes at a time.

**Linked into exactly one binary** (`EXTRA_OBJS_doom`), which is the
cjson precedent doing real work here: doomgeneric is GPL-2 and toy-os is
MIT. The two coexist as an aggregation -- a separate program in the same
repository -- and the per-binary link is what makes "nothing else can
accidentally depend on GPL code" a property of the build rather than a
promise. `userland/ports/doom/README.md` says so where somebody editing
it will look.

**Our backend lives OUTSIDE the vendored directory**, in
`userland/doom/`, so the boundary between third-party and written-here
is a directory boundary rather than a convention. It implements the five
`DG_*` functions and nothing else.

**The file list is WILDCARDED, unlike `EXTRA_OBJS_toywm`'s**, and the
difference is who owns it. The WM's units are ours, so a stray `.c`
there should fail to link rather than be absorbed silently; this list is
upstream's own `SRC_DOOM`, so curating it by hand would mean keeping a
second copy of somebody else's build in step.

**Warnings are off for the vendored tree and the frame-size warning is
NOT.** 36,000 lines of 1990s C does not pass `-Wall -Wextra`, and the
rule for the directory is that nobody may fix it -- so those warnings
are noise nobody is permitted to act on, and noise that would bury a
real one from our own code. The frame-size warning stays (raised to
16 KiB) because it is not style: a frame larger than
`UADDR_STACK_GROW_GAP` leaps the growable region and dies on a stack
that was willing to grow for it.

### Two `KEY_*` vocabularies that cannot share a translation unit

`api/keyboard.h` and doomgeneric's `doomkeys.h` both define `KEY_F2`,
`KEY_F3`, `KEY_F4` and `KEY_F10`, with different values, and neither is
ours to rename. So the app (`userland/gui/apps/doom.c`) includes one and
the backend (`userland/doom/dg_toyos.c`) includes the other, and they
meet through `dg_toyos.h`, which includes neither.

That header carries toy-os's codes written out as `TOYKEY_*` literals --
a COPY, which this project normally refuses. What makes it acceptable is
that `doom.c` `_Static_assert`s every one of them against the real macro:
the check compiles in the file that can see both, and a drift in either
direction is a build error rather than a key that quietly stops working.
Without it the failure mode is a wrong constant mapping to a key Doom
does nothing with, which looks exactly like an unbound control.

### Enter is 0x0A here and 0x0D in Doom, and the first test could not see it

The port shipped able to open its menu and unable to START A GAME.
`/etc/kbs` maps the Enter key to `\n` (0x0A), which is what a terminal
and a line editor want and is not going to change; `doomkeys.h` defines
`KEY_ENTER` as 0x0D. So the menu opened, the arrow keys moved the
highlight, and no item could ever be chosen. Nothing crashed, nothing
logged, and a screenshot of the menu looked perfect.

Translated in `to_doom_key()`, which is what that function is for: both
sides are right for their own world, and the seam is where they meet.
(Backspace is the same shape -- 0x08 here, 0x7f in Doom.)

**THE TEST WRITTEN FOR IT WAS USELESS, AND THAT IS THE LESSON.** The
first version pressed Enter and asserted the screen CHANGED -- and it
passed with the fix reverted, because Doom's attract demo keeps playing
behind the menu, so the screen changes whatever Enter does. A green run
proves nothing until the control has been seen to fail; this one was run
and did not fail.

The oracle that works is the INVERSE: once a new game starts, the player
is standing still, so the screen goes nearly static -- and a demo, by
definition, cannot. Measured both ways: **0.0009** of pixels changing
between frames with a game started, against **0.38-0.67** with the fix
reverted. Three orders of magnitude, where the obvious assertion had
none. It pairs with the existing "DOOM animates" check, since animating
before and still after is a combination only a working Enter produces.

### What the port actually needed, measured rather than assumed

Three things were built for this port in advance. Two of them turned out
not to be required, and saying so is worth more than the tidier story:

- **Key releases WERE required, absolutely.** `DG_GetKey(int *pressed,
  unsigned char *key)` asks for an EDGE; a press-only OS cannot answer
  its signature. And Doom's stock controls are fire on Ctrl, run on
  Shift, strafe on Alt -- three of five are modifier keys, which
  produced no client-visible event at all before they became keys.
- **The 1 MiB image ceiling was NOT required.** Measured at 0.72 MiB
  after `--gc-sections`. The 770 KB figure quoted while planning was the
  sum of the object files' sections; the linker drops what nothing
  reaches. It would have fitted, with 28% to spare.
- **The growable stack was NOT required.** Proven by disabling
  `grow_stack()` and running Doom for 1,750 frames at ~34 fps with no
  fault. It fits in the original four pages.

Both memory changes stand on their own merits and neither was wasted --
but the honest record is that the ceiling was close rather than binding,
and the stack was never the constraint the roadmap had guessed it was.
The roadmap had flagged the stack as the risk and said outright it was
"untested whether Doom's actual stack depth would exceed it". It did not.

### The bug Doom found, which is the whole point of the exercise

Doom died at startup with `W_GetNumForName: STCFN33 not found!` -- a
missing lump, apparently. The lump in `doom1.wad` is `STCFN033`, and
`hu_stuff.c` builds that name with `M_snprintf(buffer, 9, "STCFN%.3d",
j)`. **`kernel/lib/kfmt.c` parsed precision and then ignored it for
integer conversions**, so `%.3d` of 33 produced `33` and Doom asked for a
lump that does not exist.

The header said so explicitly -- precision was "HONOURED ONLY BY THE
FLOAT ONES" -- and justified it by pointing at `%s`, where precision
TRUNCATES and truncating a value is the one thing this file's formatters
may not do. That reasoning was right about `%s` and wrong to generalise:
**precision on an integer can only ADD digits.** The version that
dropped them was the one silently producing a wrong string.

Fixed, with the two corners C specifies and neither of them obvious:

- **The '0' flag is IGNORED when a precision is given**, so `%08.3d` of
  42 is `"     042"` and not `"00000042"`.
- **The two paddings count different things.** The '0' flag pads the
  whole field, sign included (`%08d` of -7 is `"-0000007"`, eight
  characters); a precision pads the DIGITS, sign excluded (`%.8d` of -7
  is `"-00000007"`, nine). The first version of the fix conflated them
  and put one zero too few in front of every negative number -- caught
  by a KTEST written before the code was trusted.

`kfmt.c` is compiled into BOTH RINGS, so this fixed the kernel's own
formatter at the same time. The KTEST that used to assert the old
behaviour (`%.3d` of 5 is `"5"`) now asserts `"005"` and explains why it
changed. **This is the case for building somebody else's program**: the
formatter had tests, the tests passed, and the tests encoded the bug.

## Reading a terminal flushes `stdout` first, and only `stdout`

`printf("Give me two numbers: "); scanf("%d %d", ...)` -- the shape of
every "enter a value" program ever written -- printed nothing until the
program exited. `stdout` is line-buffered on a terminal, the prompt has
no newline, and `refill()` in `userland/libc/stdio.c` went straight to
`sys_read`. So the screen stayed blank while the program blocked, and
the prompt and the answer appeared together at `exit()`, on one line.

It was reported as a difference from Linux and Windows, where the same
source is correct, and the report was right. C11 7.21.3p3 lists this
among the moments a line-buffered stream transmits: "when input is
requested on an unbuffered stream, or when input is requested on a
line-buffered stream that requires the transmission of characters from
the host environment". glibc implements it in `_IO_new_file_underflow`
and MSVC's CRT does the equivalent. toy-os was the outlier, and
`fflush(stdout)` after every prompt is a workaround for a libc gap
rather than the idiom C asks for.

`flush_stdout_for_read()` now runs before any read that can block --
`refill()`, and the unbuffered path of `fgetc()`.

**`stdout` alone, not every line-buffered stream.** glibc HAS the
general form (`_IO_flush_all_linebuffered`) and ships the narrow one,
because the case being served is a prompt and a prompt goes to
`stdout`; the general form walks the whole stream table on every
refill to serve something nothing has ever wanted. Copy the shape,
not the size.

**Gated on the INPUT stream, not the output one.** The flush fires only
when the stream being read is line-buffered or unbuffered -- i.e. a
terminal. Reading a file flushes nothing, which is both what glibc does
and what stops a program that pipes input through a filter paying a
syscall per buffer. It is cheap when it does fire: `flush_write()`
returns immediately on an empty buffer, so the common case is one
compare.

**Where it is NOT placed, and why.** Not at the top of `fgetc()`: a byte
already sitting in the stream's buffer is not a read, and flushing there
would run the check for every character of a `getchar()` loop rather
than once per refill.

## printf's unknown-conversion path is a bug amplifier, so the case table is exhaustive rather than interesting

`kernel/lib/kfmt.c` is the kernel's formatter and, compiled a second
time into `libc.a`, tolibc's `printf` and `snprintf`. Its handling of a
conversion it does not recognise is to emit the letters literally and
consume no argument — which is the documented behaviour, is deliberate,
and is stated in a comment that calls the alternative "far worse".

The trouble is what it does to a *missing feature*. A conversion nobody
implemented is indistinguishable from a typo, so it takes the literal
path, eats nothing, and **every later conversion in the same call reads
the wrong argument**. A gap therefore corrupts output that has nothing
to do with it, arbitrarily far away from itself, and the symptom points
somewhere else entirely.

Three have shipped:

- **`%.3d`** — precision ignored on integers. Doom asked its WAD for
  lump `STCFN33` instead of `STCFN033` and died at startup with
  `W_GetNumForName: STCFN33 not found!`, which reads as a missing file.
- **`%X`** — missing entirely. `/bin/font` printed `U+%04X slot %d` and
  got `U+%04X slot 103`: the `%X` consumed nothing, so the `%d` read the
  codepoint and the slot number vanished. Diagnosed as a wrong slot
  lookup first.
- **`%p`, `%o`, `%+d`, `% d`, `%#x`, `%hd`, `%hhd`** — found by reading
  the switch after the second one, before anything tripped over them.
  `%p` and `%hu` in particular are what ported C reaches for constantly.

So the rule the case table encodes is that **every conversion and every
flag C defines has a case, including the ones this implementation does
nothing with**. `h` and `hh` are accepted and ignored, because default
argument promotion has already widened a `short` or a `char` to `int`
and there is nothing narrower to read — but they must be *consumed*, and
"does nothing" and "is not parsed" look identical until the argument
after them moves.

**Consumption is asserted separately from rendering**, which is the part
that would be easy to leave out. Each of those cases puts a second `%d`
after the conversion under test and pins its value; a case checking only
the first conversion's own output passes happily while the rest of the
line is wrong — which is exactly how `%X` survived until a person read
an odd-looking slot number.

**The table runs in both rings**, as `klineedit_cases.h` does and for
the same reason plus one. kfmt is compiled twice, so "the same source"
is not "the same behaviour" across two code models and two warning sets;
and its header is one file over two implementations (`kfmt.c` shared,
`kfmt_print.c` kernel-only), so a kernel-only include creeping into the
shared half takes `snprintf` away from userland with no KTEST noticing.
The ring-3 half also goes through `<stdio.h>`'s `snprintf` rather than
`k_snprintf`, so a libc wrapper that had drifted from the shared
formatter is caught rather than agreeing with itself.

`%n` stays deliberately absent — it is the one conversion that writes
through a caller-supplied pointer, and it has been a security footgun
everywhere it exists. Floating point stays behind `k_fmt_float()`,
because `va_arg(ap, double)` alone emits SSE in a kernel built
`-mno-sse`.

## Regex lives in tolibc, not inside grep, and it is an NFA

`grep` needed a matcher and there was none anywhere in the tree — the
only pattern matching in this OS was `strstr`.

**It went into tolibc as POSIX `<regex.h>`** rather than staying private
to the command. That is against this project's usual bar (a second real
caller, not a plausible one) and *with* tolibc's, which is deliberately
the opposite: complete rather than minimal, because its audience is code
not yet written (`docs/libc-design.md`). `sed` and `awk` are the obvious
next callers and would each have reimplemented it.

**The engine is a Thompson NFA, simulated, not a backtracker.** A
backtracking matcher is markedly shorter to write and is what most
hand-rolled greps use; it also takes exponential time on `(a*)*b`
against a run of `a`. Patterns here arrive from command lines and files,
which is the same "attacker-shaped data" argument that made `ttf.c`
bounds-check every read. The NFA tracks a *set* of states, so the work
is O(pattern × text) with no bad input. It is Thompson's 1968
construction with Pike's simulation — what RE2 and Go's `regexp` use.

The price is paid in one place and stated in the header: **no
back-references**. They are exactly what an NFA cannot do and what makes
matching NP-hard. glibc supports them as a GNU extension; `regcomp()`
here refuses them by name.

**Jumps in the compiled program are RELATIVE**, which is not a detail:
it is what makes `{n,m}` a `memcpy` of an instruction range instead of a
rewrite of every jump target inside it. Interval expressions are where
regex implementations classically get this wrong.

**`grep` speaks ERE and there is no `-E`.** POSIX grep is BRE — `+ ? |`
literal, grouping spelled `\( \)` — because grep predates the extended
syntax and could not break existing scripts. That is compatibility
baggage this OS has no reason to inherit. `regcomp()` implements BRE
anyway, because it is a lexical difference of about twenty lines and a
POSIX function whose default mode errors would be a worse lie.

## An oracle that shares no code is how an expectation gets checked

`/tests/regex_test` asserts that the engine agrees with a table of
expected spans. Both halves were written by the same person in the same
hour, so it cannot catch the failure that matters most for a spec
implementation: **an expectation that is simply wrong**.

`tools/regex_hostcheck.py` compiles the same table against the host's
glibc and compares. On its first run it agreed on 71 of 74 cases and
isolated three differences, all of which turned out to be deliberate —
and one case where *my* expected span was miscounted had already been
caught minutes earlier by the same table run on the host.

The general rule, and it is the same one `tools/uimg_hostcheck.py`
follows against libjpeg: **when implementing something that has a
specification, find an independent implementation and disagree with it
on purpose.** Every difference is then either a bug or a documented
decision, and there is no third category.

## tolibc stays; porting musl was sized and declined

Asked and answered 2026-08-28, right after dynamic linking landed:
should `/lib/libc.so` be musl instead of tolibc? No -- and the reasons
are structural, not sentimental, so a future session should re-litigate
only if one of them changes.

**musl is Linux-only by construction.** Unlike newlib it has no OS
porting layer; its internals call Linux syscalls directly at hundreds
of sites. "Port musl" therefore means "make this kernel speak enough
of Linux's syscall ABI", and the missing pieces are each a real kernel
project: `fork`+`execve` (musl's `system`/`popen`/`posix_spawn` bottom
out in them; this kernel is deliberately spawn-shaped and has no COW),
`futex` (every musl lock -- pthreads, malloc, stdio -- sits on it;
wait-channels here are kernel-internal), the `*at` family (no dirfd
concept exists), `ioctl` (deliberately dedicated `tc*` syscalls
instead), and a real `struct stat` (deliberately absent -- TFS3 lacks
the fields, and invented zeroes make ported code take wrong branches).
Summed, that is larger than the whole dynamic-linking milestone was.

**And it would cost the compiled-both-rings design.** `kfmt`,
`klineedit`, `heap_core` and `completion` are one source serving ring 0
and ring 3, with shared case tables asserting both builds; errno and
TLS integrate with libsys; strace's decode matches what userland
actually calls. musl can participate in none of that -- the swap
deletes the property that both shells agree what Ctrl-A does because
they run the same lines.

**What IS taken from musl**: individual implementations, with
attribution -- math corner cases, printf edge behaviour, test vectors.
tolibc's bar is already "complete rather than second-caller"; filling
it with battle-tested code keeps everything above.

**When to revisit**: if running unmodified Linux binaries ever becomes
a goal in itself, the prerequisite chain is futex -> per-frame
refcounts -> COW fork -> execve -> the `*at`/stat/ioctl surface -- each
worth building on its own merits or not at all -- and at the end of it
a musl port is nearly mechanical. That would be a Linux-compat
milestone, not a libc swap.
