#ifndef ABI_QUERY_ABI_H
#define ABI_QUERY_ABI_H

#include <stdint.h>

// SYS_QUERY: reading FACTS -- live kernel state that is computed on
// every read and never persisted. See docs/settings-and-queries.md's
// "The vocabulary" for how a fact differs from a setting and a tunable;
// docs/query-design.md for why this is a syscall and not a /proc.
//
// ONE SYSCALL, AN INFORMATION CLASS, A TYPED STRUCT. The shape is
// Windows' NtQuerySystemInformation rather than Linux's filesystem,
// because a /proc here would need a mount table vfs.c does not have, it
// would put diagnostics ON TOP of storage (so a machine that failed to
// mount loses them), and it would make the ABI text nobody can sort.
//
// A class is either SCALAR (exactly one record -- memory totals) or a
// LIST (many, walked by index -- the providers below, processes later).
// `count` says which, and a scalar simply reports 1.

#define QUERY_NAME_MAX 16 // a provider's short name, e.g. "mem"

// "<provider>.<field>" plus its NUL -- the qualified name `config` takes,
// deliberately the same (namespace, name) shape a setting has, so
// `mem.frame_free` and `system.font_size` read alike even though only one
// of them can be written.
#define QUERY_FIELD_PATH_MAX 48

// A message rather than registers, because a syscall here gets three
// (RDI/RSI/RDX) and this needs an op, a class, an index, a buffer and a
// capacity. Same precedent as struct setting_msg and struct
// win_request_msg -- and it leaves room to grow without spending another
// syscall number per operation.
enum {
    // In: cls, index, buf, len. Out: the record, and `returned` bytes.
    QUERY_OP_RECORD      = 0,
    // In: cls. Out: `returned` = how many named fields this class has.
    QUERY_OP_FIELD_COUNT = 1,
    // In: cls, index. Out: `name` and `type` for that field.
    QUERY_OP_FIELD_INFO  = 2,
    // In: `name`, qualified ("mem.frame_free"). Out: `value` and `type`.
    QUERY_OP_FIELD_GET   = 3,
};

// FIELD NAMES AND VALUES CROSS THIS BOUNDARY; FIELD OFFSETS DO NOT.
// A record's layout stays kernel-side, so it can still grow append-only
// without any client caring, and a bad offset cannot arrive from ring 3.
// What ring 3 gets is "a fact called mem.frame_free exists, it is a
// count, and it is 12345" -- which is all `config get` needs, and all a
// generated UI needs.
struct query_msg {
    uint32_t op;       // in:  QUERY_OP_*
    uint32_t cls;      // in:  QUERY_* below
    uint32_t index;    // in:  which record, or which field; 0 for a scalar
    uint32_t len;      // in:  capacity of `buf` in bytes (QUERY_OP_RECORD)
    uint32_t returned; // out: bytes written, or the field count
    uint32_t type;     // out: QUERY_TYPE_* (the field ops)
    uint64_t buf;      // in:  user address of the output record
    uint64_t value;    // out: the field's value (QUERY_OP_FIELD_GET)
    char     name[QUERY_FIELD_PATH_MAX]; // in for FIELD_GET, out for FIELD_INFO
};

// A field's type, which is what a formatter needs and a raw u64 cannot
// say. Deliberately few: this describes how to PRINT a number, not a
// type system.
#define QUERY_TYPE_U64   0 // a plain count
#define QUERY_TYPE_BYTES 1 // a byte count -- a formatter may humanise it

// VERSION TOLERANCE IS THE CALLER'S `len`, and it is why this is not
// just a pointer. The kernel writes min(len, sizeof(record)) and reports
// how much in `returned`, so a struct that GAINS a field does not break
// a binary built against the old one -- it simply does not see the new
// field. NtQuerySystemInformation's returned-length argument exists for
// exactly this. A caller that needs to know whether a field is present
// compares `returned` against the offset of that field.
//
// The rule that makes it work: a record's existing fields never move and
// never change meaning. Growth is append-only.

