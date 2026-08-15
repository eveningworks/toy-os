#ifndef KERNEL_STACK_GUARD_H
#define KERNEL_STACK_GUARD_H

// kernel/lib/stack_protector.c used to have no header at all, on the
// grounds that __stack_chk_guard/__stack_chk_fail are only ever called
// by GCC's own generated code and so have no API surface. That stopped
// being true when the guard gained a random value: kernel_main() has to
// ask for it, at a specific point and from nowhere else.
//
// Internal (kernel/, not api/) on purpose -- an app has no business
// re-rolling the stack canary.

// Replaces the build-time __stack_chk_guard with a random one. Call
// ONLY from kernel_main(), directly, after krandom_init(). Read the
// function's own comment before moving the call: changing the guard
// while an instrumented frame is live panics that frame on return,
// which is why "directly from kernel_main()" is a requirement and not
// a style preference. A no-op (with a klog line) when krandom has no
// entropy to give.
void stack_guard_randomize(void);

#endif
