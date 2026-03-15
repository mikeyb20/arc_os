# scaffold-test

Generate a complete, buildable, registered test suite for a kernel subsystem in one command.

## When to Use

Invoke when adding tests for a kernel subsystem. This creates the test file with correct stubs, registers it in test_main.c, and adds it to CMakeLists.txt — ready to build and run.

## Invocation

- `/scaffold-test pipe` — auto-finds `kernel/fs/pipe.c`, generates stubs from its includes
- `/scaffold-test kernel/drivers/tty` — explicit path
- `/scaffold-test mymodule` — self-contained mode if no `.c` found

## Procedure

### Step 1: Resolve the Target Source File

Given the argument `<name>`:

1. If the argument contains `/` (e.g., `kernel/drivers/tty`), treat it as a path. Strip `kernel/` prefix if present to get `<layer>/<name>`. The source file is `kernel/<layer>/<name>.c`.
2. Otherwise, search for `kernel/**/<name>.c` using Glob. If exactly one match, use it. If multiple matches, ask the user which one.
3. If no `.c` file is found, use **self-contained mode** (Style B — see below).

Derive these values:
- `<name>`: the base filename without extension (e.g., `pipe`, `tty`, `keyboard`)
- `<layer_path>`: the path under `kernel/` (e.g., `fs/pipe`, `drivers/tty`)
- `<include_path>`: the `#include` path (e.g., `../kernel/fs/pipe.c`)

### Step 2: Check for Existing Test

Check if `tests/test_<name>.c` already exists. If it does, warn and ask the user before overwriting.

### Step 3: Analyze Target Includes (Style A — Include .c Directly)

Read the target `.c` file. Extract all `#include` lines. For each include, look up the stub block needed from the **Stub Lookup Table** below. Collect all needed guards and stubs.

### Step 4: Generate `tests/test_<name>.c`

Use the appropriate style:

**Style A** (target `.c` exists): Include the `.c` directly with header guards and stubs.
**Style B** (no target `.c`): Self-contained test with inline type definitions.

#### Style A Template

```c
/* arc_os — Host-side tests for <name> implementation */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Stubs for kernel dependencies --- */

/* Guard out kernel headers */
<HEADER_GUARDS from stub lookup>

<GFP_FLAGS if kmalloc guard present>

<STUB_BLOCKS from stub lookup>

/* Now include <name>.c directly */
#include "<include_path>"

/* --- Tests --- */

TEST(<name>_placeholder_init) {
    /* TODO: test initialization */
    ASSERT_TRUE(1);
    return 0;
}

TEST(<name>_placeholder_basic) {
    /* TODO: test basic functionality */
    ASSERT_TRUE(1);
    return 0;
}

/* --- Suite --- */

TestCase <name>_tests[] = {
    TEST_ENTRY(<name>_placeholder_init),
    TEST_ENTRY(<name>_placeholder_basic),
};
int <name>_test_count = sizeof(<name>_tests) / sizeof(<name>_tests[0]);
```

#### Style B Template (Self-Contained)

```c
/* arc_os — Host-side tests for <name> */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Inline type definitions (match kernel types) --- */

/* TODO: reproduce types from kernel headers here */

/* --- Tests --- */

TEST(<name>_placeholder_init) {
    /* TODO: test initialization */
    ASSERT_TRUE(1);
    return 0;
}

TEST(<name>_placeholder_basic) {
    /* TODO: test basic functionality */
    ASSERT_TRUE(1);
    return 0;
}

/* --- Suite --- */

TestCase <name>_tests[] = {
    TEST_ENTRY(<name>_placeholder_init),
    TEST_ENTRY(<name>_placeholder_basic),
};
int <name>_test_count = sizeof(<name>_tests) / sizeof(<name>_tests[0]);
```

### Step 5: Edit `tests/test_main.c`

1. Add extern declarations after the last `extern` block (after line 66):
   ```c
   extern TestCase <name>_tests[];
   extern int <name>_test_count;
   ```

2. Add suite entry to the `suites[]` array (after line 117, before the closing `}`):
   ```c
   { "<name>",    <name>_tests,    &<name>_test_count },
   ```

### Step 6: Edit `tests/CMakeLists.txt`

1. Add `test_<name>.c` to the `add_executable(test_runner ...)` source list (after line 32, before the closing `)`).

2. Add CTest entry (after line 86):
   ```cmake
   add_test(NAME test_<name>    COMMAND test_runner --suite <name>)
   ```

3. If the target `.c` includes `<limine.h>` or headers from `kernel/include/`, add:
   ```cmake
   set_source_files_properties(test_<name>.c PROPERTIES
       COMPILE_FLAGS "-idirafter ${CMAKE_SOURCE_DIR}/kernel/include"
   )
   ```

### Step 7: Verify Build

Run `cmake -B build_host && cmake --build build_host` to confirm the new test compiles. Then run `ctest --test-dir build_host -R test_<name>` to confirm the placeholder tests pass.

### Step 8: Report

```
Created tests/test_<name>.c (<N> stubs, <M> placeholder tests)
Registered in tests/test_main.c (extern + suites[])
Added to tests/CMakeLists.txt (source + CTest entry)
Build: PASS / FAIL
Tests: PASS / FAIL (<M> placeholder tests)

Next: Replace placeholder tests with real test cases.
```

---

## Stub Lookup Table

When the target `.c` includes a kernel header, apply the corresponding guard define and stub block. All stubs MUST be `static` to avoid linker clashes with other test files.

### `lib/kprintf.h`

Guard: `#define ARCHOS_LIB_KPRINTF_H`

