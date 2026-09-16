#ifndef TOYABI_H
#define TOYABI_H

#include <stdint.h>

// THE USERLAND ABI'S VERSION, and the one thing a dynamic program and
// the libraries it loads must agree on.
//
// WHY THIS EXISTS. `/bin/about` on the bare-metal laptop was left a
// build behind while `/lib/libc.so` was replaced, and it did not fail
// to start -- it page-faulted inside `__rt_tls_init`, writing into its
// own text segment, because an old executable's static TLS geometry
// does not match a new libc's. A skew has to be REFUSED BY NAME, the
// way this project refuses a progressive JPEG or an unparseable config,
// rather than discovered as a wild pointer three frames later.
//
// **THIS IS A STAMP, NOT SYMBOL VERSIONING.** glibc carries several
// ABIs in one file through version nodes, so a decades-old binary keeps
// running; that is a great deal of machinery and this project has one
// userland, built together, shipped together. What it needs is the
// cheap half: notice the mismatch and say so. A program built against a
// different version does NOT run -- it is refused with a sentence, and
// the answer is to rebuild it.
//
// WHEN TO BUMP IT: any change that makes an already-built program wrong
// against the new libraries. The TLS block's size or alignment, a
// struct any exported function takes or returns BY VALUE, a function's
// signature, the meaning of a syscall wrapper's arguments. Adding a new
// exported function does NOT need a bump -- an old program never calls
// it, and a new program against an old library fails to resolve it by
// name, which already reports itself clearly.
#define TOY_ABI_VERSION 1u

// 'T' 'A' 'B' 'I'. The loader reads these records from a program it has
// not relocated yet, so a wrong address has to be recognisable as
// garbage rather than believed.
#define TOY_ABI_MAGIC 0x49424154u

struct toy_abi_stamp {
    uint32_t magic;
    uint32_t version;
};

#endif // TOYABI_H
