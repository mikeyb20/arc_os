/* arc_os — Echo: reads lines from stdin, echoes them back */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("arc_os echo shell. Type something!\n");

    char buf[256];
    for (;;) {
        printf("echo> ");
        ssize_t n = read(0, buf, sizeof(buf));
        if (n <= 0) break;
        write(1, buf, (size_t)n);
    }
    return 0;
}
