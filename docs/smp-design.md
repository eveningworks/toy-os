# SMP: more than one core, staged

**Status: STAGE 1 IS BUILT (2026-08-30); STAGE 2 IS BUILT (the Local
APIC 2026-08-30, its TIMER 2026-09-06) EXCEPT THE I/O APIC; STAGES 3-7
ARE DESIGNED, NOT BUILT.** The ACPI table walk and the MADT's processor
list exist (`kernel/acpi/`, read by `/bin/acpi` and `/bin/lscpu`), the
Local APIC is enabled and carries MSI-X for seven drivers, and the tick
now runs on the LAPIC timer through `kernel/clockevent.h` -- but nothing
starts a second core, and every MADT entry still reports `online: no`.
What stage 2 still lacks is the I/O APIC and the MADT's interrupt source
overrides; legacy lines are still delivered by the 8259 through the
LAPIC's LINT0 in virtual wire mode. The document is meant to be executed
in order, each stage shippable and testable on its own, with the honest
case against at the end.

**Stage 1 landed for a different reason than SMP**, which is worth
knowing before reading it as progress: shutdown needed the FADT, so the
table walk had to exist, and parsing the MADT beside it cost one file.
That is the whole of it. The hard part of this document begins at
stage 2.

**Read this before touching `kernel/arch/x86_64/irq.c`, the scheduler's
`current_index`, or anything that adds a module-level buffer to a
subsystem a syscall can reach.**

**The one-sentence version:** toy-os should discover its other cores
through ACPI, start them, give each one its own GDT/TSS/stack and a
per-CPU pointer to what it is running, and let them all run user code in
parallel behind a SINGLE kernel lock -- then split that lock subsystem
by subsystem, in the order the measurements below give.

## What has just landed, and why it matters here

