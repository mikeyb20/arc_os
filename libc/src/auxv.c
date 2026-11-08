/* arc_os libc — getauxval and getenv */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/auxv.h>

extern unsigned long *__arc_auxv;

unsigned long getauxval(unsigned long type) {
    for (unsigned long *a = __arc_auxv; a != NULL && a[0] != AT_NULL; a += 2) {
        if (a[0] == type) return a[1];
    }
    errno = ENOENT;
    return 0;
}

char *getenv(const char *name) {
    if (environ == NULL || name == NULL) return NULL;
    size_t n = strlen(name);
    for (char **e = environ; *e != NULL; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    }
    return NULL;
}
