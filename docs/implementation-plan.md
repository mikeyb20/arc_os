# arc_os Implementation Plan — Chunked Build-Out

## Context

arc_os is a custom x86_64 OS (monolithic kernel, C11, NASM, Limine bootloader). The kernel boots in QEMU, has a working memory manager (PMM/VMM/kmalloc), preemptive multitasking, PCI enumeration, VirtIO-blk driver, and a VFS layer with ramfs. This plan breaks the overview.md roadmap into concrete, session-sized implementation chunks.

**User decisions**: Build x86_64-elf-gcc from source, medium chunk size (~100-300 lines/session), NASM assembly entry stub, install tools as step 1.

---

## Phase 0: Foundations & Tooling

### Chunk 0.1 — Install System Packages — [x] DONE
Install nasm, qemu, xorriso, and cross-compiler build dependencies.
```bash
sudo apt-get install -y nasm qemu-system-x86 xorriso mtools \
    build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo
```
**Milestone**: `nasm --version`, `qemu-system-x86_64 --version`, `xorriso --version` all succeed.

### Chunk 0.2 — Build x86_64-elf-gcc Cross-Compiler — [x] DONE
Build binutils 2.42 + GCC 14.1.0 targeting `x86_64-elf`, installed to `$HOME/opt/cross`. ~30-60 min compile time.

**Milestone**: `x86_64-elf-gcc --version` and `x86_64-elf-ld --version` succeed.

### Chunk 0.3 — Build System & Project Skeleton (~235 lines, 8 files) — [x] DONE
Create directory structure and all build infrastructure.

**Files**:
- `toolchain-x86_64.cmake` — Cross-compiler config, freestanding flags, linker script path
- `CMakeLists.txt` — Top-level: project, NASM support, kernel subdirectory, iso/run/debug targets
- `kernel/CMakeLists.txt` — Kernel sources, link to produce `kernel.elf`
- `kernel/arch/x86_64/linker.ld` — ELF64, ENTRY(_start), virtual base `0xFFFFFFFF80000000`, sections: `.limine_requests`, `.text`, `.rodata`, `.data`, `.bss`, exports boundary symbols
- `limine.conf` — Boot config (`protocol: limine`, `path: boot():/boot/kernel.elf`)
- `tools/run.sh` — Build + QEMU launch with `-serial stdio -m 256M -no-reboot`
- `tools/debug.sh` — Same but with `-s -S` for GDB attach
- `tools/make-iso.sh` — Create ISO with xorriso + limine bios-install

**Directories**: `kernel/{arch/x86_64,boot,lib,mm,proc,drivers,fs,net,ipc}`, `tools/`, `tests/`, `docs/`

**Milestone**: `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake` configures without error.

### Chunk 0.4 — Obtain Limine & Freestanding Headers — [x] DONE
- Clone Limine v8.x binary branch, build the `limine` CLI tool
- Clone `freestnd-c-hdrs-0bsd` for `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`

**Milestone**: `limine/limine-bios-cd.bin`, `limine.h`, and `stdint.h` all exist.

---

## Phase 1: Booting & Early Kernel

### Chunk 1.1 — NASM Entry Stub + Minimal kmain + First Boot (~180 lines, 5 files) — [x] DONE

**Files**:
- `kernel/arch/x86_64/entry.asm` — `_start`: zero BSS, call kmain, halt loop. Limine provides stack + long mode.
- `kernel/boot/limine_requests.c` — Embeds all Limine request structs (framebuffer, memmap, HHDM, RSDP, kernel address). ONLY file that includes `<limine.h>`.
- `kernel/boot/kmain.c` — Minimal `kmain(void)` that halts.
- `kernel/lib/mem.c` — `memcpy`, `memset`, `memmove`, `memcmp` (GCC requires these even freestanding).

**Milestone**: `qemu-system-x86_64 -cdrom build/arc_os.iso` boots without triple fault. CPU halts cleanly.

### Chunk 1.2 — Serial Port Output (~130 lines, 3 files) — [x] DONE

**Files**:
- `kernel/arch/x86_64/io.h` — Inline `inb`/`outb`/`inw`/`outw`/`io_wait`
- `kernel/arch/x86_64/serial.h` + `serial.c` — COM1 (0x3F8) init, putchar, puts

**Milestone**: QEMU serial shows `[BOOT] arc_os kernel booting...` and `[BOOT] Hello from kmain!`

### Chunk 1.3 — BootInfo Abstraction + kprintf (~300 lines, 4 files) — [x] DONE

