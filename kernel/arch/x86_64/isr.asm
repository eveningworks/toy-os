bits 64

extern isr_dispatch
; isr_dispatch returns WHERE TO RESUME, in rax -- see idt.c's wrapper and
; scheduler.c's design comment. It used to be read out of the global
; g_next_kernel_rsp instead, which a nested interrupt could clobber; the
; global is still how scheduler.c asks for a context switch, but the
; answer now rides the C stack, one copy per call.

section .text

global idt_load
idt_load:
    lidt [rdi]
    ret

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; dummy error code
    push qword %1        ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1        ; vector number (error code already pushed by CPU)
    jmp isr_common
%endmacro

; Exceptions 0-31. 8, 10-14, 17, 21, 29, 30 push an error code automatically.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; IRQs remapped to 32-47, then the MSI vectors at 48-63 (lapic.h's
; LAPIC_VECTOR_BASE..LAST), then 64-71 for the I/O APIC's inputs above
; the ISA range (GSI 16-23, irq.h's irq_vector()). One stub apiece
; either way -- the vector number is what tells isr_dispatch() which
; controller to acknowledge, so the ranges must not overlap and this
; %rep is where that is decided.
%assign i 32
%rep 40
ISR_NOERR i
%assign i i+1
%endrep

; The LAPIC's spurious vector (lapic.h's LAPIC_SPURIOUS_VECTOR). It
; needs a gate for the same reason every vector does -- one delivered
; with no descriptor behind it is a #GP, and the spurious one arrives
; precisely when something has already gone slightly wrong.
ISR_NOERR 255

; Syscall entry: software interrupt (int 0x80), raised deliberately by
; ring-3 code, not a hardware IRQ -- its IDT gate gets DPL=3 (see
; idt_init() in idt.c) so ring 3 is actually allowed to invoke it. No
; error code, so it uses the same ISR_NOERR shape as everything else.
ISR_NOERR 128

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp         ; pass pointer to saved state as arg
    call isr_dispatch

    mov rsp, rax         ; isr_dispatch RETURNS where to resume. Usually
                          ; the block just pushed (a no-op); when it is
                          ; not, that is the entire context-switch
                          ; mechanism. A return value rather than a
                          ; global so a NESTED interrupt cannot clobber
                          ; the frame an outer handler will resume.

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16           ; pop vector number + error code
    iretq
