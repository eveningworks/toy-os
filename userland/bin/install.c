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
#include "lib/cmd.h"
#include "lib/ufileop.h"
#include "lib/human.h"

#define USAGE \
    "install [--disk <name>] [--esp <MiB>] confirm\n" \
    "  --disk <name>  the target (`lsblk`); refuses the one this machine runs from\n" \
    "  --esp <MiB>    size of the FAT32 /boot partition (default 64)\n" \
    "  confirm        required -- this ERASES the target disk"

#define SECTOR_BYTES 512
#define BIOS_BOOT_SECTORS 2048     // 1 MiB, and where GRUB's core.img goes
#define FIRST_LBA         2048     // the 1 MiB alignment every modern tool uses

#define TARGET_ROOT "/mnt"
#define TARGET_BOOT "/mnt/boot"
#define GRUB_DIR    "/boot/boot/grub/i386-pc"

// WHAT IS NOT COPIED. `/boot` is a mount point for the SOURCE's ESP and
// is copied separately (into the target's, which is a different volume);
// `/mnt` is where the target itself is mounted, so copying it would copy
// the target into itself; `/tmp` is scratch by definition. Each is
// recreated empty on the target, because a mount point has to exist.
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

static uint64_t disk_sectors(const char *name) {
    struct query_blkdev b;
    for (int i = 0; sys_query_record(QUERY_BLKDEV, i, &b, sizeof b) >= (int)sizeof b; i++) {
        if (strcmp(b.name, name) != 0) continue;
        if (b.parent[0]) return 0;   // a partition, not a disk
        return b.sectors;
    }
    return 0;
}

// A whole file into a fresh buffer. The caller frees it. Used for
// boot.img (512 bytes) and core.img (tens of KiB), neither of which
// belongs on a 2 KiB frame.
static void *slurp(const char *path, uint64_t *out_size) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) return 0;
    if (st.size == 0) return 0;
    uint8_t *buf = malloc((size_t)st.size);
    if (!buf) return 0;
    int fd = sys_open(path, 0)  /* read-only is the default */;
    if (fd < 0) { free(buf); return 0; }
    uint64_t got = 0;
    while (got < st.size) {
        int64_t n = sys_read(fd, buf + got, (size_t)(st.size - got));
        if (n <= 0) break;
        got += (uint64_t)n;
    }
    sys_close(fd);
    if (got != st.size) { free(buf); return 0; }
    *out_size = got;
    return buf;
}

// One partition table: BIOS boot, ESP, and the root taking the rest.
static int partition(const char *disk, uint64_t sectors, uint64_t esp_mib) {
    struct mkpart_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.device, sizeof req.device, "%s", disk);
    req.kind = MKPART_KIND_GPT;
    req.count = 3;
    req.flags = MKPART_CONFIRM;

    uint64_t esp = esp_mib * 1024 * 1024 / SECTOR_BYTES;
    // GPT keeps its backup header and entry array in the last 33.
    uint64_t usable = sectors - FIRST_LBA - 33;
    if (usable <= BIOS_BOOT_SECTORS + esp) {
        cmd_fail("install", "the disk is too small for a 1 MiB boot partition, "
                            "the ESP and a root");
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
    printf("install: %s: %s\n", path, sys_strerror(err));
}

static const struct ufileop_policy COPY_POLICY = {
    .ctx = 0, .on_conflict = 0, .on_progress = 0, .on_error = copy_error,
};

// Everything at the root except the three mount points, then those three
// recreated empty. A directory at a time rather than one copy of "/",
// because "/" contains the target.
static int copy_system(void) {
    struct sys_dirent *ents = malloc(sizeof(struct sys_dirent) * SYS_LISTDIR_MAX);
    if (!ents) { cmd_fail("install", "out of memory"); return 0; }
    int n = sys_listdir("/", ents, SYS_LISTDIR_MAX);
    if (n < 0) { free(ents); cmd_fail("install", "/"); return 0; }

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
        sys_mkdir(dst);   // already there is not an error
    }
    return ok;
}

