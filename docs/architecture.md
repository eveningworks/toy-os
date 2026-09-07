# How toy-os is laid out

Which directory holds what, and why the boundaries are where they are.
`kernel/include/README.md` covers the header split by audience;
`apps/README.md` and `userland/` have their own notes.

---

Directories are subsystems, not filing cabinets — where a file lives says
what kind of thing it is.

```
kernel/
  arch/x86_64/  anything a different CPU would need rewritten: boot and
                long mode, interrupts, GDT/TSS/IDT, PIC, paging, ASLR
  core/         bring-up and whole-machine concerns: kernel_main,
                multiboot, timers, serial + the debug console
  mm/           physical frames, address spaces, the kernel heap
  proc/         ELF64 loader, the syscall table, scheduler, window server
  fs/           TFS3, FAT32 and ramfs behind the VFS and its mount table
  net/          ARP, IPv4, ICMP, UDP, TCP and the socket layer
  acpi/         the firmware tables, the MADT walk, and shutdown
  tty/          the terminal object: line discipline, ptys, tty0
  lib/          services with no hardware of their own: the shared
                toolkit (strings, numbers, formatting, paths, line
                editing), JSON, klog, /etc config, entropy, tunables
  drivers/      one piece of hardware each, in the directory of the
                REGISTRY it plugs into rather than the bus it sits on:
                block/, display/, net/, sound/, input/, plus the shared
                buses virtio/ and usb/ (see kernel/README.md)
  test/         the KTEST harness itself
  include/      split by audience and ENFORCED by include paths: api/
                (what apps may use), abi/ (the kernel<->userland
                contract), kernel/ (internal, off apps/'s path)

apps/           kernel-space programs: the shell and its tab
                completion, the desktop launcher, a calculator engine.
                No GUI code lives here any more.

userland/       ring-3 programs, split by ROLE:
  rt/           crt0, libsys, the signal trampoline, linker scripts
  libc/         tolibc -- the C library, aiming to be COMPLETE;
                also built -fpic into /lib/libc.so
  ldso/         /lib/ld-toy.so -- the dynamic loader, freestanding
  dynlib/       shared-library sources (the Stage-2 proof lib)
  ui/           Toykit -- the toolkit clients program against;
                also built -fpic into /lib/libuapp.so
  lib/          non-UI libraries: the tosh shell, images, sound,
                markdown, the pager, history
  include/      headers every ring-3 program can reach
  wm/           the window manager, itself a ring-3 program
  fm/           the File Manager, split by concern like wm/
  gui/          windowed apps: apps/, demos/, system/
                                   -> seeded under /bin/wm/
  bin/          command-line tools -> seeded to /bin
  tests/        single-mechanism diagnostics -> seeded to /tests
  ports/        vendored third-party source, kept separate on purpose
  backends/     OUR side of a vendored port (backends/doom/),
                deliberately outside ports/

seed/, data/    what gets mirrored onto disk.img at build time
tools/          build, test and delivery tooling (see Development)
docs/           design records, specs, and the decision log
```

Source discovery is recursive, so a new file — or a whole directory —
under `kernel/` or `apps/` needs no Makefile edit. Header dependencies
are tracked, and `tools/check_deps.py` proves per build directory that
the tracking is actually live.

`kernel_main()` brings up the hardware and calls `apps_start()`, which
launches the shell without naming it: the shell is simply the first
entry in the app registry, and that seam keeps the kernel core ignorant
of what apps exist.

