; See context_switch.h for the design and why the return address is
; captured as data (ctx->rip) instead of left on the stack for a plain
; `ret` to find later -- anything the caller does after this call
; returns (further pushes, e.g. building an iretq frame) legitimately
; reuses that stack slot, so it can't be trusted to still hold the
; original value by the time process_context_restore runs.

bits 64
section .text

; int process_context_save(struct kernel_context *ctx)
; ctx layout: rsp(0), rbx(8), rbp(16), r12(24), r13(32), r14(40), r15(48), rip(56)
global process_context_save
process_context_save:
    mov [rdi+8],  rbx
    mov [rdi+16], rbp
    mov [rdi+24], r12
    mov [rdi+32], r13
    mov [rdi+40], r14
    mov [rdi+48], r15
    mov rax, [rsp]        ; our own return address -- read it now, before
    mov [rdi+56], rax     ; anything else can overwrite that stack slot
    lea rax, [rsp+8]      ; RSP as it will be once we return normally
    mov [rdi+0], rax
    xor eax, eax
    ret

; void process_context_restore(struct kernel_context *ctx, int value)
global process_context_restore
process_context_restore:
    mov rbx, [rdi+8]
    mov rbp, [rdi+16]
    mov r12, [rdi+24]
    mov r13, [rdi+32]
    mov r14, [rdi+40]
    mov r15, [rdi+48]
    mov rsp, [rdi+0]      ; RSP as if process_context_save() had just
                           ; returned normally
    mov eax, esi           ; value (2nd arg) becomes the "return value"
    ; This jump bypasses isr_common's normal epilogue entirely, which
    ; would otherwise `iretq` and -- as part of that -- restore RFLAGS
    ; from the interrupt frame, re-enabling interrupts (our int 0x80
    ; gate, like every other interrupt gate here, clears IF on entry).
    ; Skipping that means interrupts would stay off forever after an
    ; exit syscall, since nothing else re-enables them. sti first.
    sti
    jmp qword [rdi+56]     ; jump directly to the saved return address
