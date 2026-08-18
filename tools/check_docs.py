#!/usr/bin/env python3
"""tools/check_docs.py -- the documentation rules that rotted before.

WHY THIS EXISTS
---------------
Every documentation convention this repo has abandoned was abandoned for
the same reason: it needed a human to keep a *number* true, and nobody
did. Build numbers indexed a changelog. Milestone numbers needed three
renumberings and a translation table. Target versions predicted releases
nobody had committed to. Each rotted quietly, because a stale pointer
reads exactly like a live one.

The rules that replaced them are in CLAUDE.md ("prefer facts that cannot
go stale"). This checks the handful of them a script can check, so they
do not rot the same way. It is deliberately NARROW: every check below is
for something definitively dead or definitively malformed, never for
style. A guard that false-alarms is a guard people switch off.

WHAT IT DOES NOT CHECK, on purpose
----------------------------------
`Milestone N` references in prose. They are dead numbers, but they
appear throughout `docs/decisions.md` and in source comments as
HISTORICAL references, and `docs/roadmap-details.md` ends with a legend
for resolving them. Flagging those would be noise, and the noise would
be what makes someone stop running this.

Run it directly, or let `tools/preflight.sh` and CI run it.
"""

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files allowed to say "CHANGELOG": the ones that exist to explain that
# there is no changelog. Anything else naming it is a pointer into a
# file that was deleted on 2026-08-18.
CHANGELOG_OK = {
    "CLAUDE.md",
    "tools/set_version.sh",
    "tools/check_docs.py",
    ".claude/skills/toy-os-feature-workflow/SKILL.md",
    ".claude/skills/toy-os-feature-workflow/references/delivery-checklist.md",
    ".claude/skills/toy-os-feature-workflow/references/doc-templates.md",
    "docs/decisions.md",
}

TEXT_EXT = {".md", ".c", ".h", ".py", ".sh", ".yml", ".json", ".ld", ".asm"}


def tracked_files():
    out = subprocess.check_output(["git", "ls-files"], cwd=REPO, text=True)
    for rel in out.split("\n"):
        if not rel:
            continue
        if os.path.splitext(rel)[1] in TEXT_EXT:
            yield rel


def read(rel):
    with open(os.path.join(REPO, rel), errors="replace") as f:
        return f.read()


def check_no_changelog_pointers(problems):
    """The four changelog files are gone; pointers into them are dead."""
    for rel in tracked_files():
        if rel in CHANGELOG_OK:
            continue
        for i, line in enumerate(read(rel).split("\n"), 1):
            if "CHANGELOG" in line:
                problems.append(
                    f"{rel}:{i}: names CHANGELOG, which was deleted -- point at "
                    f"the commit instead\n      {line.strip()[:90]}")


def check_roadmap_has_no_versions(problems):
    """Target versions were removed: the maintainer picks at release time."""
    rel = "docs/roadmap.md"
    for i, line in enumerate(read(rel).split("\n"), 1):
        if line.startswith("### ") and re.search(r"target v[0-9]", line):
            problems.append(f"{rel}:{i}: milestone heading carries a target "
                            f"version\n      {line.strip()[:90]}")


def check_milestones_are_named(problems):
    """Milestones are titles, not numbers -- that is what stopped the
    renumbering treadmill. A heading that reintroduces a number starts
    it again."""
    for rel in ("docs/roadmap.md", "docs/roadmap-details.md"):
        for i, line in enumerate(read(rel).split("\n"), 1):
            if re.match(r"^#+ (~~)?Milestone \d", line):
                problems.append(f"{rel}:{i}: milestone heading is numbered -- "
                                f"milestones are named\n      {line.strip()[:90]}")


def check_no_duplicate_roadmap_entries(problems):
    """Two of this repo's own roadmap edits duplicated an entry and one
    deleted three, all silently: nothing tests documentation, and a
    slice-replacement that lands on the wrong boundary looks fine in a
    diff nobody reads closely."""
    for rel in ("docs/roadmap.md", "docs/roadmap-details.md"):
        text = read(rel)
        for kind, pattern in (("heading", r"^#{2,3} .+$"),
                              ("checkbox", r"^\s*- \[[ x]\] \*\*.+$")):
            seen = {}
            for i, line in enumerate(re.findall(pattern, text, re.M), 1):
                seen.setdefault(line.strip(), []).append(i)
            for line, hits in seen.items():
                if len(hits) > 1:
                    problems.append(f"{rel}: duplicated {kind} x{len(hits)}"
                                    f"\n      {line[:90]}")


def check_decisions_index_is_current(problems):
    """docs/decisions.md is generated from docs/decisions/. A stale index
    is the failure the generator exists to prevent -- an entry added to a
    file and never linked is invisible, which is how the hand-maintained
    version quietly stopped being an index."""
    gen = os.path.join(REPO, "tools", "gen_decisions_index.py")
    r = subprocess.run([sys.executable, gen, "--check"],
                       capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        problems.append("docs/decisions.md is stale -- run "
                        "tools/gen_decisions_index.py")


def check_internal_doc_links(problems):
    """A relative link from one doc to another that does not exist. The
    roadmap split produced exactly this: a pointer to a `## Details`
    section that had moved to its own file."""
    for rel in tracked_files():
        if not rel.endswith(".md"):
            continue
        base = os.path.dirname(os.path.join(REPO, rel))
        for i, line in enumerate(read(rel).split("\n"), 1):
            for target in re.findall(r"\]\(([^)#:]+\.md)[)#]", line):
                if not os.path.exists(os.path.join(base, target)):
                    problems.append(f"{rel}:{i}: link to a file that does not "
                                    f"exist: {target}")


def main():
    problems = []
    for check in (check_no_changelog_pointers,
                  check_roadmap_has_no_versions,
                  check_milestones_are_named,
                  check_no_duplicate_roadmap_entries,
                  check_decisions_index_is_current,
                  check_internal_doc_links):
        check(problems)

    if not problems:
        print("check_docs: ok -- no dead changelog pointers, no numbered or "
              "versioned milestones, no duplicated roadmap entries, the "
              "decisions index is current, no broken doc links")
        return 0

    print(f"check_docs: {len(problems)} problem(s)\n")
    for p in problems:
        print(f"  {p}")
    print("\nSee CLAUDE.md's \"prefer facts that cannot go stale\" rule.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
