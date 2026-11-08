#ifndef ARCHOS_MM_MM_SYSCALLS_H
#define ARCHOS_MM_MM_SYSCALLS_H

/* Register the memory syscalls (brk) under their Linux numbers. Called by
 * syscall_init. mmap, munmap and mprotect come with M1-07. */
void mm_syscalls_init(void);

#endif /* ARCHOS_MM_MM_SYSCALLS_H */
