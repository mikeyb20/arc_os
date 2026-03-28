/* arc_os coreutil — kill: send signal to process */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill [-<signal>] <pid>...\n");
        return 1;
    }

    int signo = SIGTERM;
    int argstart = 1;

    if (argv[1][0] == '-') {
        signo = atoi(&argv[1][1]);
        argstart = 2;
    }

    int ret = 0;
    for (int i = argstart; i < argc; i++) {
        pid_t pid = (pid_t)atoi(argv[i]);
        if (kill(pid, signo) < 0) {
            fprintf(stderr, "kill: cannot signal pid %d\n", (int)pid);
            ret = 1;
        }
    }
    return ret;
}
