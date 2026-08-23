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
PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
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
    .data : { *(.data .data.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data

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
