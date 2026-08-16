; boot.asm
; Multiboot2-compliant entry point. GRUB loads us in 32-bit protected mode.
; We set up paging + long mode, then jump into 64-bit code and call kernel_main.

bits 32

section .multiboot2
align 8
mb2_header_start:
    dd 0xE85250D6                ; magic
    dd 0                         ; architecture (0 = i386/x86)
    dd mb2_header_end - mb2_header_start
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start)) & 0xFFFFFFFF

    ; framebuffer request tag: ask for a linear graphics framebuffer.
    ; width/height/depth are only a preference -- GRUB picks the closest
    ; mode it can and reports the actual values in the boot info tag.
    ; 1280x720 (16:9) gives noticeably more room than the original
    ; 800x600 while still fitting comfortably on modern displays; gfx.c's
    ; GFX_MAX_PIXELS back buffer and the Makefile's QEMU display flags
    ; were sized/set to match -- see gfx.c's comment if this changes again.
    align 8
    dw 5      ; type = framebuffer
    dw 0      ; flags
    dd 20     ; size
    dd 1280   ; preferred width
    dd 720    ; preferred height
    dd 32     ; preferred depth (bits per pixel)

    ; end tag
    align 8
    dw 0    ; type
    dw 0    ; flags
    dd 8    ; size
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

align 4096
; All three are exported because kernel ASLR has to rewrite the two
; levels of internal pointers after relocating the image -- the copied
; tables still point at the originals. See reloc.c.
global p4_table
p4_table: resb 4096
global p3_table
p3_table: resb 4096
global p2_tables
p2_tables: resb 4096 * 4   ; four P2 tables => 4 x 1GiB = 4GiB identity mapped

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    mov edi, ebx        ; save multiboot2 info pointer (passed by GRUB in ebx)

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code_segment:long_mode_start

    hlt

check_multiboot:
    cmp eax, 0x36d76289   ; multiboot2 bootloaders pass this magic in eax
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "1"
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "2"
    jmp error

setup_page_tables:
    ; identity map the first 4GiB using 2MiB pages: P3 has 4 entries,
    ; each pointing at its own P2 table covering 1GiB.
    ;
    ; The PML4E and PDPTEs below get the USER bit (0b111, not just
    ; 0b11 present+writable) even though nothing is user-accessible by
    ; default -- x86-64 paging ANDs permissions down the WHOLE walk, so
    ; a missing USER bit at any parent level blocks ring-3 access no
    ; matter what the leaf PDE/PTE says. Actual access is still
    ; determined by the leaf entries: the 2MiB PDEs below stay
    ; supervisor-only (no USER bit), and only kernel/arch/x86_64/paging.c's
    ; paging_make_user_page() ever adds USER, to specific 4KiB pages.
    mov eax, p3_table
    or eax, 0b111        ; present + writable + user
    mov [p4_table], eax

    mov ecx, 0           ; which 1GiB region (0..3)
.map_p3_entry:
    mov eax, p2_tables
    mov edx, ecx
    imul edx, 4096       ; each P2 table is 4096 bytes
    add eax, edx
    or eax, 0b111        ; present + writable + user
    mov [p3_table + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .map_p3_entry

    mov ecx, 0           ; global 2MiB page index, 0..2047 (4GiB / 2MiB)
.map_p2_table:
    mov eax, 0x200000    ; 2MiB
    mul ecx
    or eax, 0b10000011   ; present + writable + huge page
    mov edx, ecx
    shl edx, 3           ; * 8 bytes per entry
    mov [p2_tables + edx], eax

    inc ecx
    cmp ecx, 2048
    jne .map_p2_table

    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5        ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080   ; EFER MSR
    rdmsr
    or eax, 1 << 8         ; long mode enable
    or eax, 1 << 11        ; NXE -- honor the NX bit (PTE bit 63) everywhere.
                            ; Nothing here sets bit 63: the map built below is
                            ; deliberately still flat present+writable, because
                            ; getting the permissions right needs the linker's
                            ; section symbols and is far easier in C than in
                            ; 32-bit pre-long-mode asm. paging_enforce_wx()
                            ; (paging.c) rewrites this map at the top of
                            ; kernel_main() -- NX on every huge PDE, one 4KiB
                            ; split so .text can be read-only and executable,
                            ; and CR0.WP. vmm.c uses the same bit for
                            ; per-process user pages. See docs/decisions.md.
    wrmsr

    mov eax, cr0
    or eax, 1 << 31        ; enable paging
    or eax, 1 << 0         ; enable protected mode (already on, harmless)
    mov cr0, eax

    ret

error:
    ; print "ERR: X" to top-left of VGA text buffer, then halt
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt

section .rodata
gdt64:
    dq 0 ; null descriptor
.code_segment: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; executable, code segment, present, 64-bit
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
bits 64
global long_mode_start
extern kernel_relocate_boot
long_mode_start:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; multiboot info ptr is already in edi/rdi from _start, which is
    ; also where the SysV first argument goes.
    ;
    ; Kernel ASLR happens here, between paging coming up and the kernel
    ; proper starting: the identity map already covers the whole low
    ; 4GiB, so the image can be copied anywhere in it, and nothing has
    ; run yet that would need un-doing. kernel_relocate_boot() returns
    ; how far it moved the image, or 0.
    ;
    ; Pushed TWICE to keep rsp 16-byte aligned across the call, which
    ; is what SysV requires at a call site. One push would leave it
    ; 8-aligned and break any SSE the callee's code generation used.
    push rdi
    push rdi
    call kernel_relocate_boot
    pop rdi
    pop rdi

    ; rax = delta. Both of these are no-ops when it is 0.
    ;
    ; The stack moves first: .bss was COPIED rather than zeroed, so the
    ; relocated stack already holds this frame byte for byte, and
    ; adding the delta lands on the same position within it. Then jump
    ; into the relocated kernel_main -- `lea rel` gives its address in
    ; the image we are still executing, so the delta has to be added.
    add rsp, rax
    lea rcx, [rel kernel_main]
    add rcx, rax
    call rcx

    cli
.hang:
    hlt
    jmp .hang
