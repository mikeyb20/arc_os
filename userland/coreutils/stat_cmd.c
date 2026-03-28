/* arc_os coreutil — stat: display file status */

#include <stdio.h>
#include <sys/stat.h>

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
               st.st_type == 1 ? "directory" : "regular file");
        printf(" Inode: %lu\tMode: %o\n",
               (unsigned long)st.st_ino, (unsigned)st.st_mode);
        printf("   Uid: %u\tGid: %u\n", st.st_uid, st.st_gid);
    }
    return 0;
}
