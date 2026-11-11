/*
 * lane_rss — run a command and report its peak RSS (M3-06a), for RSS budgets of whole programs.
 *
 *   lane_rss <program> [args...]     runs it, then prints "lane_rss: peak RSS N KiB" on stderr
 *
 * The child is forked from this small process, so its ru_maxrss is the program's own: a program
 * a Python driver starts directly carries the driver's RSS from before exec (about 13 MB).
 * Exit status: the program's.
 */
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: lane_rss <program> [args...]\n", stderr);
        return 2;
    }
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        execv(argv[1], argv + 1);
        perror("lane_rss: exec");
        _exit(127);
    }
    int status;
    struct rusage ru;
    if (child < 0 || wait4(child, &status, 0, &ru) != child) {
        perror("lane_rss: fork/wait4");
        return 1;
    }
    fprintf(stderr, "lane_rss: peak RSS %ld KiB\n", ru.ru_maxrss);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
