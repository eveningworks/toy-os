---
name: toy-os-feature-workflow
description: >
  Use this skill whenever the user asks for a new feature, fix, or change to
  toy-os (their x86-64 hobby OS, repo at ~/CodingProjects/toy-os).
  Triggers on things like "add support for X",
  "can we improve Y", "let's build Z", or any bug report against the OS,
  shell, GUI, or filesystem -- even if the user doesn't say "toy-os" by name,
  treat any request touching kernel/, apps/, userland/, or the shell/GUI/
  filesystem behavior of this OS as toy-os work. This is the end-to-end
  playbook -- research the code first, present real implementation choices
  before writing anything, build and verify in QEMU with a positive control,
  write the docs in the repo's established style, and ship the change to the
  user's checkout. Do not start editing
  kernel/app code for this repo without consulting this skill first.
---

# toy-os feature workflow

Each session starts with no memory of the last; following the same
sequence every time is what makes it a continuation rather than a fresh
contractor improvising. `CLAUDE.md` holds the *mechanics* (layout, build,
QMP API) and wins over this file; this file is the *order of operations*.
Re-read `CLAUDE.md` fresh each session -- the workflow has changed under
this skill before.

**THIS FILE IS THE PLAYBOOK ONLY. Lessons live in `references/`; read the
one SECTION you need, never a whole file** (some are 150 KB+ and every
reader pays for them):

| File | Read when |
|---|---|
| `references/testing-quickref.md` | step 4 in full: test order, QMP helpers, harness traps |
| `references/session-testing.md` | before writing a test, or believing one |
| `references/session-diagnosis.md` | something is broken and you cannot see why |
| `references/session-gui.md` | Toykit, widgets, the compositor |
| `references/session-design.md` | why something landed the way it did |
| `references/session-history.md` | the dated per-session notes (where the project stood after each big day) |
| `references/questions-that-worked.md` | phrasing step 2's choices and mockups |
| `references/doc-templates.md` | step 5's doc shapes, a comment before/after |
| `references/delivery-checklist.md` | step 6, releases, and steps 5-6 in full |

## The sequence

1. **Research before proposing anything.** A "simple" request usually
   touches more than it looks. Survey with a CHEAP agent (Sonnet/Haiku,
   `Explore`) when it spans more than a couple of files; come back with
   what exists and what a change would touch.

2. **Present a few real choices before writing code** (`AskUserQuestion`;
   standing instruction). Choices come from the research -- real forks
   (data layout, where a setting lives, how far to build this session).
   **Anything visible gets MOCKUPS FIRST** -- a Design canvas in toy-os's
   own palette with real data, published before asking
   (`questions-that-worked.md`). Ask whether a GUI addition should be a
   reusable `userland/ui/` widget. Skip only for an unambiguous one-liner.
   Say what Linux/Windows/Wayland do first (CLAUDE.md).

3. **Implement** in the real checkout. **Comments are an invariant and a
   trap, not an essay**: would the sentence be true had nobody got it
   wrong? does `docs/decisions.md` already say it? naming a real system is
   a clause, justifying it is a decisions entry; >~6 lines on a small
   thing is a smell (`doc-templates.md` has a before/after).

4. **Build and test, scaled to what changed** -- cheapest first:
   `boot_smoke_test.py` -> `make test` / `vm.py exec` (text) -> QMP only
   for pixels, layout, input. Add a test (KTEST beside the code, or a
   tool), and **watch it go red** on a deliberately broken build before
   trusting it. Test against a COPY of `disk.img`; ask the user to close
   their QEMU before `make iso`/`preflight.sh`. Read pixel values, not
   impressions. Full detail and every harness trap:
   `references/testing-quickref.md`.

5. **Write the docs in the existing style.** No changelog: the COMMIT
   MESSAGE is problem, then change, then every file with a note, plus a
   `Release-note:` trailer for anything a person can see or do. A new
   command needs its `docs/commands/` page in the same change. Roadmap
   items are one line (detail in `roadmap-details.md`, same title);
   urgency is `**NEXT**` + `gen_next_up.py`. A `docs/decisions.md` entry
   only for a "why this way" a future session would re-litigate. Full
   text: `delivery-checklist.md`, "Steps 5 and 6 in full".

6. **Ship.** Gate with `tools/preflight.sh` (and `gui_regress.py --logs`
   after anything userland or drawn), commit with plain `git`, push
   verified work to `main` without asking (tags, releases, force-pushes
   and history rewrites are confirmed first), and `update_server.py
   --publish` so System Update offers it. In the hand-over: every file
   added/edited, a **"try it yourself"** guide (flags, what to type, what
   to expect, or "nothing to see"), external hosts contacted, and the
   stack drawn if a layer boundary moved. Reusable tooling goes in
   `tools/` with a `docs/tools.md` entry, named by a runner.

## Delegating to agents (2026-10-05, after one night burned a session limit twice)

- **Fresh agent, short brief** -- branch, SHA, the exact list, where to
  test, which slots. Never keep resuming a large agent: it re-reads its
  whole history every round.
- **Parallel only for independent tracks**, each in its own worktree; an
  agent never checks out anything in another worktree (scratch trees go
  under the scratchpad via `git worktree add`).
- **Cheap models for research, search and test runs**; Opus for hard code.
- **Briefs do not say "read the skill" or "read session-testing.md"** --
  give CLAUDE.md plus the one reference section that matters.
- **Code reviews only when the user asks.** Never review a fix-up commit.
- **Two strikes:** if a fix round introduces a new regression, stop
  patching -- write the design down in a few lines and ask (or decide
  once, overnight) before another round. **Check a premise in the code
  before directing an agent on it.**
- Merge once a track is green and covers what was asked; minor findings go
  on the roadmap, not into another round.

## Verification habits this project rewards

- **A green test proves nothing until you have seen it go red** --
  break the thing, see the RIGHT assertion fail, restore.
- **Assert round trips, not appearances** -- check bytes through an
  independent path, compare a state against itself after a cycle.
- **Pick an assertion the failure mode cannot pass** -- ask what a
  broken version would still pass.
- **When something fails twice, stop reasoning and go look** (a
  screenshot, a `dmesg` line).
- **Suspect your own test before the code, but verify either way.**
- **A mechanism that explains the symptoms is not the mechanism that
  caused them** -- prefer the discriminating experiment.

## When the request is small

A one-line fix or copy edit needs no question round, no decisions entry,
and a short commit message. Use the ceremony the change deserves.