// The ESP's contents: the kernel, grub.cfg and the modules, which on
// this disk all live under one directory. Copied AFTER the target's ESP
// is mounted, and separately from the root, because it is a different
// volume on both sides.
static int copy_boot(void) {
    struct sys_stat st;
    if (sys_stat("/boot/boot", &st) < 0 || !st.is_dir) {
        cmd_fail("install", "/boot/boot");
        return 0;
    }
    printf("  /boot/boot\n");
    return ufileop_copy("/boot/boot", TARGET_BOOT "/boot", &g_op, &COPY_POLICY) == UFILEOP_OK;
}

static int write_bootloader(const char *disk) {
    struct install_boot_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.device, sizeof req.device, "%s", disk);

    uint64_t boot_size = 0, core_size = 0;
    void *boot = slurp(GRUB_DIR "/boot.img", &boot_size);
    if (!boot) { cmd_fail("install", GRUB_DIR "/boot.img"); return 0; }
    void *core = slurp(GRUB_DIR "/core.img", &core_size);
    if (!core) { free(boot); cmd_fail("install", GRUB_DIR "/core.img"); return 0; }

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

int main(int argc, char **argv) {
    const char *disk = 0;
    uint64_t esp_mib = 64;
    int confirmed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) { disk = argv[++i]; continue; }
        if (strcmp(argv[i], "--esp") == 0 && i + 1 < argc) { esp_mib = (uint64_t)atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "confirm") == 0) { confirmed = 1; continue; }
        cmd_usage(USAGE);
        return 1;
    }
    if (!disk) { cmd_usage(USAGE); return 1; }
    if (esp_mib < 8) { cmd_fail("install", "--esp must be at least 8 MiB"); return 1; }

    char mine[16] = "";
    root_disk(mine, sizeof mine);
    if (mine[0] && strcmp(mine, disk) == 0) {
        printf("install: %s is the disk this machine is running from -- refused.\n", disk);
        printf("install: install ONTO another disk; there is no reading of "
               "\"over myself\" that ends with a working machine.\n");
        return 1;
    }

    uint64_t sectors = disk_sectors(disk);
    if (!sectors) { cmd_fail("install", "no such disk"); return 1; }

    char size[16];
    human_size(size, sizeof size, sectors * SECTOR_BYTES);
    if (!confirmed) {
        printf("install: this ERASES %s (%s) and puts this system on it:\n", disk, size);
        printf("  %sp1  1M    BIOS boot  (GRUB's core.img)\n", disk);
        printf("  %sp2  %lluM  ESP        (FAT32 -- the kernel and grub.cfg)\n",
               disk, (unsigned long long)esp_mib);
        printf("  %sp3  rest  TFS3       (the root)\n", disk);
        printf("install: re-run with `confirm` as the last argument if that is what you want.\n");
        return 1;
    }

    char p2[24], p3[24];
    snprintf(p2, sizeof p2, "%sp2", disk);
    snprintf(p3, sizeof p3, "%sp3", disk);

    step("partitioning");
    if (!partition(disk, sectors, esp_mib)) return 1;

    step("formatting");
    if (!format_one(p2, "fat32")) return 1;
    if (!format_one(p3, "tfs3")) return 1;

    step("mounting the target");
    if (!mount_one(p3, TARGET_ROOT)) return 1;
    // The mount point has to exist on the volume it is mounted onto,
    // which is the target's root -- so this is made after it is mounted,
    // not before.
    sys_mkdir(TARGET_BOOT);
    if (!mount_one(p2, TARGET_BOOT)) { sys_umount(TARGET_ROOT); return 1; }

    step("copying the system");
    int ok = copy_system();
    step("copying /boot");
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
    if (ok) ok = write_bootloader(disk);

    if (!ok) {
        printf("install: FAILED -- %s is in an unknown state; re-run to start over.\n", disk);
        return 1;
    }
    printf("install: done. Boot %s on its own and it should come up.\n", disk);
    return 0;
}
