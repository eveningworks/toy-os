#ifndef QUERY_H
#define QUERY_H

#include <stdint.h>
#include <stddef.h>
#include "query_abi.h" // the classes and their records -- shared with ring 3

// The query registry: one place that knows what FACTS the kernel can
// report about itself.
//
// A subsystem that owns a fact REGISTERS a provider here at boot, the
// same way a graphics card registers a `display_driver`, a disk a
// `block_device` and a setting a `struct setting`. `kernel/mm/` owns
// memory, `kernel/proc/` owns processes, a driver owns its own device.
// There is no central table listing them and no init call to forget.
//
// WHAT REGISTRATION ADDS over the four ad-hoc syscalls this replaces
// (SYS_SYSINFO, SYS_PROC_INFO, SYS_PCI_INFO, SYS_CPU_INFO) is the
// sentence none of them could say: "a fact called meminfo exists, it has
// one record, and a record is this many bytes". Four syscalls that are
// each one information class means the syscall number grows by one per
// fact, and nothing can answer "what facts exist?" -- which is what
// `config` needs in order to read a fact at all.
//
// A FACT IS NOT A SETTING. It is computed on every read and has no
// stored form, so there is deliberately no `set` here and no default to
// reset to. See docs/settings-and-queries.md's "The vocabulary".
//
// THE TRAP, shared with the settings registry: a provider is stored by
// POINTER, not copied, so anything registered must have static storage
// duration. Registering a stack local leaves the registry holding a
// dangling pointer that reads as plausible garbage rather than crashing.

// Registered providers. Raised from 16 as the introspection commands
// moved to /bin: each one is a class, and the remaining set (dmesg,
// fsck, debug) will want more. A provider is a pointer, so the table is
// cheap; the cap exists to bound the walk, not to ration them.
#define QUERY_MAX 32

// The largest record any class may declare. It bounds the one stack
// buffer that reads a record in order to pull a named field out of it,
// so a provider declaring something enormous cannot blow a kernel stack
// (which is 16 KiB with a single guard page below it). A class needing
// more than this is a LIST of smaller records, which is the shape the
// registry is built for anyway.
#define QUERY_RECORD_MAX 256

// EVERY NAMED FIELD IS 64 BITS. Both QUERY_TYPE_* values are u64-backed,
// so the field reader copies eight bytes from the offset and is done --
// no per-type switch, and no way for a type and a width to disagree.
// A narrower field would have to be widened in the record rather than
// described as narrow here.

// One named, readable number inside a record.
//
// THE OFFSET NEVER CROSSES THE SYSCALL BOUNDARY. The kernel resolves a
// name to an offset and hands back the VALUE, so a record can still grow
// append-only without any client caring, and a bad offset cannot arrive
// from ring 3. What ring 3 sees is a name, a type and a number.
//
// This is what makes a fact addressable the way a setting is --
// `config get mem.frame_free` beside `config get system.font_size`.
// Without it a fact has no flat name at all, and nothing could list
// facts or read one value.
struct query_field {
    const char *name;   // "frame_free" -- the part after the dot
    uint32_t    type;   // QUERY_TYPE_*
    uint32_t    offset; // into the class's record
};

