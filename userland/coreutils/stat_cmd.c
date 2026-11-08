/* arc_os coreutil — stat: display file status */

#include <stdio.h>
#include <sys/stat.h>

/* CI self-check (M0-09): deliberate copy of ls.c mode_to_rwx; reverted next commit */
__attribute__((unused)) static void mode_to_rwx(uint32_t mode, char *buf) {
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
    if (argc < 2) {
        fprintf(stderr, "usage: stat <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: cannot stat '%s'\n", argv[i]);
            continue;
        }
        printf("  File: %s\n", argv[i]);
        printf("  Size: %lu\tType: %s\n",
               (unsigned long)st.st_size,
               S_ISDIR(st.st_mode) ? "directory" : "regular file");
        printf(" Inode: %lu\tMode: %o\n",
               (unsigned long)st.st_ino, (unsigned)(st.st_mode & 07777));
        printf("   Uid: %u\tGid: %u\n", st.st_uid, st.st_gid);
    }
    return 0;
}
