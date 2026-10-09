# AML: a namespace this kernel can walk, staged

**Status: STAGES 0 AND 1 ARE BUILT (2026-08-31); STAGE 2 IS BUILT AS
FAR AS `_PRT` NEEDS IT (2026-09-10: integers, strings, buffers,
packages and NameStrings decoded and resolved, `kernel/acpi/aml_data.c`);
STAGE 3 IS DESIGNED, NOT BUILT.** The first consumer of stage 2 is PCI
interrupt routing (`kernel/acpi/acpi_prt.c`), which reads `_PRT` by its
measured shapes and evaluates nothing but one flag -- the line this
document draws holds; see `docs/decisions/drivers.md`. Two things the
walk got wrong were found by it: a `Scope` re-opens an existing object
rather than declaring a second one, and the namespace is built at boot
now, in `acpi_init()`. A table can be captured and checksum-verified
(`acpi --dump`, `tools/acpi_dump.py`), and `kernel/acpi/aml.c` walks the
DSDT and SSDTs into a namespace of declarations -- 336 nodes and 53
devices on QEMU's DSDT, with nothing refused. Nothing is executed and no
value is decoded yet.

**This reverses a decision.** `docs/decisions.md` says ACPI here "stops
at the tables" and "must not grow into" a subsystem, with the `_S5_`
byte scan as the single deliberate exception. That was the right call
when the only question was how to power the machine off. It stopped
being sufficient on 2026-08-31, when a laptop needed its GPE wake set
and the only place that is written down is a `_PRW` object per device.
The reversal is the maintainer's, made knowingly; this document is what
makes it a plan rather than a drift.

**Read this before adding anything that parses AML**, and note the line
this draws: **a namespace walk is not an interpreter.** Stages 0-3 read
DECLARATIONS and never execute a Method. Stage 4 is where a real
interpreter would begin, and it is deliberately not scheduled.

**The one-sentence version:** parse the DSDT and the SSDTs into a tree
of named objects, skipping every Method body by its length, so that
constant Names like `_PRW` can be found where they actually live --
and stop there, because everything past that point needs execution.

## What real systems do

**Linux** carries ACPICA, Intel's reference implementation: a full AML
interpreter with a namespace, method execution, an operation-region
subsystem that talks to the embedded controller, and a garbage
collector. It is roughly 60,000 lines and it is a vendored upstream
project, not something Linux maintains itself. **Windows** does the same
thing with its own interpreter in `acpi.sys`. **FreeBSD** vendors ACPICA
too. Nobody hand-writes this twice.

The lesson is not "vendor ACPICA" -- it is far past what this OS wants,
and its portability layer alone is larger than `kernel/acpi/`. The
lesson is **where the natural seam is**: ACPICA itself separates the
namespace (`nsxxx.c` -- building and searching the tree) from the
interpreter (`exxx.c` -- executing opcodes). The namespace half is the
part with a small, closed grammar. That is the half worth having.

**What toy-os should copy: the split.** What it should not copy: the
size, the operation regions, or the ambition. See the honest case at
the end.

## What exists today

`kernel/acpi/acpi_power.c` has ~60 lines of AML reading, all of it in
`acpi_scan_s5()`:

- `aml_small_int()` -- ZeroOp/OneOp/BytePrefix/WordPrefix into a byte.
- A PkgLength decode: `p += 1 + ((aml[p] >> 6) & 3)`.
- A byte scan for `NameOp '_S5_' PackageOp`, with the `\_S5_` variant.

That is a scan, not a walk: it looks for one signature anywhere in the
table and never learns what scope it was in. It works because `_S5_` is
unique and at the root. It is the right shape for exactly one object.

## The staging

Each stage ships and is testable on its own.

### Stage 0 -- `acpi --dump <TABLE>`, and a fixture  *(BUILT)*

Hex-dump a named ACPI table from inside toy-os, the way `lsusb -D`
dumps a USB configuration. Same argument, and it is the same argument
that was made for USB descriptors earlier the same day: **a parser for
firmware data must be tested against real firmware data, and a
hand-written fixture only ever agrees with the parser written beside
it.**

