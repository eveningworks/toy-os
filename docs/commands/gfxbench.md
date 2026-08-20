# gfxbench

**a shell builtin.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    gfxbench [iterations]

## Description

Times full-screen framebuffer fills *and* console scrolls, reporting ms/frame, an fps ceiling, MB/s, which write-combining mechanism is live, and whether the console is double-buffered. Meaningful only under `make run KVM=1` or on real hardware — plain QEMU's TCG ignores memory types, so both console modes measure the same there. See `decisions.md`.