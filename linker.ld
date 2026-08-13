ENTRY(_start)

SECTIONS
{
    . = 1M;

    .boot :
    {
        *(.multiboot2)
    }

    .text :
    {
        *(.text)
    }

    .rodata :
    {
        *(.rodata*)
    }

    /* In-kernel test descriptors. KTEST() (kernel/include/kernel/ktest.h)
       drops one struct ktest_case per test into .ktests, and the runner
       walks the array between these two symbols -- so tests register
       themselves by existing, with no list to maintain by hand.
       ALIGN(8) matters: the entries are structs of pointers and the
       runner indexes them as an array, so any padding the linker
       inserted between input sections would be read as a bogus entry. */
    . = ALIGN(8);
    .ktests :
    {
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4096);
    _kernel_end = .; /* first byte after everything the linker placed --
                         used by pmm.c to know what physical range to
                         reserve for the kernel image itself */
}
