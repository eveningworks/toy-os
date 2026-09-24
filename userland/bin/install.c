// install -- put this running system onto another disk, and make it boot.
//
// Five steps, each of which is an existing command's operation done
// through the same syscall it uses: partition (SYS_MKPART), format
// (SYS_MKFS), mount (SYS_MOUNT), copy (ufileop), write the bootloader
// (SYS_INSTALL_BOOT). Nothing here is a special path into the kernel --
// an installer that needed one would be an installer whose steps could
// not be checked by hand.
//
// THE LAYOUT IS THE ONE THIS DISK ALREADY HAS, and that is a constraint
// rather than a preference: `core.img` carries its prefix baked in at
// the host's `grub-mkimage` time -- `(hd0,gpt2)/boot/grub` -- so the
// copy this installs only finds its config if the ESP is partition 2 on
// the target as well. Anaconda and the Debian installer generate a
// fresh core image per target instead; toy-os has no `grub-mkimage`,
// so it reproduces the layout that the image it copies expects.
//
//   p1  1 MiB   BIOS boot    GRUB's core.img, no filesystem
//   p2  64 MiB  ESP (FAT32)  the kernel, grub.cfg, the modules
//   p3  rest    TFS3         the root
//
// `--mbr` WRITES THE OTHER LAYOUT, and it exists because a GPT disk
// booted by a legacy BIOS is the combination consumer firmware most
// often refuses -- a machine can be in CSM mode, have the disk in its
// boot order, and still not touch it. MBR has no BIOS-boot type, so
// there the core image goes in the gap before the first partition and
// the FAT32 partition is the first one, marked ACTIVE:
//
//   gap 1 MiB   (no entry)   GRUB's core.img, sectors 1..2047
//   p1  64 MiB  FAT32        the kernel, grub.cfg, the modules  [active]
//   p2  rest    TFS3         the root
//
// That moves the boot partition from 2 to 1 and the table format from
// gpt to msdos, and BOTH are baked into core.img's prefix -- which is
// why the build stages a second core image (`core-msdos.img`,
// tools/install_grub.py) rather than this choosing at install time.
//
// WHAT IT REFUSES: the disk this machine is running from. An installer
// handed the running root is an installer being asked to saw off its own
// branch, and there is no reading of "reinstall over myself" that ends
// with a working machine.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "partition_abi.h"
#include "mount_abi.h"
#include "query_abi.h"
#include "errno.h"      // EEXIST -- an existing directory is not a failure
#include "lib/cmd.h"
#include "lib/ufileop.h"
#include "lib/human.h"
#include "lib/dirsort.h"
#include "lib/ufile.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define USAGE \
    "install [--disk <name>] [--esp <MiB>] [--mbr] confirm\n" \
    "install --bootloader confirm\n" \
    "  --disk <name>  the target (`lsblk`); refuses the one this machine runs from\n" \
    "  --esp <MiB>    size of the FAT32 /boot partition (default 64)\n" \
    "  --mbr          write an MBR table instead of GPT, for firmware that\n" \
    "                 will not boot a GPT disk in legacy/CSM mode\n" \
    "  --bootloader   rewrite THIS machine's bootloader in place and change\n" \
    "                 nothing else -- no partitioning, no files touched\n" \
    "  confirm        required -- this ERASES the target disk"

#define SECTOR_BYTES 512
#define BIOS_BOOT_SECTORS 2048     // 1 MiB, and where GRUB's core.img goes
#define FIRST_LBA         2048     // the 1 MiB alignment every modern tool uses

#define TARGET_ROOT "/mnt"
#define TARGET_BOOT "/mnt/boot"

// THE PAYLOAD, and it is in the ROOT rather than in /boot on purpose.
// A LIVE boot has no /boot at all -- GRUB loads the kernel and a
// filesystem image into RAM, and nothing drives the medium afterwards
// (there is no USB mass-storage driver) -- and installing from live
// media is how a real machine gets toy-os. Every toy-os filesystem
// carries this directory, so there is ONE path here rather than one per
// boot kind. Staged by tools/install_grub.py --stage-payload.
#define PAYLOAD "/install"

