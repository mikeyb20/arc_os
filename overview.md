# Operating System Development Roadmap (v2)

A long-term project outline for building a custom operating system. Designed around two core principles:

1. **Leverage First, Replace Later** — Use existing, battle-tested tools (bootloaders, libc, filesystems) behind clean abstraction boundaries. This lets you focus on the interesting kernel work early and swap in custom implementations at any time without touching the rest of the system.

2. **Scale Through Interfaces** — Every subsystem communicates through well-defined internal APIs. This enables parallel development, independent testing, and the ability to rip out and rewrite any layer without cascading breakage.

---

## Project Philosophy: The Abstraction Boundary Model

The OS is structured as a stack of replaceable layers. Each layer exposes a stable internal interface to the layers above it. The implementation behind that interface can be swapped freely.

```
┌─────────────────────────────────────────────────┐
│              User-Space Applications            │
├─────────────────────────────────────────────────┤
│              System Call Interface               │
├─────────────────────────────────────────────────┤
│   VFS   │  Scheduler  │  IPC  │  Net Stack      │
├─────────────────────────────────────────────────┤
│        Memory Manager  │  Driver Framework       │
├─────────────────────────────────────────────────┤
│     Hardware Abstraction Layer (HAL)             │
├─────────────────────────────────────────────────┤
│     Bootloader Protocol Interface                │
├─────────────────────────────────────────────────┤
│              Hardware / QEMU                     │
└─────────────────────────────────────────────────┘
```

Each horizontal line is an **abstraction boundary**. The key rule: layers only talk downward through the defined interface, never by reaching into another layer's internals.

**What this buys you:**
- Swap GRUB for Limine for your own bootloader — the kernel doesn't care, it reads a `BootInfo` struct
- Swap your bitmap allocator for a buddy allocator — the scheduler doesn't care, it calls `pmm_alloc_page()`
- Swap your FAT32 driver for ext2 — user programs don't care, they call `open()`/`read()`
- Port to a new architecture — only the HAL changes, everything above stays the same

---

## Phase 0: Foundations & Tooling

### 0.1 Target Architecture Selection
- **Recommended start**: x86_64 (best documentation, QEMU support, largest OS dev community)
- **Future port target**: RISC-V (cleaner ISA, growing ecosystem, validates your HAL design)
- ARM (AArch64) as a third option if targeting embedded/mobile
- Primary development on QEMU; Bochs as a secondary validator
- Plan HAL boundaries from day one so porting is realistic later

### 0.2 Cross-Compiler & Build System
- **Leverage**: Use a pre-built LLVM/Clang cross-compiler (easier to set up than GCC cross)
- **Build system**: CMake with a freestanding toolchain file (scales well, good IDE integration)
  - Alternative: Meson (cleaner syntax, native cross-compilation support)
- Linker scripts for kernel binary layout (per-architecture, selected by build config)
- **Leverage**: Docker or Nix flake for reproducible toolchain — anyone can build with one command
- Build targets: kernel image, initrd, bootable ISO, QEMU launch script
- Out-of-tree build support from the start (build/ directory, not polluting source tree)

### 0.3 Project Structure for Scale
```
myos/
├── kernel/
│   ├── arch/              # Architecture-specific (HAL implementations)
│   │   ├── x86_64/
│   │   └── riscv64/       # Added later, validates HAL
│   ├── boot/              # Boot protocol parsing
│   ├── mm/                # Memory management
│   ├── proc/              # Process & thread management
│   ├── fs/                # VFS and filesystem drivers
│   │   ├── vfs/
│   │   ├── ramfs/
│   │   ├── ext2/          # Added later
│   │   └── fat32/         # Added later
│   ├── drivers/           # Device drivers
│   │   ├── char/
│   │   ├── block/
│   │   └── net/
│   ├── net/               # Network stack
│   ├── ipc/               # IPC mechanisms
│   ├── security/          # Permissions, capabilities
│   └── lib/               # Kernel utility library (string, list, etc.)
├── libc/                  # User-space C library (or musl port)
├── userland/              # User-space programs
│   ├── init/
│   ├── shell/
│   └── coreutils/
├── tools/                 # Build tools, image creation scripts
├── tests/                 # Host-side unit tests, integration test scripts
├── docs/                  # Architecture decision records, API docs
└── CMakeLists.txt
```

