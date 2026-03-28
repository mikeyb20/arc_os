/* arc_os libc — stdlib: exit, atoi, abs */

#include <stdlib.h>
#include <syscall.h>

void exit(int status) {
    syscall1(SYS_EXIT, (uint64_t)status);
    for (;;) __asm__ volatile ("ud2");
}

int atoi(const char *s) {
    int result = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

int abs(int n) {
    return n < 0 ? -n : n;
}
