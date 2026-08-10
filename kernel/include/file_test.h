#ifndef FILE_TEST_H
#define FILE_TEST_H

// file_test_run(): loads and runs userland/file_test.c (see grub.cfg's
// module2 lines) -- a ring-3 program that exercises SYS_OPEN/SYS_WRITE
// (fd-aware now)/SYS_READ/SYS_CLOSE against the in-memory filesystem
// (fs.c). Writes a short string into a real file, closes it, reopens it
// for reading, reads it back, and echoes what it read to stdout (fd 1)
// -- proving the write and the read round-trip through fs.c rather than
// just trusting each syscall in isolation. Returns (unlike echotest /
// wintest -- this one isn't interactive or modal, it just runs and
// exits, same shape as writetest/ptrtest).
void file_test_run(void);

#endif
