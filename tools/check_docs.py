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


# The roadmap is ONE LINE PER ITEM (restructured 2026-08-18, from 2,939
# lines to ~750). Long enough that the ordering is readable at a glance
# is the entire point of it; the reasoning lives in roadmap-details.md.
ROADMAP_ITEM_MAX = 140


def check_roadmap_items_are_one_line(problems):
    """The roadmap grew to 2,939 lines by accumulating a paragraph per
    item, and nobody could read the build order out of it any more. The
    rule that replaced it -- one line per item, detail in
    roadmap-details.md -- is exactly the kind nobody remembers, so it is
    checked rather than stated: every convention this project has lost
    was one nothing verified."""
    text = read("docs/roadmap.md")
    lines = text.split("\n")
    for i, line in enumerate(lines, 1):
        if not re.match(r"^- \[[ x]\]", line):
            continue
        # A continuation line is an indented non-blank directly under an
        # item -- which is how a paragraph gets in.
        nxt = lines[i] if i < len(lines) else ""
        if nxt.startswith("  ") and nxt.strip():
            problems.append(f"docs/roadmap.md:{i}: roadmap item wraps onto a "
                            f"second line -- move the detail to "
                            f"roadmap-details.md\n      {line.strip()[:90]}")
        elif len(line) > ROADMAP_ITEM_MAX:
            problems.append(f"docs/roadmap.md:{i}: roadmap item is "
                            f"{len(line)} chars (max {ROADMAP_ITEM_MAX})"
                            f"\n      {line.strip()[:90]}")


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


# Docs big enough that a bulk edit is done with a script rather than by
# hand -- which is where the duplication risk lives.
BULK_EDITED = ("CLAUDE.md", "docs/roadmap.md", "docs/roadmap-details.md",
               "docs/decisions.md", "README.md", "apps/README.md",
               ".claude/skills/toy-os-feature-workflow/SKILL.md")


