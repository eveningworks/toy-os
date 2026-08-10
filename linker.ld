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
