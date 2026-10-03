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


# `Milestone N` in PROSE is frozen per file (2026-09-03): a reference
# written when the number was current stays as history, resolved by the
# legend at the end of roadmap-details.md, but no file may gain one and
# no new file may start. A ratchet, not a fact: only ever lower it.
MILESTONE_PROSE_BASELINE = {
    "README.md": 1,
    "apps/README.md": 1,
    "data/wm/startup/README.md": 1,
    "kernel/README.md": 1,
    ".claude/skills/toy-os-feature-workflow/SKILL.md": 3,
    ".claude/skills/toy-os-feature-workflow/references/delivery-checklist.md": 1,
    ".claude/skills/toy-os-feature-workflow/references/doc-templates.md": 1,
    ".claude/skills/toy-os-feature-workflow/references/questions-that-worked.md": 2,
    ".claude/skills/toy-os-feature-workflow/references/session-design.md": 13,
    ".claude/skills/toy-os-feature-workflow/references/session-diagnosis.md": 1,
    ".claude/skills/toy-os-feature-workflow/references/session-gui.md": 2,
    ".claude/skills/toy-os-feature-workflow/references/session-testing.md": 1,
    "CLAUDE.md": 1,
    "docs/decisions.md": 2,
    "docs/decisions/build.md": 2,
    "docs/decisions/gui.md": 20,
    "docs/decisions/kernel.md": 18,
    "docs/decisions/shell.md": 4,
    "docs/decisions/storage.md": 9,
    "docs/filesystem-layout.md": 7,
    "docs/live-cd-design.md": 1,
    "docs/process-isolation.md": 3,
    "docs/roadmap-details.md": 10,
    "docs/roadmap.md": 2,
    "docs/testing.md": 2,
    "docs/tfs2-spec.md": 1,
    "docs/tfs3-design.md": 10,
    "docs/tfs3-spec.md": 2,
    "docs/tools.md": 4,
    "docs/uapp-design.md": 15,
    "docs/wm-ring3-design.md": 11,
}
MILESTONE_REF = re.compile(r"Milestone [0-9]+|\bM[0-9]{2}\b")


def check_milestone_prose_frozen(problems):
    for rel in tracked_files():
        if not rel.endswith(".md"):
            continue
        n = len(MILESTONE_REF.findall(read(rel)))
        b = MILESTONE_PROSE_BASELINE.get(rel, 0)
        if n > b:
            problems.append(f"{rel}: {n} `Milestone N` reference(s), frozen at {b} -- "
                            f"name the milestone (docs/roadmap.md) instead")


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
    # THE GENERATED "Next up" BLOCK IS EXEMPT. gen_next_up.py copies each
    # marked item and appends its section name, so a hand-written item
    # that fits can still fail here as its own generated copy -- which is
    # a rule punishing the wrong line, in a file the author cannot edit.
    # The cap is about what somebody types; a derived line's length is a
    # consequence of it. Both are still checked at the source item.
    generated = False
    for i, line in enumerate(lines, 1):
        if "BEGIN next-up" in line:
            generated = True
        elif "END next-up" in line:
            generated = False
        if generated:
            continue
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


def check_roadmap_details_headings_resolve(problems):
    """Every `###` in roadmap-details.md must name something that still
    exists: a roadmap SECTION, a roadmap ITEM, or a bugs.md entry.

    The pairing by TITLE is the whole mechanism -- CLAUDE.md sends a
    reader from an item to its long form by name, so a heading nobody
    can arrive at is detail that is written and never found. It drifts
    silently, because the two files are edited apart: an item gets
    reworded as the work progresses, or ticked and rephrased, and its
    heading keeps the old title. Eight had drifted when this check was
    written -- two of them section renames (`SMP` vs `SMP
    (multi-core)`), the rest items reworded in place.

    Matching is deliberately loose -- case, `~~`, `**`, and a trailing
    `-- DONE ...` are ignored, and a prefix counts -- because the rule
    is "a reader can find it", not "the strings are equal"."""
    import re as _re

    def norm(t):
        t = t.replace("~~", "").replace("**", "")
        t = _re.sub(r"\s+", " ", t).strip().lower()
        return _re.sub(r"\s*--\s*done\b.*$", "", t)

    targets = set()
    for rel in ("docs/roadmap.md", "docs/bugs.md"):
        for line in read(rel).split("\n"):
            if line.startswith("## ") or line.startswith("### "):
                targets.add(norm(line.lstrip("# ")))
            m = _re.match(r"^- \[[ x]\] (.+)$", line)
            if m:
                n = norm(m.group(1))
                targets.add(n)
                targets.add(n.split(" -- ")[0].strip())
    targets.discard("")

    for i, line in enumerate(read("docs/roadmap-details.md").split("\n"), 1):
        if not line.startswith("### "):
            continue
        n = norm(line[4:])
        if any(n == t or (len(t) > 25 and (n.startswith(t[:60]) or t.startswith(n[:60])))
               for t in targets):
            continue
        problems.append(f"docs/roadmap-details.md:{i}: heading matches no "
                        f"roadmap section, roadmap item or bugs.md entry -- "
                        f"rename it to match, or delete it\n      {line[:90]}")


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