def check_no_duplicated_sections(problems):
    """A heading appearing twice in one document.

    This exists because it happened: an edit to CLAUDE.md used an end
    anchor that occurred EARLIER in the file than its start anchor, so
    `s[:start] + new + s[end:]` re-appended everything between them --
    2,673 lines, silently, and the file went from 3,094 lines to 5,789
    in a commit whose diff nobody could read at that size. Every heading
    in the second copy was a duplicate and nothing noticed.

    Headings are the cheap signal: a document with the same `##` twice
    is either duplicated or badly organised, and both want fixing."""
    for rel in BULK_EDITED:
        path = os.path.join(REPO, rel)
        if not os.path.exists(path):
            continue
        seen = {}
        for line in read(rel).split("\n"):
            if re.match(r"^#{2,3} \S", line):
                seen[line.strip()] = seen.get(line.strip(), 0) + 1
        for line, n in seen.items():
            if n > 1:
                problems.append(f"{rel}: heading appears {n} times -- a bulk "
                                f"edit may have duplicated a region"
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


def check_next_up_is_current(problems):
    """docs/roadmap.md's "Next up" section is generated from the `**NEXT**`
    markers on the items themselves. A stale section is exactly what the
    generator exists to prevent: urgency claimed in one place and the
    work living in another, drifting the moment an item is reworded or
    ticked."""
    gen = os.path.join(REPO, "tools", "gen_next_up.py")
    r = subprocess.run([sys.executable, gen, "--check"],
                       capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        problems.append("docs/roadmap.md's Next up section is stale -- run "
                        "tools/gen_next_up.py --write")


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


def check_tools_are_documented(problems):
    """Every tool in tools/ is named somewhere in CLAUDE.md.

    CLAUDE.md already states the rule -- "any genuinely reusable tooling
    built during a session belongs in tools/... update the files that
    describe tools/ to match" -- and until now nothing enforced it. A
    tool nobody documented is a tool the next session rewrites from
    scratch, which is the exact cost the tools/ directory exists to
    avoid.

    Deliberately a NAME check and nothing more: it says the tool is
    mentioned, not that what is written about it is still true. That is
    the honest limit of what a script can tell.
    """
    claude = read("CLAUDE.md")
    tools_dir = os.path.join(REPO, "tools")
    if not os.path.isdir(tools_dir):
        return
    for name in sorted(os.listdir(tools_dir)):
        if os.path.splitext(name)[1] not in (".py", ".sh"):
            continue
        if name not in claude:
            problems.append(f"tools/{name} is not mentioned in CLAUDE.md -- "
                             f"add it to the `## tools/` listing")


# Commands that are real but are not something a person types at a
# prompt, so a page for them would document the wrong thing. Each line
# is a reason, not an apology.
COMMAND_PAGE_EXEMPT = {
    "init":  "pid 1, spawned by the kernel -- nobody runs it",
    "hello": "the smallest possible ELF, a loader fixture",
    "tosh":  "a shell, not a command -- docs/conventions/shell.md covers it",
    "gui3":  "an alias for `gui`, kept so older notes still resolve",
    "nano":  "an alias for `edit`",
}


def shell_commands():
    """Every command a person can type: /bin programs and shell builtins.

    The /bin list comes from the SEED TREE rather than from
    userland/bin/*.c, because the Makefile renames three of them on the
    way in (tests/echo -> echo_test and friends) and the name on disk is
    the name people type. The builtins come from dispatch() itself,
    which is the only authority on what the shell handles.

    THERE ARE TWO SHELLS AND BOTH ARE ASKED. This used to read only
    apps/shell.c, which was right when the kernel shell was the only one
    with builtins -- but /bin/tosh has its own, and the ones that are
    NOT also kernel builtins were invisible to this check. `jobs` and
    `fg` were the first two, and they are exactly the shape of thing
    that ships undocumented: a builtin whose page nothing demands.
    """
    names = set()
    seed_bin = os.path.join(REPO, "seed", "sync", "bin")
    if os.path.isdir(seed_bin):
        for n in os.listdir(seed_bin):
            if os.path.isfile(os.path.join(seed_bin, n)):
                names.add(n)
    for m in re.finditer(r'k_strcmp\(cmd, "([a-z0-9_]+)"\)', read("apps/shell.c")):
        names.add(m.group(1))
    for m in re.finditer(r'seq\(cmd, "([a-z0-9_]+)"\)', read("userland/lib/tosh.c")):
        names.add(m.group(1))
    return names


def check_every_command_has_a_page(problems):
    """Every command has a page in docs/commands/, and every page a command.

    THE HALF THAT MATTERS IS THE FIRST ONE. A command that ships with no
    documentation is not discovered by anybody reading the docs -- it is
    discovered by somebody typing `help` and finding a name nothing
    explains. docs/roadmap.md has wanted "a check that every builtin
    actually has a page" since the man-pages milestone was written.

    The second half catches the opposite drift: a page for a command
    that has been deleted or renamed, which is worse than no page,
    because it reads as current.

    It needs the SEED TREE to exist (`make iso`), which preflight always
    runs first. Skipped rather than failed when it does not, so a bare
    `check_docs.py` in a fresh checkout still does its other work.
    """
    seed_bin = os.path.join(REPO, "seed", "sync", "bin")
    if not os.path.isdir(seed_bin):
        return
    pages_dir = os.path.join(REPO, "docs", "commands")
    if not os.path.isdir(pages_dir):
        problems.append("docs/commands/ does not exist -- one page per command")
        return
    pages = {os.path.splitext(n)[0] for n in os.listdir(pages_dir)
             if n.endswith(".md")}
    commands = shell_commands()

    for name in sorted(commands - pages):
        if name in COMMAND_PAGE_EXEMPT:
            continue
        problems.append(f"`{name}` has no docs/commands/{name}.md")
    for name in sorted(pages - commands - {"README"}):
        problems.append(f"docs/commands/{name}.md documents nothing that "
                         f"exists -- renamed or deleted?")


def check_commands_index_is_current(problems):
    """docs/commands/README.md's index matches the pages on disk.

    The coverage check above refuses a command with no page; this
    refuses a page nobody can FIND. Both halves are needed -- a page
    that exists and is unlinked is documentation only somebody who
    already knew about it will read.
    """
    gen = os.path.join(REPO, "tools", "gen_commands_index.py")
    if not os.path.isfile(gen):
        return
    r = subprocess.run([sys.executable, gen, "--check"],
                        capture_output=True, text=True)
    if r.returncode != 0:
        problems.append((r.stdout or r.stderr).strip() or
                         "docs/commands/README.md is stale -- run "
                         "tools/gen_commands_index.py")


def check_command_synopsis_matches(problems):
    """A page's Synopsis is the program's own cmd_usage() string.

    ONLY WHERE THE PROGRAM DECLARES ONE. Sixteen of the /bin programs
    call cmd_usage() with a literal; the rest take no arguments or print
    their own help, and there is nothing to compare against. Checking
    where it is possible is worth more than a rule that applies
    everywhere and verifies nothing.

    This is the anti-drift half of the folder: the PROSE on a page is
    what only a person can write and no script should police, while the
    SYNTAX is exactly what goes quietly wrong when a flag is added.
    """
    pages_dir = os.path.join(REPO, "docs", "commands")
    if not os.path.isdir(pages_dir):
        return
    bin_dir = os.path.join(REPO, "userland", "bin")
    if not os.path.isdir(bin_dir):
        return
    for src in sorted(os.listdir(bin_dir)):
        if not src.endswith(".c"):
            continue
        name = os.path.splitext(src)[0]
        page = os.path.join(pages_dir, name + ".md")
        if not os.path.isfile(page):
            continue  # the coverage check above already said so
        body = open(os.path.join(bin_dir, src), encoding="utf-8").read()
        m = re.search(r'cmd_usage\("([^"]*)"', body)
        if not m:
            continue
        usage = m.group(1)
        if usage not in open(page, encoding="utf-8").read():
            problems.append(f"docs/commands/{name}.md does not carry the "
                             f"program's own usage line: {usage!r}")


def main():
    problems = []
    for check in (check_no_changelog_pointers,
                  check_roadmap_has_no_versions,
                  check_milestones_are_named,
                  check_no_duplicate_roadmap_entries,
                  check_roadmap_items_are_one_line,
                  check_no_duplicated_sections,
                  check_decisions_index_is_current,
                  check_next_up_is_current,
                  check_internal_doc_links,
                  check_tools_are_documented,
                  check_every_command_has_a_page,
                  check_commands_index_is_current,
                  check_command_synopsis_matches):
        check(problems)

    if not problems:
        print("check_docs: ok -- no dead changelog pointers, no numbered or "
              "versioned milestones, no duplicated roadmap entries, one line "
              "per roadmap item, the decisions index and Next up are "
              "current, no broken "
              "doc links, every tool documented, every command has a page "
              "and a link")
        return 0

    print(f"check_docs: {len(problems)} problem(s)\n")
    for p in problems:
        print(f"  {p}")
    print("\nSee CLAUDE.md's \"prefer facts that cannot go stale\" rule.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