### 0.4 Development Environment & Debugging
- **Leverage**: QEMU with `-s -S` flags for GDB remote debugging
- **Leverage**: QEMU `-serial stdio` for kernel log output (your most important debug tool)
- **Leverage**: QEMU `-monitor` for inspecting registers, memory, page tables at runtime
- Custom kernel logging framework (printk equivalent) with severity levels and subsystem tags
- QEMU test runner script: build → launch → check serial output → pass/fail
- **Leverage**: GitHub Actions or similar CI for automated build + boot tests on every commit
- Host-side unit testing with a framework like Google Test (test kernel data structures and algorithms natively on the host, no boot required)

### 0.5 Documentation & Architecture Decision Records
- Maintain ADRs (Architecture Decision Records) for every major choice
- Internal API documentation for each subsystem boundary
- Reference material index: Intel SDM, AMD APM, ACPI spec, OSDev Wiki, relevant RFCs
- Design docs before implementation for each phase (we'll build these together)

---

## Phase 1: Booting & Early Kernel

### 1.1 Bootloader Protocol Abstraction
- **Leverage initially**: Limine bootloader (modern, well-maintained, easy to use)
  - Alternative: GRUB2 with Multiboot2 protocol
  - Limine's protocol gives you: memory map, framebuffer, ACPI RSDP, kernel load address, SMP info
- **Abstraction boundary**: Define a `BootInfo` structure that your kernel consumes
  - Memory map entries, framebuffer info, ACPI pointer, command line, initrd location
  - The kernel ONLY reads `BootInfo` — it never knows which bootloader provided it
- **Replace later**: Write your own UEFI bootloader that populates the same `BootInfo` struct
  - This becomes a standalone project you can tackle in any phase
  - Your kernel won't need a single line changed

### 1.2 Early Kernel Initialization
- Assembly entry stub (minimal, arch-specific, lives in `kernel/arch/x86_64/`)
- Parse `BootInfo` to discover memory, framebuffer, ACPI
- Early console output: framebuffer text rendering with a built-in bitmap font
- BSS zeroing and C++ global constructor invocation
- Early kernel log over serial (always available, even when framebuffer breaks)
- Transition to C/C++ as fast as possible — the assembly stub should be <100 lines

### 1.3 Hardware Abstraction Layer (HAL) — Architecture Setup
- **Key insight**: Everything arch-specific goes behind a HAL interface from day one
- HAL interface includes:
  - `hal_init()` — GDT, IDT, segment registers, CPU mode setup
  - `hal_enable_interrupts()` / `hal_disable_interrupts()`
  - `hal_read_timer()` / `hal_set_timer()`
  - `hal_invalidate_page()` / `hal_switch_address_space()`
  - `hal_context_switch()`
  - `hal_get_cpu_id()` (for SMP later)
- x86_64 implementation: GDT, IDT, long mode verification, SSE/AVX enable
- This is where you'd add `kernel/arch/riscv64/` later with the same interface

### 1.4 Interrupt & Exception Handling
- ISR stubs in arch-specific assembly
- Generic interrupt dispatcher in C/C++ (arch code calls into generic handler)
- CPU exception handlers: page fault, GPF, double fault, invalid opcode
- **Leverage initially**: Use PIC (simpler) for interrupt routing
- **Replace later**: APIC/IOAPIC (required for SMP, more capable)
- Interrupt registration API: drivers register handlers for specific IRQs
- Double-fault handler with dedicated stack (IST entry on x86_64)

### 1.5 Timer & Timekeeping
- **Leverage initially**: PIT for basic tick generation (simple, always available)
- **Replace later**: APIC timer (per-CPU, necessary for SMP) or HPET
- Timekeeping abstraction: `timer_get_ticks()`, `timer_set_callback(interval, fn)`
- Wall-clock time from RTC, monotonic time from tick counter
- Foundation for scheduler quantum and sleep/timeout mechanisms

---

## Phase 2: Memory Management

### 2.1 Physical Memory Manager (PMM)
- Parse memory map from `BootInfo` (mark usable, reserved, ACPI, bootloader-reclaimable)
- **Start with**: Bitmap allocator (simple to implement, easy to debug)
- **Replace later**: Buddy allocator (faster, less fragmentation, O(log n) alloc)
- PMM interface: `pmm_alloc_page()`, `pmm_free_page()`, `pmm_alloc_contiguous(n)`
- Memory zone tracking (DMA-capable below 16MB, normal, high)
- Statistics: total pages, free pages, allocated pages — queryable from debug shell
- Reserve kernel image pages, framebuffer pages, initrd pages

### 2.2 Virtual Memory Manager (VMM)
- Page table management (4-level on x86_64, behind HAL for portability)
- HAL paging interface: `hal_map_page(virt, phys, flags)`, `hal_unmap_page(virt)`, `hal_switch_address_space(pml4)`
- Higher-half kernel mapping (kernel lives at 0xFFFF800000000000 or similar)
- Kernel address space layout:
  - Physical memory identity map (or mapped at known offset)
  - Kernel code/data
  - Kernel heap region
  - Per-CPU regions (for SMP later)
  - Device MMIO mappings
- Page fault handler: currently just panic with diagnostic info, extended later for demand paging and COW

### 2.3 Kernel Heap (kmalloc)
- **Start with**: Simple linked-list allocator or dlmalloc-style
- **Replace later**: Slab allocator for frequently allocated objects (TCBs, inodes, etc.)
- Interface: `kmalloc(size, flags)`, `kfree(ptr)`, `krealloc(ptr, new_size)`
- Flags: `GFP_KERNEL`, `GFP_DMA`, `GFP_ZERO`
- Debug features from day one: allocation canaries, free-after-use poisoning, allocation tracking
- Heap grows by requesting pages from the VMM

### 2.4 User-Space Memory Management (deferred to Phase 5)
- Per-process address spaces with separate page tables
- Copy-on-write (COW) for fork()
- mmap / munmap for memory-mapped files and anonymous mappings
- brk/sbrk for user heap
- Swapping and page replacement — much later, when everything else is solid

---

## Phase 3: Process & Thread Management

### 3.1 Kernel Threading
- Thread Control Block (TCB): register save area, kernel stack pointer, state, priority
- Context switch through HAL: `hal_context_switch(old_tcb, new_tcb)`
- Kernel thread create/destroy API
- Idle thread (runs when nothing else is schedulable, halts CPU)
- Thread-local storage mechanism for per-thread kernel data
- Kernel work queues: deferred work items processed by worker threads

### 3.2 Process Abstraction
- Process Control Block (PCB): PID, address space, file descriptor table, children list, state
- Process states: Created → Ready → Running → Blocked → Zombie → Terminated
- **Design choice**: Decide early between fork+exec (Unix) vs. spawn (Windows/Plan 9) model
  - fork+exec is more complex (COW paging) but is standard for Unix-like
  - spawn is simpler and arguably more modern
  - Can support both behind a common interface
- Process hierarchy: parent tracking, orphan reparenting to init
- PID allocation: simple incrementing counter initially, bitmap or recycling later

### 3.3 Scheduler
- **Abstraction boundary**: Scheduler pluggability — define a scheduler interface
  - `sched_pick_next()`, `sched_add_thread()`, `sched_remove_thread()`, `sched_yield()`
- **Start with**: Round-robin (dead simple, gets multitasking working)
- **Replace later**: Priority-based with multilevel feedback queues
- **Replace even later**: CFS-like fair scheduler with per-CPU run queues
- Cooperative scheduling first (threads explicitly yield), then add timer-driven preemption
- Sleep queue: threads blocked on timers, I/O, or locks
- SMP awareness designed in from the start (per-CPU run queues) even if SMP comes later

### 3.4 Synchronization Primitives
- Spinlocks with interrupt disabling (for short critical sections)
- Mutexes (sleeping locks for longer critical sections)
- Semaphores, condition variables, reader-writer locks
- **Leverage**: Use proven algorithms (MCS locks, ticket locks) rather than inventing your own
- Lock ordering discipline: document a global lock hierarchy to prevent deadlocks
- Debug feature: lock dependency tracking and deadlock detection (like Linux's lockdep)

---

## Phase 4: Hardware Abstraction & Device Drivers

### 4.1 Driver Framework
- Driver registration system: drivers register with a bus/device type
- Probe/attach model: bus enumerates devices, calls matching driver's probe function
- Interrupt routing: driver registers an IRQ handler through a central dispatcher
- I/O model: interrupt-driven with optional DMA support
- **Scalability**: Loadable kernel modules (optional, adds significant complexity)
  - Alternative: compile-time driver selection (simpler, still flexible)

### 4.2 Bus Enumeration
- PCI/PCIe enumeration: scan configuration space, build device tree
- **Leverage**: ACPI table parsing with a minimal ACPI library
  - uACPI (lightweight, designed for hobby OSes) or LAI (Lightweight ACPI Implementation)
  - Gives you: interrupt routing (MADT), power management (FADT), device discovery
- USB host controller discovery via PCI
- Device tree support for ARM/RISC-V (when you port)

### 4.3 Essential Drivers (Minimum Viable OS)
- **Serial / UART**: 16550 — first driver you write, your debug lifeline
- **Framebuffer**: Parse from BootInfo, pixel drawing, bitmap font text console
- **Keyboard**: PS/2 first (simpler, works in QEMU), USB HID later
- **Storage**: 
  - **Leverage initially**: VirtIO block device (simplest, QEMU-native, well-documented)
  - **Replace later**: AHCI (SATA) for real hardware, NVMe for modern drives
- **RTC**: Basic wall-clock time, CMOS read on x86

### 4.4 VirtIO as a Development Accelerator
- VirtIO is a family of virtual device standards specifically designed for VMs
- Much simpler than real hardware drivers, well-documented, consistent interface
- Available VirtIO devices: block, net, console, GPU, input, filesystem (9p), RNG
- **Strategy**: Get your OS functional with VirtIO devices in QEMU, then write real hardware drivers as a separate workstream
- Shared virtqueue ring buffer model — learn it once, apply to all VirtIO devices

### 4.5 Advanced Drivers (future workstreams)
- USB stack: xHCI host controller → USB device framework → HID, mass storage, etc.
- NVMe driver for modern storage
- GPU: modesetting, framebuffer mode switching, resolution detection
- Audio: Intel HDA
- Each of these is a substantial sub-project that can be worked on independently

---

## Phase 5: System Calls & User Space

### 5.1 System Call Interface
- **Abstraction boundary**: The syscall table IS the user-kernel contract
- Syscall entry: SYSCALL/SYSRET on x86_64 (fast path, no interrupt overhead)
- Syscall dispatcher: table of function pointers indexed by syscall number
- Calling convention: arguments in registers, return value in rax
- Argument validation: every pointer from user space must be checked before dereference
- Versioning strategy: reserve syscall number ranges, never reuse numbers
- Start with a minimal set: exit, write, read, open, close, fork/spawn, exec, wait, mmap, brk, getpid

### 5.2 ELF Loader
- Parse ELF64 headers and program headers
- Load PT_LOAD segments into user address space
- Set up user stack with argc, argv, envp, auxiliary vector
- Handle PIE (Position-Independent Executables) for ASLR later
- **Leverage**: Study musl's ELF loader for reference implementation details

### 5.3 C Library (libc)
- **Leverage initially**: Port musl libc (lightweight, well-written, POSIX-compliant)
  - Requires implementing the syscalls musl expects (this drives your syscall design)
  - Gives you: printf, malloc, string functions, pthreads stubs, everything
  - Massively accelerates user-space development
- **Alternative**: Port newlib (more common in embedded/hobby OS, simpler syscall layer)
- **Replace later**: Write your own libc if you want full control or a non-POSIX API
- **Abstraction boundary**: User programs link against libc, never call syscalls directly
  - This means you can change syscall numbers/conventions without breaking programs

### 5.4 Init Process & User-Space Bootstrap
- PID 1 (init) is loaded from the initrd filesystem
- Init's responsibilities: mount root filesystem, start essential services, launch shell
- **Leverage initially**: A statically linked init binary you cross-compile
- **Replace later**: A proper init system (sysvinit-like, or something custom)
- First milestone: init calls write() to print "Hello from userspace!" over syscall

### 5.5 Program Execution Pipeline
- exec() family: load a new ELF binary into the current process's address space
- Dynamic linking (deferred — substantial complexity, requires a linker/loader like ld.so)
- Environment variables and argument passing
- Working directory tracking in PCB
- shebang (#!) handling for scripts

---

## Phase 6: File Systems

### 6.1 Virtual File System (VFS) Layer
- **This is the critical abstraction** — all filesystem access goes through VFS
- VFS operations interface (each FS driver implements this):
  - `open`, `close`, `read`, `write`, `seek`, `stat`, `readdir`
  - `mkdir`, `rmdir`, `unlink`, `rename`, `chmod`, `chown`
  - `mount`, `unmount`
- Inode abstraction: unique ID, type, size, permissions, timestamps, ops pointer
- Dentry cache: resolved path components cached for performance
- File descriptor table: per-process, maps fd numbers to open file objects
- Mount table: maps mount points to filesystem instances
- Path resolution: walk dentry tree, handle `.`, `..`, symlinks, mount crossings

### 6.2 Initial Filesystem: ramfs/tmpfs
- In-memory filesystem — no disk needed, perfect for early development
- Implement full VFS interface against in-memory data structures
- Used for rootfs before real disk filesystem is ready
- initrd contents extracted into ramfs at boot
- Stays useful forever: tmpfs for /tmp, /run, /dev/shm

### 6.3 On-Disk File System
- **Leverage initially**: FAT32 implementation
  - Simpler than ext2, well-documented, useful for EFI system partitions
  - Read-write support is tractable as a first on-disk FS
- **Replace/add later**: ext2 (good learning exercise, more Unix-native)
- **Replace/add later**: Custom filesystem designed around your OS's strengths
- Block device layer: abstract over VirtIO-blk, AHCI, NVMe
  - `block_read(dev, lba, count, buf)`, `block_write(dev, lba, count, buf)`
- Buffer cache / page cache: cache disk blocks in memory, write-back policy
- **Much later**: Journaling for crash recovery (ext3/ext4-style)

### 6.4 Special Filesystems
- devfs: Device nodes (/dev/null, /dev/zero, /dev/tty, /dev/sda, etc.)
- procfs: Process info (/proc/[pid]/status, /proc/meminfo, /proc/cpuinfo)
- sysfs: Device/driver topology
- These are all just VFS implementations backed by kernel data structures instead of disk

---

## Phase 7: Inter-Process Communication (IPC)

### 7.1 Core IPC Mechanisms
- **Pipes**: Unidirectional byte streams via kernel ring buffer
  - Implement as a VFS file type — read/write work on pipe fds
- **FIFOs**: Named pipes, visible in the filesystem
- **Signals**: POSIX signal delivery (SIGTERM, SIGKILL, SIGCHLD, etc.)
  - Signal handler registration, default actions, signal masking
- **Wait/exit**: Process exit status collection, zombie reaping

### 7.2 Advanced IPC (deferred)
- Shared memory (mmap with MAP_SHARED)
- Unix domain sockets
- Message passing (if going microkernel or hybrid)
- Eventfd, epoll/kqueue-style event notification
- These become important when you have complex multi-process applications

---

## Phase 8: Networking

### 8.1 Network Driver
- **Leverage initially**: VirtIO-net (simplest, well-documented, QEMU-native)
- **Replace later**: Intel e1000/e1000e (ubiquitous real hardware, great documentation)
- Common NIC interface: `nic_send(packet)`, `nic_set_receive_callback(fn)`
- Ring buffer management for transmit and receive descriptors

### 8.2 Network Stack
- **Leverage option**: Port lwIP (Lightweight IP) — a full TCP/IP stack designed for embedded
  - Gives you TCP, UDP, DHCP, DNS, ICMP immediately
  - Lets you focus on the socket API and driver integration
  - Well-tested, used in production embedded systems
- **Build your own** (if you want the learning experience):
  - Layer 2: Ethernet frame handling, MAC addressing
  - ARP: Address resolution, ARP cache
  - IPv4: Packet routing, fragmentation, TTL
  - ICMP: Ping (first network milestone: respond to ping)
  - UDP: Simple datagram delivery
  - TCP: Connection state machine, sliding window, retransmission (this is the big one)
  - Checksum calculation for each layer
- **Abstraction boundary**: Socket layer talks to "the network stack" regardless of which implementation is behind it

### 8.3 Socket API
- BSD socket interface: socket, bind, listen, accept, connect, send, recv, close
- Socket buffer management (sk_buff equivalent)
- Blocking and non-blocking I/O modes
- select/poll/epoll-style multiplexing (ties into your scheduler's wait mechanisms)
- Integrate with VFS: sockets are file descriptors

### 8.4 Network Services
- DHCP client (get an IP address automatically)
- DNS stub resolver (resolve hostnames)
- Loopback interface (127.0.0.1)
- Routing table with default gateway support

---

## Phase 9: Security & Permissions

### 9.1 User & Group System
- UID/GID model (root = 0, regular users > 0)
- /etc/passwd and /etc/group parsing (or simpler config format)
- Login authentication (PAM is overkill — simple password hash checking)
- Process credentials: real UID, effective UID, saved UID
- su/sudo equivalent for privilege escalation

### 9.2 File Permissions
- Unix rwx model: owner, group, other
- Permission checking in VFS on every file operation
- setuid/setgid execution
- umask per-process
- **Later**: POSIX ACLs or capability-based security

### 9.3 Kernel Hardening
- Stack canaries on kernel functions (-fstack-protector)
- Guard pages around kernel stacks (detect stack overflow)
- W^X enforcement: no memory region is both writable and executable
- ASLR for user-space binaries (randomize mmap base, stack, heap)
- KASLR: randomize kernel load address (requires bootloader support)
- Syscall argument sanitization (no kernel pointer leaks)

---

## Phase 10: Shell & User-Space Utilities

### 10.1 Shell
- **Leverage option**: Port an existing shell (dash — minimal POSIX shell, ~25k lines)
  - Gives you a fully functional shell immediately
  - Requires: fork, exec, wait, pipe, dup2, open, close, read, write, signal handling
  - Great validation that your syscall layer is correct
- **Build your own** (in parallel or as replacement):
  - Line editor with basic editing (backspace, cursor movement)
  - Command parsing: tokenize, handle pipes, redirects, background (&)
  - Built-in commands: cd, exit, export, alias, echo
  - External command execution: fork + exec + wait
  - I/O redirection: >, >>, <, 2>&1
  - Piping: cmd1 | cmd2 | cmd3
  - Job control: Ctrl+C, Ctrl+Z, fg, bg, jobs
  - Tab completion and history — nice to have, not essential

### 10.2 Core Utilities
- **Leverage**: Port BusyBox — single binary providing 300+ Unix utilities
  - Extremely well-tested, designed for minimal systems
  - Gives you ls, cp, cat, grep, mount, ps, kill, and hundreds more
  - Validates your libc and syscall layer extensively
  - Requires: a working libc, fork/exec, basic filesystem operations
- **Build your own** selectively (for specific utils you want to understand deeply):
  - File: ls, cat, cp, mv, rm, mkdir, chmod, chown
  - Text: grep, wc, sort, head, tail
  - System: ps, kill, mount, free, uname, dmesg
  - A minimal text editor (ed-like first, then something screen-based)

### 10.3 System Image & Package Management
- initrd/initramfs builder script: pack kernel + root filesystem into bootable image
- Cross-compilation sysroot for building user-space programs
- **Leverage**: A Makefile-based ports system (like CRUX or OpenBSD ports)
  - Each "port" is a Makefile that downloads, patches, cross-compiles, and installs a package
- **Later**: A proper package manager with dependency tracking

---

## Phase 11: Graphics & Windowing (Optional)

### 11.1 Framebuffer Console
- Bitmap font rendering on linear framebuffer
- Hardware cursor or software cursor
- Scrolling, ANSI escape code support (colors, cursor movement)
- Virtual terminal switching (Ctrl+Alt+F1-F6 style)
- Double buffering to prevent tearing

### 11.2 Display Server & Windowing
- **Leverage option**: Port a minimal Wayland compositor (like TinyWL)
  - Requires: a working framebuffer, input events, shared memory IPC
- **Build your own**:
  - Compositor: client submits buffer → server composites to screen
  - Window management: move, resize, focus, z-order
  - Input event routing to focused client
  - Shared memory for client framebuffers
  - Simple widget toolkit for basic UI elements

### 11.3 GPU (far future)
- Modesetting: resolution and display mode switching
- **Leverage**: Port Mesa for OpenGL/Vulkan (enormous undertaking)
- More realistic: 2D software rendering with hardware-assisted blitting

---

## Phase 12: Symmetric Multiprocessing (SMP)

### 12.1 Multi-Core Bring-Up
- **Leverage**: Limine can provide SMP info and start APs for you
- AP (Application Processor) initialization: each core gets its own GDT, IDT, stack, TSS
- Per-CPU data structure (accessed via GS segment on x86_64 or tp register on RISC-V)
- Inter-Processor Interrupts (IPIs) for cross-core communication
- TLB shootdown: when page tables change, notify other cores

### 12.2 SMP-Safe Kernel
- Audit all locks: convert uniprocessor interrupt-disable to proper spinlocks
- Per-CPU run queues in scheduler
- Lock-free data structures where appropriate (per-CPU counters, RCU for read-heavy paths)
- Big Kernel Lock initially (one lock for everything), then fine-grained locking

### 12.3 Advanced Scheduling (SMP)
- Load balancing across CPU run queues
- CPU affinity: pin threads to specific cores
- NUMA awareness for memory allocation (if targeting NUMA hardware)
- Core parking for power management

---

## Phase 13: Stability, Testing & Hardening

### 13.1 Testing Infrastructure
- **Leverage**: Host-side unit tests with Google Test or Catch2
  - Test kernel algorithms (allocators, schedulers, data structures) on the host
  - Much faster iteration than booting a VM
- Integration tests: QEMU boot scripts that run test programs and check serial output
- **Leverage**: syzkaller-style syscall fuzzing (once syscall layer is stable)
- Stress tests: fork bombs, memory pressure, network flood, filesystem torture
- **Leverage**: Kernel sanitizers where possible (KASAN for memory, UBSAN for undefined behavior)

### 13.2 Performance & Profiling
- In-kernel profiling: function entry/exit tracing, timestamp sampling
- Benchmarks: syscall latency, context switch time, filesystem throughput, network bandwidth
- Page fault rate monitoring, cache miss analysis
- Boot time optimization (deferred initialization, parallel driver probing)

### 13.3 Documentation
- Architecture Decision Records for every major choice
- Subsystem interface documentation (the "contracts" between layers)
- System call reference manual
- Driver development guide
- User-space programming guide
- Build and contribution guide

---

## Existing Tools & Libraries Quick Reference

This table summarizes what you can leverage at each phase and what you'd build when replacing it:

| Phase | Leverage (Start With) | Build Your Own (Replace Later) |
|-------|----------------------|-------------------------------|
| Boot | Limine bootloader | Custom UEFI bootloader |
| ACPI | uACPI or LAI | Custom ACPI parser |
| Storage | VirtIO-blk | AHCI, NVMe drivers |
| Network | VirtIO-net | e1000, real NIC drivers |
| Net Stack | lwIP (optional) | Custom TCP/IP stack |
| libc | musl or newlib | Custom libc |
| Shell | dash (port) | Custom shell |
| Utilities | BusyBox (port) | Custom coreutils |
| Windowing | TinyWL / custom | Full compositor |
| Testing | Google Test, syzkaller | Custom test harness |

---

## Suggested Order of Attack (Revised)

Each step has a concrete milestone — a thing you can boot and demonstrate.

1. **Phase 0** → Toolchain builds, QEMU launches, GDB attaches
2. **Phase 1.1–1.2** → Kernel boots via Limine, prints "Hello" to framebuffer and serial
3. **Phase 1.3–1.5** → Interrupts work, timer ticks, keyboard echoes characters
4. **Phase 2.1–2.3** → Physical allocator, paging, kmalloc all working; can allocate and free
5. **Phase 3.1–3.3** → Two kernel threads alternating output on screen (multitasking works)
6. **Phase 4.3** → VirtIO-blk reads a sector from a disk image
7. **Phase 6.1–6.2** → VFS + ramfs: can create/read/write files in memory
8. **Phase 5.1–5.4** → First user-space ELF binary runs, calls write() syscall
9. **Phase 5.3** → musl libc ported, user programs can use printf()
10. **Phase 6.3** → FAT32 read/write on VirtIO-blk disk image
11. **Phase 10** → Shell running (ported dash or custom), can launch programs
12. **Phase 7** → Pipes and signals working — can do `ls | grep foo`
13. **Phase 8** → Ping works over VirtIO-net (ICMP echo reply)
14. **Phase 9** → Multi-user login, file permissions enforced
15. **Phase 12** → Boots and runs on multiple cores
16. **Phase 11, 13** → Graphics, polish, hardening — ongoing

---

## Key Design Decisions to Make Early

These ripple through the entire project. We should discuss each one in depth:

1. **Monolithic vs Microkernel vs Hybrid** — How much runs in kernel space? (Monolithic is simpler and what I'd recommend to start; microkernel is cleaner but harder to get performing well)
2. **Target Architecture** — x86_64 recommended for primary, RISC-V as validation port
3. **POSIX Compliance Level** — Following POSIX closely makes porting software vastly easier (musl, BusyBox, dash all assume POSIX). Recommended: POSIX-compatible with room for extensions
4. **Language** — C++ gives you RAII, templates, namespaces over C; Rust gives you safety but less OS dev ecosystem. C is the most battle-tested for kernels. Recommend C++ or C with your experience
5. **Kernel Personality** — Unix-like recommended (enables reuse of enormous ecosystem)
6. **Licensing** — MIT/BSD for maximum flexibility, GPL if you want copyleft
7. **Primary Use Case** — Shapes every priority decision downstream