def check_toolkit_index_is_current(problems):
    """docs/toolkit.md is generated from the ring-3 toolkit headers' top
    comments. Stale, it hides exactly the helper a session was about to
    rewrite -- the reason it exists (gen_toolkit_index.py)."""
    gen = os.path.join(REPO, "tools", "gen_toolkit_index.py")
    r = subprocess.run([sys.executable, gen, "--check"],
                       capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        problems.append("docs/toolkit.md is stale or a header has no top comment -- "
                        "run tools/gen_toolkit_index.py (" + r.stdout.strip().splitlines()[0] + ")")


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
    """Every tool in tools/ has an entry in docs/tools.md.

    A tool nobody documented is a tool the next session rewrites from
    scratch, which is the cost tools/ exists to avoid. This once asked
    CLAUDE.md as well, through a hand-kept list of every tool by runner;
    that list cost ~1k tokens of always-loaded context and `--list` on
    each runner is the live answer, so the reference is the one place
    now. It was the REFERENCE that went missing when only CLAUDE.md was
    checked (`multidisk_test.py`), which is why this is the half kept.

    Deliberately a NAME check and nothing more: it says the tool is
    mentioned, not that what is written about it is still true.
    """
    text = read("docs/tools.md")
    tools_dir = os.path.join(REPO, "tools")
    if not os.path.isdir(tools_dir):
        return
    for name in sorted(os.listdir(tools_dir)):
        if os.path.splitext(name)[1] not in (".py", ".sh"):
            continue
        if name not in text:
            problems.append(f"tools/{name} is not mentioned in docs/tools.md -- "
                            "add an entry; it is the full reference")


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


def check_every_driver_is_listed(problems):
    """Every DRIVER_DECLARE has a row in docs/devices.md.

    A hand-written inventory drifts -- LICENSE's did, twice, which is
    why check_licenses.py exists. This is the same guard for the driver
    list: it checks the NAME appears, not that what the page says about
    the driver is true, because nothing static can read a match table
    and know what hardware it means.
    """
    page = read("docs/devices.md")
    if page is None:
        problems.append("docs/devices.md is missing")
        return
    for rel in tracked_files():
        if not rel.startswith("kernel/") or not rel.endswith(".c"):
            continue
        body = read(rel)
        if body is None:
            continue
        for name in re.findall(r'DRIVER_DECLARE\(\s*"([^"]+)"', body):
            if f"`{name}`" not in page:
                problems.append(f"{rel}: driver `{name}` has no row in "
                                f"docs/devices.md")


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


# C string escapes that appear in a usage line. \n is the only one that
# matters in practice (a two-line synopsis), but a literal backslash has
# to be handled or the unescaping corrupts a Windows-ish path example.
def _unescape(text):
    return text.replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"')


def usage_string(body):
    """The program's usage text, from either form, or None.

    `cmd_usage("...")` directly, or `cmd_usage(NAME)` where NAME is a
    file-scope constant built from one or more adjacent literals:

        static const char *USAGE =
            "mount [-r] ... <mountpoint>\n"
            "       mount     list what is mounted";

    Adjacent literals are concatenated the way C does, so a synopsis
    split over lines compares as the one string a reader sees.
    """
    m = re.search(r'cmd_usage\(\s*"((?:[^"\\]|\\.)*)"', body)
    if m:
        return _unescape(m.group(1))

    m = re.search(r'cmd_usage\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)', body)
    if not m:
        return None
    name = m.group(1)

    # The constant's definition: a #define or a file-scope char pointer
    # or array, followed by one or more adjacent string literals.
    decl = re.search(
        r'(?:#\s*define\s+' + re.escape(name) + r'\b'
        r'|(?:static\s+)?const\s+char\s*\*?\s*' + re.escape(name) +
        r'(?:\s*\[\s*\])?\s*=)'
        r'((?:\s*"(?:[^"\\]|\\.)*")+)', body)
    if not decl:
        return None
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', decl.group(1))
    return _unescape("".join(parts))


def check_command_synopsis_matches(problems):
    """A page's Synopsis is the program's own cmd_usage() string.

    ONLY WHERE THE PROGRAM DECLARES ONE. Most /bin programs do; the rest
    take no arguments or print their own help, and there is nothing to
    compare against. Checking where it is possible is worth more than a
    rule that applies everywhere and verifies nothing.

    This is the anti-drift half of the folder: the PROSE on a page is
    what only a person can write and no script should police, while the
    SYNTAX is exactly what goes quietly wrong when a flag is added.

    **IT USED TO MATCH ONLY A STRING LITERAL, AND FOUR PROGRAMS ESCAPED
    IT SILENTLY** -- `mount`, `umount`, `grep` and `mkpart` all pass a
    named `USAGE` constant. `mount`'s source argument then grew a whole
    new form (a device name) with nothing comparing the page against the
    program, which is precisely what this check exists to catch. A
    skipped program looked identical to a program with no usage at all,
    so the gap could not be seen from the output either.

    Both forms are resolved now, including a constant built from several
    adjacent literals -- `mount`'s spans two lines, and taking only the
    first would have compared half a synopsis and called it a match.
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
        usage = usage_string(body)
        if usage is None:
            continue
        # A page carries the usage inside a Markdown code block, which
        # is indented four spaces on EVERY line. Compare against both
        # forms rather than stripping indentation away: keeping it exact
        # means a synopsis whose own alignment drifts is still caught,
        # which matters for the two-line ones.
        indented = "\n".join("    " + ln for ln in usage.split("\n"))
        text = open(page, encoding="utf-8").read()
        if usage not in text and indented not in text:
            problems.append(f"docs/commands/{name}.md does not carry the "
                             f"program's own usage line: {usage!r}")


def _literals(text):
    """C's adjacent string literals at the start of `text`, joined."""
    m = re.match(r'((?:\s*"(?:[^"\\]|\\.)*")+)', text)
    if not m:
        return None
    return _unescape("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))))


def uargs_table(body):
    """A program's lib/uargs.h table, or None: (name, usage forms, long
    options, short options, command names)."""
    prog = re.search(r'struct\s+uargs_prog\s+\w+\s*=\s*\{(.*?)\n\};', body, re.S)
    if not prog:
        return None
    block = prog.group(1)
    name = re.search(r'\.name\s*=\s*"([^"]+)"', block)
    usage = re.search(r'\.usage\s*=\s*(.*)', block, re.S)
    if not name or not usage:
        return None
    usage = _literals(usage.group(1)) or ""
    longs, shorts, cmds = [], [], []
    opts = re.search(r'struct\s+uargs_opt\s+\w+\[\]\s*=\s*\{(.*?)\n\};', body, re.S)
    if opts:
        for lng, sh in re.findall(r"\{\s*(?:\"([^\"]+)\"|0)\s*,\s*(?:'(.)'|0)\s*,", opts.group(1)):
            if lng:
                longs.append(lng)
            if sh:
                shorts.append(sh)
    table = re.search(r'struct\s+uargs_cmd\s+\w+\[\]\s*=\s*\{(.*?)\n\};', body, re.S)
    if table:
        cmds = re.findall(r'\{\s*"([^"]+)"\s*,', table.group(1))
    return name.group(1), usage.split("\n"), longs, shorts, cmds


def check_command_options_documented(problems):
    """A program built on lib/uargs.h declares its options and commands
    in a table; its page must carry the usage line(s) and name every one
    of them. The table is what -h/--help prints, so this is the page and
    the help agreeing -- the part a person adding a flag forgets."""
    pages_dir = os.path.join(REPO, "docs", "commands")
    bin_dir = os.path.join(REPO, "userland", "bin")
    if not os.path.isdir(pages_dir) or not os.path.isdir(bin_dir):
        return
    for src in sorted(os.listdir(bin_dir)):
        if not src.endswith(".c"):
            continue
        page = os.path.join(pages_dir, os.path.splitext(src)[0] + ".md")
        if not os.path.isfile(page):
            continue
        t = uargs_table(open(os.path.join(bin_dir, src), encoding="utf-8").read())
        if t is None:
            continue
        name, forms, longs, shorts, cmds = t
        text = open(page, encoding="utf-8").read()
        rel = f"docs/commands/{os.path.basename(page)}"
        for form in forms:
            if f"{name} {form}".rstrip() not in text:
                problems.append(f"{rel} does not carry the usage line {name} {form!r}")
        for lng in longs:
            if f"--{lng}" not in text:
                problems.append(f"{rel} does not mention --{lng}, which {name} accepts")
        for sh in shorts:
            if not re.search(r"(?<![\w-])-" + re.escape(sh) + r"(?![\w])", text):
                problems.append(f"{rel} does not mention -{sh}, which {name} accepts")
        for c in cmds:
            if f"`{c}" not in text and f"`{name} {c}" not in text:
                problems.append(f"{rel} does not name the command `{c}`, which {name} accepts")


def main():
    problems = []
    for check in (check_no_changelog_pointers,
                  check_roadmap_has_no_versions,
                  check_milestone_prose_frozen,
                  check_milestones_are_named,
                  check_no_duplicate_roadmap_entries,
                  check_roadmap_details_headings_resolve,
                  check_roadmap_items_are_one_line,
                  check_no_duplicated_sections,
                  check_decisions_index_is_current,
                  check_next_up_is_current,
                  check_toolkit_index_is_current,
                  check_internal_doc_links,
                  check_tools_are_documented,
                  check_every_driver_is_listed,
                  check_every_command_has_a_page,
                  check_commands_index_is_current,
                  check_command_synopsis_matches,
                  check_command_options_documented):
        check(problems)

    if not problems:
        print("check_docs: ok -- no dead changelog pointers, no numbered or "
              "versioned milestones, no duplicated roadmap entries, one line "
              "per roadmap item, the decisions index, Next up and the "
              "toolkit index are current, no broken "
              "doc links, every tool documented, every command has a page "
              "and a link, every driver listed")
        return 0

    print(f"check_docs: {len(problems)} problem(s)\n")
    for p in problems:
        print(f"  {p}")
    print("\nSee CLAUDE.md's \"prefer facts that cannot go stale\" rule.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