The kernel already has every table mapped and `QUERY_ACPI_TABLE` already
lists them; this is a dump provider over the bytes, and a `--dump`
flag in `/bin/acpi`. A real DSDT is ~28 KB, so it needs a range
(`--dump DSDT --at 0x1200 --len 256`) rather than emitting all of it.

**Payoff on its own:** every future ACPI question stops costing a round
trip through a second operating system.

**And a measurement that changes what stage 3 needs: the laptop's DSDT
is 103,006 bytes.** At 512 bytes a command that is 201 commands typed by
hand, so TRANSFERRING it is not the plan. The plan is that the parser
runs ON that machine and reports what it found -- `acpi --prw` listing
the GPE numbers it derived. QEMU's 8,605-byte DSDT is the development
fixture for the WALK; the laptop is the oracle for the WAKE SET, and it
answers in one line instead of 100 KB of hex.

### Stage 1 -- the namespace walk, declarations only  *(BUILT)*

Parse a table's AML term list into a tree. The grammar that must be
understood:

- **NameString**: RootChar `\`, ParentPrefixChar `^`, DualNamePath `2E`,
  MultiNamePath `2F`, NullName `00`, and the bare 4-byte NameSeg.
- **Containers**, which open a scope and whose bodies are walked:
  `DefScope` (10), `DefDevice` (5B 82), `DefPowerRes` (5B 84),
  `DefThermalZone` (5B 85), `DefProcessor` (5B 83, deprecated but
  present on older firmware).
- **`DefName` (08)**, which binds a name to a DataObject.
- **`DefMethod` (14)**, whose PkgLength is what lets its body be skipped
  entirely. That is the trick that keeps this a walk: a Method's body is
  opaque, and a parser that does not execute need not understand it.
- **The rest of what legally appears in a scope**, each of which needs
  its own length rule. **This is where the first draft of this document
  was WRONG**: it said "skipped by its PkgLength", and several of these
  do not have one. `DefOpRegion` (5B 80) is `NameString`, a space byte,
  then two TermArgs; `DefMutex` (5B 01), `DefEvent` (5B 02),
  `DefExternal` (15) and `DefAlias` (06) are a NameString plus a fixed
  trailer. Only the Field forms (5B 81/86/87) carry a PkgLength.
  Measured on QEMU's own DSDT, which has seven OperationRegions in its
  root scope, so this is not a corner case -- it is the second thing the
  parser meets.
- **Anything else: REFUSE.** Stop the walk and count it, rather than
  guess a length. The result is a partial namespace, which is a
  different answer from a wrong one.

The output is a tree of `(parent, 4-char segment, kind, offset)`. No
values are decoded at this stage beyond recording where the object's
data begins.

**The trap to write down now:** AML is UNTRUSTED INPUT, from firmware
this kernel did not write, parsed in ring 0. Every PkgLength, every
NameString and every offset is bounds-checked against the table's own
length, and a malformed table makes the walk STOP rather than guess --
the posture `ttf.c` takes for fonts and `usb_enum.c` for descriptors.
The failure mode must be "no namespace", never "a namespace built from
whatever followed".

**Testable on its own:** `acpi --namespace` prints the tree; a KTEST
walks a real DSDT captured in stage 0 and asserts the device count and
a few known paths.

### Stage 2 -- constant DataObjects

Decode the DataObject forms a `Name` can hold without execution:
integers (Zero/One/Byte/Word/DWord/QWord prefixes), Strings, Buffers
with constant lengths, and Packages of those. `aml_small_int()` grows
into this rather than being replaced.

**Not included:** anything whose value is an expression. `Name(X,
Add(1,2))` is legal AML and needs the interpreter; such an object is
recorded as PRESENT WITH AN UNREAD VALUE, which is a different answer
from absent and must not be conflated with it.

### Stage 3 -- `_PRW`, and the wake set

**MEASURED 2026-10-09, AND THE PREMISE BELOW DID NOT SURVIVE IT.** On the
machine it was for, all 17 `_PRW`s are Methods -- 12 `Return (GPRW (gpe,
state))`, 3 `Return (Package)`, 2 conditional -- so a reader of constant
Names finds none of them. And none wakes from S5, so the wake set this
stage would compute there is empty, which is what toy-os already does.
The two-press power button has another cause (`docs/bugs.md`).
`tools/aml_walk.py --prw` is the measurement. What follows is the
original plan, kept for the record.

**This stage is the actual fix, and there is now evidence that nothing
cheaper substitutes.** Three versions were measured on the machine that
needs it (`docs/decisions.md`): disable everything and it powers off but
takes two presses of the power button; restore the enables and it
reboots; rearm only the GPEs whose status stays clear and it ALSO
reboots, with nothing having re-latched. That last result is why this
stage is load-bearing -- **the waking source is not asserting when the
sleep is prepared**, so no measurement taken at one instant can find it.
Only a declared wake set can.

With stages 1 and 2, the wake set is a search: every Device with a
`_PRW` child, whose package's first element is an integer, contributes
that GPE bit. `acpi_poweroff()` then does what Linux does -- disable
all, clear all, enable only the wake set -- instead of restoring the
firmware's enables wholesale.

**This is the stage that pays for the project**, and it is worth being
honest that the payment is small. (This used to add that restoring the
firmware's enables had fixed the machine that prompted it. It had not:
restoring them made that machine reboot, as `docs/decisions.md`
records.) What stage 3 buys is the
removal of a residual risk (a non-wake GPE re-latching) and a wake set
that is *correct* rather than a superset.

The `_PRW` first element may also be a Package naming a GPE block
device and an index, on GPIO-signalled systems. Those are skipped and
COUNTED, so "we ignored three of them" is visible rather than silent.

### Stage 4 -- method execution (NOT SCHEDULED)

Everything above stops at declarations. Battery state, thermal zones,
lid state, `_STA`, `_CRS` and the embedded controller all need Methods
run, which needs an operand stack, integer/string/buffer semantics,
control flow, mutexes, and OperationRegion handlers for SystemMemory,
SystemIO and the EC. That is the 60,000-line project, and it is where
this document stops rather than pausing.

## Testing

- **A real DSDT is the fixture**, captured with stage 0 from the machine
  that needs the feature. **Measured, once stage 0 existed: QEMU's q35
  DSDT is 8605 bytes with 53 `Device` declarations and ZERO `_PRW`
  objects.** So it tests stage 1 well and cannot test stage 3 at all --
  the wake set needs the laptop's tables, and that is a fact about the
  emulator rather than a gap in the plan.
- `tools/acpi_dump.py` is the capture, and it verifies the ACPI checksum
  rather than trusting that the bytes arrived: asking for the whole
  table in one console command lost 48 of 8605 bytes, scattered.
- **The walk must be fuzzed against truncation**: for a real table,
  every prefix of it must either parse or stop cleanly, and none may
  read past the end. That is a cheap KTEST loop and it is the one that
  matters, because the input is firmware.
- **A positive control for stage 3**: with the wake set deliberately
  emptied, the machine that reported the bug must fail to stay off.

## The honest case against

**The payoff is thin and the surface is wide.** Stage 3 replaces a
working superset with a correct set, on a symptom already fixed. Every
line of stages 1-2 parses vendor firmware in ring 0, which is the same
class of attack surface `ttf.c` is, without `ttf.c`'s justification
(fonts are user-supplied and unavoidable; the DSDT is read once at
boot).

**And the next question after `_PRW` needs stage 4.** Battery, lid,
thermal -- the things a person actually wants from ACPI on a laptop --
are all Methods. So stages 1-3 do not open the door they look like they
open: they land one wake set and then stop at the same wall, one level
further in.

**The counter-argument, which is why this is scheduled anyway:** the
namespace is the half with a closed grammar and a bounded size, the
`_S5_` scan is already a worse version of it (a signature hunt with no
notion of scope), and a second such scan for `_PRW` would be the third
copy of a pattern that wants to be a parser. Building the small correct
thing once is cheaper than three scans that each work by luck.

## Out of scope, deliberately

- Vendoring ACPICA. Its portability layer alone is larger than
  `kernel/acpi/`.
- OperationRegion access of any kind, especially the embedded
  controller: that is a bus driver wearing an ACPI hat.
- Writing anything back to the namespace. This walk is read-only, and
  the moment it is not, every object needs a lock and a lifetime.
- `_OSI`, `_REV` and the OS-identification games firmware plays. They
  only matter to code that executes Methods.
