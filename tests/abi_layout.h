#ifndef ARCHOS_TESTS_ABI_LAYOUT_H
#define ARCHOS_TESTS_ABI_LAYOUT_H

/* One ABI fact: a syscall number, a constant, a struct size or a field offset. */
typedef struct {
    const char *name;
    long        value;
} AbiItem;

/* Each table ends with a NULL name. */
extern const AbiItem abi_ours[];        /* abi_layout_ours.c: kernel/include/uapi */
extern const AbiItem abi_host_linux[];  /* abi_layout_host_linux.c: host Linux uapi */

#endif /* ARCHOS_TESTS_ABI_LAYOUT_H */
