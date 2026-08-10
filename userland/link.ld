/* Places the program at VMM_USER_BASE (see kernel/include/vmm.h) --
   these two must stay in sync; a plain linker script can't #include a
   C header's #define, so the address is repeated here with a comment. */
ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    .text : { *(.text) }
    .rodata : { *(.rodata*) }
    .data : { *(.data) }
    .bss : { *(COMMON) *(.bss) }
}
