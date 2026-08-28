; /lib/ld-toy.so's entry: hand the pristine SysV stack to the C half,
; then jump to whatever entry it returns -- with RSP restored, so the
; real program's crt0 sees argc at (%rsp) exactly as if the kernel had
; entered it directly.
bits 64
section .text
global _start
extern ldso_main

_start:
    mov rbx, rsp        ; callee-saved: survives ldso_main
    mov rdi, rsp        ; the SysV block: argc, argv, envp, auxv
    and rsp, -16
    call ldso_main      ; returns the executable's entry point
    mov rsp, rbx
    jmp rax