// ---- the classes ----------------------------------------------------

// THE REGISTRY DESCRIBING ITSELF. One record per registered provider, so
// "what facts exist?" is answered by the same syscall rather than by a
// second mechanism -- and so the LIST path has a real caller from the
// first commit instead of being an unvalidated half (this project's
// standing rule about a seam with one implementation).
//
// Class 0 deliberately: it is the one class a caller can ask for without
// having been told anything first.
#define QUERY_PROVIDERS 0

// Memory: the frame allocator and the kernel heap. SCALAR.
#define QUERY_MEMINFO   1

// The mounted filesystem: which backend, whether it persists, and how
// full it is. SCALAR.
#define QUERY_FSINFO    2

// The firmware memory map, one record per region. LIST.
#define QUERY_MEMMAP    3

// One record per DANGLING mapping found by walking every live address
// space against the frame allocator -- so ZERO RECORDS MEANS HEALTHY.
// LIST.
#define QUERY_MMAUDIT   4

// Where the random bytes SYS_GETRANDOM returns are coming from. SCALAR.
#define QUERY_RANDOM    5

// The attached disk's partition table itself -- which kind, how many
// entries. SCALAR. The entries are QUERY_PARTITION.
#define QUERY_PARTTABLE 6

// One record per partition. LIST.
#define QUERY_PARTITION 7

// The kernel heap: what kmalloc has, what it has handed out, and what
// its debug mode has caught. SCALAR.
#define QUERY_HEAP      8

// READING THIS PERFORMS A SCAN. Every poisoned free block is verified
// right now and the damage count comes back -- which is a FACT by this
// project's definition (computed fresh on every read, no stored form),
// not an action needing a write. QUERY_MMAUDIT already works this way.
// SCALAR.
#define QUERY_HEAPCHECK 9

// The ATA disk: which transfer path is in use, and what the hardware
// offers. SCALAR.
#define QUERY_ATA       10

// One record per live kernel stack. LIST.
#define QUERY_KSTACK    11

// One record per syscall that has been measured, when kernel.kstack_track
// is on. LIST -- and legitimately EMPTY when tracking is off, which is a
// different answer from "every syscall used 0 bytes".
#define QUERY_KSTACK_SYSCALL 12


// QUERY_PROVIDERS' record.
struct query_provider_info {
    uint32_t cls;                 // the QUERY_* number to ask for
    uint32_t record_size;         // bytes in one of this class's records
    uint32_t count;               // records available RIGHT NOW; 1 for a scalar
    uint32_t flags;               // QUERY_F_*
    char     name[QUERY_NAME_MAX];
};

// This class is a LIST -- `count` may be anything, including 0, and a
// caller walks 0..count-1. Absent means SCALAR: exactly one record.
// Stated as a flag rather than inferred from `count == 1`, because a
// list that happens to hold one element is not a scalar and a caller
// deciding by arithmetic would get that wrong exactly once.
#define QUERY_F_LIST (1u << 0)

// QUERY_MEMINFO's record.
//
// Frames rather than bytes for the allocator, with the frame size
// alongside: the allocator counts frames, and a caller that wants bytes
// multiplies rather than assuming 4096 -- which is the assumption that
// would silently break the day this kernel gains huge pages.
struct query_meminfo {
    uint64_t frame_total;      // frames the allocator manages
    uint64_t frame_free;
    uint64_t frame_bytes;      // bytes per frame
    uint64_t heap_total_bytes; // the KERNEL heap (api/heap.h), not a process's
    uint64_t heap_used_bytes;
};

// QUERY_FSINFO's record.
//
// THE NAME IS IN THE RECORD, NOT A NAMED FIELD. Every named field is 64
// bits (see struct query_field), so a string cannot be one -- which is
// why `config get fs.backend` cannot print the name while
// `config get fs.used_bytes` works. That is the right trade: the fields
// facility exists to make NUMBERS addressable, and widening it to
// strings would put a length and an encoding into the ABI for one
// caller. A tool that wants the name reads the whole record, which is
// what /bin/df does.
//
// `flags` rather than a bool per property, and the record carries the
// usage numbers as well, so `df` is ONE read rather than this plus
// SYS_SYSINFO -- two reads of a changing filesystem can disagree with
// each other, and a "used" from one moment beside a "total" from
// another is a number nobody can trust.
#define QUERY_FS_PERSISTENT (1u << 0) // survives a reboot; absent means RAM-only
#define QUERY_FS_MOUNTED    (1u << 1) // a filesystem is mounted at all

