/* arc_os — host-side test runner: every suite but the VFS ones (vfs_main.c) */

#include "test_framework.h"

/* Collect tests from test files */
extern TestCase cmdline_tests[];
extern int cmdline_test_count;
extern TestCase qr_tests[];
extern int qr_test_count;
extern TestCase qr_screen_tests[];
extern int qr_screen_test_count;
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
extern TestCase bus_tests[];
extern int bus_test_count;
extern TestCase irq_tests[];
extern int irq_test_count;
extern TestCase irq_vector_tests[];
extern int irq_vector_test_count;
extern TestCase dma_tests[];
extern int dma_test_count;
extern TestCase virtio_tests[];
extern int virtio_test_count;
extern TestCase string_tests[];
extern int string_test_count;
extern TestCase syscall_tests[];
extern int syscall_test_count;
extern TestCase elf_tests[];
extern int elf_test_count;
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
extern TestCase signal_tests[];
extern int signal_test_count;
extern TestCase waitqueue_tests[];
extern int waitqueue_test_count;
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
extern TestCase mutex_tests[];
extern int mutex_test_count;
extern TestCase rbtree_tests[];
extern int rbtree_test_count;
extern TestCase vma_tests[];
extern int vma_test_count;
extern TestCase mm_tests[];
extern int mm_test_count;
extern TestCase acpi_tests[];
extern int acpi_test_count;
extern TestCase lapic_tests[];
extern int lapic_test_count;
extern TestCase percpu_tests[];
extern int percpu_test_count;
extern TestCase fb_console_tests[];
extern int fb_console_test_count;
extern TestCase passwd_tests[];
extern int passwd_test_count;
extern TestCase socket_net_tests[];
extern int socket_net_test_count;
extern TestCase arc_string_tests[];
extern int arc_string_test_count;
extern TestCase arc_printf_tests[];
extern int arc_printf_test_count;
extern TestCase ansi_tests[];
extern TestCase panic_tests[];
extern TestCase smbios_tests[];
extern int smbios_test_count;
extern int panic_test_count;
extern int ansi_test_count;

int main(int argc, char **argv) {
    /* clang-format off */
    TestSuite suites[] = {
        { "mem",     mem_tests,     &mem_test_count },
        { "pmm",     pmm_tests,     &pmm_test_count },
        { "kprintf", kprintf_tests, &kprintf_test_count },
        { "kmalloc", kmalloc_tests, &kmalloc_test_count },
        { "isr",     isr_tests,     &isr_test_count },
        { "thread",  thread_tests,  &thread_test_count },
        { "sched",   sched_tests,   &sched_test_count },
        { "process", process_tests, &process_test_count },
        { "pci",     pci_tests,     &pci_test_count },
        { "bus",     bus_tests,     &bus_test_count },
        { "irq",     irq_tests,     &irq_test_count },
        { "irq_vector", irq_vector_tests, &irq_vector_test_count },
        { "dma",     dma_tests,     &dma_test_count },
        { "virtio",  virtio_tests,  &virtio_test_count },
        { "string",  string_tests,  &string_test_count },
        { "syscall", syscall_tests, &syscall_test_count },
        { "elf",     elf_tests,     &elf_test_count },
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
        { "signal",       signal_tests,       &signal_test_count },
        { "waitqueue",    waitqueue_tests,    &waitqueue_test_count },
        { "path",         path_tests,         &path_test_count },
        { "exec_argv",    exec_argv_tests,    &exec_argv_test_count },
        { "net_util",     net_util_tests,     &net_util_test_count },
        { "ethernet",     ethernet_tests,     &ethernet_test_count },
        { "arp",          arp_tests,          &arp_test_count },
        { "ipv4",         ipv4_tests,         &ipv4_test_count },
        { "icmp",         icmp_tests,         &icmp_test_count },
        { "mutex",        mutex_tests,        &mutex_test_count },
        { "rbtree",       rbtree_tests,       &rbtree_test_count },
        { "vma",          vma_tests,          &vma_test_count },
        { "mm",           mm_tests,           &mm_test_count },
        { "acpi",         acpi_tests,         &acpi_test_count },
        { "lapic",        lapic_tests,        &lapic_test_count },
        { "percpu",       percpu_tests,       &percpu_test_count },
        { "fb_console",   fb_console_tests,   &fb_console_test_count },
        { "passwd",       passwd_tests,       &passwd_test_count },
        { "socket_net",   socket_net_tests,   &socket_net_test_count },
        { "arc_string",   arc_string_tests,   &arc_string_test_count },
        { "arc_printf",   arc_printf_tests,   &arc_printf_test_count },
        { "ansi",         ansi_tests,         &ansi_test_count },
        { "cmdline",      cmdline_tests,      &cmdline_test_count },
        { "qr",           qr_tests,           &qr_test_count },
        { "qr_screen",    qr_screen_tests,    &qr_screen_test_count },
        { "panic",        panic_tests,        &panic_test_count },
        { "smbios",       smbios_tests,       &smbios_test_count },
    };
    /* clang-format on */
    return test_harness_main(argc, argv, "arc_os host-side tests", suites, (int)(sizeof(suites) / sizeof(suites[0])));
}
