#ifndef ARCHOS_FS_PASSWD_H
#define ARCHOS_FS_PASSWD_H

#include <stdint.h>

/* Maximum lengths for passwd fields */
#define PASSWD_NAME_MAX   32
#define PASSWD_PASS_MAX   64
#define PASSWD_SHELL_MAX  128
#define PASSWD_HOME_MAX   128

/* Parsed passwd entry (one line of /etc/passwd) */
typedef struct {
    char     name[PASSWD_NAME_MAX];
    char     password[PASSWD_PASS_MAX];  /* Plaintext for now (no hashing) */
    uint32_t uid;
    uint32_t gid;
    char     home[PASSWD_HOME_MAX];
    char     shell[PASSWD_SHELL_MAX];
} PasswdEntry;

/* Maximum number of users */
#define PASSWD_MAX_USERS 16

/* Parse /etc/passwd from a buffer in standard colon-delimited format:
 *   name:password:uid:gid:gecos:home:shell
 * Populates entries[] up to max_entries.
 * Returns number of entries parsed, or -1 on error. */
int passwd_parse(const char *buf, uint32_t buf_len,
                 PasswdEntry *entries, int max_entries);

/* Look up a user by name in a parsed entry array.
 * Returns pointer to the matching entry, or NULL if not found. */
const PasswdEntry *passwd_find_user(const PasswdEntry *entries, int count,
                                     const char *name);

/* Look up a user by UID in a parsed entry array.
 * Returns pointer to the matching entry, or NULL if not found. */
const PasswdEntry *passwd_find_uid(const PasswdEntry *entries, int count,
                                    uint32_t uid);

#endif /* ARCHOS_FS_PASSWD_H */