Stubs:
```c
/* kprintf/KERNEL_PANIC stubs */
static void kprintf(const char *fmt, ...) { (void)fmt; }
#define KERNEL_PANIC(fmt, ...) do { printf("PANIC: " fmt "\n", ##__VA_ARGS__); abort(); } while(0)
```

### `mm/kmalloc.h`

Guard: `#define ARCHOS_MM_KMALLOC_H`

Stubs:
```c
/* GFP flags */
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01

/* kmalloc/kfree/krealloc via libc */
static void *kmalloc(size_t size, uint32_t flags) {
    void *p = malloc(size);
    if (p && (flags & GFP_ZERO)) memset(p, 0, size);
    return p;
}
static void kfree(void *ptr) { free(ptr); }
static void *krealloc(void *ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    return realloc(ptr, new_size);
}
```

### `lib/mem.h`

Guard: `#define ARCHOS_LIB_MEM_H`

No stubs needed — libc `memset`/`memcpy`/`memcmp` work on host.

### `lib/string.h`

Guard: `#define ARCHOS_LIB_STRING_H`

No stubs needed — libc string functions work on host.

**Special case**: If the target `.c` defines functions that collide with libc names (e.g., `strlen`, `strcmp`), add rename macros:
```c
/* Rename to avoid libc collisions */
#define strlen k_strlen
#define strcmp k_strcmp
#define strncmp k_strncmp
#define strncpy k_strncpy
#define strchr k_strchr
```

### `proc/spinlock.h`

Guard: `#define ARCHOS_PROC_SPINLOCK_H`

Stubs:
```c
typedef struct { volatile uint32_t locked; uint64_t saved_flags; } Spinlock;
#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }
static inline void spinlock_acquire(Spinlock *lock) { lock->locked = 1; }
static inline void spinlock_release(Spinlock *lock) { lock->locked = 0; }
```

### `proc/waitqueue.h`

Guard: `#define ARCHOS_PROC_WAITQUEUE_H`

Stubs:
```c
struct Thread;
typedef struct WaitQueue { Spinlock lock; struct Thread *head; struct Thread *tail; } WaitQueue;
#define WAITQUEUE_INIT { .lock = SPINLOCK_INIT, .head = NULL, .tail = NULL }
static void wq_init(WaitQueue *wq) { (void)wq; }
static void wq_sleep(WaitQueue *wq, Spinlock *lock) { (void)wq; lock->locked = 0; }
static int wq_wake(WaitQueue *wq) { (void)wq; return 0; }
static int wq_wake_all(WaitQueue *wq) { (void)wq; return 0; }
```

**Dependency**: Requires the spinlock stubs above. If `waitqueue.h` is needed, `spinlock.h` guard + stubs must also be included.

### `proc/thread.h`

Guard: `#define ARCHOS_PROC_THREAD_H`

Stubs (adapt based on what the target actually uses):
```c
typedef enum { THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_DEAD } ThreadState;
typedef struct Thread {
    uint64_t tid;
    ThreadState state;
    uint64_t kernel_rsp;
    void *stack_base;
    struct Thread *next;
} Thread;
static Thread *thread_current(void) { static Thread t = {0}; return &t; }
```

### `proc/process.h`

Guard: `#define ARCHOS_PROC_PROCESS_H`

Stubs (adapt based on what the target actually uses):
```c
typedef enum { PROC_RUNNING, PROC_ZOMBIE } ProcessState;
typedef struct Process {
    uint32_t pid;
    ProcessState state;
    uint64_t page_table;
} Process;
static Process *proc_current(void) { static Process p = {0}; return &p; }
```

### `arch/x86_64/serial.h`

Guard: `#define ARCHOS_ARCH_X86_64_SERIAL_H`

Stubs:
```c
static void serial_putchar(char c) { (void)c; }
```

### `arch/x86_64/io.h`

Guard: `#define ARCHOS_ARCH_X86_64_IO_H`

Stubs:
```c
static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0; }
static inline void io_wait(void) {}
```

### `mm/vmm.h`

Guard: `#define ARCHOS_MM_VMM_H`

Stubs:
```c
static uint64_t vmm_get_hhdm_offset(void) { return 0; }
static int vmm_map_page_in(uint64_t pml4, uint64_t vaddr, uint64_t phys, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)phys; (void)flags; return 0;
}
```

### `mm/pmm.h`

Guard: `#define ARCHOS_MM_PMM_H`

Stubs:
```c
static uint64_t pmm_alloc_page(void) {
    void *p = malloc(4096);
    if (p) memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}
static void pmm_free_page(uint64_t addr) { free((void *)(uintptr_t)addr); }
```

---

## libc Name Collision Detection

If the target `.c` file defines any of these function names, they collide with libc on the host:
- `strlen`, `strcmp`, `strncmp`, `strncpy`, `strchr`, `strstr`, `memset`, `memcpy`, `memcmp`, `memmove`

Detection: grep the target `.c` for function definitions matching `^(type)\s+(name)\s*\(`. If found, add `#define name k_name` before the include, and use `k_name` in the tests.

Reference: `tests/test_string.c` uses this pattern.

---

## Edge Cases

- **Test file already exists**: Warn, show current test count, ask before overwriting
- **Kernel `.c` not found**: Use Style B (self-contained). Inform user: "No kernel source found for '<name>' — generating self-contained test."
- **Name collision with libc**: Auto-detect and add rename macros (see above)
- **Target includes `<limine.h>`**: Add `-idirafter` property in CMakeLists.txt (see Step 6)
- **Multiple kernel files match**: List matches, ask user to disambiguate
- **Target has complex dependency chain**: Read transitive includes and add stubs for all of them. The canonical pattern is test_pipe.c which stubs 4 subsystems.
- **Build fails after generation**: Read the compiler errors and fix — usually a missing stub or type mismatch
