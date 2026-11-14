/* arc_os — host test runner for the VFS suites, linked with the real
 * kernel/fs sources over vfs_env.c (M1-11) */

#include "test_framework.h"

extern TestCase vfs_tests[];
extern int vfs_test_count;
extern TestCase fd_tests[];
extern int fd_test_count;
extern TestCase pipe_tests[];
extern int pipe_test_count;
extern TestCase devfs_tests[];
extern int devfs_test_count;
extern TestCase procfs_tests[];
extern int procfs_test_count;
extern TestCase fat32_tests[];
extern int fat32_test_count;
extern TestCase ext2_tests[];
extern int ext2_test_count;
extern TestCase cred_tests[];
extern int cred_test_count;
extern TestCase filemap_tests[];
extern int filemap_test_count;

int main(int argc, char **argv) {
    TestSuite suites[] = {
        {"vfs", vfs_tests, &vfs_test_count},
        {"fd", fd_tests, &fd_test_count},
        {"pipe", pipe_tests, &pipe_test_count},
        {"devfs", devfs_tests, &devfs_test_count},
        {"procfs", procfs_tests, &procfs_test_count},
        {"fat32", fat32_tests, &fat32_test_count},
        {"ext2", ext2_tests, &ext2_test_count},
        {"cred", cred_tests, &cred_test_count},
        {"filemap", filemap_tests, &filemap_test_count},
    };
    return test_harness_main(argc, argv, "arc_os VFS host tests", suites, (int)(sizeof(suites) / sizeof(suites[0])));
}