// WHAT THE INSTALLED BOOTLOADER CAN DO, one line of GRUB module names,
// written by `--bootloader`. In /etc because it describes this machine's
// configuration rather than the build -- and because a flash sends /etc
// NEW FILES ONLY, so an update never overwrites what the machine itself
// last recorded here.
#define STAMP_PATH "/etc/grub-core.modules"

// WHAT IS NOT COPIED. `/boot` is a mount point -- for the source's ESP on
// a disk boot, and for nothing at all on a live one -- and the target's
// is written from PAYLOAD instead; `/mnt` is where the target itself is
// mounted, so copying it would copy the target into itself; `/tmp` is
// scratch by definition. Each is recreated empty on the target, because
// a mount point has to exist.
static const char *const SKIP[] = { "boot", "mnt", "tmp" };

// ~30 KB, and a ring-3 frame holds 2 KiB -- file scope, like every other
// ufileop caller (see ufileop.h).
static struct ufileop g_op;

static int is_skipped(const char *name) {
    for (unsigned i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++)
        if (strcmp(name, SKIP[i]) == 0) return 1;
    return 0;
}

static void step(const char *what) { printf("install: %s\n", what); }

// cmd_fail() appends the errno the last failing syscall left, which is
// the right thing when one did and NOISE when none did -- a disk name
// that does not exist reported as "no such disk: out of range". This is
// for the refusals this program makes on its own.
static void refuse(const char *why) { printf("install: %s\n", why); }

// The disk the ROOT is on, by name -- what this refuses to install onto.
// QUERY_BLKDEV flags the root's own device, which is a partition, so the
// answer is that entry's parent.
static int root_disk(char *out, int cap) {
    struct query_blkdev b;
    for (int i = 0; sys_query_record(QUERY_BLKDEV, i, &b, sizeof b) >= (int)sizeof b; i++) {
        if (!b.is_root) continue;
        snprintf(out, (size_t)cap, "%s", b.parent[0] ? b.parent : b.name);
        return 1;
    }
    return 0;
}

static uint64_t disk_sectors(const char *name, uint64_t *block_size) {
    struct query_blkdev b;
    for (int i = 0; sys_query_record(QUERY_BLKDEV, i, &b, sizeof b) >= (int)sizeof b; i++) {
        if (strcmp(b.name, name) != 0) continue;
        if (b.parent[0]) return 0;   // a partition, not a disk
        *block_size = b.block_size ? b.block_size : SECTOR_BYTES;
        return b.sectors;
    }
    return 0;
}

// A whole file into a fresh buffer (lib/ufile.h). The caller frees it.
// Used for boot.img (512 bytes) and core.img (tens of KiB), neither of
// which belongs on a 2 KiB frame. No cap: these are our own artifacts.
static void *slurp(const char *path, uint64_t *out_size) {
    uint8_t *buf = 0;
    size_t len = 0;
    if (ufile_slurp(path, 0, &buf, &len) != UFILE_OK) return 0;
    *out_size = len;
    return buf;
}

