/* /lib/ld-toy.so links HERE: ELF_LDSO_BASE in kernel/include/kernel/elf.h
   -- the two must agree, and the kernel refuses an executable whose
   image reaches this address. A fixed-base ET_EXEC on purpose: the
   kernel loads it with the same elf_load() every program uses, and the
   loader never has to relocate itself (see docs/decisions.md).
   Same three-segment W^X shape as userland/rt/link.ld, minus TLS --
   this program has no __thread state, which is the point of its
   freestanding syscall stubs. */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x8010000000;

    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata*) } :rodata
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data
}
