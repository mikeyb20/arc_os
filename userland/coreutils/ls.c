/* arc_os coreutil — ls: list directory contents */

#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <sys/stat.h>
#include <stdint.h>

/* Kernel DirEntry layout */
typedef struct {
    char     name[256];
    uint64_t inode_num;
    uint8_t  type;
} DirEntry;

static void mode_to_rwx(uint32_t mode, char *buf) {
    buf[0] = (mode & 0400) ? 'r' : '-';
    buf[1] = (mode & 0200) ? 'w' : '-';
    buf[2] = (mode & 0100) ? 'x' : '-';
    buf[3] = (mode & 0040) ? 'r' : '-';
    buf[4] = (mode & 0020) ? 'w' : '-';
    buf[5] = (mode & 0010) ? 'x' : '-';
    buf[6] = (mode & 0004) ? 'r' : '-';
    buf[7] = (mode & 0002) ? 'w' : '-';
    buf[8] = (mode & 0001) ? 'x' : '-';
    buf[9] = '\0';
}

int main(int argc, char **argv) {
    int long_format = 0;
    const char *path = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) long_format = 1;
        else path = argv[i];
    }

    /* Resolve relative "." */
    char resolved[256];
    if (strcmp(path, ".") == 0) {
        char cwd[256];
        syscall2(SYS_GETCWD, (uint64_t)cwd, sizeof(cwd));
        strncpy(resolved, cwd, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
        path = resolved;
    }

    DirEntry entries[64];
    int64_t count = syscall3(SYS_READDIR, (uint64_t)path, (uint64_t)entries, 64);
    if (count < 0) {
        fprintf(stderr, "ls: cannot access '%s'\n", path);
        return 1;
    }

    for (int64_t i = 0; i < count; i++) {
        if (long_format) {
            /* Build full path for stat */
            char fullpath[512];
            if (strcmp(path, "/") == 0)
                snprintf(fullpath, sizeof(fullpath), "/%s", entries[i].name);
            else
                snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entries[i].name);

            struct stat st;
            if (syscall2(SYS_STAT, (uint64_t)fullpath, (uint64_t)&st) == 0) {
                char type_ch = st.st_type == 1 ? 'd' : '-';
                char rwx[10];
                mode_to_rwx(st.st_mode, rwx);
                printf("%c%s %d %d %5lu %s\n", type_ch, rwx,
                       (int)st.st_uid, (int)st.st_gid, (unsigned long)st.st_size,
                       entries[i].name);
            } else {
                printf("? %s\n", entries[i].name);
            }
        } else {
            char suffix = entries[i].type == 1 ? '/' : ' ';
            printf("%s%c  ", entries[i].name, suffix);
        }
    }
    if (!long_format && count > 0) printf("\n");
    return 0;
}
