# /gen-tests — Generate Tests

## Instructions

You are generating tests for code that has already been implemented. Your
tests should verify the code works correctly, catch regressions, and
document expected behavior through examples.

Write tests that a future developer can read to understand what the code
is supposed to do — tests are documentation.

## Step 1: Identify Test Targets

If the user specified files or modules, test those. If not:
1. Read the plan's "Test Approach" section for guidance
2. Check `git diff main..HEAD --name-only` for recently changed files
3. Prioritize: new files first, then significantly modified files

## Step 2: Resolve the Target Source File

Given the argument `<name>`:

1. If the argument contains `/` (e.g., `kernel/drivers/tty`), treat it as a path.
   Strip `kernel/` prefix if present to get `<layer>/<name>`. Source: `kernel/<layer>/<name>.c`.
2. Otherwise, search for `kernel/**/<name>.c` using Glob. If exactly one match, use it.
   If multiple matches, ask the user.
3. If no `.c` file is found, use **self-contained mode** (Style B).

Derive:
- `<name>`: base filename without extension
- `<layer_path>`: path under `kernel/`
- `<include_path>`: the `#include` path (e.g., `../kernel/fs/pipe.c`)

## Step 3: Check for Existing Test

Check if `tests/test_<name>.c` already exists. If so, warn and ask before overwriting.

## Step 4: Analyze Existing Test Patterns

Examine existing test files in `tests/`:
- Test framework used (test_framework.h)
- Naming conventions
- Stub patterns
- Suite registration

Match existing patterns exactly. Do not introduce a new testing style.

## Step 5: Analyze Target Includes (Style A)

Read the target `.c` file. Extract all `#include` lines. For each include,
look up the stub block needed from the Stub Lookup Table below.

## Step 6: Generate Test File

### Style A Template (target `.c` exists)

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

TEST(<name>_init_basic) {
    /* Test initialization */
    ASSERT_TRUE(1);
    return 0;
}

/* ... 8+ tests covering happy path, edge cases, failure paths ... */

/* --- Suite --- */

TestCase <name>_tests[] = {
    TEST_ENTRY(<name>_init_basic),
    /* ... */
};
int <name>_test_count = sizeof(<name>_tests) / sizeof(<name>_tests[0]);
```

### Style B Template (self-contained, no target `.c`)

```c
/* arc_os — Host-side tests for <name> */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Inline type definitions (match kernel types) --- */
/* TODO: reproduce types from kernel headers here */

/* --- Tests --- */

TEST(<name>_placeholder_init) {
    ASSERT_TRUE(1);
    return 0;
}

/* --- Suite --- */

TestCase <name>_tests[] = {
    TEST_ENTRY(<name>_placeholder_init),
};
int <name>_test_count = sizeof(<name>_tests) / sizeof(<name>_tests[0]);
```

## Step 7: Test Categories (mandatory coverage)

### Happy Path (mandatory)
- Each public function with typical valid inputs
- At least one test per public interface

### Edge Cases (mandatory)
- Empty/nil/zero inputs
- Boundary values
- Single-element collections

### Error Conditions (mandatory for functions that can fail)
- Allocation failure via kmalloc_force_fail
- Invalid inputs produce appropriate errors (not crashes)
- Partial failure doesn't corrupt state

### Minimum: 8 test cases

## Step 8: Register the Suite

1. Add extern declarations to `tests/test_main.c`:
   ```c
   extern TestCase <name>_tests[];
   extern int <name>_test_count;
   ```

2. Add suite entry to `suites[]` array:
   ```c
   { "<name>",    <name>_tests,    &<name>_test_count },
   ```

3. Add `test_<name>.c` to `tests/CMakeLists.txt` `add_executable(test_runner ...)`.

4. Add CTest entry:
   ```cmake
   add_test(NAME test_<name>    COMMAND test_runner --suite <name>)
   ```

## Step 9: Validate

1. Build: `cmake -B build_host && cmake --build build_host`
2. Run: `ctest --test-dir build_host -R test_<name>`
3. Report results

## Stub Lookup Table

### `lib/kprintf.h`
Guard: `#define ARCHOS_LIB_KPRINTF_H`
```c
static void kprintf(const char *fmt, ...) { (void)fmt; }
#define KERNEL_PANIC(fmt, ...) do { printf("PANIC: " fmt "\n", ##__VA_ARGS__); abort(); } while(0)
```