// One partition table: BIOS boot, ESP, and the root taking the rest.
static int partition(const char *disk, uint64_t sectors, uint64_t esp_mib, int mbr) {
    struct mkpart_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.device, sizeof req.device, "%s", disk);
    req.flags = MKPART_CONFIRM;

    uint64_t esp = esp_mib * 1024 * 1024 / SECTOR_BYTES;

    if (mbr) {
        // No BIOS-boot partition: core.img lives in the gap below
        // FIRST_LBA, which is why the boot partition is p1 here.
        //
        // IT KEEPS THE ESP ROLE, and therefore MBR type 0xEF, even
        // though nothing here will ever be booted by UEFI. The type is
        // what `partition_is_firmware()` reads to keep a partition OUT
        // of the root scan, and typing it 0x0C instead made the boot
        // scan mount /boot as the root -- measured, not guessed. It
        // costs nothing: a BIOS boots this disk through boot.img in the
        // MBR, which never looks at a partition's type.
        req.kind = MKPART_KIND_MBR;
        req.count = 2;
        uint64_t usable = sectors - FIRST_LBA;
        if (usable <= esp) {
            refuse("the disk is too small for the boot partition and a root");
            return 0;
        }
        req.entries[0].start_lba = FIRST_LBA;
        req.entries[0].sectors = esp;
        req.entries[0].role = MKPART_ROLE_ESP;   // also what marks it ACTIVE
        snprintf(req.entries[0].name, sizeof req.entries[0].name, "boot");

        req.entries[1].start_lba = FIRST_LBA + esp;
        req.entries[1].sectors = usable - esp;
        req.entries[1].role = MKPART_ROLE_DATA;
        snprintf(req.entries[1].name, sizeof req.entries[1].name, "toyos");

        if (sys_mkpart(&req) < 0) { cmd_fail("install", "mkpart"); return 0; }
        return 1;
    }

    req.kind = MKPART_KIND_GPT;
    req.count = 3;

    // GPT keeps its backup header and entry array in the last 33.
    uint64_t usable = sectors - FIRST_LBA - 33;
    if (usable <= BIOS_BOOT_SECTORS + esp) {
        refuse("the disk is too small for a 1 MiB boot partition, the ESP and a root");
        return 0;
    }

    req.entries[0].start_lba = FIRST_LBA;
    req.entries[0].sectors = BIOS_BOOT_SECTORS;
    req.entries[0].role = MKPART_ROLE_BIOS_BOOT;
    snprintf(req.entries[0].name, sizeof req.entries[0].name, "BIOS boot");

    req.entries[1].start_lba = FIRST_LBA + BIOS_BOOT_SECTORS;
    req.entries[1].sectors = esp;
    req.entries[1].role = MKPART_ROLE_ESP;
    snprintf(req.entries[1].name, sizeof req.entries[1].name, "EFI System");

    req.entries[2].start_lba = req.entries[1].start_lba + esp;
    req.entries[2].sectors = usable - BIOS_BOOT_SECTORS - esp;
    req.entries[2].role = MKPART_ROLE_DATA;
    snprintf(req.entries[2].name, sizeof req.entries[2].name, "toyos");

    if (sys_mkpart(&req) < 0) { cmd_fail("install", "mkpart"); return 0; }
    return 1;
}

static int format_one(const char *part, const char *type) {
    struct mkfs_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", part);
    snprintf(req.fstype, sizeof req.fstype, "%s", type);
    req.flags = MKFS_CONFIRM;
    if (sys_mkfs(&req) < 0) { cmd_fail("install", part); return 0; }
    return 1;
}

static int mount_one(const char *part, const char *point) {
    struct mount_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", part);
    snprintf(req.point, sizeof req.point, "%s", point);
    if (sys_mount(&req) < 0) { cmd_fail("install", point); return 0; }
    return 1;
}

// An error in one file does not abandon the other thousand -- ufileop's
// contract, and coreutils'. They are printed, and the overall result
// still says failed.
static void copy_error(void *ctx, const char *path, int err) {
    (void)ctx;
    cmd_fail_err("install", path, err);
}

static const struct ufileop_policy COPY_POLICY = {
    .ctx = 0, .on_conflict = 0, .on_progress = 0, .on_error = copy_error,
};

// Everything at the root except the three mount points, then those three
// recreated empty. A directory at a time rather than one copy of "/",
// because "/" contains the target.
static int copy_system(void) {
    struct sys_dirent *ents = malloc(sizeof(struct sys_dirent) * SYS_LISTDIR_MAX);
    if (!ents) { refuse("out of memory"); return 0; }
    int n = sys_listdir("/", ents, SYS_LISTDIR_MAX);
    if (n < 0) { free(ents); cmd_fail("install", "/"); return 0; }
    // Sorted so the progress this prints is the same on two machines,
    // which is what makes a transcript of a failed install comparable.
    dirsort(ents, n, DIRSORT_NAME, 0);

    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (is_skipped(ents[i].name)) continue;
        char src[64], dst[64];
        snprintf(src, sizeof src, "/%s", ents[i].name);
        snprintf(dst, sizeof dst, TARGET_ROOT "/%s", ents[i].name);
        printf("  %s\n", src);
        if (ufileop_copy(src, dst, &g_op, &COPY_POLICY) != UFILEOP_OK) ok = 0;
    }
    free(ents);

    for (unsigned i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++) {
        char dst[64];
        snprintf(dst, sizeof dst, TARGET_ROOT "/%s", SKIP[i]);
        mkdir(dst, 0755);   // already there is not an error
    }
    return ok;
}

