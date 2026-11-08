#ifndef ARCHOS_PROC_EXEC_STACK_H
#define ARCHOS_PROC_EXEC_STACK_H

#include <stdint.h>
#include <stddef.h>

/* System V x86_64 initial process stack (ADR-0002), built by execve and by
 * init_launch. Layout from the top of the user stack down:
 *
 *   8 zero bytes, AT_EXECFN string, argv strings, envp strings,
 *   "x86_64" (AT_PLATFORM), 16 AT_RANDOM bytes, padding,
 *   auxv pairs ending in AT_NULL, NULL, envp[], NULL, argv[], argc  <- RSP
 *
 * RSP is 16-byte aligned and points at argc. Pure code: no user memory is
 * touched, so it is unit-tested on the host (tests/test_exec_argv.c). */

/* Bytes of argv + envp strings plus their pointers one execve may pass
 * (Linux: -E2BIG past the limit). Leaves half of the 64 KiB stack to the program. */
#define EXEC_ARG_MAX   (32 * 1024)
/* Most argv + envp entries */
#define EXEC_MAX_PTRS  1024
/* Size of the kernel buffer the stack is built in: strings and pointers
 * (EXEC_ARG_MAX), the path, auxv, platform and random bytes */
#define EXEC_STACK_BUF (EXEC_ARG_MAX + 4096 + 1024)

/* Kernel copy of argv and envp. Strings are packed in data: argv first. */
typedef struct {
    uint32_t argc;
    uint32_t envc;
    uint32_t used;                    /* bytes of data in use */
    uint32_t off[EXEC_MAX_PTRS];      /* argv[0..argc), then envp[0..envc) */
    char     data[EXEC_ARG_MAX];
} ExecStrings;

/* Values the auxiliary vector carries */
typedef struct {
    uint64_t phdr;        /* program headers in user memory */
    uint64_t phent;
    uint64_t phnum;
    uint64_t entry;
    uint64_t uid, euid, gid, egid;
    uint64_t secure;
    uint64_t hwcap;       /* CPUID 1 EDX */
    uint64_t hwcap2;      /* Linux x86 HWCAP2 bits (0: none enabled) */
    uint64_t minsigstksz; /* largest signal frame (sig_frame_max_size) */
    uint8_t  random[16];
} ExecAux;

/* Reset s to no strings. */
void exec_strings_init(ExecStrings *s);

/* Append one kernel string to argv (is_env == 0) or envp. envp entries must
 * come after all argv entries. Returns 0 or -E2BIG. */
int exec_strings_add(ExecStrings *s, const char *str, int is_env);

/* Bytes of strings plus pointers counted against EXEC_ARG_MAX. */
size_t exec_strings_cost(const ExecStrings *s);

/* Most bytes (NUL included) the next string may take, 0 if no entry fits.
 * A caller copying in place writes at s->data + s->used, then commits. */
size_t exec_strings_room(const ExecStrings *s);

/* Record the len bytes (NUL included) just written at s->data + s->used as
 * the next argv (is_env == 0) or envp entry. len must be <= the room. */
void exec_strings_commit(ExecStrings *s, size_t len, int is_env);

/* Build the stack in buf, the kernel image of the user range
 * [stack_top - buf_size, stack_top). execfn is the path given to execve.
 * Returns the user RSP (pointing at argc), or 0 if it does not fit. */
uint64_t exec_build_stack(uint8_t *buf, size_t buf_size, uint64_t stack_top,
                          const ExecStrings *s, const char *execfn,
                          const ExecAux *aux);

#endif /* ARCHOS_PROC_EXEC_STACK_H */