// Declares a u64 field of `rec` by its member name, so the offset is
// derived rather than written out. A hand-written offset is a number
// somebody has to keep true when a struct changes -- exactly the shape
// this project deletes wherever it finds it.
#define QUERY_FIELD(rec, member, ty) \
    { #member, (ty), (uint32_t)__builtin_offsetof(rec, member) }

struct query_provider {
    uint32_t    cls;          // QUERY_* (abi/query_abi.h)
    const char *name;         // "meminfo" -- at most QUERY_NAME_MAX-1 chars
    uint32_t    record_size;  // sizeof the class's record
    uint32_t    flags;        // QUERY_F_LIST, or 0 for a scalar

    // How many records exist RIGHT NOW. A scalar provider returns 1.
    // May legitimately return 0 (nothing to report yet) -- which is a
    // successful answer, not an error.
    int (*count)(void);

    // Writes record `index` into `out`, which is record_size bytes of
    // KERNEL memory the caller owns. Returns 1 on success, 0 if `index`
    // names nothing.
    //
    // It fills a kernel struct and the syscall copies it out; a provider
    // never sees a user pointer, so it cannot get the copy wrong.
    int (*fill)(int index, void *out);

    // The record's named fields, for `config get <provider>.<field>`.
    // MAY BE NULL with a zero count: a LIST class is deliberately not
    // addressable as a single value, because an index baked into a name
    // means a different record a second later -- which is sysctl's worst
    // corner and why Linux keeps processes out of sysctl entirely. Such
    // a class is read whole, by a tool built for it.
    const struct query_field *fields;
    uint32_t                  field_count;
};

// Announce a provider. Returns 1 on success, 0 if the table is full, the
// class is already registered, or the provider is malformed (no name, no
// fill, a zero record_size). REFUSING A DUPLICATE CLASS matters: two
// providers for one class would be resolved by registration order, i.e.
// by boot sequence, which is the failure the settings registry already
// refuses for a duplicate (namespace, name).
int query_register(const struct query_provider *p);

// How many providers are registered, and the one at `index`
// (0..query_count()-1), or NULL. The order is registration order and is
// not meaningful -- a caller wanting a particular class asks for it.
// Removes a previously registered provider. Exists for TESTS: a KTEST
// runs in the live kernel, so a fixture provider left behind would show
// up in `meminfo --list` and in every later test's walk of class 0. The
// settings registry carries the same call for the same reason. Nothing
// in the running system unregisters.
void query_unregister(const struct query_provider *p);

int query_count(void);
const struct query_provider *query_at(int index);

// The provider serving `cls`, or NULL if nothing does.
const struct query_provider *query_find(uint32_t cls);

// The provider named `name`, or NULL. What a command-line tool taking a
// fact's NAME rather than its number uses.
const struct query_provider *query_find_by_name(const char *name);

// Read one record into `out` (at least `cap` bytes of kernel memory).
// Returns the number of bytes written, or a negative errno
// (abi/errno.h): -ENOENT for an unknown class, -ERANGE for an index past
// the end, -EINVAL for a buffer too small to hold the record.
//
// THE ONE READER. The syscall, the kernel shell and any future in-kernel
// consumer all come through here, so the ring-0 and ring-3 answers to
// the same question cannot drift -- they are not two readers agreeing,
// they are one function.
int query_read(uint32_t cls, int index, void *out, uint32_t cap);

// Reads one NAMED field, qualified as "<provider>.<field>". Returns 0
// with *out_value and *out_type filled, or a negative errno: -ENOENT if
// no such provider or field, -ENOTSUP if the class exists but is a LIST
// and so has no single value, -EINVAL for a malformed name.
//
// -ENOTSUP rather than -ENOENT for a list is the whole point of having
// it: "there is no such fact" and "that fact is a table, ask for it with
// a tool that can show one" are different sentences, and a command that
// gave the first for the second would send somebody looking for a typo.
int query_field_get(const char *qualified, uint64_t *out_value, uint32_t *out_type);

// Registers the providers the CORE owns -- today just the registry
// describing itself. Called from kernel_main() the way settings_init()
// is; each subsystem registers its own beside it.
void query_init(void);

// kernel/mm/'s provider (memory). Declared here rather than in pmm.h
// because it is about the registry, not about the allocator.
void mem_query_init(void);

// kernel/core/'s provider for the firmware memory map. Declared here
// rather than in multiboot.h for the same reason -- it is about the
// registry, not about parsing multiboot tags.
void multiboot_query_init(void);

// kernel/fs/'s provider (which filesystem is mounted, and how full).
// MUST be registered after the filesystem is mounted is NOT true --
// the provider reads fs.h on every read, so registering it early simply
// means it reports "not mounted" until something is. Registering it
// late would be the bug: a fact absent from the registry cannot be
// asked for at all, and "nothing is mounted" is a legitimate answer
// that a caller needs to be able to receive.
void fs_query_init(void);

// kernel/lib/'s provider for the entropy source. Separate from
// SYS_GETRANDOM on purpose -- see the comment in krandom_query.c.
void krandom_query_init(void);

// kernel/drivers/'s providers for the disk's partition table and its
// entries. Two classes, because "no partitions" and "no partition
// table" are different answers a list alone cannot distinguish.
void partition_query_init(void);

// kernel/mm/'s heap providers -- the counters, and the scan that runs
// on read (see heap_query.c on why those are two classes).
void heap_query_init(void);

// kernel/drivers/'s provider for the ATA transfer path. The FORCING
// half is kernel.ata_nodma, a tunable -- this only reports.
void ata_query_init(void);

// kernel/proc/'s providers for kernel stacks and per-syscall depth.
void kstack_query_init(void);

// kernel/proc/'s provider for the physical console -- its owner, its
// foreground group, and whether a compositor holds the keyboard. Reads
// kernel/tty.h and api/keyboard.h on every read, so registration order
// against either of them does not matter.
void tty_query_init(void);

// kernel/drivers/input/'s rolling log of key events -- the scancode, the
// keycode, the character and the modifiers of each, which /bin/kbd
// prints. A LIST over a ring the driver fills from its interrupt
// handler; see kernel/keyboard_tap.h for why it records unconditionally.
void kbdtap_query_init(void);

#endif // QUERY_H
