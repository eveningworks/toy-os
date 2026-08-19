# Decisions: Workflow and tooling

How this project is built, tested and delivered -- the dev tooling, the
test harnesses, and the conventions around them.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

## The GUI suite's wall clock is its slowest tool, so the fix was a timeout knob rather than parallelism

`gui_regress.py` runs 24 tools totalling ~425 tool-seconds. The obvious
levers were tried and measured, and two of the three did almost nothing:

| change | wall |
|---|---|
| `-j4` (as it was) | 76s |
| `-j8` on a 16-core host | 72s |
| `-j8` + KVM (`--kvm`) | 64s |
| shortening the slowest tool's timeout | **61s** |

Raising the job count bought four seconds and KVM another eight, because
the suite was never CPU-bound. Its slow tools sit WAITING on real
timeouts, and neither more cores nor a faster guest shortens a
wall-clock wait. `forcequit_test.py` alone waits out the WM's 3-second
not-responding timeout about ten times -- once per dialog it raises, six
of them in the repeat loop -- which pinned the whole suite at ~72s under
every configuration.

So the fix was `gui pingtimeout` (`userland/wm/wm_debug.c`), turning that
timeout down to 0.4s for that tool's run: 72s to 34s, and the suite to
61s. The new floor is `notepad` at 42s.

**Why a WM constant may be moved by a test at all.** The timeout's VALUE
is not what `forcequit_test.py` asserts -- the detection, the dialog and
force quit are, and the tool's own docstring already treated raising the
timeout as a positive control. Shortening it therefore loses no
coverage. It is a variable rather than a registered setting for two
reasons: it has no user-facing meaning worth a Control Panel row, and a
persisted value would silently change how the desktop treats a slow app
on every later boot. `gui watchdog <ms>` is the same shape and the
precedent.

Two rules ride with it, both of which would otherwise turn a speedup
into a silently weaker test. The tool must ASSERT the knob took --
without that, a missing or refused command leaves every wait shorter
than the timeout it is waiting for, and the whole tool fails in a way
that reads exactly like the feature being broken. And every NEGATIVE
check ("a hung app nobody is closing does not raise a dialog") must wait
a MULTIPLE of the timeout rather than a fixed number of seconds, so
shrinking it keeps the relationship those proofs depend on.

**The transferable half is the measurement, not the knob:** on a fan-out,
look at the maximum and never the sum. Parallelism and faster guests
both act on the sum, which is why both barely moved. The same session
also opened by asserting from memory that the suite took "about seven
minutes" when it took 76 seconds -- the difference was the session's own
polling, not the suite. Measure the baseline before optimising it.
