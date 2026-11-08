/* arc_os coreutil — ps: list processes (reads /proc/<pid>/status) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("  PID  STATE  NAME\n");

    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        fprintf(stderr, "ps: cannot read /proc\n");
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Skip non-numeric entries (meminfo, uptime) */
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;

        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        printf("%s", buf);
    }
    closedir(dir);
    return 0;
}
