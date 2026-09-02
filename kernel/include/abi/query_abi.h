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

// The terminals: who owns each, what is in front of it, and who is
// holding the keyboard away from it. LIST -- one record per live
// terminal, tty0 being the physical console.
//
// **IT WAS SCALAR FOR ONE COMMIT**, with a note saying it would become
// a list when there was more than one terminal. There is: a pty per
// Terminal window (docs/tty-design.md). This is the growth the registry
// is shaped for, and the reason nothing here was ever addressed as "the
// console" by name.
#define QUERY_TTY       13

// The last few hundred KEY EVENTS, as the driver saw them: the PS/2 wire
// byte, the evdev keycode, what entered the byte stream, and which
// modifiers were held. LIST, oldest retained record first.
//
// **A RING THE KERNEL KEEPS WHETHER OR NOT ANYTHING IS READING**, which
// is the whole point and the reason it is not an arm-and-drain
// interface: the question this answers is "what did the key I just
// pressed actually do", asked AFTER it did the wrong thing. An
// on-demand tap can only ever watch keys pressed from now on, so every
// use of it starts by reproducing the bug. dmesg makes the same trade.
#define QUERY_KBDTAP    14


// ONE GLYPH OF THE FONT THE MACHINE IS DRAWING WITH: its metrics, where
// its ink actually sits, and a 1-bit map of that ink. LIST -- one record
// per atlas slot, so `index` IS the slot and the set is the same 101
// glyphs everything else here uses (font_ttf.h).
//
// **WHAT IT IS FOR.** A glyph that rasterised to nothing is
// pixel-identical to a space, to a missing character and to a font that
// failed to load, and that has already cost one hunt here -- a client
// read a session-font cell as entirely blank while the kernel had
// logged 101/101 glyphs built (docs/bugs.md). "Is this glyph empty
// right now" had no answer anywhere. It does now, and `max_coverage`
// answers the harder half: a glyph can have ink and still be too faint
// to read, which no yes/no flag can distinguish.
//
// **THIS IS RING 0's VIEW, WHICH IS NOT NECESSARILY A CLIENT'S.** A GUI
// client draws from its own read-only mapping of the atlas
// (WIN_REQ_FONT), so the interesting failure is the two DISAGREEING.
// `hash` is what makes that answerable without shipping the bitmap
// across: same coverage bytes, same hash. /bin/font computes the same
// hash over the mapping it was handed and says whether they match.
#define QUERY_FONTGLYPH 15

// The ink map is 1 BIT PER PIXEL, and the coverage bytes are 8. That is
// not a shortcut, it is the split of the two questions: WHERE the ink
// is belongs to ring 0, which is where a glyph either got rasterised or
// did not, and HOW DARK it is belongs to whoever draws it. A record is
// capped at QUERY_RECORD_MAX (256 bytes, api/query.h) and coverage
// bytes for one cell blow that at any size worth looking at -- the
// header's own advice is that a class needing more is a list of smaller
// records, and a list of ROWS would need a second selector this message
// has nowhere to put.
#define QUERY_FONTGLYPH_INK_MAX 200 // bytes of ink map carried per record
#define QUERY_FONTGLYPH_MAP_W_MAX 64 // columns; wider cells are CLIPPED

#define QUERY_FONTGLYPH_FACE      (1u << 0) // a face from /usr/share/fonts,
                                             // not the baked fallback
#define QUERY_FONTGLYPH_SYNTHETIC (1u << 1) // its bold was SMEARED, not loaded
#define QUERY_FONTGLYPH_CLIPPED_W (1u << 2) // the ink map lost columns
#define QUERY_FONTGLYPH_CLIPPED_H (1u << 3) // ...and/or rows

struct query_fontglyph {
    uint32_t slot;        // atlas slot, == `index`
    uint32_t codepoint;   // what this slot draws; 32..126 plus six Latin-1
    uint32_t flags;       // QUERY_FONTGLYPH_*
    // FNV-1a over the cell's COVERAGE bytes, cell_w * cell_h of them.
    // Over the whole cell rather than the ink box, so two glyphs that
    // agree on where the ink is and disagree on its shape still differ.
    uint32_t hash;

