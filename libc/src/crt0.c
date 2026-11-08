/* arc_os — C runtime startup (System V process start, ADR-0002).
 *
 * The kernel enters _start with RSP pointing at argc:
 *   argc, argv[0..argc-1], NULL, envp[...], NULL, auxv pairs ..., AT_NULL
 * _start passes that pointer to __libc_start, which records environ and the
 * auxiliary vector, then calls main(argc, argv, envp) and exit(). */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

extern int main(int argc, char **argv, char **envp);

char **environ;
unsigned long *__arc_auxv;   /* read by getauxval() */

__attribute__((noreturn, used))
void __libc_start(uint64_t *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char **e = envp;
    while (*e != NULL) e++;
    environ = envp;
    __arc_auxv = (unsigned long *)(e + 1);
    exit(main(argc, argv, envp));
}

/* Clear RBP (end of frame chain), hand RSP to C, keep the call ABI-aligned. */
__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    xor %rbp, %rbp\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call __libc_start\n"
    "    hlt\n"
    ".size _start, . - _start\n");
