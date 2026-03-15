# add-syscall

Generate all boilerplate for a new syscall — number define, handler stub, dispatch registration, user-space wrapper, test entry, and documentation.

## When to Use

Invoke when adding a new system call to the kernel. This creates the handler, assigns the number, registers it, adds the user-space wrapper, and updates tests and docs.

## Invocation

- `/add-syscall getppid` — zero-arg syscall, auto-assign number
- `/add-syscall getcwd buf:ptr size:size_t` — typed args with pointer validation
- `/add-syscall mmap addr:ptr len:size_t prot:int flags:int fd:fd offset:off_t` — 6-arg complex

## Argument Syntax

```
/add-syscall <name> [arg1:type arg2:type ...]
```

Arguments are `name:type` pairs. Supported type tags:

| Type Tag | C Type | Validation Generated |
|----------|--------|---------------------|
| `int` / `uint` / (bare) | `uint64_t` | None |
| `ptr` | `void *` | `user_ptr_valid(ptr, <companion_size>)` — looks for next `size_t` arg |
| `path` | `const char *` | `user_ptr_valid(ptr, 1)` |
| `fd` | `int` | `fd_get()` + null check returning `-EBADF` |
| `size_t` | `uint64_t` | None |
| `off_t` | `int64_t` | None |

Maximum 6 arguments (SYSCALL convention limit: RDI, RSI, RDX, R10, R8, R9). Error if more than 6.

## Procedure

### Step 1: Parse Invocation

Extract `<name>` and argument list. Validate:
- Name is `snake_case` and lowercase
- Arg count <= 6
- All type tags are recognized

### Step 2: Determine Syscall Number

Read `kernel/arch/x86_64/syscall.h` to get all existing `SYS_*` defines.

**Check if already defined**:
- If `SYS_<NAME>` exists AND has a registered handler in `syscall_init()` in `kernel/arch/x86_64/syscall.c`: report "SYS_<NAME> already exists with handler at number <N>." Stop.
- If `SYS_<NAME>` exists but has NO handler registered: report "SYS_<NAME> is defined at number <N> but not implemented. Generating handler stub at that number." Use the existing number.

**Assign new number**:
- Current defined numbers (as of Phase 7): 0-22
- Pre-defined but unimplemented: SYS_FSTAT(12), SYS_DUP(13), SYS_GETPPID(15) — these have defines but no handlers
- Next available: find the first gap in 0-63, or use max+1
- If number >= current `SYSCALL_MAX` (64), bump `SYSCALL_MAX` and warn

### Step 3: Generate Handler Stub

Add the handler function in `kernel/arch/x86_64/syscall.c`, just before the `/* --- Dispatcher --- */` comment.

Template:
```c
/* SYS_<NAME>: <brief description — ask user or infer from name> */
static int64_t sys_<name>(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    <void_unused_args>
    <ptr_validation_blocks>
    <fd_validation_blocks>
    /* TODO: implement */
    return -ENOSYS;
}
```

For unused args (args not in the invocation), add `(void)aN;` casts.

**Pointer validation** (for `ptr` and `path` typed args):
```c
if (!user_ptr_valid((const void *)a<N>, <companion_size>)) return -EINVAL;
```
For `ptr`: companion size is the next `size_t` arg value. If no companion, use `1`.
For `path`: always `user_ptr_valid(ptr, 1)`.

**FD validation** (for `fd` typed args):
```c
Process *p = proc_current();
if (p == NULL || p->fd_table == NULL) return -ENOSYS;
VfsFile *file = fd_get(p->fd_table, (int)a<N>);
if (file == NULL) return -EBADF;
```

### Step 4: Register in `syscall_init()`

Add the registration call in `syscall_init()`, in the `/* 5. Register built-in handlers */` section:
```c
syscall_register(SYS_<NAME>, sys_<name>);
```

Group with related syscalls (process syscalls together, file syscalls together, etc.).

