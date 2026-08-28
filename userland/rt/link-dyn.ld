/* The link script for a DYNAMIC executable -- userland/rt/link.ld plus
   what dynamic linking adds, because a script with explicit PHDRS gets
   no segments it did not declare: .interp needs a PT_INTERP, .dynamic
   a PT_DYNAMIC, and the dynamic sections (.dynsym/.dynstr/.hash/
   .rela.*) plus the GOT/PLT need homes, or the linker scatters them as
   orphans. Static programs keep using link.ld untouched.

   FILEHDR + PHDRS on the first segment maps the ELF and program
   headers at the image base -- that is what lets the kernel pass
   AT_PHDR as base + e_phoff with no copy (abi/auxv.h), so the first
   section starts at base + SIZEOF_HEADERS rather than at the base. */
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FILEHDR PHDRS FLAGS(5);
    interp  PT_INTERP  FLAGS(4);
    rodata  PT_LOAD    FLAGS(4);
    data    PT_LOAD    FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
    tls     PT_TLS     FLAGS(4);
}

SECTIONS
{
    . = 0x8000000000 + SIZEOF_HEADERS;

    .interp : { *(.interp) } :text :interp
    .hash    : { *(.hash) } :text
    .dynsym  : { *(.dynsym) } :text
    .dynstr  : { *(.dynstr) } :text
    .rela.dyn : { *(.rela.dyn) *(.rela.got) } :text
    .rela.plt : { *(.rela.plt) } :text
    .plt     : { *(.plt) *(.plt.*) } :text
    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata*)
        /* Same TLS-geometry QUADs as link.ld -- userland/rt/tls.c
           reads them in every program, dynamic or not. */
        . = ALIGN(8);
        __rt_tlsdesc = .;
        QUAD(SIZEOF(.tdata));
        QUAD(SIZEOF(.tdata) + SIZEOF(.tbss));
        QUAD(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
    } :rodata
    . = ALIGN(4096);
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss  : { *(.tbss .tbss.*) *(.tcommon) } :data :tls
    .dynamic : { *(.dynamic) } :data :dynamic
    .got     : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data
    .data : { *(.data .data.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data

    PROVIDE_HIDDEN(__tls_template = ADDR(.tdata));
}