Threads (`docs/conventions/kernel.md`, "A THREAD IS A SLOT WHOSE `tgid`
NAMES SOMEBODY ELSE") are the prerequisite this project already paid
for. Not because SMP needs threads -- it does not -- but because the
scheduler entity is now the right shape: a slot is a schedulable thing
with its own kernel stack, FP state and trapframe, and a process is a
group of them sharing an address space. That is exactly the object a
per-CPU run queue holds.

It also creates the first real reason to want SMP in this system: two
threads of one program can now be runnable at the same instant, and on
one core that is a time slice each.

And it creates the first TLB shootdown hazard. Before threads, one
address space was live on one CPU by construction; now two cores can be
in the same PML4, so an unmap on one must be seen by the other. See
stage 6.

## What real systems do

**Linux 2.0 (1996) shipped SMP behind a single Big Kernel Lock.** One
CPU inside the kernel at a time; user code ran in parallel, everything
else serialised. It worked, it scaled badly, and removing it took
fifteen years -- the last `lock_kernel()` came out in 2.6.39 (2011).
The interesting part is not that they regretted it: it is that the BKL
was what made SMP *shippable* before the locking audit was done, and the
split then happened subsystem by subsystem against real measurements
rather than guesses.

**Windows NT was SMP from 3.1 (1993)**, designed rather than retrofitted:
per-CPU `KPRCB`, IRQL as a per-CPU interrupt-masking level, spinlocks
with a defined acquisition order, and a dispatcher lock. NT never had a
BKL because it never had a uniprocessor kernel to convert.

**Both discover CPUs the same way** -- the ACPI MADT's Local APIC
entries -- and start them the same way, with the INIT-SIPI-SIPI sequence
into a real-mode trampoline. Linux's is `arch/x86/realmode/rm/trampoline_64.S`.
There is no alternative shape worth considering here; this part is
architecture, not design.

**Where toy-os sits:** structurally it is pre-2.0 Linux. A preemptive
uniprocessor kernel with no spinlock primitive, whose only mutual
exclusion is "turn preemption off". So the BKL path is not an
imitation of a mistake -- it is the same position Linux was in, and the
same move is available.

## What is single-core here today, measured

Not a survey of everything that could be wrong. These are the things
counted in the tree on 2026-08-26.

| | today | why SMP breaks it |
|---|---|---|
| GDT, TSS, `kernel_stack0` | one of each (`kernel/arch/x86_64/gdt.c`) | `TSS.RSP0` is per CPU: two cores sharing one would land two ring-3 traps on one stack |
| IDT | one, loaded once | shareable as-is -- the IDT is read-only to the CPU. Each core must still `lidt` it |
| interrupt controller | 8259 PIC, 16 lines, handler chain per line (`irq.c`, 75 lines) | the PIC delivers to one CPU. MSI and IPIs both need a Local APIC, which this kernel has none of |
| timer | LAPIC timer at 100 Hz, PIT as fallback (2026-09-06) | this row is DONE for the BSP -- `kernel/clockevent.h`. Each AP still needs its own, which is what `per_cpu` on the device marks |
| "what is running" | `current_index`, `rotation_pos`, `g_next_kernel_rsp` -- three file-scope globals | each has to become per CPU. `g_next_kernel_rsp` is read by `isr_common`'s epilogue in assembly |
| run queue | one `procs[64]` array scanned by `find_next_runnable()` | correct under a lock; a per-core queue is a later optimisation, not a correctness fix |
| mutual exclusion | `scheduler_preempt_disable()`, 11 call sites outside tests, in 4 files | turning preemption off on ONE core stops nothing on another. Every one of these is a critical section that needs a real lock |
| filesystem | `tfs3.c` parses through module-level scratch (`g_blk`, `g_ptr_blk`, `g_bbm`, the journal image) | two cores in `fs_read()` overwrite each other's block. This is the bug the preemption guard already exists for, minus the guard |
| kernel heap | one free list (`g_head`/`g_tail` in `heap_core.c`) | two concurrent `kmalloc`s corrupt the list |
| console | `vga.c`'s cursor and back buffer | interleaved output, and a torn present |
| ACPI | tables and the MADT (`kernel/acpi/`, 2026-08-30) | this row is now DONE -- the core count is a fact, and `/bin/acpi` prints it |

**The RSDP is already reachable**, which is what made stage 1 cheap:
GRUB passes it in multiboot2 tags 14/15, and `kernel/core/multiboot.c`
already walked that list for the framebuffer, the command line, the
memory map and the modules. Finding the MADT needs no AML interpreter --
the tables that matter here are fixed-layout structures, and the
interpreter is only needed for the parts of ACPI this project has said
it does not want. That prediction held: `kernel/acpi/` is four files
and no interpreter.

## The staging

Each stage boots and is testable on its own. Stages 1-3 change no
behaviour at all -- they add facts and then add cores that do nothing --
which is what makes them safe to land separately.

### Stage 1 -- ACPI tables, read-only -- DONE 2026-08-30

Find the RSDP from the multiboot2 tag, validate its checksum, walk the
XSDT (or RSDT on an older table), and expose the MADT. Nothing acts on
it.

**What shipped**, and where it differs from the sketch above. The RSDP
is found from tag 15, then tag 14, then by scanning the EBDA and the
BIOS ROM area -- the scan was not planned and exists because "no tag"
and "no ACPI" are otherwise the same answer and want opposite responses.
It is a fact and a command as intended, but three classes rather than
one: `QUERY_CPUS` for the processors, plus `QUERY_ACPI` and
`QUERY_ACPI_TABLE`, because poweroff needed the FADT read anyway and the
numbers it depends on had to be visible when a shutdown fails. The
command is `/bin/acpi`, not `cpuinfo`; `lscpu` grew the one line that
counts cores, since CPUID describes only the core executing it.

The FADT came in too, which the note below said to avoid. That was not
scope creep in this direction -- shutdown needed it (`docs/decisions.md`,
"ACPI stops at the tables"), and the table walk it needed is this stage.
**HPET is still untouched**: its table is listed now and nothing reads
it, which is exactly where that roadmap item wanted to start.

**What this must NOT become:** an ACPI subsystem. AML is not wanted at
all -- the DSDT's `_S5_` byte scan is the single, argued exception, and
anything requiring AML to be EVALUATED (battery, thermal, S3, GPEs) is
out. Parse fixed-layout tables and stop.

**Two things stage 2 inherits.** The MADT walk already reads the type-5
Local APIC address override, so `acpi_get_state()->lapic_phys` is the
right address rather than the 32-bit field's. It does NOT yet read the
type-2 interrupt source overrides, which stage 2's own trap note is
about -- that is a loop body in `acpi_madt_init()`, not new machinery.

### Stage 2 -- the Local APIC, still one core -- PARTLY DONE

Enable the BSP's LAPIC, move the timer off the PIT and onto the LAPIC
timer, and keep the PIC path working for a machine that reports no APIC.
Route the legacy IRQs through the I/O APIC using the MADT's interrupt
source overrides.

**The LAPIC itself landed 2026-08-30** and brought MSI/MSI-X with it,
which this section did not anticipate as part of the stage -- it framed
MSI-X as a later beneficiary. **The TIMER landed 2026-09-06**, behind a
`clockevent` registry rather than the bare switch sketched here: two
implementations exist (`pit`, `lapic-timer`), the higher-rated one takes
the tick at boot, and `nomsi` or a CPU with no APIC keeps the PIT. That
registry is the shape stage 3 wants, since a per-CPU timer is one
clockevent per core. The calibration is against the PIT, which is why it
happens in `kernel_main()` and not in `lapic_init()` -- it needs
interrupts already on.

**The I/O APIC landed 2026-09-10 and this stage is DONE.**
`acpi_madt_init()` records the type-1 controllers and every type-2
override, `kernel/arch/x86_64/ioapic.c` programs the redirection table
and `irq.c` migrates every unmasked line off the 8259 (virtual wire
mode retired, LINT0 closed), and PCI INTx pins are routed by the
firmware's `_PRT` (`kernel/acpi/acpi_prt.c`) -- see
`docs/conventions/kernel.md` and `docs/decisions/drivers.md`.

This is worth landing alone even if SMP stops here: it is what MSI-X
needs (`docs/roadmap.md`'s Local APIC item), it gives more than 16
vectors, and a per-core timer is the prerequisite for anything tickless.

**The trap this stage carries:** an interrupt source override says the
ISA IRQ number is not the GSI. Ignoring them works on QEMU's default
machine and fails on real hardware and on some QEMU machine types --
which is exactly the class `tools/qemu_matrix.py` exists to catch.

### Stage 3 -- application processors, parked

INIT-SIPI-SIPI each AP into a trampoline below 1 MiB that walks it from
real mode to long mode and into a C entry point. Each AP gets its own
GDT, its own TSS with its own `RSP0` stack, loads the shared IDT, enables
its LAPIC, and halts in a loop.

**Per-CPU state arrives here, and the mechanism is `swapgs` + GS.base.**
Each core's GS.base points at its own `struct cpu`, which holds at
minimum: the APIC id, the current thread, the per-CPU idle context, and
the depth of whatever lock it holds. This is what `current_index` becomes.
Linux's `this_cpu_ptr`, NT's `KPRCB`. `swapgs` on kernel entry and exit
is the part that must not be got wrong: it is why the kernel entry path,
not the scheduler, is the first thing to change in this stage.

`cpuinfo` now reports the APs as online, which is the whole test.

### Stage 4 -- a real spinlock, and the kernel lock

Two things, in this order.

**A spinlock primitive.** A ticket lock, not a test-and-set: a
test-and-set lock is unfair and can starve a waiter indefinitely, which
under a hypervisor turns into a several-millisecond stall. It must use
`cpu_relax()` in its spin (`kernel/include/kernel/barrier.h` already
says why, and the reason is the same: KVM's Pause-Loop Exiting). It must
also record the holder for a diagnostic, because the first SMP deadlock
is otherwise a machine that simply stops.

**One lock around the kernel.** Taken on every entry from ring 3 --
syscall, fault, interrupt -- and released on the way out. User code runs
on every core in parallel; kernel code does not.

The rule that makes this safe is the one it is easy to get wrong: **an
interrupt handler must take the same lock**, or an IRQ on core B walks
into a data structure core A is halfway through. That means interrupts
must be disabled while the lock is held on the same core (or the handler
deadlocks against itself), which is exactly the `spin_lock_irqsave`
discipline Linux and NT's IRQL both encode.

### Stage 5 -- the scheduler picks per core

`find_next_runnable()` becomes "find a runnable slot no other core is
running", and `switch_to()` writes the per-CPU `current` rather than a
global. One global run queue under the kernel lock is the right first
version: a per-core queue is a scalability optimisation and it brings
load balancing, work stealing and affinity with it -- three problems
this system does not have yet.

**The kernel's own rotation participant (`ROT_KERNEL`) is the awkward
part.** It exists because the kernel context is a schedulable thing here.
With N cores it becomes the per-core idle loop, which is what
`scheduler_idle()` already is -- but "the kernel context" as a single
saved trapframe (`kernel_saved_rsp`) is a uniprocessor idea and has to
become per CPU.

### Stage 6 -- TLB shootdown

Once two cores can be in one address space -- which threads make
routine -- unmapping a page needs every core that has it cached to
invalidate it. An IPI to the cores running that address space, and a
wait for their acknowledgement, before the frame is freed.

The paths that unmap today: `vmm_unmap_user_page()` (window revocation),
address-space teardown, and the poisoning of a dead client's mapping.
Teardown is safe by construction (the address space has no runners
left); the other two are not.

### Stage 7 -- split the lock, in measured order

The order comes from the table above, and each split is its own change
with its own test:

1. **The kernel heap** -- one lock inside `heap_core.c`. Every subsystem
   allocates, so this is the most contended thing in the kernel.
2. **The filesystem** -- and this one is not a lock, it is the scratch
   buffers. `tfs3.c`'s module-level state is what
   `scheduler_preempt_disable()` was papering over; the honest fix is
   per-call state, and a lock over the backend is the interim.
3. **The console** -- a lock around `vga.c`'s present and cursor.
4. **The scheduler's own table**, which is what lets a core reschedule
   without holding the kernel lock.
5. Everything else, when something measures it.

**Do not skip the measurement.** The BKL is not a bug to be ashamed of;
splitting a lock nothing contends for adds a race surface and buys
nothing.

## Testing, and the part that is genuinely hard

**Every automated test in this repo runs TCG, and TCG serialises.**
`qemu-system-x86_64 -smp 4` under TCG runs the cores round-robin in one
host thread, so a missing lock produces a green suite. This is the same
gap `kernel/include/kernel/barrier.h` already documents for memory
barriers, and it is worse here: the whole point of the work is
concurrency.

So the SMP suite has to be a KVM suite. `tools/kvm_soak.py` is the
existing shape to extend, and the stages have very different
testability:

- Stages 1-3 are testable under TCG, because "how many cores are online"
  is a fact, not a race. `cpuinfo` reporting four online cores under
  `-smp 4` is a real check.
- Stages 4-7 are only meaningfully testable under KVM with `-smp`, under
  load, for minutes -- and the honest statement is that a soak test
  finding nothing is weak evidence. The strong evidence is a lock
  discipline that is stated and checked by inspection, plus an assertion
  in the lock itself that catches a recursive acquire.
- **A positive control is mandatory here and unusually easy**: remove
  one lock and the soak must fail. If it does not, the soak is not
  exercising the path.

## The honest case against

**One core is enough for what this system does.** The desktop, the
compositor and every app in the tree fit inside a 100 Hz round-robin
with time to spare; the measured problems in `docs/bugs.md` are not
throughput problems. SMP would make nothing here faster that anybody
notices.

**It adds a bug class this project cannot test well.** Every existing
correctness argument in the kernel assumes one thread of control, and
some of them are load-bearing and subtle -- the lost-wakeup arguments in
`scheduler_block_current()` and `sys_waitpid()` are correct *because*
interrupts are off and there is one core. Each of those becomes a
lock-ordering argument instead, and TCG cannot check any of them.

**The parts worth having are separable.** Stage 2 -- the Local APIC and
a per-core timer -- delivers MSI-X, more vectors and tickless idle
without a single line of locking. If the goal is "the machine gets
better", stage 2 is most of the value and none of the risk.

**What SMP is actually for here** is that it is a real operating system
milestone with a real design, and the locking audit it forces would find
things. That is a legitimate reason to build it. It is not a performance
argument, and it should not be sold as one.

## Out of scope, deliberately

NUMA. CPU hotplug. Scheduling domains, load balancing and work stealing.
CPU affinity as an API. Per-core memory allocators. `sched_ext`-style
pluggable policy -- and note the common misreading: Linux does not "let
you switch schedulers", it has compile-time classes.
