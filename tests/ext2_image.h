#ifndef ARCHOS_TESTS_EXT2_IMAGE_H
#define ARCHOS_TESTS_EXT2_IMAGE_H

/* Host side of the ext2 write suite (M1-15): ext2 images made by mkfs.ext2
 * in sparse temporary files, mapped shared so the kernel code writes the
 * file itself, then checked with e2fsck and read back with debugfs
 * (e2fsprogs, tools/dev-setup.sh). Host headers only: kept apart from the
 * kernel's uapi headers. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char path[256];
    uint8_t *data;   /* the image, mapped MAP_SHARED */
    uint64_t size;
} HostImage;

/* A fresh image of mib MiB (the file is removed when the program ends; it
 * stays mapped, as the volume on it stays mounted): mkfs.ext2 with 4 KiB blocks unless opts says
 * otherwise (opts is added to the command: "-N 64", "-b 1024"). With tree
 * non-NULL, it is a shell script run in an empty directory whose contents
 * the image starts with (mkfs -d), and the directories get htree indexes
 * (e2fsck -fyD) as a Linux-written volume has. 0, or -1 (printed). */
int himg_make(HostImage *im, uint64_t mib, const char *opts, const char *tree);

/* msync, then `e2fsck -fn`: its exit status (0 = clean). On anything else
 * its report is printed. */
int himg_fsck(const HostImage *im);

/* msync, then `debugfs -R cmd`: its output (stdout, NUL-terminated, at most
 * size - 1 bytes) in out. Bytes read, or -1. */
long himg_debugfs(const HostImage *im, const char *cmd, char *out, size_t size);

/* A file's contents read by debugfs (`dump`): a malloc'd buffer the caller
 * frees, its length in *len; NULL if debugfs could not read it. */
uint8_t *himg_dump(const HostImage *im, const char *path, uint64_t *len);

#endif /* ARCHOS_TESTS_EXT2_IMAGE_H */
