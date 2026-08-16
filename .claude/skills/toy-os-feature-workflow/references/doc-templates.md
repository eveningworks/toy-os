# Doc shapes to copy, not reinvent

toy-os's docs have a consistent voice across many changes. The fastest
way to keep matching it is to read a few recent entries before writing
a new one, but if you want a template plus real excerpts to anchor the
tone, they're below. **Check these against the live files before
trusting them blindly** -- this repo's conventions have changed before
(see the versioning entry below, and the note at the top of the main
skill file), so a stale template here is a real risk, not a
hypothetical one.

## CHANGELOG.md entry

Current format (Keep a Changelog style): every change gets an entry
under the `## [Unreleased]` heading, in the appropriate subsection --
`### Added`, `### Changed`, `### Fixed`, `### Removed`, `### Deprecated`,
`### Security` -- pick whichever fits, most toy-os entries are `Added`
(a new capability) or `Changed`/`Fixed`. No version number or tier
attaches to an individual entry anymore; that only happens when
`tools/set_version.sh <version>` (no `-dev`) cuts a release and stamps
the whole `[Unreleased]` section at once. (You'll still see old
`## Build <N> (<tier>, +<delta>)` headings further down in the file, or
in `CHANGELOG-archive.md` -- those are frozen history from before the
versioning switch, not a format to write new entries in.)

Within an entry, roughly this order (not every entry needs every part
-- a small fix is often 2-3 sentences total):

1. **What was asked**, close to verbatim if it's a direct quote-worthy
   request, otherwise a faithful paraphrase.
2. **What research found** -- what the code actually did before this
   change, including anything surprising (a stale doc claim, a
   landmine, a terminology mixup that got cleared up first).
3. **What choices were presented and what got picked**, with a one-line
   "why" for the winning option and, often, a one-line "why not" for
   what got passed over. This is the connective tissue between "we
   asked" and "here's what shipped" -- don't skip the why.
4. **Specific technical detail on what changed**, naming files in
   backticks -- not "updated X" but what changed about X and why that
   shape was chosen. A multi-file change often becomes its own bullet
   list within the entry.
5. **A closing note on what was verified** -- what was actually tested
   and how (`boot_smoke_test.py`, specific QMP interactions, a reboot
   to check persistence, a regression re-run of other tests), naming
   the proof, not just asserting correctness.

Real example -- the entry written for the versioning switch itself,
live in `CHANGELOG.md`'s `## [Unreleased]` section:

> ### Changed
> - Versioning switched from a per-change build-number scheme
>   (`tools/bump_build.sh <fix|feature|major>`, a git tag `build-N` on
>   every push) to semantic versioning with a `-dev` suffix during
>   development. `VERSION` (repo root) now holds a plain semver string
>   -- `0.1.0-dev` to start -- read by `tools/gen_version.sh` into
>   `kernel/include/version.h`/`TOYOS_VERSION` exactly like
>   `BUILD_NUMBER` was before... Requested directly, to stop needing a
>   fix/feature/major judgment call and a tag on every small change --
>   see `docs/decisions.md`'s entry on this for the full reasoning.
> - Commit messages going forward list each changed/added file with a
>   one-line note in the body..., so a commit is skimmable on GitHub
>   without opening the full diff.

Notice this real example is much more compact than a full 5-part
writeup -- it's a process/tooling change, not a feature, so it skips
straight to what changed and why. Match the entry's weight to the
change's, the same judgment call the main skill file describes for
when to skip ceremony entirely.

## docs/decisions.md entry

Only add one when the change answers a "why does toy-os work this
way" question a future session would plausibly hit again -- most
changes don't clear that bar (the Nordic-keyboard change did, because
"why Latin-1 and not UTF-8" and "why only 3 remapped keys" are exactly
the kind of thing a later session extending this would want answered
without re-deriving it; plenty of smaller changes reference an
existing entry rather than getting a new one).

Shape: a `##` heading naming the decision, then a few sentences --
not the full reasoning restated, just enough to answer the question
plus a pointer into the relevant `CHANGELOG.md` (or `CHANGELOG-archive.md`,
for anything old enough to have moved there) entry for the complete
writeup. This file is an index, not a second copy of the history.
Every entry also gets a line in the grouped index at the top of the
file -- add it, or the index silently stops being one.

**When a later change supersedes an existing entry, don't rewrite or
delete it -- amend it in place with a dated note** ("**Updated at
Milestone 15:** ...", or a "**Superseded -- was X, see below**"
header for a full reversal), keeping the original reasoning as the
record. Real examples live in decisions.md itself: the Esc-exits-GUI
entry (full reversal, original preserved below the note), the
single-backend-VFS and timestamps entries (partial updates, and the
timestamps one also RETITLED because its old title stated the
opposite of the current behavior -- retitle when the title itself
became the lie, and update the index line to match). The exception:
a recorded claim that was simply WRONG (never true, not outdated)
gets deleted, not amended -- a corrected entry still implies
something was once broken.

Real example:

> ## Nordic keyboard/character support: Latin-1, not UTF-8; 3 remapped keys, not a full layout
>
> Adding Å/Ä/Ö support meant three separable choices, made the same way
> each time: keep the codebase's existing "1 char = 1 cell = 1 glyph"
> assumption intact rather than take on the much bigger UTF-8 rework it
> doesn't need yet.
>
> **Encoding: Latin-1/ISO-8859-1 single bytes... not UTF-8.** Every
> byte-buffer boundary in this kernel... already assumes one byte is
> one character is one glyph cell; UTF-8 would break that assumption
> everywhere a multi-byte Nordic letter crossed it...

## docs/roadmap.md (forward-looking, not-built-yet items)

For a change that was *planned but not built* (a "just plan it" answer
to a scope question) -- this used to live in README.md's "Ideas for
what's next" section, but that section moved to its own file,
`docs/roadmap.md`, to keep README focused on "what toy-os can do
today." Checkbox-list format, roughly grouped into "In progress / up
next" and "Backlog" near the top (an "At a glance" section), with full
detail further down:

```
- [ ] <short description of the idea> -- <why/what it needs, one line>
```

When something listed there actually gets built later, check the box
and note it (`- [x] ...`), following the file's own "Recently done"
section pattern -- don't just delete the line. The file's own intro
explains this is "actively maintained, not a stale wishlist"; treat
that literally.

## README.md

For a change that was actually built (not just planned): extend the
existing feature/capability description in place, wherever the repo
already documents that area (README's own feature list, `apps/README.md`
for app-specific behavior, etc.) rather than appending a new paragraph
somewhere else. A reader looking up "what does the font system
support" should find a new Nordic-glyph note right next to the rest of
the font description, not in a separate location. README.md was
trimmed down to focus on current capabilities -- if what you're adding
feels more like background/rationale than "what toy-os does today," it
likely belongs in `docs/decisions.md` or `docs/roadmap.md` instead of
growing README further.
