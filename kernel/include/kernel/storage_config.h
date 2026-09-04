#ifndef STORAGE_CONFIG_H
#define STORAGE_CONFIG_H

// `storage.sync` -- whether a write is durable when it returns.
//
// TFS3 commits one journal transaction per fs_write*() call and ends it
// with TWO real device flushes (tfs3.c's txn_commit). That is a
// stronger promise than Linux or Windows make: there, a write(2) lands
// in the page cache and returns, writeback runs on a timer, and the
// journal commits every few seconds with one barrier for thousands of
// operations. toy-os pays for the difference on every write, and on a
// real SSD it pays a great deal -- a FLUSH CACHE forces the drive's
// DRAM to NAND, where an emulated one returns almost immediately. That
// is why a throughput number measured in QEMU says nothing about
// hardware, and why this knob exists at all.
//
// The choices are ext4's `barrier`/`nobarrier` under different names.
int storage_sync_strict(void); // 1 = strict (the default), 0 = lazy

void storage_config_init(void);          // adopt /etc/storage.conf at boot
void storage_config_setting_register(void);

// FOR THE KTEST ONLY: set the live flag without touching /etc. The
// setting's own apply() persists, and a test that used it would leave
// the machine in whichever mode it happened to finish in.
void storage_config_set_strict_for_test(int strict);

#endif
