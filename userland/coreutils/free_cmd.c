/* arc_os coreutil — free: display memory usage (reads /proc/meminfo) */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "free: cannot read /proc/meminfo\n");
        return 1;
    }

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    return 0;
}