struct query_fsinfo {
    uint64_t used_bytes;   // meaningful only with QUERY_FS_MOUNTED
    uint64_t total_bytes;  // usable DATA space, excluding metadata
    uint64_t flags;        // QUERY_FS_*
    char     name[QUERY_NAME_MAX]; // "tfs3" -- fs_backend_name()
};

// QUERY_MEMMAP's record. One firmware-reported region.
//
// The TYPE is the multiboot number, passed through rather than
// translated: a name is a formatter's business, and inventing an enum
// here would mean two tables to keep in step for no gain.
#define QUERY_MEMMAP_USABLE   1 // the one value worth naming in the ABI

struct query_memmap {
    uint64_t base;
    uint64_t length;
    uint64_t type; // 1 = usable RAM; anything else is reserved of some kind
};

// QUERY_MMAUDIT's record -- ONE DANGLING MAPPING.
//
// A list rather than a count, because the ADDRESSES are the diagnostic.
// A summary can say a space has two violations and cannot say where the
// second one is, which is exactly the point at which "how many" stops
// being enough. Zero records is the healthy answer, and it is a
// successful read rather than an error.
//
// The cost, stated because it is unusual for a provider: counting the
// records requires the same full walk as reading one, so a read of N
// findings walks every live address space N+1 times. That is deliberate
// and it is cheap where it matters -- a HEALTHY machine has N=0 and
// pays exactly one walk. A machine with findings is already broken and
// can afford a few more.
struct query_mmaudit {
    uint64_t pid;
    uint64_t vaddr; // the mapping
    uint64_t frame; // the physical frame it points at, which pmm thinks is FREE
};

// QUERY_RANDOM's record -- how good SYS_GETRANDOM's bytes are.
//
// WHY THIS IS A FACT AND NOT PART OF SYS_GETRANDOM. That syscall
// deliberately does not report quality (see abi/syscall_abi.h), and the
// reasoning there still holds: a program that could check it per draw
// would mostly use it to carry on anyway. This is the DIAGNOSTIC
// question -- "what is this machine's entropy source?" -- which is a
// different question with a different audience, and the answer nobody
// could get from ring 3 at all. `quality` is ORDERED BY TRUST, so a
// caller may compare it; the name is for printing.
#define QUERY_RANDOM_NONE   0 // krandom_init() has not run
#define QUERY_RANDOM_JITTER 1 // TSC jitter -- WEAK under an emulator
#define QUERY_RANDOM_VIRTIO 2 // virtio-rng: the host's entropy
#define QUERY_RANDOM_HW     3 // RDSEED/RDRAND

struct query_random {
    uint64_t quality;              // QUERY_RANDOM_*, ordered by trust
    char     name[QUERY_NAME_MAX]; // "TSC jitter", "hardware (RDSEED/RDRAND)"
};

// QUERY_PARTTABLE's record -- the table, not its entries.
//
// SEPARATE FROM THE ENTRIES because a table with no partitions and NO
// TABLE AT ALL are different answers, and a list alone cannot tell them
// apart: both are zero records. This repo's own disk has no partition
// table, so that is the common case rather than a corner one.
#define QUERY_PART_NONE 0
#define QUERY_PART_MBR  1
#define QUERY_PART_GPT  2

struct query_parttable {
    uint64_t kind;        // QUERY_PART_*
    uint64_t entry_count; // how many QUERY_PARTITION records exist
    uint8_t  disk_guid[16]; // GPT only; all-zero otherwise
};

