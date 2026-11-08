/* arc_os — ABI layout test (M0-05, ADR-0001).
 *
 * Compares every syscall number, errno and flag value, struct size and field
 * offset in kernel/include/uapi (abi_layout_ours.c) with the same item from the
 * build host's headers: Linux uapi (abi_layout_host_linux.c) and, for the few
 * items those lack (d_type values, AF_*, SOCK_*, dirent64 offsets), glibc here.
 * Its own program (ctest test_abi_layout): the header sets cannot share a
 * translation unit. Exit status 0 = every item matches. */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include "abi_layout.h"

typedef struct dirent64 abi_dirent64;   /* glibc: same offsets as linux_dirent64 */

#define ABI_NR(name)
#define ABI_VAL(name)
#define ABI_SIZE(type)
#define ABI_OFF(type, f)
#define ABI_VAL_G(name)    { #name, (long)(name) },
#define ABI_OFF_G(type, f) { #type "." #f, (long)offsetof(type, f) },

static const AbiItem abi_host_glibc[] = {
#include "abi_layout_list.h"
    { NULL, 0 }
};

static const AbiItem *find(const AbiItem *table, const char *name) {
    for (int i = 0; table[i].name != NULL; i++) {
        if (strcmp(table[i].name, name) == 0) return &table[i];
    }
    return NULL;
}

int main(void) {
    int checked = 0, failed = 0;
    for (int i = 0; abi_ours[i].name != NULL; i++) {
        const AbiItem *ours = &abi_ours[i];
        const AbiItem *host = find(abi_host_linux, ours->name);
        if (host == NULL) host = find(abi_host_glibc, ours->name);
        if (host == NULL) {
            printf("FAIL: %s: no host value\n", ours->name);
            failed++;
        } else if (ours->value != host->value) {
            printf("FAIL: %s: arc_os %ld, host %ld\n", ours->name, ours->value, host->value);
            failed++;
        }
        checked++;
    }
    printf("test_abi_layout: %d items checked, %d mismatches\n", checked, failed);
    return (failed || checked == 0) ? 1 : 0;
}