**Files**:
- `kernel/boot/bootinfo.h` — Bootloader-agnostic `BootInfo`, `MemoryMapEntry`, `Framebuffer` structs
- `kernel/boot/limine.c` — Translates Limine responses → `BootInfo`. Only Limine-aware code.
- `kernel/lib/kprintf.h` + `kprintf.c` — Serial-backed kprintf with `%s`, `%d`, `%x`, `%p`, `%u`, `%lu`, `%lx`

**Milestone**: Serial prints memory map entries, HHDM offset, framebuffer dimensions, kernel addresses.

### Chunk 1.4 — Framebuffer Console (~290 lines, 3 files) — [ ] NOT STARTED (deferred)

**Files**:
- `kernel/drivers/fb_console.h` + `fb_console.c` — Pixel rendering with bitmap font, cursor, scrolling, newline
- `kernel/drivers/font8x16.c` — 8x16 VGA bitmap font (256 glyphs)

**Milestone**: QEMU window shows "arc_os v0.1" in white text on black background.

### Chunk 1.5 — GDT Setup (~140 lines, 3 files) — [x] DONE

**Files**:
- `kernel/arch/x86_64/gdt.h` + `gdt.c` — Null + kernel CS/DS + user CS/DS + TSS entry. Load GDTR.
- `kernel/arch/x86_64/gdt_load.asm` — `lgdt`, reload CS via far return, reload DS/ES/SS/FS/GS

**Milestone**: `[HAL] GDT loaded`. Verify with GDB: CS=0x08, DS=0x10. No triple fault.

### Chunk 1.6 — IDT + Exception Handlers (~300 lines, 4 files) — [x] DONE

**Files**:
- `kernel/arch/x86_64/idt.h` + `idt.c` — 256 IDT entries, `lidt`, `idt_set_gate()`
- `kernel/arch/x86_64/isr_stubs.asm` — Macro-generated ISR stubs for exceptions 0-31
- `kernel/arch/x86_64/exceptions.c` — C handler prints vector name, error code, RIP, RSP, CR2

**Milestone**: Deliberate divide-by-zero prints diagnostic instead of triple fault.

### Chunk 1.7 — PIC + IRQ Infrastructure (~200 lines, 4 files) — [x] DONE

**Files**:
- `kernel/arch/x86_64/pic.h` + `pic.c` — Remap PIC1→32-39, PIC2→40-47, EOI, mask/unmask
- `kernel/arch/x86_64/irq.h` + `irq.c` — Handler table, `irq_register_handler()`, `irq_dispatch()`
- Update `isr_stubs.asm` with IRQ stubs for vectors 32-47

**Milestone**: `[HAL] PIC initialized`. After `sti`, no triple fault.

### Chunk 1.8 — PIT Timer (~80 lines, 2 files) — [x] DONE

**Files**:
- `kernel/arch/x86_64/pit.h` + `pit.c` — Channel 0 at 100Hz, tick counter, handler prints every second

**Milestone**: Serial shows `[TIMER] 1 seconds`, `[TIMER] 2 seconds`, ...

### Chunk 1.9 — PS/2 Keyboard (~140 lines, 2 files) — [x] DONE

**Files**:
- `kernel/drivers/ps2_keyboard.h` + `ps2_keyboard.c` — IRQ 1 handler, scancode set 1 → ASCII, shift/caps, echo to serial + framebuffer

**Milestone**: Type in QEMU → characters appear on screen and serial. **First interactive milestone.**

### Chunk 1.10 — HAL Consolidation (~120 lines, 2 files) — [ ] NOT STARTED (deferred)

**Files**:
- `kernel/arch/hal.h` — Portable interface: `hal_init()`, `hal_early_putchar()`, `hal_enable/disable_interrupts()`, `hal_save/restore_interrupt_state()`, `hal_read_timer()`, `hal_set_timer()`
- `kernel/arch/x86_64/hal_x86_64.c` — Implements HAL by calling existing x86_64 functions

**Milestone**: Same behavior, cleaner architecture. HAL abstraction boundary enforced.

---

## Phase 2: Memory Management

### Chunk 2.1 — Physical Memory Manager (~190 lines, 2 files) — [x] DONE
- `kernel/mm/pmm.h` + `pmm.c` — Bitmap allocator. Parse BootInfo memory map, track 4KB frames.
- **Milestone**: Reports correct page counts. Alloc/free roundtrip works.

### Chunk 2.2 — Virtual Memory Manager (~280 lines, 3 files) — [x] DONE
- `kernel/mm/vmm.h` + `vmm.c` — 4-level page table management.
- `kernel/arch/x86_64/paging.c` — HAL wrappers for `invlpg`, CR3 read/write
- **Milestone**: Kernel switches to its own page tables and keeps running.