### Step 5: Add Define to `kernel/arch/x86_64/syscall.h`

If not already defined, add the `#define SYS_<NAME> <N>` after the last existing define, maintaining numerical order.

### Step 6: Add User-Space Wrapper to `userland/shell/shell.c`

1. Add `#define SYS_<NAME> <N>` to the syscall numbers section (after line 35).

2. If the syscall needs more than 3 args and only `syscall3` exists, add the needed wrapper. Check if `syscall4`/`syscall5`/`syscall6` already exist first.

**syscall4** (if needed and not present):
```c
static inline int64_t syscall4(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

**syscall5** (if needed and not present):
```c
static inline int64_t syscall5(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8  __asm__("r8")  = a4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

**syscall6** (if needed and not present):
```c
static inline int64_t syscall6(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8  __asm__("r8")  = a4;
    register uint64_t r9  __asm__("r9")  = a5;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

### Step 7: Add Dispatch Tests to `tests/test_syscall.c`

Add 2 tests to the test file:

```c
TEST(<name>_number_routed) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(<N>, handler_add);
    ASSERT_EQ(test_dispatch(<N>, 10, 20, 0, 0, 0, 0), 30);
    return 0;
}

TEST(<name>_unregistered_returns_enosys) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    ASSERT_EQ(test_dispatch(<N>, 0, 0, 0, 0, 0, 0), -38);
    return 0;
}
```

Add them to the `syscall_tests[]` array:
```c
TEST_ENTRY(<name>_number_routed),
TEST_ENTRY(<name>_unregistered_returns_enosys),
```

Update the comment at the top if one exists about test count.

### Step 8: Update Documentation

Edit `.claude/skills/arc-os-architecture/reference/syscall-table.md`:
- Add entry to the "Scaffolded Numbering" table with the new number, name, and notes

### Step 9: Verify

1. Run host tests: `cmake -B build_host && cmake --build build_host && ctest --test-dir build_host -R test_syscall`
2. Run kernel build: `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake && cmake --build build`
3. Report results

### Step 10: Report

```
Added SYS_<NAME> = <N>
  Handler:    kernel/arch/x86_64/syscall.c (sys_<name>, returns -ENOSYS stub)
  Define:     kernel/arch/x86_64/syscall.h
  Registered: syscall_init() in syscall.c
  Userland:   userland/shell/shell.c (#define SYS_<NAME>)
  Tests:      tests/test_syscall.c (+2 dispatch tests)
  Docs:       .claude/skills/arc-os-architecture/reference/syscall-table.md

Host tests: PASS/FAIL
Kernel build: PASS/FAIL

Next: Implement the TODO body in sys_<name>().
```

---

## Pre-Defined Unimplemented Syscalls

These have `#define` entries in syscall.h but no handler registered:

| Number | Name | Likely Signature |
|--------|------|------------------|
| 12 | SYS_FSTAT | `int fstat(int fd, struct stat *buf)` |
| 13 | SYS_DUP | `int dup(int oldfd)` |
| 15 | SYS_GETPPID | `pid_t getppid(void)` |

When implementing one of these, skip Step 5 (define already exists) and note in the report: "Used existing define SYS_<NAME> = <N>".

---

## Edge Cases

- **Name already has handler**: Report and stop (don't overwrite existing implementation)
- **Name defined but no handler**: Use existing number, generate handler stub
- **>6 args**: Error — "SYSCALL convention supports max 6 arguments (RDI,RSI,RDX,R10,R8,R9)"
- **Number >= SYSCALL_MAX**: Bump `SYSCALL_MAX` in syscall.h, warn about table size change
- **ptr arg without size companion**: Use `1` as size for validation (path-like behavior)
- **Multiple fd args**: Generate separate validation for each
- **syscall4/5/6 already exists**: Don't duplicate, just use it
- **User-space wrapper in other binaries**: Only shell.c is modified (init.c and echo.c use inline wrappers per-file; the user can copy from shell.c)
