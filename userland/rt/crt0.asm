; crt0 -- the C runtime entry point for every ring-3 program.
;
; Replaces the hand-written `void _start(void)` each userland program
; used to carry, and with it the assumption that a plain C function can
; serve as a process entry point. It can't, quite: GCC compiles one
; expecting a return address to have been pushed (RSP % 16 == 8 on
; entry) and sizes its prologue from there, so the kernel had to hand
; over a deliberately mis-aligned RSP to keep such functions working.
; See kernel/proc/elf_run.c's layout comment for the history.
;
; With a real assembly entry point, the standard SysV convention applies
; instead: RSP is 16-byte aligned and points at argc.
;
;     (%rsp)      argc
;     8(%rsp)     argv[0] .. argv[argc-1]
;                 NULL          <- argv terminator
;                 NULL          <- envp (empty; toy-os has no environment yet)
;
; Written in assembly rather than C with __attribute__((naked)) because
; reading (%rsp) is exactly the thing C has no way to express -- a naked
; function would just be this same asm with a less obvious wrapper
; around it.

bits 64

section .text
global _start
extern main
; exit(), not sys_exit(). A hosted crt0 calls exit(), which the libc
; defines to run atexit handlers and FLUSH STDIO before making the real
; _exit syscall -- and since stdout is buffered, that flush is what
; makes a program's last printf() reach the screen at all. This was
; sys_exit() until the stream layer existed and there was nothing for
; exit() to do.
;
; The cost of the hook is that every ring-3 program now pulls exit(),
; fflush() and the standard streams out of libc.a whether or not it
; prints anything. -ffunction-sections plus --gc-sections keeps that to
; the flush path rather than the whole of stdio.
;
; A program that calls sys_exit() itself still bypasses all of it, which
; <stdio.h> documents as the trap that comes with buffering.
extern exit

_start:
    ; argc / argv / envp into the SysV argument registers for main().
    mov     rdi, [rsp]              ; argc
    lea     rsi, [rsp + 8]          ; argv
    ; envp sits one slot past argv's NULL terminator:
    ;   argv + (argc + 1) * 8
    lea     rdx, [rsi + rdi*8 + 8]  ; envp

    ; Align the stack for the call.
    ;
    ; SysV states the rule at the CALLEE's entry: %rsp + 8 is a multiple
    ; of 16 when control reaches a function, i.e. %rsp % 16 == 8 there,
    ; because the caller's `call` has just pushed 8 bytes of return
    ; address. So %rsp must be 16-ALIGNED right before the call, which
    ; is exactly what the kernel already hands us -- this `and` only
    ; re-establishes it defensively.
    ;
    ; Do NOT `sub rsp, 8` here. Doing that leaves main() entered with
    ; %rsp % 16 == 0, and GCC then emits `movaps`/`movapd` against stack
    ; slots it believes are 16-aligned but aren't. That faults rather
    ; than mis-storing: every GUI client took a #GP a few instructions
    ; into main() while this line was here, and the plain non-SSE
    ; programs were entirely unaffected -- which is what makes the bug
    ; look mysterious rather than like an alignment problem.
    and     rsp, -16

    call    main

    ; main()'s return value is the exit status, exactly as in C.
    mov     edi, eax
    call    exit

    ; exit() does not return. If it somehow does, stop here rather than
    ; running off into whatever follows in .text -- a fault with an
    ; obvious cause beats executing arbitrary bytes.
.hang:
    hlt
    jmp     .hang

; No executable stack, same as every other object in this build.
section .note.GNU-stack noalloc noexec nowrite progbits
