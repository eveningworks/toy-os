# screenshots/

**Closed as of 2026-08-15 -- nothing new goes in here.** The convention
of saving representative screenshots as testing evidence is retired, at
the maintainer's request: the artifacts were not being used, and
producing them slowed the testing loop down for no return.

What is already here stays -- it is a record of past testing passes and
costs nothing to keep. Do not add to it.

**Screenshots are still a testing tool**, and a good one: take as many
as a check needs, into a scratch directory outside the repo. What
changed is only that they stop being saved here afterwards. For
*evidence*, `docs/gui-guidelines.md` already asks for something better
-- pixel values with a control point (`tools/pixel_probe.py`) and a
pass/fail table from `tools/gui_regress.py`, both of which a reader can
check rather than interpret.

Show the user an image when SEEING it is the answer: a layout that has
to be looked at, a rendering question a number cannot settle. Not to
prove a check passed.

---

The original conventions, kept for reading the folders below:

QEMU screendumps kept as testing evidence, organized by when they were
taken -- one subfolder per calendar date a testing pass happened
(`screenshots/2026-08-09/`, `screenshots/2026-08-10/`, and so on).

Used to be one subfolder per `TOYOS_VERSION`: first a hand-bumped
`v0.1.0`-style number, then briefly a date-plus-build-counter string
that changed on literally every build, then a Windows-style build
number (`tools/bump_build.sh`) that changed once per real change.
`TOYOS_VERSION` is now a semver string with a `-dev` suffix
(`tools/set_version.sh`, see `docs/decisions.md`) that only changes
when starting a new dev round or cutting a release -- but dated
folders stayed the convention throughout all of these, since a single
change/testing pass often produces several screenshots before the
version string itself changes at all, and the date is still the grain
that naturally groups "one testing pass" together regardless of how
the version string is generated.

Why dated subfolders rather than one flat folder: screenshots are a
point-in-time record ("this is what the GUI looked like and did on
this date"), not a live asset that gets overwritten as the UI changes.
Keeping each date's screenshots together makes it possible to look back
at what changed visually over time, the same way `CHANGELOG.md` records
what changed functionally.

The `v0.2.0/` ... `v0.7.0/` folders still here are from that first
hand-bumped-version scheme, kept frozen as historical record: nothing
new goes into them, and they're deliberately not renamed into dated
folders (the date each was taken isn't recoverable from the files, so
a rename would be inventing one). Read them as "somewhere in that
version's development"; everything since is dated.

`readme/` is the one non-dated folder, and deliberately so: those two
images are embedded in `README.md` and are a *maintained asset* rather
than a point-in-time record -- when the desktop or the shell changes
enough that they misrepresent it, retake them in place. Everything else
here is evidence of what was true on a given day and is never updated
after the fact.

Convention for adding to this during a testing pass:
- Name files for what they show, not the order they were taken in --
  `calculator_7_plus_3.png`, not `shot16.png`. The generic `shot*.png`/
  `screendump*.png` names that pile up in a testing session are scratch
  and shouldn't make it in here as-is; rename to something a person
  skimming the folder can understand without opening the file.
- One subfolder per calendar date (`YYYY-MM-DD`). Create the next one
  the first time a testing pass happens on that date rather than
  pre-creating empty folders -- and reuse the same day's folder for
  every testing pass that happens that day, even across unrelated
  features.
- This is evidence a testing pass actually happened and what it found,
  not exhaustive coverage -- a handful of representative screenshots
  per pass (one per feature/area touched) is the right amount, not
  every intermediate screendump from the session.
- That last rule has drifted in practice: a day with several unrelated
  features lands several passes in one folder, and `2026-08-10/` ended
  up over a hundred files. The rule is per *pass*, not per folder, so a
  busy day's folder being large is fine -- but if you're adding to a
  folder that's already big, that's the moment to check your own pass
  is contributing representatives and not a full session dump. Pruning
  an old folder is a judgment call for the maintainer, not something a
  session should do on its own: these are a point-in-time record, and
  deleting one is not recoverable from the repo alone once committed.