### `mm/kmalloc.h`
Guard: `#define ARCHOS_MM_KMALLOC_H`
```c
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01
static void *kmalloc(size_t size, uint32_t flags) {
    void *p = malloc(size);
    if (p && (flags & GFP_ZERO)) memset(p, 0, size);
    return p;
}
static void kfree(void *ptr) { free(ptr); }
static void *krealloc(void *ptr, size_t old_size, size_t new_size) {
    (void)old_size; return realloc(ptr, new_size);
}
```

### `lib/mem.h`
Guard: `#define ARCHOS_LIB_MEM_H` — No stubs (libc works on host)

### `lib/string.h`
Guard: `#define ARCHOS_LIB_STRING_H` — No stubs (libc works on host).
If target defines colliding names (strlen, strcmp, etc.), add rename macros:
```c
#define strlen k_strlen
#define strcmp k_strcmp
```

### `proc/spinlock.h`
Guard: `#define ARCHOS_PROC_SPINLOCK_H`
```c
typedef struct { volatile uint32_t locked; uint64_t saved_flags; } Spinlock;
#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }
static inline void spinlock_acquire(Spinlock *lock) { lock->locked = 1; }
static inline void spinlock_release(Spinlock *lock) { lock->locked = 0; }
```

### `proc/waitqueue.h`
Guard: `#define ARCHOS_PROC_WAITQUEUE_H` (requires spinlock stubs)
```c
struct Thread;
typedef struct WaitQueue { Spinlock lock; struct Thread *head; struct Thread *tail; } WaitQueue;
#define WAITQUEUE_INIT { .lock = SPINLOCK_INIT, .head = NULL, .tail = NULL }
static void wq_init(WaitQueue *wq) { (void)wq; }
static void wq_sleep(WaitQueue *wq, Spinlock *lock) { (void)wq; lock->locked = 0; }
static int wq_wake(WaitQueue *wq) { (void)wq; return 0; }
static int wq_wake_all(WaitQueue *wq) { (void)wq; return 0; }
```

### `proc/thread.h`
Guard: `#define ARCHOS_PROC_THREAD_H`
```c
typedef enum { THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_DEAD } ThreadState;
typedef struct Thread { uint64_t tid; ThreadState state; uint64_t kernel_rsp;
    void *stack_base; struct Thread *next; } Thread;
static Thread *thread_current(void) { static Thread t = {0}; return &t; }
```

### `proc/process.h`
Guard: `#define ARCHOS_PROC_PROCESS_H`
```c
typedef enum { PROC_RUNNING, PROC_ZOMBIE } ProcessState;
typedef struct Process { uint32_t pid; ProcessState state; uint64_t page_table; } Process;
static Process *proc_current(void) { static Process p = {0}; return &p; }
```

### `arch/x86_64/serial.h`
Guard: `#define ARCHOS_ARCH_X86_64_SERIAL_H`
```c
static void serial_putchar(char c) { (void)c; }
```

### `arch/x86_64/io.h`
Guard: `#define ARCHOS_ARCH_X86_64_IO_H`
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
```c
static uint64_t vmm_get_hhdm_offset(void) { return 0; }
static int vmm_map_page_in(uint64_t pml4, uint64_t vaddr, uint64_t phys, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)phys; (void)flags; return 0;
}
```

### `mm/pmm.h`
Guard: `#define ARCHOS_MM_PMM_H`
```c
static uint64_t pmm_alloc_page(void) {
    void *p = malloc(4096); if (p) memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}
static void pmm_free_page(uint64_t addr) { free((void *)(uintptr_t)addr); }
```

## libc Name Collision Detection

If the target `.c` defines any of: `strlen`, `strcmp`, `strncmp`, `strncpy`,
`strchr`, `strstr`, `memset`, `memcpy`, `memcmp`, `memmove` — add
`#define name k_name` before the include. Reference: `tests/test_string.c`.

## CRITICAL RULES

- Do NOT modify implementation code — only create/modify test files
- Match the project's existing test patterns exactly
- All stubs MUST be `static`
- Tests MUST actually test something — no tautological assertions