### Chunk 2.3 — Kernel Heap (~230 lines, 2 files) — [x] DONE
- `kernel/mm/kmalloc.h` + `kmalloc.c` — Free-list allocator at `0xFFFFFFFFC0000000`.
- **Milestone**: 1000 alloc/free cycles pass stress test.

---

## Phase 3: Threading & Scheduling

### Chunk 3.1 — Thread Control Block + Creation (~150 lines, 2 files) — [x] DONE
- `kernel/proc/thread.h` + `thread.c` — TCB struct, `thread_create()`.
- **Milestone**: Can create thread_t objects with allocated stacks.

### Chunk 3.2 — Context Switch + Cooperative Scheduler (~180 lines, 3 files) — [x] DONE
- `kernel/arch/x86_64/context_switch.asm` — Save/restore callee-saved regs
- `kernel/proc/sched.h` + `sched.c` — Round-robin run queue, `sched_yield()`
- **Milestone**: Two test threads print interleaved output.

### Chunk 3.3 — Preemptive Scheduling (~100 lines) — [x] DONE
- Hook PIT timer into scheduler.
- **Milestone**: Threads without explicit yield still alternate.

### Chunk 3.4 — Synchronization Primitives (~180 lines, 4 files) — [~] PARTIAL — spinlock done (header-only), mutex/condvar/semaphore deferred
- `kernel/proc/spinlock.h` + `spinlock.c` — cli + atomic test-and-set
- `kernel/proc/mutex.h` + `mutex.c` — Sleeping lock
- **Milestone**: Shared counter reaches exactly 20000.

---

## Phases 4-6: High-Level Breakdown

### Phase 4: Drivers
| Chunk | Description | ~Lines | Status |
|-------|-------------|--------|--------|
| 4.1 | ACPI table parsing (RSDP/RSDT/MADT) | 150 | NOT STARTED |
| 4.2 | PCI bus enumeration | 200 | DONE |
| 4.3 | VirtIO common infrastructure (virtqueues) | 250 | DONE |
| 4.4 | VirtIO block device driver | 200 | DONE |

### Phase 5: Syscalls & User Space
| Chunk | Description | ~Lines | Status |
|-------|-------------|--------|--------|
| 5.1 | SYSCALL/SYSRET entry, dispatcher table | 200 | DONE |
| 5.2 | Per-process address spaces | 200 | DONE |
| 5.3 | ELF64 loader | 250 | DONE |
| 5.4 | First user-space process | 150 | DONE |
| 5.5 | fork/exec/wait | 300 | DONE |

### Phase 6: File Systems
| Chunk | Description | ~Lines | Status |
|-------|-------------|--------|--------|
| 6.1 | VFS interface + data structures | 250 | DONE |
| 6.2 | ramfs (in-memory filesystem) | 300 | DONE |
| 6.3 | File syscalls (open/read/write/close/lseek/stat/mkdir/readdir/unlink) | 200 | DONE |
| 6.4 | FAT32 read support | 300 | NOT STARTED |

---

## Totals

| Phase | Chunks | ~Lines | Key Milestone |
|-------|--------|--------|---------------|
| 0 | 4 | 235 | Toolchain + build system + boots empty kernel |
| 1 | 10 | 1,880 | Interactive kernel: serial, framebuffer, keyboard, timer |
| 2 | 3 | 700 | Memory management: alloc pages, map virtual, kmalloc |
| 3 | 4 | 610 | Preemptive multitasking with synchronization |
| 4 | 4 | 800 | VirtIO block device reads sectors |
| 5 | 5 | 1,100 | User-space ELF binary runs, fork/exec/wait |
| 6 | 4 | 1,050 | VFS + ramfs + FAT32, file syscalls |
| **Total** | **34 chunks** | **~6,375** | |

---

## Deferred Items

Items intentionally postponed from their original phase:

- **Phase 1.4**: Framebuffer console (pixel rendering, bitmap font)
- ~~**Phase 1.9**: PS/2 keyboard driver~~ **DONE**
- **Phase 1.10**: HAL consolidation (`hal.h` unified interface)
- **Phase 3.4**: Sleeping mutexes, semaphores, condition variables (spinlock done)
- **Phase 3**: Sleep queues, thread-local storage, work queues
- **Phase 4.1**: ACPI table parsing
- **Phase 4**: Memory barrier HAL abstraction, VirtIO feature caching, DMA buffer pool
- ~~**Phase 5**: fork/exec/wait (5.5), user pointer validation (copy_from_user/copy_to_user)~~ **DONE**
- **Phase 6**: Dentry cache, mount table
