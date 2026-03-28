/* arc_os coreutil — ps: list processes (reads /proc/<pid>/status) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syscall.h>
#include <stdint.h>

/* Kernel DirEntry layout */
typedef struct {
    char     name[256];
    uint64_t inode_num;
    uint8_t  type;
} DirEntry;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("  PID  STATE  NAME\n");

    DirEntry entries[64];
    int64_t count = syscall3(SYS_READDIR, (uint64_t)"/proc", (uint64_t)entries, 64);
    if (count < 0) {
        fprintf(stderr, "ps: cannot read /proc\n");
        return 1;
    }

    for (int64_t i = 0; i < count; i++) {
        /* Skip non-numeric entries (meminfo, uptime) */
        if (entries[i].name[0] < '0' || entries[i].name[0] > '9') continue;

        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/status", entries[i].name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        printf("%s", buf);
    }
    return 0;
}
