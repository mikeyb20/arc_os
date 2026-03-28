/* arc_os — Hello binary for exec() testing */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc <= 1) {
        printf("Hello from exec'd binary!\n");
    } else {
        printf("Hello");
        for (int i = 1; i < argc; i++) {
            printf(" %s", argv[i]);
        }
        printf("!\n");
    }
    return 0;
}