    // The line box. `cell_h` is the stride between glyph bitmaps and
    // `line_h` is how far apart two lines sit -- DIFFERENT NUMBERS, and
    // a glyph legitimately paints below its line (api/font_face.h).
    uint16_t cell_w, cell_h;
    uint16_t line_h, baseline;
    uint16_t advance;     // THIS glyph's advance; == cell_w on a monospace set
    uint16_t px;          // em size the set was rasterised at
    uint16_t count;       // glyphs in the set

    // The ink's bounding box within the cell, x1/y1 EXCLUSIVE. All zero
    // when there is no ink, which `max_coverage == 0` is the reliable
    // test for -- an empty box and a one-pixel box at the origin are
    // otherwise the same four numbers.
    uint16_t ink_x0, ink_y0, ink_x1, ink_y1;

    // The darkest byte anywhere in the cell. 0 means the glyph is
    // ENTIRELY BLANK. A low value means it rasterised, and faintly --
    // which looks like a font bug on screen and like a healthy glyph to
    // any has-ink test.
    uint8_t  max_coverage;
    uint8_t  weight;      // enum font_weight
    uint16_t map_w, map_h; // what the ink map below actually carries

    // 1 bit per pixel, row-major, rows padded to whole bytes:
    // bit (x & 7) of ink[y * ((map_w + 7) / 8) + (x >> 3)], MSB first.
    // Set means coverage was non-zero, not "coverage was high".
    uint8_t  ink[QUERY_FONTGLYPH_INK_MAX];
};

