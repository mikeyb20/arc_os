/* arc_os — Init process (PID 1)
 * Execs the login program for user authentication.
 * Falls back to shell if login is not available. */

#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Try login first */
    char *login_argv[] = { "/boot/login", NULL };
    execv("/boot/login", login_argv);
    /* Fall back to shell if login not found */
    char *sh_argv[] = { "/boot/shell", NULL };
    execv("/boot/shell", sh_argv);
    exit(1);
    return 1;
}
