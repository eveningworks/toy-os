#ifndef KERNEL_PENDING_REPLACE_H
#define KERNEL_PENDING_REPLACE_H

// Renames every file /bin/update staged for this boot over its target
// (abi/update_abi.h). Called once from kernel_main(), after the
// filesystems mount and before anything is spawned; a machine with no
// pending list pays one fs_size().
void fs_apply_pending_replacements(void);

#endif
