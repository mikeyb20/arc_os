# debug-kernel

Systematic kernel debugging workflow for arc_os.

## Quick Start

```bash
# Terminal 1: Boot QEMU in debug mode
./tools/run.sh -s -S

# Terminal 2: Attach GDB
gdb build/kernel.elf -ex "target remote :1234" -ex "continue"
```

## Debugging Workflow

### 1. Check Serial Output First
Serial output is the primary debug channel. Before reaching for GDB:
- Look at the last log message — which subsystem was active?
- Look for `[PANIC]` messages with register dumps
- Look for unexpected silence (kernel hung or crashed without handler)

### 2. Connect GDB to QEMU
```bash
gdb build/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kmain
(gdb) continue
```

### 3. Useful GDB Commands

#### Registers and Execution
```
info registers          # All general-purpose registers
print $rsp              # Stack pointer
print $cr3              # Page table base
stepi                   # Single instruction step
nexti                   # Step over (don't enter calls)
```

#### Memory Inspection
```
x/16x $rsp              # 16 hex words at stack pointer
x/10i $rip              # 10 instructions at instruction pointer
x/s <addr>              # String at address
print *(BootInfo*)0x... # Print struct at address
```

#### Page Tables (via QEMU Monitor)
```
(gdb) monitor info mem       # Show mapped memory regions
(gdb) monitor info tlb       # Show TLB entries
(gdb) monitor info registers # Full register dump including control regs
```

#### Breakpoints
```
break kmain                  # Break at function
break kernel/mm/pmm.c:42    # Break at file:line
watch *(uint64_t*)0x1000     # Break when memory changes
catch signal SIGSEGV         # Catch page faults
```

## Common Failure Patterns

### Triple Fault (QEMU Resets)
- **Cause**: Double fault handler itself faulted, or no IDT loaded
- **Debug**: Enable QEMU logging: `-d int,cpu_reset -D qemu.log`
- **Common fixes**: Check GDT/IDT setup, verify stack is mapped, check IST entries

### Page Fault in Kernel
- **Cause**: Accessing unmapped memory, null pointer dereference, stack overflow
- **Debug**: Page fault handler should print CR2 (faulting address) and error code
- **Common fixes**: Check page table mappings, verify pointers, check stack guard pages

### General Protection Fault (GPF)
- **Cause**: Segment violation, privilege level mismatch, bad IDT entry
- **Debug**: Check error code (selector index), verify GDT/IDT entries
- **Common fixes**: Check segment selectors, DPL fields, gate types

### Hang (No Output, No Crash)
- **Cause**: Infinite loop, deadlock, interrupts disabled and spinning
- **Debug**: Ctrl+C in GDB to break, check `$rip` location
- **Common fixes**: Check loop conditions, verify interrupt enable, check lock state

### Corrupted Output / Garbage
- **Cause**: Wrong serial port configuration, buffer overflow, stack corruption
- **Debug**: Check serial port I/O address (0x3F8), verify baud rate setup
- **Common fixes**: Verify outb() calls, check buffer bounds, inspect stack canaries

## QEMU Debug Flags Reference

```bash
# Interrupt logging
-d int

# CPU reset logging (triple faults)
-d cpu_reset

# All exceptions
-d int,cpu_reset

# Log to file
-D qemu.log

# GDB server on port 1234, pause at start
-s -S

# GDB server on custom port
-gdb tcp::9000

# No GUI (headless, serial only)
-nographic

# Extra memory for debugging
-m 512M
```

## Kernel Panic Handler Checklist

A good panic handler should output (over serial):
1. Panic message / reason
2. RIP (instruction pointer) where panic occurred
3. RSP (stack pointer)
4. CR2 (if page fault — faulting address)
5. Error code (if applicable)
6. Stack backtrace (walk RBP chain)
7. Halt the CPU (`cli; hlt`)
