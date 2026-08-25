# What good pre-implementation choices look like here

The point of asking before implementing isn't ceremony -- it's that
this user has opinions about tradeoffs (naming, scope, how much to
build now vs. later) and would rather weigh in before code exists than
after. Good choices are concrete forks that came out of research, not
generic "how thorough should I be" filler. A few real examples:

**Naming + two real design forks, from adding filesystem journaling:**
the user asked for journaling, date-code support, *and* a name for the
filesystem, all in one message. That's three independent choices, not
one -- they were asked as three separate questions (name options,
journaling design -- e.g. write-ahead log vs. alternatives -- ,
timestamp fields to track), each with a recommended default. The user
answered all three at once, and every answer was actually used
(including the chosen name, "TFS2", which then became routed through
every doc and comment from then on -- naming choices
aren't cosmetic, they propagate).

**Scope-of-session, not just scope-of-feature:** asked to plan support
for running ELF binaries from disk, the real fork wasn't "how should
this work" but "how much of this should get built today" -- research
turned up that the request bundled two genuinely separable
capabilities (a ring-3 program using existing infra vs. actually
loading from persistent disk at runtime, which needed filesystem
changes first). The question that got asked wasn't generic ("small or
large change?") but specific to what research found: build just one
piece, build both, or just write up the plan. The user picked
"just plan it," which is exactly why `docs/roadmap.md` exists -- it's
the parking lot for exactly this kind of answer (this list used to
live in README.md's "Ideas for what's next" heading, before README got
trimmed down and the section moved out to its own file).

**A tradeoff with real memory-cost implications, not a coin flip:**
same ELF-loading research surfaced that supporting files bigger than
today's fixed per-slot size meant picking a storage strategy with a
real cost tradeoff (grow every slot's size, uniformly costing static
RAM whether or not a given file needs it, vs. multi-slot chaining that
only costs extra for files that actually need it). That's worth a
question because the two options have genuinely different downsides,
not because offering choices is inherently good.

**Catching a mismatch before it wastes a research pass:** a request
for "EFI binaries" got clarified ("meant ELF?") before any research
happened, because EFI/UEFI and ELF are unrelated formats and boot
models -- going down the wrong research path would have been a much
bigger waste than one clarifying question. Don't research into the
literal words of an ambiguous request if there's a plausible
mismatch worth 10 seconds to rule out first.

**Bundling a testing-methodology decision into the choices, not
deciding it unilaterally:** adding Nordic keyboard support surfaced a
real signed-char bug affecting six places in the code -- the question
of whether to fix it as part of the same build (vs. defer and just
document it) was presented as one of the choices, not assumed. The
user picked "fix it now," which mattered because the feature literally
doesn't work end-to-end without that fix -- a plausible-sounding
"we'll do that separately" answer would have shipped a broken feature
with a straight face.

**Bundling two roadmap items when the plumbing is identical, as its
own explicit choice:** research for NX bit enforcement found that the
very next roadmap checkbox (W^X) needed the exact same code-site
change (`elf.c` actually reading each ELF segment's real `p_flags`,
today completely ignored) -- so "bundle NX + W^X together" vs. "NX
only, leave W^X for later" was asked as a real question, not silently
decided either way. The user picked bundling; the payoff showed up
later in the same research (the plain `vmm_map_user_page()` wrapper
could then default to writable-but-non-executable, making the
pre-existing stack/heap/framebuffer/window-buffer call sites secure
for free with zero call-site changes -- a fact only worth surfacing
because the bundling choice was already made). A companion question in
the same round -- kernel identity map in scope too, or userspace only
-- traded a much bigger, riskier boot-path change against a smaller
self-contained one; the user picked userspace-only, which is exactly
the kind of session-risk judgment call that belongs in front of the
user, not assumed from "more thorough is always better."

**Two-level scope questions, and honoring the boundary the user drew:**
"can we do some more compositor work?" got a first question (how big a
change -- a few small render tweaks, vs. the real Milestone 9
per-window dirty-rect item) and, once the user picked the big option, a
*second* question specifically about how far to go this session (a
3-phase breakdown: a clip-rect primitive, scene-level damage tracking,
then skipping `on_draw()` for undamaged windows). The user picked
"Phase 1+2, not 3" explicitly. That boundary was honored for an entire
session (shipped, documented, committed, pushed as its own unit) before
Phase 3 was even started -- in a *later* session, when the user asked
for it by name ("Start Phase 3"). Two lessons: don't silently roll a
deferred phase into the same delivery just because it's easy to keep
going, and a `docs/roadmap.md` note naming the deferred phase precisely
(not just "more compositor work later") is what let a fresh session
pick it up cold with zero re-explaining needed.

## The trap this repo has actually fallen into

Even with reviewed code and grep-verified consistency, a real
Nordic-keyboard session shipped a bug: five call sites were fixed with
a shared macro, a sixth site existed with the *same intent* but
different wording (`c < 128` instead of `key >= 32 && key < 127`) and
the grep for the fixed pattern didn't catch it. It was found only by
actually testing the behavior end-to-end over QMP and noticing a
character didn't render -- not by re-reading the fix. The lesson isn't
"grep harder" -- it's that a claim like "fixed every instance of X"
deserves a behavioral test, not just a second pass of the same static
check that found the first five.

A second, structurally similar trap: a **deferred phase of the same
feature can turn an existing, shipped bug from silent into visible.**
The compositor's Phase 1+2 (damage-region tracking) had a real gap --
`bring_to_front()` never damaged the *previously*-frontmost window's
rect, only the newly-promoted one, even though that window's titlebar
tint changes on every z-order swap too. Phase 1+2 shipped fine anyway,
because every window's chrome still got *drawn* every frame back then
(the damage region only clipped which pixels a draw call could touch,
it didn't skip the call) -- the missing damage report never actually
produced a wrong pixel, so nothing in QMP testing at the time caught
it. Phase 3 (a later session) started skipping the draw call itself
for undamaged windows, and that exact gap turned into a real, visible
stale-titlebar bug the moment it landed. The lesson: when a later
change makes existing logic *more precise* (skip instead of
clip-and-discard, exact instead of approximate), re-verify the
assumptions the earlier, looser version was quietly relying on --
"it worked before" can mean "it was drawn twice and got away with it,"
not "it was reported correctly."

A third trap, from shipping the taskbar notification-area (tray)
feature: **an "optimization" can silently remove a side effect nothing
ever designed on purpose, but something else had come to depend on.**
The tray/clock tick used to report no damage at all, which forced
`wm_render_frame()`'s full-screen fallback every single second -- not
because anyone wanted a once-a-second full redraw, just as an
unadvertised side effect of the clock tick being the one thing that
set `redraw_pending` with nothing more specific to say. That accidental
full redraw turned out to be load-bearing: it was what kept
`wm_render.c`'s cursor-under-pixels snapshot resynced against the real
screen every second. Scoping the tick's damage to just the taskbar
strip (a real, deliberate optimization, asked for and approved as one
of this feature's own implementation choices) removed that side effect
along with the wasted work, and the mouse cursor stopped tracking
correctly -- caught live on the user's own machine, not in QMP testing,
because it needed real elapsed time and a real PS/2 mouse to show up.
Same root pattern as the compositor trap above (a later, more-precise
change breaks an assumption an earlier, looser one was quietly
carrying) but the twist here is worth naming separately: the earlier
behavior wasn't a documented feature anyone was relying on, it was
pure incidental fallout of *how* a different feature (the clock) was
implemented -- "nothing depends on this side effect" is not something
grep or code review can confirm, only removing it and testing for real
can.

**A menu of pieces plus a scope ladder, when the request is "give me
many choices":** asked whether the kernel needed "toolkit C libraries
so code won't have to invent the wheel again", the useful move was to
*count first* -- a survey found int->string written 9 times,
int->hex 10, digit parsing 6, path resolution 3 -- and then offer the
four candidate libraries as a multi-select alongside a separate
scope question (build and migrate everything / build but migrate
later / plan only). The evidence is what made the choices real: the
strongest argument for the toolkit wasn't "duplication is bad", it was
that the most recent copy had been added by the previous commit *in
the same session*. The user picked all four and full migration. Two
lessons worth carrying: put the count in the question, and offer the
"do it all" option explicitly rather than making the user assemble it
from parts.

**Four independent forks in one feature, each a real fork:** asked to
make the shell's input line editable "with bash convention for CTRL and
ALT", research found four decisions that genuinely couldn't be
defaulted -- how Ctrl/Alt should be encoded (control codes + ESC prefix
vs. new `KEY_*` codes vs. a modifier-state query), where the shared
editing logic should live, how much of the readline keymap to build,
and how the physical console should draw a mid-line cursor. Asking all
four at once worked because each came with what research had turned up
(e.g. the driver tracked no Ctrl at all; the GUI Terminal's scrollback
widget already had a cursor API; `docs/decisions.md` had already
rejected the modifier-state shape for Shift+arrows). The user picked
the recommended option on three and "everything" on the keymap.

**When research contradicts the option the user picked, say so and
build it anyway.** In that same round the user chose "add a
cursor-move primitive to `vga.h`" -- and implementing it turned up that
the framebuffer cursor erases its cell to black when it moves, which is
fine over the blank append cell and destructive over a character, and
that `apps/editor.c` had already solved the same problem a different
way (reverse-video). The right response was neither to silently
substitute the other design nor to stop and re-ask: state the wrinkle
in a sentence, build what was chosen, and make it correct (park the
cursor solid so an idle blink can't eat the glyph). The one real bug
this feature shipped with -- a black hole where the character under the
cursor had been -- came from exactly that hazard and was caught by
QMP-testing the accept path, not by re-reading the code.

**A scope ladder where the user took the TOP rung -- and staging is
what made that safe:** asked whether a round of VFS work should be
infrastructure-only (testable against the existing filesystem, with
the new TFS3 backend landing later) or the full Milestone 15
implementation, the user picked the full milestone. What made a
milestone-sized "yes" deliverable in one stretch was slicing it into
five stages (infra -> read side + host tool -> write path -> fsck ->
default flip + docs), each of which built, passed `make verify`, and
was committed AND pushed before the next began -- so an interruption
at any point would have left `main` sane and the work resumable. The
user's only mid-flight steering was "commit and push after this
stage, just in case", i.e. MORE checkpointing -- evidence the staging
instinct matches how they want big work run. Offer the do-it-all
option, but pair it with a staging plan, not a monolith.

## A fourth trap: three small wrongs multiplying into one silent disaster

One boot quietly reformatted the development disk image as the wrong
filesystem, and no single cause explains it: (1) a stale object file
-- `struct fs_ops` had grown a field, and a `.o` built against the
old layout read garbage past the end of another object; (2) a
correctly-paranoid validation check (caps honesty) saw the garbage
and refused a perfectly good backend; (3) an earlier refactor had
silently changed which backend was the blank-disk DEFAULT (a new
entry was prepended to a list while the `#define` indexing it stayed
0). Each alone was survivable; together: nobody claims the disk, the
policy formats it with the wrong default. Lessons, each independently
useful: when runtime behavior contradicts source you have just read,
run `make clean` before theorizing (CLAUDE.md's dependency-tracking
caveat, observed live); two constants that must move together belong
one comment apart with the coupling stated; and a "defensive" check
that fires is EVIDENCE, not noise -- the honesty check turned a
silent memory-layout skew into a loud, debuggable refusal, which is
exactly what it was for. Related harness fact from the same episode:
`make verify` re-seeds `disk.img` by SYNC, not reformat, so damage
from a previous run persists across verify runs -- a fix can look
broken against leftovers; reproduce on a `make clean-disk` fresh
image before concluding anything.

## Asked for a mechanism, offer the mechanism AND the need behind it

"Can you add a `/usr/wm/startmenu` directory for the Start menu?" -- a
concrete, reasonable request with a wrong shape underneath it. Research
first showed one directory ALREADY fed both the desktop and the Start
menu (grouped by a `Category=` key), so the real gap was not a second
directory but the inability to show an entry on one surface and not the
other. A second directory would deliver it while guaranteeing drift: any
app wanted in BOTH places has its file duplicated, and a rename or a
changed `Exec=` then updates one surface only. freedesktop.org hit the
identical question and answered it with a key (`OnlyShowIn`/`NotShowIn`).

The question that worked put the mechanism choice to the user rather
than either silently substituting one or building what was literally
asked: four options -- a `ShowIn` key (recommended, with the drift
argument stated), splitting `NoDisplay` into two keys, the second
directory AS ASKED with its cost spelled out, and "nothing, it already
works via Category". The user picked the key. The pattern generalises:
when a request names a MECHANISM, research whether the underlying NEED
is already half-met, and if the named mechanism has a failure mode, put
it in the option's own description rather than arguing against the user
in prose.

## When a fix cannot be validated at the confidence you'd like, say the number

A three-session intermittent got diagnosed by instrumenting a real
failure, and the fix was principled (it addressed the exact mechanism
the probe exposed, using the approach two other tools already
documented). What could NOT be shown was a before/after RATE: the
"amplifier" that appeared to reproduce it on demand turned out not to,
so four more runs with the fix reverted passed anyway. The honest
report separates the two -- mechanism established by direct evidence,
rate improvement not established -- and the docs record that the
amplifier does not work, so the next session does not rediscover it.
Reporting "fixed, suite green" would have been true and misleading.


## Settings, groups and widgets (2026-08-19)

A long session where the user kept adding scope mid-turn, and the
question rounds are what kept it coherent. Four that earned their place:

- **"Which new UI component should the sidebar be?"** offering a real
  tree, a flat sidebar list, or extending the listbox -- with the honest
  cost of each. The project rule is to ASK before adding a widget, and
  the answer (a real `uui_tree`) was justified by naming the SECOND
  caller the roadmap already has (a file manager), which is this
  toolkit's actual bar.
- **"Where do the sidebar's categories come from?"** -- the kernel, the
  namespace, or a table in the app. Framing the third option as "the
  second source of truth the whole app exists to avoid" is what made the
  choice obvious, and it came straight from the app's own top comment.
- **"What should the app be called?"** with what KDE, GNOME, macOS and
  Windows each call it, so the answer was informed rather than a
  preference. Naming questions are cheap to ask and expensive to redo.
- **"How should I land this? It has grown well past the original
  scope."** Asked when `settings.c` was mid-rewrite and nothing was
  committable, with an honest status table of what built and what did
  not. That is the question to ask when scope has doubled -- not at the
  end, and not silently pressing on.

**And one that should have been asked and was not.** The user asked "we
need to change etc_config format, give me few choices" and I offered
four format options; the real question was what problem the change was
for, and the answer turned out to be human-facing TEXT (descriptions,
choice display names) rather than the file layout at all. When a request
names a SOLUTION, ask what it is a solution TO before enumerating
variants of it -- the four options were all technically sound and all
aimed at the wrong thing.

**And one whose OPTIONS were sound and whose AXIS was wrong (2026-08-24,
the keyboard tap).** The question was "should the kernel keep a rolling
log of key events, or record only while the tool runs?", and both
options were laid out honestly with the real cost of each -- ~5 KB of
BSS and a few instructions in the IRQ handler against having to
reproduce a bug with the tool already open. The user picked always-on,
and it was built.

Then the user said the repo might go public, and: *"I don't want my OS
to tap anyone's keyboard presses to a big buffer."* Which reversed it,
correctly, and pointed at the axis the question never mentioned. The
costs I had priced were **cycles and bytes**, both trivial -- so
"always on" looked free. The cost that decided it was **disclosure**: a
buffer of the last ~128 keystrokes is a keylogger, this kernel has no
privilege model, and nothing in the question I asked would have surfaced
that.

The rule, which is not specific to keyboards: **when an option RETAINS
user input or user data, one of the trade-offs on the table has to be
what the machine is now holding and who can read it** -- alongside
"would you want to explain this to somebody reading the repo?". Ask it
even when the practical risk on a single-user hobby OS is small, because
the answer changes what gets built and the user is the only one who can
weigh it. Two rounds of well-formed questions here still missed it; the
user supplied it unprompted.

The follow-on question that DID work, asked once the axis was clear, was
narrow and had exactly one real fork in it: *how* should it be armed
(tunable only / tunable plus auto-arm from the tool / compile-time), and
*what happens to what was already captured* when it is switched off. The
second half is the one worth copying -- "what does OFF mean for the data
that already exists" is a question a consent switch always has, and a
design that has not answered it has a decorative switch.

## FAT32 and mount points (2026-08-25) -- four forks, and the user took the biggest of each

The request was one sentence ("add FAT32 support so /boot shows as
/boot"), and research turned it into TWO roadmap milestones. The four
questions asked were:

1. **FAT scope** -- read-only / read+write / read-only-now-write-staged.
2. **Mount mechanism** -- a small real mount table / a hardcoded `/boot`
   slot / the full milestone.
3. **Mount policy** -- automatic + a `mount` command / command only /
   an `/etc/fstab`.
4. **The block seam** -- device passed at mount / swap the active device
   per call / give FAT alone a device pointer.

The user chose **read+write**, **the full milestone**, the recommended
automount, and the recommended seam.

**What made these work, and is worth copying:**

- **Every option came from the code, not from a template.** Question 4
  only exists because `blk_active()` is singular and the research found
  it; a generic "how thorough should this be?" would have surfaced none
  of the real risk.
- **Each option named its concrete cost in the repo's own terms.** "The
  shape CLAUDE.md and `check_dispatch.py` exist to prevent" is a reason
  the maintainer can weigh; "less clean" is not.
- **The recommendation was FIRST and marked**, and the user took it
  twice and overrode it twice -- which is the sign the options were real
  rather than one answer plus decoration.
- **The preamble said what Linux and Windows do BEFORE proposing**
  (Linux's `fs/fat/` is generic with the ESP an ordinary mount; Windows'
  ESP is a volume with no drive letter), then said which shape toy-os
  should copy and which size it should not.

**What the biggest-option answer then obliged**, and this is the part to
plan for: choosing "the full milestone" means the honest report has to
say which of the milestone's own listed items did NOT land, and why. One
did not (a second mount of ONE backend), and saying so plainly -- with
what it would take -- was more useful than quietly ticking eleven of
twelve boxes.
