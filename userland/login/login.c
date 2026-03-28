/* arc_os — Login program
 * Reads /etc/passwd, prompts for username and password, validates credentials,
 * sets uid/gid via syscalls, then execs the user's shell. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syscall.h>

/* Passwd entry */
typedef struct {
    char     name[32];
    char     password[64];
    unsigned uid;
    unsigned gid;
    char     home[128];
    char     shell[128];
} PwEntry;

static int read_line(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) pos--;
            continue;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static const char *parse_uint(const char *s, unsigned *out) {
    *out = 0;
    while (*s >= '0' && *s <= '9') {
        *out = *out * 10 + (*s - '0');
        s++;
    }
    if (*s == ':') s++;
    return s;
}

static const char *copy_until(const char *s, char *dst, int max, char delim) {
    int i = 0;
    while (*s && *s != delim && *s != '\n') {
        if (i < max - 1) dst[i++] = *s;
        s++;
    }
    dst[i] = '\0';
    if (*s == delim) s++;
    return s;
}

static int parse_passwd(const char *buf, PwEntry *entries, int max) {
    int count = 0;
    const char *p = buf;

    while (*p && count < max) {
        while (*p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }

        PwEntry *e = &entries[count];
        memset(e, 0, sizeof(*e));

        p = copy_until(p, e->name, 32, ':');
        p = copy_until(p, e->password, 64, ':');
        p = parse_uint(p, &e->uid);
        p = parse_uint(p, &e->gid);
        char tmp[64];
        p = copy_until(p, tmp, 64, ':');
        p = copy_until(p, e->home, 128, ':');
        p = copy_until(p, e->shell, 128, ':');
        while (*p && *p != '\n') p++;
        if (e->name[0]) count++;
    }
    return count;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Read /etc/passwd */
    char passwd_buf[2048];
    memset(passwd_buf, 0, sizeof(passwd_buf));

    int fd = open("/etc/passwd", O_RDONLY);
    if (fd >= 0) {
        read(fd, passwd_buf, sizeof(passwd_buf) - 1);
        close(fd);
    }

    PwEntry users[16];
    int user_count = parse_passwd(passwd_buf, users, 16);

    printf("\narc_os login\n\n");

    for (;;) {
        printf("login: ");
        char username[32];
        memset(username, 0, sizeof(username));
        read_line(username, (int)sizeof(username));
        printf("\n");

        if (username[0] == '\0') continue;

        PwEntry *found = NULL;
        for (int i = 0; i < user_count; i++) {
            if (strcmp(users[i].name, username) == 0) {
                found = &users[i];
                break;
            }
        }

        if (!found) { printf("Login incorrect\n\n"); continue; }

        if (found->password[0]) {
            printf("password: ");
            char password[64];
            memset(password, 0, sizeof(password));
            read_line(password, (int)sizeof(password));
            printf("\n");
            if (strcmp(found->password, password) != 0) {
                printf("Login incorrect\n\n");
                continue;
            }
        }

        printf("Welcome, %s!\n\n", found->name);

        syscall1(SYS_SETGID, found->gid);
        syscall1(SYS_SETUID, found->uid);

        if (found->home[0]) chdir(found->home);

        const char *shell = found->shell[0] ? found->shell : "/boot/shell";
        char *sh_argv[] = { (char *)shell, NULL };
        execv(shell, sh_argv);

        printf("Failed to start shell\n\n");
    }
}
