/* Places the program at VMM_USER_BASE (see kernel/include/vmm.h) --
   these two must stay in sync; a plain linker script can't #include a
   C header's #define, so the address is repeated here with a comment. */
ENTRY(_start)

/* Three separate PT_LOAD segments (explicit PHDRS, not the default one
   merged segment `-n`/nmagic would otherwise produce) with distinct,
   real permission flags -- FLAGS() values are the standard ELF p_flags
   bits, PF_X=1 | PF_W=2 | PF_R=4: text is R+X (5, never writable),
   rodata is R-only (4, never writable OR executable), data/bss is R+W
   (6, never executable). kernel/core/elf.c's loader reads these back
   per-segment and maps each one's pages accordingly (see vmm.c's
   vmm_map_user_page_flags()) -- this split is what makes that mean
   anything; a single merged RWX segment (every userland ELF before
   this) would make every page executable regardless of what elf.c
   does. The ALIGN(4096)s are load-bearing, not cosmetic: NX/W^X is
   enforced per 4KiB page, so .text and .data can't share one page
   without one of them getting the wrong permission. */
/* PT_TLS is what tells the linker how big a thread's TLS block is, and
   that is what every `%fs:offset` in the program is computed against.
   It is not loaded on its own -- .tdata's bytes reach memory through
   the data PT_LOAD below, which is why both sections name two segments.
   The kernel's ELF loader ignores anything that is not PT_LOAD
   (kernel/proc/elf.c) and userland/rt/tls.c builds each thread's block
   from the symbols this script exports. */
PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    tls    PT_TLS  FLAGS(4);
}

SECTIONS
{
    . = 0x8000000000;

    /* The `.text.*` / `.data.*` / `.bss.*` wildcards are REQUIRED, not
       tidiness: USERLAND_CFLAGS carries -ffunction-sections
       -fdata-sections (so --gc-sections can drop unreferenced widgets
       out of libuapp.a), which puts every function in its own
       `.text.<name>` section. A script matching only `*(.text)` would
       silently place none of them -- the link succeeds, the ELF is
       nearly empty, and the failure surfaces as a page fault at the
       entry point rather than as a linker error. */
    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata*) } :rodata
    . = ALIGN(4096);
    /* THREAD-LOCAL STORAGE, and the order matters: .tdata's initialised
       bytes are the template every thread's block is copied from, and
       .tbss adds the zeroed tail. .tbss is NOBITS and does not advance
       the location counter, so .bss below starts where .tbss did --
       that is correct, because no thread's TLS lives in the image. */
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss  : { *(.tbss .tbss.*) *(.tcommon) } :data :tls
    .data : { *(.data .data.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data

    /* What userland/rt/tls.c needs to build a block: where the template
       is, how much of it to copy, and how much to zero after it. The
       ADDRESS of each symbol is the value -- there is nothing at it. */
    PROVIDE_HIDDEN(__tls_template = ADDR(.tdata));
    PROVIDE_HIDDEN(__tls_filesz   = SIZEOF(.tdata));
    PROVIDE_HIDDEN(__tls_memsz    = SIZEOF(.tdata) + SIZEOF(.tbss));
    /* THE ALIGNMENT IS PART OF THE ADDRESS ARITHMETIC, not a
       performance hint: the linker resolves each `%fs:offset` against
       the block size ROUNDED UP TO IT, so a runtime that rounds to
       anything else places the .tdata template where nothing reads it.
       MAX because either section may be the empty one. */
    PROVIDE_HIDDEN(__tls_align    = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));

    /* NO CEILING HERE ANY MORE, and that is the point.

       This used to carry an ASSERT refusing any binary whose sections
       reached UADDR_HEAP_BASE -- a fixed 1 MiB above the base above --
       because SYS_SBRK started handing out pages there and a bigger
       image would have had its .bss silently aliased by its own malloc.
       One MiB is not much for a real program, and the linker was the
       only thing that knew how big the image had got.

       The kernel derives each process's heap base from where its image
       actually ENDS now (elf.c's out_image_end -> struct sched_mm's
       heap_base), which is what Linux does in fs/binfmt_elf.c. So there
       is nothing fixed for the image to collide with, and the size of a
       ring-3 program is bounded only by the memory to load it. */
}