// The target's ESP: the kernel and grub.cfg, at the paths GRUB's own
// prefix names. Written from PAYLOAD rather than copied from `/boot`,
// which does not exist on a live boot.
//
// GRUB's MODULE DIRECTORY IS NOT COPIED. `core.img` already carries
// every module grub.cfg's `insmod` asks for, so the target boots without
// it; what that target cannot do is have a HOST `grub-install` run
// against it later without re-copying them.
static int copy_boot(void) {
    if (mkdir(TARGET_BOOT "/boot", 0755) < 0 && sys_errno() != EEXIST) {
        cmd_fail("install", TARGET_BOOT "/boot");
        return 0;
    }
    if (mkdir(TARGET_BOOT "/boot/grub", 0755) < 0 && sys_errno() != EEXIST) {
        cmd_fail("install", TARGET_BOOT "/boot/grub");
        return 0;
    }
    static const char *const FILES[][2] = {
        { PAYLOAD "/kernel.bin", TARGET_BOOT "/boot/kernel.bin" },
        { PAYLOAD "/grub.cfg",   TARGET_BOOT "/boot/grub/grub.cfg" },
    };
    for (unsigned i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        printf("  %s\n", FILES[i][0]);
        if (ufileop_copy(FILES[i][0], FILES[i][1], &g_op, &COPY_POLICY) != UFILEOP_OK)
            return 0;
    }
    return 1;
}

// Is this filesystem one that can install itself? A build made without
// GRUB's BIOS target stages no payload -- it boots perfectly and cannot
// do this, which is worth saying before erasing a disk rather than after
// the copy.
static int payload_present(int mbr) {
    const char *const NEED[] = {
        PAYLOAD "/kernel.bin", PAYLOAD "/grub.cfg",
        PAYLOAD "/boot.img",
        // Checked BEFORE the disk is erased, and it is the image this
        // run will actually use: a build staged without the MBR one
        // must refuse `--mbr` rather than partition and then discover it.
        mbr ? PAYLOAD "/core-msdos.img" : PAYLOAD "/core.img",
    };
    struct sys_stat st;
    for (unsigned i = 0; i < sizeof NEED / sizeof NEED[0]; i++) {
        if (sys_stat(NEED[i], &st) < 0 || st.is_dir || st.size == 0) {
            printf("install: %s is missing -- this system carries no install "
                   "payload and cannot install itself.\n", NEED[i]);
            return 0;
        }
    }
    return 1;
}

// `in_use` is the INSTALL_BOOT_CONFIRM the kernel wants before it writes
// a boot sector on a disk something is mounted from -- false for an
// install (the target is unmounted by then) and true for a refresh,
// where the disk being written is the one this program is running from.
static int write_bootloader(const char *disk, int mbr, int in_use) {
    struct install_boot_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.device, sizeof req.device, "%s", disk);
    if (in_use) req.flags = INSTALL_BOOT_CONFIRM;

    uint64_t boot_size = 0, core_size = 0;
    void *boot = slurp(PAYLOAD "/boot.img", &boot_size);
    if (!boot) { cmd_fail("install", PAYLOAD "/boot.img"); return 0; }
    // The prefix baked into a core image names the table format AND the
    // partition number, so the layout chosen above decides which of the
    // two staged images can find its grub.cfg.
    const char *core_path = mbr ? PAYLOAD "/core-msdos.img" : PAYLOAD "/core.img";
    void *core = slurp(core_path, &core_size);
    if (!core) { free(boot); cmd_fail("install", core_path); return 0; }

    req.boot_img = (uint64_t)(uintptr_t)boot;
    req.boot_size = boot_size;
    req.core_img = (uint64_t)(uintptr_t)core;
    req.core_size = core_size;

    int rc = sys_install_boot(&req);
    free(boot);
    free(core);
    if (rc < 0) { cmd_fail("install", disk); return 0; }
    return 1;
}

