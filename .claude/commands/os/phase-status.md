Show the current implementation status of all arc_os development phases.

## Steps

1. Read `overview.md` and check the "Suggested Order of Attack" section
2. Scan the codebase for implemented components:
   - Check which directories exist under `kernel/`
   - Check for key files: entry stub, HAL, PMM, VMM, scheduler, VFS, syscall table
   - Check for build system files: `CMakeLists.txt`, toolchain file, linker script
3. For each phase/sub-phase, report status:
   - **Not Started** — No code exists for this phase
   - **In Progress** — Some code exists but the milestone isn't met
   - **Complete** — The phase milestone is achieved and working

## Phase Milestones

| Phase | Milestone |
|-------|-----------|
| 0 | Toolchain builds, QEMU launches, GDB attaches |
| 1.1-1.2 | Kernel boots via Limine, prints to framebuffer and serial |
| 1.3-1.5 | Interrupts work, timer ticks, keyboard echoes |
| 2.1-2.3 | PMM, paging, kmalloc working; can allocate and free |
| 3.1-3.3 | Two kernel threads alternating output (multitasking) |
| 4.3 | VirtIO-blk reads a sector from disk image |
| 6.1-6.2 | VFS + ramfs: create/read/write files in memory |
| 5.1-5.4 | First user-space ELF binary runs, calls write() |
| 5.3 | musl ported, user programs can use printf() |
| 6.3 | FAT32 read/write on VirtIO-blk |
| 10 | Shell running, can launch programs |
| 7 | Pipes and signals, `ls | grep foo` works |
| 8 | Ping works over VirtIO-net |
| 9 | Multi-user login, file permissions |
| 12 | Boots on multiple cores |

Report as a formatted table with the current status of each phase.
