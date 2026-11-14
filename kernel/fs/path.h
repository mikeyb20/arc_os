#ifndef ARCHOS_FS_PATH_H
#define ARCHOS_FS_PATH_H

/* Maximum length of a full path (including NUL). Paths are walked as given
 * (fs/vfs.c resolves ".", ".." and symlinks on the tree, M1-13); the old text
 * normalizer is gone. */
#define PATH_MAX 512

#endif /* ARCHOS_FS_PATH_H */