// THE BOOTLOADER, REWRITTEN IN PLACE -- `grub-install` on a machine
// that is already installed, and nothing else. No table is written, no
// partition is formatted and no file is copied: the 185 KiB core image
// goes into the embed area it already occupies and the boot sector is
// re-patched to point at it.
//
// IT EXISTS BECAUSE AN INSTALLED MACHINE COULD NOT OTHERWISE GAIN A
// BOOTLOADER IT DOES NOT ALREADY HAVE. A laptop installed before
// `gzio` joined CORE_MODULES read a gzipped kernel as raw bytes -- `no
// multiboot header found`, and the default menu entry was dead until
// the rescue entry was picked by hand. The full installer cannot help:
// it ERASES the disk and refuses the one the machine runs from.
//
// THE RUNNING DISK IS THE ONLY TARGET, and that is what makes the table
// format knowable: QUERY_PARTTABLE answers for this machine's disk, and
// the prefix baked into each staged core image (`(hd0,gpt2)` against
// `(hd0,msdos1)`) has to match it or GRUB comes up at a rescue prompt
// having found no config. Another disk is the full installer's job.
static int refresh_bootloader(int confirmed) {
    char disk[16] = "";
    if (!root_disk(disk, sizeof disk)) {
        refuse("this machine has no disk-backed root -- there is no "
               "bootloader here to rewrite.");
        return 1;
    }

    struct query_parttable t;
    if (sys_query_record(QUERY_PARTTABLE, 0, &t, sizeof t) < (int)sizeof t ||
        t.kind == QUERY_PART_NONE) {
        refuse("this disk has no partition table -- refusing to guess "
               "where the core image goes.");
        return 1;
    }
    int mbr = (t.kind == QUERY_PART_MBR);

    if (!payload_present(mbr)) return 1;

    if (!confirmed) {
        printf("install: this rewrites %s's bootloader in place (%s layout):\n",
               disk, mbr ? "MBR" : "GPT");
        printf("  boot sector    LBA 0, keeping this disk's partition table\n");
        printf("  core image     %s\n",
               mbr ? PAYLOAD "/core-msdos.img" : PAYLOAD "/core.img");
        printf("install: partitions, filesystems and files are untouched. "
               "The core image is\n");
        printf("install: read back before the boot sector is written, so a "
               "bad write leaves the\n");
        printf("install: bootloader you have now. Re-run with `confirm` if "
               "that is what you want.\n");
        return 1;
    }

    step("writing the bootloader");
    if (!write_bootloader(disk, mbr, 1)) {
        printf("install: FAILED -- %s still has the bootloader it had.\n", disk);
        return 1;
    }

    // WHAT WENT IN, WRITTEN DOWN. The core image lives in raw sectors
    // nothing can open and is lzma-compressed besides, so its module set
    // cannot be recovered by looking at the disk. This stamp is the only
    // record, and `remote.py flash` reads it before sending a kernel
    // whose unpacking GRUB has to do.
    uint64_t mod_size = 0;
    void *mods = slurp(PAYLOAD "/core.modules", &mod_size);
    if (mods) {
        int fd = open(STAMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, mods, (size_t)mod_size);
            close(fd);
        } else {
            printf("install: could not write %s -- the bootloader is in "
                   "place, but nothing records what it can do.\n", STAMP_PATH);
        }
        free(mods);
    }

    printf("install: done. %s boots the newly written GRUB from now on; "
           "nothing else changed.\n", disk);
    return 0;
}

