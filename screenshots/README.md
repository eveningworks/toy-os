# screenshots/

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
