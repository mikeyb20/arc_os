#ifndef ARCHOS_FS_DEVFS_H
#define ARCHOS_FS_DEVFS_H

#include "fs/vfs.h"

/* The /dev filesystem: null, zero and tty, all static (no allocation). Call
 * once after vfs_init; returns its superblock. */
SuperBlock *devfs_init(void);

/* The terminal as an open File (O_RDWR), for fds 0-2 of a process that has
 * nothing there. Returns it with a new reference. */
File *devfs_console(void);

/* Terminal ioctl (TIOCSPGRP, TIOCGPGRP, TCGETS, TCSETS*, TIOCGWINSZ,
 * TIOCSWINSZ); arg is a user address. -ENOTTY for anything else. */
int64_t devfs_tty_ioctl(File *file, uint64_t request, uint64_t arg);

#endif /* ARCHOS_FS_DEVFS_H */
