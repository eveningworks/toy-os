#ifndef ABI_AUXV_H
#define ABI_AUXV_H

// The auxiliary vector: (type, value) pairs the kernel places after
// envp's NULL terminator, ending with AT_NULL -- SysV's shape, Linux's
// type numbers. ONLY a dynamic executable gets one (the kernel is
// telling /lib/ld-toy.so where the program it must finish loading
// is); a static program's stack ends at envp's NULL exactly as before,
// because inventing entries nobody reads is how an ABI accumulates
// fiction.
//
// AT_PHDR points INTO THE MAPPED IMAGE: a dynamic executable's first
// PT_LOAD carries the ELF and program headers (userland/rt/link-dyn.ld
// says FILEHDR PHDRS), so the kernel passes base + e_phoff and copies
// nothing.

#define AT_NULL  0
#define AT_PHDR  3 // the executable's program headers, mapped
#define AT_PHENT 4 // sizeof one program header
#define AT_PHNUM 5
#define AT_ENTRY 9 // the EXECUTABLE's entry -- where ld-toy jumps when done

#endif
