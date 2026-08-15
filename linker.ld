ENTRY(_start)

/* Four PT_LOAD segments with real, distinct permission flags, rather
   than the one merged RWX segment this used to produce (which is what
   `ld: warning: kernel.bin has a LOAD segment with RWX permissions`
   was complaining about on every build). FLAGS() values are the
   standard ELF p_flags bits, PF_X=1 | PF_W=2 | PF_R=4.

   Nothing LOADS the kernel by these flags -- GRUB maps nothing, it
   copies the image in and jumps -- so unlike userland/rt/link.ld's
   equivalent split these are documentation plus a linker check, not
   enforcement. The enforcement is paging_enforce_wx()
   (kernel/arch/x86_64/paging.c), which reads the __ktext_start /
   __ktext_end / __kdata_start symbols below at boot and rewrites the
   identity map's permissions to match. The two must agree: a section
   that moves across one of those symbols changes its permissions. */
PHDRS
{
    boot   PT_LOAD FLAGS(4);
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 1M;

    /* Every ALIGN(4096) below is load-bearing, not tidiness. W^X is
       enforced per 4KiB page, so two sections sharing a page must get
       the same permissions -- and the more permissive one always wins,
       which is the wrong direction every time. .text sharing a page
       with .rodata would make constants executable; sharing one with
       .data would make code writable. */
    __kimage_start = .;

    .boot :
    {
        *(.multiboot2)
    } :boot

    . = ALIGN(4096);
    __ktext_start = .;

    .text :
    {
        *(.text)
    } :text

    . = ALIGN(4096);
    __ktext_end = .;

    .rodata :
    {
        *(.rodata*)
    } :rodata

    /* Placed explicitly rather than left as an orphan: with PHDRS
       declared, where the linker puts an orphan section decides which
       segment (and therefore which permissions) it lands in, and
       "wherever ld felt like" is not a thing to leave to chance in a
       file whose whole job is now saying what is executable. */
    .eh_frame :
    {
        *(.eh_frame)
    } :rodata

    /* In-kernel test descriptors. KTEST() (kernel/include/kernel/ktest.h)
       drops one struct ktest_case per test into .ktests, and the runner
       walks the array between these two symbols -- so tests register
       themselves by existing, with no list to maintain by hand.
       ALIGN(8) matters: the entries are structs of pointers and the
       runner indexes them as an array, so any padding the linker
       inserted between input sections would be read as a bogus entry.
       They are `static const`, hence the read-only segment. */
    . = ALIGN(8);
    .ktests :
    {
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
    } :rodata

    . = ALIGN(4096);
    __kdata_start = .;

    .data :
    {
        *(.data)
    } :data

    .bss :
    {
        *(COMMON)
        *(.bss)
    } :data

    . = ALIGN(4096);
    _kernel_end = .; /* first byte after everything the linker placed --
                         used by pmm.c to know what physical range to
                         reserve for the kernel image itself */
}
