/* arc_os — Minimal host-side test framework */

#include "test_framework.h"

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Collect tests from test files */
extern TestCase mem_tests[];
extern int mem_test_count;
extern TestCase pmm_tests[];
extern int pmm_test_count;
extern TestCase kprintf_tests[];
extern int kprintf_test_count;
extern TestCase kmalloc_tests[];
extern int kmalloc_test_count;
extern TestCase isr_tests[];
extern int isr_test_count;
extern TestCase thread_tests[];
extern int thread_test_count;
extern TestCase sched_tests[];
extern int sched_test_count;
extern TestCase process_tests[];
extern int process_test_count;
extern TestCase pci_tests[];
extern int pci_test_count;
extern TestCase virtio_tests[];
extern int virtio_test_count;
extern TestCase string_tests[];
extern int string_test_count;
extern TestCase vfs_tests[];
extern int vfs_test_count;
extern TestCase syscall_tests[];
extern int syscall_test_count;
extern TestCase elf_tests[];
extern int elf_test_count;
extern TestCase fd_tests[];
extern int fd_test_count;
extern TestCase vmm_tests[];
extern int vmm_test_count;
extern TestCase spinlock_tests[];
extern int spinlock_test_count;
extern TestCase gdt_tests[];
extern int gdt_test_count;
extern TestCase idt_tests[];
extern int idt_test_count;
extern TestCase bootinfo_tests[];
extern int bootinfo_test_count;
extern TestCase pic_tests[];
extern int pic_test_count;
extern TestCase user_access_tests[];
extern int user_access_test_count;
extern TestCase tty_tests[];
extern int tty_test_count;
extern TestCase keyboard_tests[];
extern int keyboard_test_count;
extern TestCase shell_tests[];
extern int shell_test_count;
extern TestCase pipe_tests[];
extern int pipe_test_count;
extern TestCase signal_tests[];
extern int signal_test_count;
extern TestCase waitqueue_tests[];
extern int waitqueue_test_count;
extern TestCase fat32_tests[];
extern int fat32_test_count;
extern TestCase devfs_tests[];
extern int devfs_test_count;
extern TestCase procfs_tests[];
extern int procfs_test_count;
extern TestCase path_tests[];
extern int path_test_count;
extern TestCase exec_argv_tests[];
extern int exec_argv_test_count;
extern TestCase net_util_tests[];
extern int net_util_test_count;
extern TestCase ethernet_tests[];
extern int ethernet_test_count;
extern TestCase arp_tests[];
extern int arp_test_count;
extern TestCase ipv4_tests[];
extern int ipv4_test_count;
extern TestCase icmp_tests[];
extern int icmp_test_count;
extern TestCase cred_tests[];
extern int cred_test_count;
extern TestCase udp_tests[];
extern int udp_test_count;
extern TestCase socket_tests[];
extern int socket_test_count;
extern TestCase loopback_tests[];
extern int loopback_test_count;

static void run_suite(const char *suite_name, TestCase *tests, int count) {
    printf("[%s] Running %d tests\n", suite_name, count);
    for (int i = 0; i < count; i++) {
        tests_run++;
        int result = tests[i].fn();
        if (result == 0) {
            tests_passed++;
            printf("  PASS: %s\n", tests[i].name);
        } else {
            tests_failed++;
        }
    }
}

typedef struct {
    const char *name;
    TestCase *tests;
    int *count;
} Suite;

int main(int argc, char **argv) {
    Suite suites[] = {
        { "mem",     mem_tests,     &mem_test_count },
        { "pmm",     pmm_tests,     &pmm_test_count },
        { "kprintf", kprintf_tests, &kprintf_test_count },
        { "kmalloc", kmalloc_tests, &kmalloc_test_count },
        { "isr",     isr_tests,     &isr_test_count },
        { "thread",  thread_tests,  &thread_test_count },
        { "sched",   sched_tests,   &sched_test_count },
        { "process", process_tests, &process_test_count },
        { "pci",     pci_tests,     &pci_test_count },
        { "virtio",  virtio_tests,  &virtio_test_count },
        { "string",  string_tests,  &string_test_count },
        { "vfs",     vfs_tests,     &vfs_test_count },
        { "syscall", syscall_tests, &syscall_test_count },
        { "elf",     elf_tests,     &elf_test_count },
        { "fd",      fd_tests,      &fd_test_count },
        { "vmm",      vmm_tests,      &vmm_test_count },
        { "spinlock",  spinlock_tests,  &spinlock_test_count },
        { "gdt",       gdt_tests,       &gdt_test_count },
        { "idt",       idt_tests,       &idt_test_count },
        { "bootinfo",  bootinfo_tests,  &bootinfo_test_count },
        { "pic",       pic_tests,       &pic_test_count },
        { "user_access", user_access_tests, &user_access_test_count },
        { "tty",          tty_tests,          &tty_test_count },
        { "keyboard",     keyboard_tests,     &keyboard_test_count },
        { "shell",        shell_tests,        &shell_test_count },
        { "pipe",         pipe_tests,         &pipe_test_count },
        { "signal",       signal_tests,       &signal_test_count },
        { "waitqueue",    waitqueue_tests,    &waitqueue_test_count },
        { "fat32",        fat32_tests,        &fat32_test_count },
        { "devfs",        devfs_tests,        &devfs_test_count },
        { "procfs",       procfs_tests,       &procfs_test_count },
        { "path",         path_tests,         &path_test_count },
        { "exec_argv",    exec_argv_tests,    &exec_argv_test_count },
        { "net_util",     net_util_tests,     &net_util_test_count },
        { "ethernet",     ethernet_tests,     &ethernet_test_count },
        { "arp",          arp_tests,          &arp_test_count },
        { "ipv4",         ipv4_tests,         &ipv4_test_count },
        { "icmp",         icmp_tests,         &icmp_test_count },
        { "cred",         cred_tests,         &cred_test_count },
        { "udp",          udp_tests,          &udp_test_count },
        { "socket",       socket_tests,       &socket_test_count },
        { "loopback",     loopback_tests,     &loopback_test_count },
    };
    int suite_count = (int)(sizeof(suites) / sizeof(suites[0]));

    /* Optional --suite filter */
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            filter = argv[++i];
        }
    }

    printf("=== arc_os host-side tests ===\n\n");

    for (int s = 0; s < suite_count; s++) {
        if (filter == NULL || strcmp(filter, suites[s].name) == 0) {
            run_suite(suites[s].name, suites[s].tests, *suites[s].count);
        }
    }

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
