/* arc_os coreutil — ls: list directory contents */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>

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
        if (getcwd(resolved, sizeof(resolved)) == NULL) {
            fprintf(stderr, "ls: cannot get current directory\n");
            return 1;
        }
        path = resolved;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        fprintf(stderr, "ls: cannot access '%s'\n", path);
        return 1;
    }

    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;   /* no -a */
        count++;
        if (long_format) {
            /* Build full path for stat */
            char fullpath[512];
            if (strcmp(path, "/") == 0)
                snprintf(fullpath, sizeof(fullpath), "/%s", de->d_name);
            else
                snprintf(fullpath, sizeof(fullpath), "%s/%s", path, de->d_name);

            struct stat st;
            if (stat(fullpath, &st) == 0) {
                char type_ch = S_ISDIR(st.st_mode) ? 'd' : '-';
                char rwx[10];
                mode_to_rwx(st.st_mode, rwx);
                printf("%c%s %d %d %5lu %s\n", type_ch, rwx,
                       (int)st.st_uid, (int)st.st_gid, (unsigned long)st.st_size,
                       de->d_name);
            } else {
                printf("? %s\n", de->d_name);
            }
        } else {
            char suffix = de->d_type == DT_DIR ? '/' : ' ';
            printf("%s%c  ", de->d_name, suffix);
        }
    }
    closedir(dir);
    if (!long_format && count > 0) printf("\n");
    return 0;
}