_Static_assert(sizeof(struct query_fontglyph) <= 256,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");


// THE KERNEL LOG, in byte slices. LIST -- record `index` is the slice
// starting at `index * QUERY_KLOG_DATA`, counted from the OLDEST BYTE
// STILL RETAINED at the moment of the call.
//
// **THIS IS WHAT `dmesg` READS, and it is a fact rather than a file.**
// Linux hands the log over as a character device (/dev/kmsg, one
// record per read(), each carrying a sequence number so a reader can
// see that records aged out); FreeBSD exposes the whole ring through
// the kern.msgbuf SYSCTL instead. The second shape is the one that
// ports: there is no mount table here and SYS_QUERY is deliberately not
// /proc (docs/query-design.md), so a device node would need
// infrastructure that does not exist to buy nothing this does not
// already give.
//
// **BYTES, NOT LINES, and that is a real choice.** A record per line
// would be tidier to describe and would cost a scan of the ring per
// record -- O(n) each, O(n^2) to walk -- because klog.c stores bytes
// and a line has no index. A long line would still have to be split, so
// the tidiness would not even be complete. A reader writing the bytes
// to stdout does not care where the boundaries fall.
//
// `first` IS THE POINT OF THE RECORD. It is the absolute offset, since
// boot, that this slice's first byte came from -- so a reader walking
// several records can tell that the ring moved underneath it (the
// kernel kept logging while it read) rather than silently splicing two
// eras of the log together. Compare consecutive records: `first` must
// advance by exactly the previous record's `len`.
#define QUERY_KLOG      16

// The AHCI host bus adapter: its version, what it offers, and which
// port carries the drive. SCALAR. The ports themselves are
// QUERY_AHCI_PORT -- the same split as QUERY_PARTTABLE/QUERY_PARTITION,
// and for the same reason: one thing with one set of numbers, and rows.
#define QUERY_AHCI      18

// One record per IMPLEMENTED port. LIST -- so a machine with no AHCI
// controller has zero records, which is a successful answer.
#define QUERY_AHCI_PORT 19

// The USB devices an xHCI controller enumerated. A LIST -- one record
// per device -- read by /bin/lsusb. There is deliberately no scalar
// "controller" class beside it: `lsdev` already names the controller,
// and a second class carrying one line would be a second thing to keep
// true. A machine with no controller simply has zero records.
#define QUERY_USB       17

// Bytes of log per record. Sized so the whole record fits
// QUERY_RECORD_MAX (256, api/query.h) with the header on top.
// One record per registered block device -- disks and partitions.
// LIST. See block.h's device table and `/bin/lsblk`.
#define QUERY_BLKDEV 20

// One record per MAPPING of every live process: the image, the heap,
// the stack, and each SYS_MMAP region. LIST. `bytes` is the
// RESERVATION -- pages arrive on touch, so residency is a different
// (and unasked) question. /bin/pmap filters by pid client-side; the
// kernel enumerates everything because a list provider's index has
// nowhere to carry a second selector (QUERY_FONTGLYPH's reasoning).
#define QUERY_PROCMAP 21

// WHAT THE ACPI TABLES SAID, decoded. SCALAR. Read by /bin/acpi, and
// the reason it is a fact rather than a setting: nothing here is
// stored, it is what the firmware handed this boot.
//
// The fields worth knowing about are the ones a failed shutdown needs:
// `pm1a_cnt` (the port the S5 write goes to), `slp_typ_a` (the value
// the DSDT's `_S5_` object named), and `flags` -- ACPI_F_S5 absent
// means the machine cannot be powered off through ACPI at all and the
// legacy path is what will run.
#define QUERY_ACPI 23

// One record per ACPI table this boot found and checksummed. LIST -- a
// machine with no ACPI has zero records, which is a successful answer.
// The split from QUERY_ACPI is the same one QUERY_PARTTABLE/
// QUERY_PARTITION already makes: one thing with one set of numbers,
// and rows.
#define QUERY_ACPI_TABLE 24

// One record per logical processor the MADT lists. LIST. NOTHING IS
// RUNNING ON THEM -- this kernel is single-core, and the list is the
// first stage of docs/smp-design.md rather than evidence of the rest.
#define QUERY_CPUS 25

// The network devices: one record per registered NIC, with its
// addresses and counters. LIST. What `/bin/ifconfig` reads.
#define QUERY_NETDEV 22

// THE RAW CONFIGURATION DESCRIPTOR of each enumerated USB device, in
// slices. LIST -- one record per QUERY_USBDESC_DATA bytes of one
// device, tagged with the slot it came from and the offset within it.
//
// A LIST OF SLICES rather than one record because a configuration runs
// to hundreds of bytes and QUERY_RECORD_MAX is 256 (api/query.h, which
// says a class needing more is a list of smaller records). Same shape
// as QUERY_KLOG, and for the same reason.
//
// WHY THE RAW BYTES AND NOT A DECODED RECORD. This is what a device
// that no driver here binds has to say for itself -- the class-specific
// descriptors a class driver refused to walk are exactly the ones a
// decoded record would have had to know about in advance. The bytes
// also paste straight into a KTEST fixture, which is the only way a
// device nobody has is ever tested against.
#define QUERY_USBDESC 26

// THE RAW BYTES OF EACH ACPI TABLE, in slices. LIST, the same shape as
// QUERY_USBDESC and for the same reason: a parser for firmware data has
// to be tested against real firmware data, and a hand-written fixture
// only ever agrees with the parser written beside it.
//
// ITS OWN ENUMERATION, not QUERY_ACPI_TABLE's. The DSDT is not in the
// XSDT -- it is reached through the FADT -- so a machine's `acpi` output
// lists 22 tables and none of them is the one with the AML in it. This
// class appends it, and every record carries its table's SIGNATURE so a
// reader never has to correlate two indexes.
#define QUERY_ACPIDUMP 27

// What kernel is actually running: its version, the commit it was built
// from, and when it was compiled. SCALAR.
//
// IT EXISTS BECAUSE A PROGRAM CANNOT KNOW THIS. `about` used to print
// TOYOS_VERSION_FULL from its own build, which is right only while the
// kernel and userland come from one image -- and wrong the moment a
// machine is updated over the network a piece at a time. That is not
// hypothetical: the bare-metal laptop reported 214d29e while running
// 083cf8e, and nothing on it could say otherwise, because the kernel
// prints its version in ONE place and that place is a panic.
#define QUERY_VERSION 28

// Which drivers this build has, and what each one is driving. LIST.
//
// The per-class registries answer "what devices are present", which is
// a different question: a driver compiled in and bound to nothing
// appears in none of them, and which driver claimed a given USB device
// was recorded only in the boot log. See api/driver.h.
#define QUERY_DRIVER 29

#define QUERY_KLOG_DATA 232

struct query_klog {
    // Absolute offset of data[0], counted from the first byte ever
    // logged. NOT a ring position -- see the class comment.
    uint64_t first;
    // Bytes ever written and bytes still retained, as of this call.
    // Reported on every record rather than once, because there is no
    // "once" in a list interface and a reader that sampled them
    // separately would be comparing two different instants.
    uint64_t total;
    uint32_t retained;
    uint32_t len;                     // valid bytes in data[]
    uint8_t  data[QUERY_KLOG_DATA];
};

_Static_assert(sizeof(struct query_klog) <= 256,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

// QUERY_USBDESC's record -- one slice of one device's configuration.
// Bytes of descriptor per record; the header above it makes 256.
#define QUERY_USBDESC_DATA 232

struct query_usbdesc {
    uint64_t slot;       // xHCI slot id -- which device this slice is from
    uint32_t total;      // wTotalLength of that device's whole configuration
    uint32_t offset;     // byte offset of data[0] within it
    uint32_t len;        // valid bytes in data[]
    uint32_t reserved;
    uint8_t  data[QUERY_USBDESC_DATA];
};

_Static_assert(sizeof(struct query_usbdesc) <= 256,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

// QUERY_ACPIDUMP's record -- one slice of one table.
#define QUERY_ACPIDUMP_DATA 224

struct query_acpidump {
    uint64_t table;      // index in this class's own enumeration
    uint32_t total;      // that table's whole length, header included
    uint32_t offset;     // byte offset of data[0] within it
    uint32_t len;        // valid bytes in data[]
    uint32_t reserved;
    char     signature[8];   // NUL-terminated, unlike in the table itself
    uint8_t  data[QUERY_ACPIDUMP_DATA];
};

_Static_assert(sizeof(struct query_acpidump) <= 256,
               "a query record must fit QUERY_RECORD_MAX -- see api/query.h");

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
    uint64_t phys_usable_bytes; // RAM the firmware map calls usable, UNCAPPED --
                                // frame_total * frame_bytes is what is managed
    // The share of frame_total/frame_free at or above 4 GiB: managed,
    // mapped and used by the kernel heap, but not yet by ring-3 pages.
    // A reader wanting "what a process can get today" subtracts them.
    // APPENDED, so an older caller's shorter record still reads the
    // fields above.
    uint64_t frame_total_high;
    uint64_t frame_free_high;
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

#define QUERY_FS_RDONLY     (1u << 2) // mounted read-only; a write is refused
#define QUERY_FS_ROOT       (1u << 3) // this is the root mount

// ONE RECORD PER MOUNT, root first. It was a scalar while there was one
// filesystem; Real mount points made that a lie the moment /boot was
// mounted, and `df` printing one line for a machine with three
// filesystems is the shape of wrong answer nobody notices. Index 0 is
// still the root, so a reader that only ever asks for record 0 keeps
// getting exactly what it used to.
struct query_fsinfo {
    uint64_t used_bytes;   // meaningful only with QUERY_FS_MOUNTED
    uint64_t total_bytes;  // usable DATA space, excluding metadata
    uint64_t flags;        // QUERY_FS_*
    char     name[QUERY_NAME_MAX]; // "tfs3" -- the backend
    char     point[64];    // "/" or "/boot" -- FS_PATH_MAX, spelled out since abi/ has no fs.h
    char     device[QUERY_NAME_MAX]; // "ata3", "virtio-blk2", or "" for a backend with no volume
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

// QUERY_VERSION's record. Fixed char arrays rather than pointers: a
// query copies a record to ring 3, and a pointer into kernel .rodata is
// not something ring 3 can follow.
// QUERY_DRIVER's record. `devices` is space-separated and may be empty,
// which is a real answer -- "in this build, driving nothing".
struct query_driver {
    char name[16];      // "ahci", "r8153"
    char cls[12];       // "block", "net", "input", ...
    char file[64];      // the source file it declared itself in, for -v
    char desc[48];      // one line saying what it is, or "" -- also -v
    char devices[64];   // "net0 usb:13", or "" for none
};

struct query_version {
    char version[16];   // "0.3.0-dev" -- TOYOS_VERSION
    char build_id[24];  // "426601f", or "426601f-dirty", or "unknown"
    char stamp[24];     // "2026-09-01 10:39:12", to the second
};

struct query_random {
    uint64_t quality;              // QUERY_RANDOM_*, ordered by trust
    char     name[QUERY_NAME_MAX]; // "TSC jitter", "hardware (RDSEED/RDRAND)"
};

// QUERY_BLKDEV's record -- one per registered block device, disks and
// partitions alike. A LIST: the table's shape is the whole answer, and
// what is in it depends on the machine.
//
// Every disk a driver found is here even when it carries nothing that
// is mounted, which is the point of the table (block.h): a device that
// is not enumerated cannot be named, and one that cannot be named
// cannot be mounted.
struct query_blkdev {
    char name[16];      // "ata0", "ahci0p1" -- what `mount` and `root=` take
    char parent[16];    // the disk a partition sits on; empty for a disk
    uint64_t sectors;   // 512 bytes each
    uint64_t base_lba;  // where it starts on its parent; 0 for a disk
    uint64_t is_root;   // 1 if this is the device the root is mounted from
    uint64_t persistent;// 0 for a RAM-backed live image
};

// QUERY_NETDEV's record -- one registered network device.
//
// The counters are the CORE's, not the driver's, so "received" means
// "reached the stack" rather than "the hardware saw something". A card
// whose rx_packets climbs while rx_dropped climbs with it is being
// handed frames faster than net_poll() drains them, which is a
// different fault from a silent one.
struct query_netdev {
    char name[16];       // "net0"
    char driver[16];     // "e1000", "virtio-net"
    uint64_t mac;        // six bytes, low-order first (mac[0] is bits 0-7)
    uint64_t ip;         // host byte order; 0 means unconfigured
    uint64_t netmask;
    uint64_t gateway;
    uint64_t mtu;
    uint64_t rx_packets, rx_bytes, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_dropped;
    // LINK STATE, three-valued. `link_known` 0 means the driver has no
    // way to ask -- which is not the same as "down", and `ifconfig` says
    // nothing rather than guessing. `link_bps` is what the WIRE
    // negotiated, so a gigabit adapter on a USB 2 port still says
    // 1000000000.
    uint64_t link_known;
    uint64_t link_up;
    uint64_t link_bps;
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

    // The WHOLE DISK's size, in 512-byte sectors -- not the mounted
    // volume's, which is what `df` reports and which is smaller than
    // this once a filesystem lives in a partition. Carried on the
    // table's record because a partition layout cannot be reasoned
    // about without it: `mkpart` needs it to place `rest`, and a
    // reader needs it to say how much of the disk is unallocated.
    uint64_t disk_sectors;
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

// QUERY_USB's record -- one enumerated USB device.
//
// BOTH THE IDS AND THE STRINGS ARE CARRIED, because they answer
// different questions and can legitimately disagree. The ids are what
// /usr/share/hwdata/usb.ids is keyed on; the strings are what the
// DEVICE says about itself, which PCI has no equivalent of. `lsusb`
// prints the database name and shows the strings under -v, which is
// what real lsusb does.
//
// Empty strings are normal: a device is not required to have any.
struct query_usb {
    uint64_t port;          // 1-based root port
    uint64_t slot;          // xHCI slot id
    uint64_t speed;         // QUERY_USB_SPEED_*
    uint64_t vendor_id;
    uint64_t product_id;
    uint64_t dev_class;     // from the device descriptor; often 0
    uint64_t if_class;      // the interface a driver bound, if any
    uint64_t if_subclass;
    uint64_t if_protocol;
    uint64_t bound;         // 1 when a driver in this build claimed it
    char     manufacturer[32];
    char     product[32];
};

// QUERY_USB's speed values. Named rather than passing the xHCI protocol
// speed ID through, because that one is redefinable per controller.
#define QUERY_USB_SPEED_UNKNOWN 0
#define QUERY_USB_SPEED_LOW     1
#define QUERY_USB_SPEED_FULL    2
#define QUERY_USB_SPEED_HIGH    3
#define QUERY_USB_SPEED_SUPER   4

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

// QUERY_AHCI's record.
#define QUERY_AHCI_PRESENT (1u << 0) // a controller was found and mapped
#define QUERY_AHCI_DRIVE   (1u << 1) // a SATA drive answered IDENTIFY
#define QUERY_AHCI_IRQ     (1u << 2) // completions arrive by interrupt
#define QUERY_AHCI_64BIT   (1u << 3) // CAP.S64A
#define QUERY_AHCI_NCQ     (1u << 4) // CAP.SNCQ -- advertised, and not used
#define QUERY_AHCI_SSS     (1u << 5) // CAP.SSS: staggered spin-up
#define QUERY_AHCI_LBA48   (1u << 6) // the DRIVE's addressing, not the HBA's
#define QUERY_AHCI_TRIM    (1u << 7) // DATA SET MANAGEMENT: freed blocks are discarded

#define QUERY_AHCI_MODEL_MAX 48 // IDENTIFY's 40 characters, rounded up

struct query_ahci {
    uint64_t flags;             // QUERY_AHCI_*
    uint64_t version;           // the VS register: 0x00010301 is 1.3.1
    uint64_t ports_impl;        // implemented ports, not ports with a drive
    uint64_t command_slots;     // CAP.NCS + 1; this driver uses one
    uint64_t active_port;       // the port carrying the block device
    uint64_t irq;               // PIC line, 0 when polled
    uint64_t sector_count;
    uint64_t max_sectors_xfer;
    char     model[QUERY_AHCI_MODEL_MAX];
};

// QUERY_AHCI_PORT's record. `port` is the HARDWARE's number and the
// record index is a position in the implemented list -- they differ
// whenever PI has a gap, which is why both exist.
#define QUERY_AHCI_PORT_DEVICE  (1u << 0) // DET says the link is up to a device
#define QUERY_AHCI_PORT_RUNNING (1u << 1) // PxCMD.ST and .FRE are both set
#define QUERY_AHCI_PORT_ACTIVE  (1u << 2) // this port carries the block device

struct query_ahci_port {
    uint64_t port;
    uint64_t flags;             // QUERY_AHCI_PORT_*
    uint64_t det;               // PxSSTS.DET: 3 = present, link established
    uint64_t ipm;               // PxSSTS.IPM: 1 = active
    uint64_t speed;             // PxSSTS.SPD: 1/2/3 = 1.5/3/6 Gbps
    uint64_t signature;         // PxSIG: 0x00000101 is a SATA disk
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

// QUERY_TTY's record.
//
// **THREE PARTIES, NOT ONE, and that is the whole reason this exists.**
// A terminal with an owner and no foreground group, one nobody owns
// while a compositor holds the keyboard, and one nobody owns at all are
// three different machines, and from outside they all look like "typing
// does nothing". `tty` prints them apart.
//
// The pids are 64-bit because a named field would have to be
// (api/query.h). This class declares NO named fields -- a LIST is
// deliberately not addressable as `tty.owner_pid`, because an index
// baked into a name means a different record a second later, which is
// sysctl's worst corner and why Linux keeps processes out of it.
#define QUERY_TTY_COMPOSITOR (1u << 0) // a compositor owns the keyboard
#define QUERY_TTY_CLAIMED    (1u << 1) // a ring-3 process is reading fd 0
#define QUERY_TTY_SUSPENDED  (1u << 2) // ring 0's blocking readers stood down
#define QUERY_TTY_BYPASS     (1u << 3) // this terminal's discipline is muted

struct query_tty {
    uint64_t owner_pid;       // the process that owns it; 0 = nobody
    uint64_t foreground_pgid; // the group an interrupt reaches; 0 = none
    uint64_t compositor_pid;  // the registered compositor; 0 = none
    uint64_t flags;           // QUERY_TTY_*
    // Appended when this class became a LIST. The four above kept their
    // offsets, which is the append-only rule this whole header runs on.
    uint64_t index;           // 0 is the physical console
    uint64_t lflag;           // TTY_* (abi/tty_abi.h) -- what the
                              // discipline is doing: canonical, echoing,
                              // signal-generating
    char     driver[QUERY_NAME_MAX]; // "console", "pty"
};

// QUERY_KBDTAP's record -- ONE KEY EVENT, at every stage at once.
//
// The four columns are four different encodings of the same keypress,
// and a keyboard bug is almost always one stage disagreeing with the
// next: the wire said the right thing and the keycode is wrong (a hole
// in a translation table), or the keycode is right and the character is
// wrong (the layout file). Reading them one at a time cannot show that,
// which is why they are one record rather than four facts.
//
// A LIST, so NO named fields (see api/query.h): an index baked into a
// name would mean a different keypress a second later.

// `wire` carries the 0xE0 prefix as a FLAG rather than as a second
// record, because the prefix is not an event -- nothing happened when
// it arrived, and a reader counting keypresses must not see two.
#define QUERY_KBDTAP_EXTENDED (1u << 0) // the byte followed an 0xE0 prefix
#define QUERY_KBDTAP_DOWN     (1u << 1) // a press; absent means a release

// How many codes one event can put into the byte stream. TWO, because
// Alt-<key> is encoded terminal-style as ESC then the key (api/
// keyboard.h) and that is one keypress producing two bytes. Nothing
// here produces three.
#define QUERY_KBDTAP_PRODUCED_MAX 2

struct query_kbdtap {
    // MONOTONIC AND NEVER REUSED, which is what makes a ring readable
    // through a snapshot interface: a reader remembers the last seq it
    // printed, so a record it has already seen is recognisable and a
    // GAP is a burst it missed rather than a silent loss.
    uint64_t seq;
    uint64_t ticks;      // pit_ticks() when it arrived (api/timer.h)
    uint64_t flags;      // QUERY_KBDTAP_*
    // The PS/2 wire byte, INCLUDING its release bit -- 0x1E is A down
    // and 0x9E is A up, which is what the wire really said. **ZERO
    // MEANS THE KEY DID NOT ARRIVE OVER PS/2 AT ALL** (0x00 is not a
    // scancode), and that is a fact worth reading rather than a hole:
    // a virtio-input or USB keyboard has no scancodes to report, and a
    // blank column names which driver you are debugging.
    uint64_t wire;
    uint64_t keycode;    // Linux evdev (kernel/input.h's INPUT_KEY_*)
    uint64_t mods;       // KEY_MOD_* held when it was processed
    // What this event put into the console byte stream: the KEY_* codes
    // and characters of api/keyboard.h. Zero-filled past `produced`,
    // and a `produced` of 0 is honest -- a modifier, a release, and a
    // key this layout does not map all produce nothing.
    uint64_t produced;   // how many of the two below are meaningful
    uint64_t produced_code[QUERY_KBDTAP_PRODUCED_MAX];
};


// --- QUERY_PROCMAP records -------------------------------------------

#define QUERY_PROCMAP_IMAGE 1
#define QUERY_PROCMAP_HEAP  2
#define QUERY_PROCMAP_STACK 3
#define QUERY_PROCMAP_ANON  4 // SYS_MMAP, MAP_ANONYMOUS
#define QUERY_PROCMAP_FILE  5 // SYS_MMAP, file-backed

struct query_procmap {
    uint64_t pid;
    uint64_t kind;     // QUERY_PROCMAP_*
    uint64_t base;     // first virtual address
    uint64_t bytes;    // the reservation's span, not residency
    uint64_t prot;     // SYS_PROT_* for the mmap kinds, else 0
    uint64_t file_off; // QUERY_PROCMAP_FILE only
    char     path[64]; // likewise; FS_PATH_MAX's 64, not the field-name 48
};


// --- QUERY_ACPI / QUERY_ACPI_TABLE / QUERY_CPUS records ---------------

// EVERY FIELD IS u64 because every NAMED field must be (api/query.h),
// and a struct where some fields are addressable by name and others
// are not is a distinction nobody can see from `config get acpi.<tab>`.
struct query_acpi {
    uint64_t flags;          // ACPI_F_* (kernel/acpi.h) -- the kernel-side names
    uint64_t rsdp_source;    // ACPI_RSDP_*: 0 none, 1 multiboot2 tag, 2 BIOS scan
    uint64_t rsdp_revision;  // 0 for ACPI 1.0, 2 for 2.0+
    uint64_t table_count;
    uint64_t rsdt_phys;      // the RSDT or XSDT actually walked
    uint64_t dsdt_phys;
    uint64_t pm1a_cnt;       // I/O ports; 0 on a hardware-reduced platform
    uint64_t pm1b_cnt;
    uint64_t smi_cmd;
    uint64_t acpi_enable;    // the byte written to smi_cmd to enter ACPI mode
    uint64_t slp_typ_a;      // from `_S5_`; meaningless unless the S5 flag is set
    uint64_t slp_typ_b;
    uint64_t reset_space;    // 0 system memory, 1 system I/O
    uint64_t reset_addr;
    uint64_t reset_value;
    uint64_t sleep_control_space;
    uint64_t sleep_control_addr;
    uint64_t lapic_phys;
    uint64_t ioapic_count;
    uint64_t cpu_count;
};

struct query_acpi_table {
    uint64_t address;
    uint64_t length;
    uint64_t revision;
    // NUL-terminated here, unlike in the table itself, where all three
    // are fixed-width and a full-length value has no terminator.
    char     signature[8];
    char     oem_id[8];
    char     oem_table_id[12];
};

struct query_cpu {
    uint64_t acpi_id;
    uint64_t apic_id;
    uint64_t flags;   // ACPI_CPU_* (kernel/acpi.h)
};

#endif // ABI_QUERY_ABI_H
