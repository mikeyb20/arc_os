/* arc_os coreutil — chown: change file owner */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: chown <owner[:group]> <file>...\n");
        return 1;
    }

    /* Parse owner:group */
    char *spec = argv[1];
    char *colon = strchr(spec, ':');
    uid_t uid = (uid_t)atoi(spec);
    gid_t gid = (gid_t)-1;
    if (colon) {
        *colon = '\0';
        uid = (uid_t)atoi(spec);
        gid = (gid_t)atoi(colon + 1);
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            fprintf(stderr, "chown: cannot change owner of '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
