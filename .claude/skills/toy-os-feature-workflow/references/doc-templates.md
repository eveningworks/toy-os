# Doc shapes to copy, not reinvent

toy-os's docs have a consistent voice across many changes. The fastest
way to keep matching it is to read a few recent entries before writing
a new one, but if you want a template plus real excerpts to anchor the
tone, they're below. **Check these against the live files before
trusting them blindly** -- this repo's conventions have changed before
(see the versioning entry below, and the note at the top of the main
skill file), so a stale template here is a real risk, not a
hypothetical one.

## The commit message (this replaced the CHANGELOG entry)

**`CHANGELOG.md` is CLOSED and has been since 2026-08-15. Do not write
entries for it.** This section used to teach that format, and a session
following it would have been adding to a frozen file -- which is exactly
the stale-template risk the top of this file warns about, caught on
2026-08-18.

What replaced it, and where each kind of thing goes now:

- **What changed, file by file** -> the COMMIT MESSAGE. Subject line is
  a short summary; the body lists each changed or added file with a
  one-line note. That is what makes a commit skimmable on GitHub
  without opening the diff.
- **How a mechanism works, and the trap in it** -> a comment next to the
  code. This is what actually gets found by whoever edits it.
- **Why this way and not the obvious way** -> `docs/decisions.md`,
  written out in full rather than as a pointer (template below).
- **What is broken or not built yet** -> `docs/roadmap.md`, with a
  reproduction precise enough to replay (template below).

The commit body is where a real change earns its length. The shape that
has held up:

1. **What was wrong**, stated as the symptom someone would report.
2. **What actually caused it** -- especially when that differs from the
   obvious suspect, which in this repo it often does.
3. **The file list**, one line each.
4. **What was verified**, naming the numbers: which suites, how many
   checks, and the positive control if one was run.
5. **What was NOT established.** A measured "this stopped reproducing
   and I do not know why" is worth more than a claimed fix.

Match the weight to the change. A docs-only or one-line fix is two or
three sentences and a file list; a feature or a real bug hunt is the
full five. The same judgment the main skill file describes for when to
skip ceremony entirely.


## docs/decisions.md entry

**Entries live in `docs/decisions/<area>.md`** -- kernel, storage,
drivers, gui, shell, build, workflow -- and `docs/decisions.md` is a
GENERATED index over them. Add the entry to the right file, then run
`tools/gen_decisions_index.py`; `tools/check_docs.py` fails the build if
you forget. Never hand-edit the index.

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
The index in `docs/decisions.md` is GENERATED -- run `tools/gen_decisions_index.py` after adding an entry, never edit it by hand.

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
already documents that area (`docs/features.md`, `apps/README.md` for
app-specific behavior, etc.) rather than appending a new paragraph
somewhere else. README.md has been a short FRONT PAGE since 2026-10-02 --
highlights, try it, status, docs, license -- with build and run detail in
`docs/building.md`; touch its Highlights only for a headline-level
capability, and its Known gaps when one closes. A reader looking up "what does the font system
support" should find a new Nordic-glyph note right next to the rest of
the font description, not in a separate location. README.md was
trimmed down to focus on current capabilities -- if what you're adding
feels more like background/rationale than "what toy-os does today," it
likely belongs in `docs/decisions.md` or `docs/roadmap.md` instead of
growing README further.

## A source comment -- before and after

The rule is in `SKILL.md` step 3. This is the case that produced it: one
`#define` in `kernel/include/abi/win_proto.h`, written by a session that
had read CLAUDE.md's "cap the anecdote at one clause" and broke it
anyway. 37 lines became 10, and nothing a reader needs was lost.

**Before** (the maintainer's word for it: "this is like a war story
now"):

```c
#define WIN_REQ_CURSOR     24 // `window`: which one; a: a WIN_CURSOR_*.
                           // "While the pointer is inside my content
                           // area, show this."
                           //
                           // Set on POINTER MOTION, not once at startup:
                           // a window is not uniformly one thing, and
                           // the field this exists for occupies a few
                           // rows of a window whose toolbar and status
                           // bar want the arrow. That is why this is a
                           // request a client repeats rather than a hint
                           // it declares (WIN_REQ_HINTS) -- X11's
                           // XDefineCursor is per-window and xterm shows
                           // an I-beam over its scrollbar because of it.
                           //
                           // **IDEMPOTENT, AND THE CLIENT IS EXPECTED TO
                           // FILTER.** Sending the shape a window
                           // already has is accepted and tells the
                           // compositor nothing, so uapp_set_cursor()
                           // drops the no-op rather than putting a
                           // syscall and an event on every mouse move.
                           //
                           // An unknown shape is refused (0), not
                           // clamped: a client compiled against a later
                           // WIN_CURSOR_* than the running kernel should
                           // find out, rather than silently getting an
                           // arrow forever.
                           //
                           // The shape is per WINDOW and dies with it.
                           // There is no "reset on leave" message: the
                           // compositor already stops honouring it the
                           // moment the pointer leaves the content area,
                           // so a client that never resets cannot leave
                           // a wrong cursor over somebody else's window.
```

**After**:

```c
#define WIN_REQ_CURSOR     24 // `window`: which one; a: a WIN_CURSOR_*.
                           // Honoured only inside that window's content
                           // area, so a client that never resets cannot
                           // strand a shape elsewhere.
                           //
                           // Set per MOTION, not once: a window is not
                           // uniformly one thing. Idempotent, and
                           // uapp_set_cursor() drops the no-op rather
                           // than sending one per mouse move. An unknown
                           // shape is refused, not clamped.
```

**What each cut was.** The X11/xterm sentence justified the design --
that is `docs/decisions.md`'s job, and it says it there. The
"IDEMPOTENT, AND THE CLIENT IS EXPECTED TO FILTER" paragraph argued for
a decision the word "idempotent" already states. The refusal paragraph
explained why refusing beats clamping; the word "refused" is the part a
reader editing this needs. The last paragraph re-derived the clamp,
which the first sentence had already given.

**What survived, and why.** The field meanings (nobody can guess them);
the clamp (an invariant a caller must not assume around); "per MOTION,
not once" (the trap -- an implementer's instinct is to set it at
startup); "drops the no-op" (a caller wondering about the cost);
"refused, not clamped" (a caller handling the return).
