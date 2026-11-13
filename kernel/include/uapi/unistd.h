#ifndef ARCHOS_UAPI_UNISTD_H
#define ARCHOS_UAPI_UNISTD_H

/* Linux x86_64 syscall numbers (ADR-0001), shared by the kernel and libarc.
 *
 * Copied from the archived Linux v6.12 arch/x86/entry/syscalls/syscall_64.tbl
 * ("common" and "64" rows only). Only calls listed in syscall-table.md are here;
 * add a number when its syscall is added, never invent one. tests/test_abi_layout.c
 * checks every value against the host's <asm/unistd_64.h>. */

/* Size of the kernel dispatch table: above the highest Linux number (462 in v6.12). */
#define ARC_NR_SYSCALLS 512

#define __NR_read              0
#define __NR_write             1
#define __NR_open              2
#define __NR_close             3
#define __NR_stat              4
#define __NR_fstat             5
#define __NR_lstat             6
#define __NR_lseek             8
#define __NR_mmap              9
#define __NR_mprotect          10
#define __NR_munmap            11
#define __NR_brk               12
#define __NR_rt_sigaction      13
#define __NR_rt_sigprocmask    14
#define __NR_rt_sigreturn      15
#define __NR_ioctl             16
#define __NR_pread64           17
#define __NR_pwrite64          18
#define __NR_readv             19
#define __NR_writev            20
#define __NR_access            21
#define __NR_pipe              22
#define __NR_mremap            25
#define __NR_madvise           28
#define __NR_dup               32
#define __NR_dup2              33
#define __NR_nanosleep         35
#define __NR_getpid            39
#define __NR_socket            41
#define __NR_connect           42
#define __NR_accept            43
#define __NR_sendto            44
#define __NR_recvfrom          45
#define __NR_bind              49
#define __NR_listen            50
#define __NR_clone             56
#define __NR_fork              57
#define __NR_execve            59
#define __NR_exit              60
#define __NR_wait4             61
#define __NR_kill              62
#define __NR_fcntl             72
#define __NR_fsync             74
#define __NR_ftruncate         77
#define __NR_getcwd            79
#define __NR_chdir             80
#define __NR_rename            82
#define __NR_mkdir             83
#define __NR_rmdir             84
#define __NR_unlink            87
#define __NR_chmod             90
#define __NR_fchmod            91
#define __NR_chown             92
#define __NR_fchown            93
#define __NR_umask             95
#define __NR_sysinfo           99
#define __NR_getuid            102
#define __NR_getgid            104
#define __NR_setuid            105
#define __NR_setgid            106
#define __NR_geteuid           107
#define __NR_setpgid           109
#define __NR_getppid           110
#define __NR_getpgid           121
#define __NR_arch_prctl        158
#define __NR_tkill             200
#define __NR_futex             202
#define __NR_sched_setaffinity 203
#define __NR_sched_getaffinity 204
#define __NR_getdents64        217
#define __NR_set_tid_address   218
#define __NR_fadvise64         221
#define __NR_clock_gettime     228
#define __NR_exit_group        231
#define __NR_openat            257
#define __NR_mkdirat           258
#define __NR_newfstatat        262
#define __NR_unlinkat          263
#define __NR_renameat          264
#define __NR_utimensat         280
#define __NR_dup3              292
#define __NR_pipe2             293

#endif /* ARCHOS_UAPI_UNISTD_H */