// QUERY_PARTITION's record -- one partition.
//
// MBR and GPT fields in one record rather than two classes: a caller
// switches on `kind` (carried here too, so a record is readable on its
// own) and reads the half that applies. Two classes would mean a
// program that had to ask which one to walk before walking it.
struct query_partition {
    uint64_t kind;         // QUERY_PART_*, so a record stands alone
    uint64_t lba_start;    // both flavours, in sectors
    uint64_t lba_count;    // GPT reports an END; this is the length either way
    uint64_t mbr_type;     // MBR only
    uint8_t  type_guid[16];   // GPT only
    uint8_t  unique_guid[16]; // GPT only
    char     name[40];     // GPT only; "" for MBR
};

// QUERY_HEAP's record.
//
// `quarantined` is its own field and does NOT belong to either of the
// other two: a block withdrawn by a red-zone violation is deliberately
// stranded, so used + free stops summing to total once it is nonzero.
// Reporting it separately is what stops that looking like an accounting
// bug (see api/heap.h).
struct query_heap {
    uint64_t total_bytes;       // claimed from pmm
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t quarantined_bytes;
    uint64_t rz_checks;         // red-zone checks performed since boot
    uint64_t violations;        // and how many found damage
    uint64_t debug;             // 1 when kernel.heap_debug is on
};

// QUERY_HEAPCHECK's record. `damaged` is 0 for a healthy heap, which is
// the normal answer; `checked` says how many blocks the scan looked at,
// so "0 damaged" and "nothing to look at" are distinguishable.
struct query_heapcheck {
    uint64_t damaged;
    uint64_t checked;
};

// QUERY_ATA's record.
#define QUERY_ATA_PRESENT   (1u << 0) // a drive answered IDENTIFY
#define QUERY_ATA_DMA_HW    (1u << 1) // the controller offers Bus-Master DMA
#define QUERY_ATA_DMA_ON    (1u << 2) // transfers are actually going through it
#define QUERY_ATA_TRIM      (1u << 3) // DATA SET MANAGEMENT: freed blocks are discarded

struct query_ata {
    uint64_t flags;             // QUERY_ATA_*
    uint64_t max_sectors_xfer;  // per transfer
    uint64_t sector_count;      // the drive's capacity, in sectors
};

// QUERY_KSTACK's record -- one live kernel stack.
//
// `used` is a HIGH-WATER MARK, not a current depth: it is how deep this
// stack has ever been, which is the number that says whether 16 KiB is
// enough. A current depth would be near zero for every process that is
// not running right now, i.e. all of them.
#define QUERY_KSTACK_CANARY_OK (1u << 0)
#define QUERY_KSTACK_FRAME_OK  (1u << 1) // kernel_rsp points inside this stack
// The legacy `run` loader's stack. It has no scheduler slot and so no
// pid, which is exactly why it is flagged rather than left to be
// inferred from a pid of 0 -- a reader would take that for init's.
#define QUERY_KSTACK_LEGACY    (1u << 2)

// The ABI carries its OWN name length rather than including
// api/scheduler.h: abi/ is the kernel<->userland contract and must not
// depend on a kernel API header (kernel/include/README.md). The two are
// kept in step by a _Static_assert in the provider, so a mismatch is a
// build error rather than a truncated name.
#define QUERY_KSTACK_NAME_MAX 32

struct query_kstack {
    uint64_t slot, pid, state;
    uint64_t size, used;        // bytes
    uint64_t flags;             // QUERY_KSTACK_*
    uint64_t base, guard;       // the stack, and the unmapped page below it
    uint64_t kernel_rsp, rip, cs;
    char     name[QUERY_KSTACK_NAME_MAX];
};

// QUERY_KSTACK_SYSCALL's record. The NAME travels with the number
// because the kernel's syscall-name table is strace's and has no
// ring-3 copy -- a client left to map numbers itself would grow a
// second table that drifts.
struct query_kstack_syscall {
    uint64_t nr;
    uint64_t peak;              // deepest this syscall has ever gone, bytes
    char     name[24];
};

#endif // ABI_QUERY_ABI_H
