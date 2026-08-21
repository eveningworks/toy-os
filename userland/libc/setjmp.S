// setjmp/longjmp for x86-64 SysV. See <setjmp.h> for what is saved and,
// more importantly, what is not.
//
// .S rather than .asm because everything under userland/ is built with
// gcc and this needs no NASM syntax; the kernel's own .asm files are
// assembled separately (see the Makefile's source discovery).
//
// jmp_buf layout, eight 64-bit words:
//   0: rbx   1: rbp   2: r12   3: r13   4: r14   5: r15   6: rsp   7: rip
//
// rsp is saved as the value it will have AFTER the return, i.e. one
// slot past the return address this call pushed. Saving the raw rsp
// instead would restore into the middle of setjmp's own frame, which
// works right up until something writes there.

    .text
    .globl setjmp
    .type setjmp, @function
setjmp:
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax      // rsp as it will be after `ret`
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax       // the return address
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax         // a direct call returns 0
    ret
    .size setjmp, .-setjmp

    .globl longjmp
    .type longjmp, @function
longjmp:
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax           // longjmp(env, 0) must return 1
1:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    // Jump rather than push-and-ret: pushing the target onto the
    // restored stack would write one word BELOW the saved rsp, which is
    // memory the target frame does not own.
    jmp     *56(%rdi)
    .size longjmp, .-longjmp

// No executable stack, same as every other object here.
    .section .note.GNU-stack,"",@progbits