int main(int argc, char **argv) {
    const char *disk = 0;
    uint64_t esp_mib = 64;
    int confirmed = 0;
    int mbr = 0;
    int bootloader = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) { disk = argv[++i]; continue; }
        if (strcmp(argv[i], "--esp") == 0 && i + 1 < argc) { esp_mib = (uint64_t)atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "--mbr") == 0) { mbr = 1; continue; }
        if (strcmp(argv[i], "--bootloader") == 0) { bootloader = 1; continue; }
        if (strcmp(argv[i], "confirm") == 0) { confirmed = 1; continue; }
        cmd_usage(USAGE);
        return 1;
    }
    if (bootloader) {
        // The target is this machine's own disk and the layout is read
        // from it, so neither flag has a meaning here -- refused rather
        // than ignored, since a `--mbr` silently doing nothing is how
        // somebody ends up believing they chose a layout.
        if (disk || mbr) {
            refuse("--bootloader rewrites THIS machine's bootloader; "
                   "--disk and --mbr do not apply.");
            return 1;
        }
        return refresh_bootloader(confirmed);
    }
    if (!disk) { cmd_usage(USAGE); return 1; }
    if (esp_mib < 8) { refuse("--esp must be at least 8 MiB"); return 1; }

    char mine[16] = "";
    root_disk(mine, sizeof mine);
    if (mine[0] && strcmp(mine, disk) == 0) {
        printf("install: %s is the disk this machine is running from -- refused.\n", disk);
        printf("install: install ONTO another disk; there is no reading of "
               "\"over myself\" that ends with a working machine.\n");
        return 1;
    }

    if (!payload_present(mbr)) return 1;

    uint64_t block_size = SECTOR_BYTES;
    uint64_t sectors = disk_sectors(disk, &block_size);
    if (!sectors) {
        printf("install: %s: no such disk (a WHOLE disk, as `lsblk` names it)\n", disk);
        return 1;
    }
    // What this installs is BIOS-booted GRUB, whose boot sector and
    // core.img blocklist are 512-byte formats -- the kernel refuses
    // SYS_INSTALL_BOOT on anything else. Said before a byte is written.
    if (block_size != SECTOR_BYTES) {
        printf("install: %s has %llu-byte sectors; a BIOS boot needs 512\n",
               disk, (unsigned long long)block_size);
        return 1;
    }

    char size[16];
    human_size(size, sizeof size, sectors * SECTOR_BYTES);
    if (!confirmed) {
        printf("install: this ERASES %s (%s) and puts this system on it, as %s:\n",
               disk, size, mbr ? "MBR" : "GPT");
        if (mbr) {
            printf("  (gap)  1M    -          (GRUB's core.img, sectors 1..2047)\n");
            printf("  %sp1  %lluM  FAT32      (the kernel and grub.cfg) [active]\n",
                   disk, (unsigned long long)esp_mib);
            printf("  %sp2  rest  TFS3       (the root)\n", disk);
        } else {
            printf("  %sp1  1M    BIOS boot  (GRUB's core.img)\n", disk);
            printf("  %sp2  %lluM  ESP        (FAT32 -- the kernel and grub.cfg)\n",
                   disk, (unsigned long long)esp_mib);
            printf("  %sp3  rest  TFS3       (the root)\n", disk);
        }
        printf("install: re-run with `confirm` as the last argument if that is what you want.\n");
        return 1;
    }

    // WHICH PARTITIONS THOSE ARE DEPENDS ON THE LAYOUT: MBR has no
    // BIOS-boot partition, so everything shifts down by one.
    char pboot[24], proot[24];
    snprintf(pboot, sizeof pboot, "%sp%d", disk, mbr ? 1 : 2);
    snprintf(proot, sizeof proot, "%sp%d", disk, mbr ? 2 : 3);

    step("partitioning");
    if (!partition(disk, sectors, esp_mib, mbr)) return 1;

    step("formatting");
    if (!format_one(pboot, "fat32")) return 1;
    if (!format_one(proot, "tfs3")) return 1;

    step("mounting the target");
    if (!mount_one(proot, TARGET_ROOT)) return 1;
    // The mount point has to exist on the volume it is mounted onto,
    // which is the target's root -- so this is made after it is mounted,
    // not before.
    mkdir(TARGET_BOOT, 0755);
    if (!mount_one(pboot, TARGET_BOOT)) { sys_umount(TARGET_ROOT); return 1; }

    step("copying the system");
    int ok = copy_system();
    step("writing the target's /boot");
    if (ok) ok = copy_boot();

    // UNMOUNT BEFORE THE BOOTLOADER, for two reasons. The ESP's
    // write-back cache is flushed by the unmount, so the kernel this
    // disk is about to be told to boot is actually on it; and the kernel
    // refuses to write a boot sector to a disk anything is mounted from
    // without a confirm flag, which is the right refusal to be subject
    // to rather than to talk our way past.
    step("unmounting");
    sys_umount(TARGET_BOOT);
    sys_umount(TARGET_ROOT);

    step("writing the bootloader");
    if (ok) ok = write_bootloader(disk, mbr, 0);

    if (!ok) {
        printf("install: FAILED -- %s is in an unknown state; re-run to start over.\n", disk);
        return 1;
    }
    printf("install: done. Boot %s on its own and it should come up.\n", disk);
    return 0;
}
