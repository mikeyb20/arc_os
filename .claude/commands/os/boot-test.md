Build the kernel, boot QEMU headless with a timeout, capture serial output, and check for panics and expected milestone messages.

## Steps

1. **Build**: Run the `/build` workflow. If the build fails, report the failure and stop.

2. **Check prerequisites**:
   - Verify `build/arc_os.iso` exists
   - Verify `qemu-system-x86_64` is installed (`which qemu-system-x86_64`)
   - If either is missing, report and stop

3. **Boot QEMU headless with timeout**:
   First, create a test disk if it doesn't exist:
   ```bash
   TEST_DISK="build/test_disk.img"
   if [ ! -f "$TEST_DISK" ]; then
     dd if=/dev/zero of="$TEST_DISK" bs=1M count=32 2>/dev/null
     printf '\x55\xAA' | dd of="$TEST_DISK" bs=1 seek=510 conv=notrunc 2>/dev/null
   fi
   ```
   Then boot:
   ```bash
   timeout 10 qemu-system-x86_64 \
     -cdrom build/arc_os.iso \
     -serial stdio \
     -display none \
     -m 256M \
     -no-reboot \
     -no-shutdown \
     -boot d \
     -drive file=build/test_disk.img,format=raw,if=none,id=disk0 \
     -device virtio-blk-pci,drive=disk0 2>&1
   ```
   Capture all output (stdout + stderr) into a variable.

4. **Check exit status**:
   - Exit 0: QEMU exited normally (kernel halted or shutdown)
   - Exit 124: Timeout — kernel was still running (may be OK if it's waiting for input)
   - Other: QEMU error

5. **Scan for failure patterns** in serial output:
   - `PANIC` — kernel panic
   - `TRIPLE FAULT` — CPU triple fault
   - `DOUBLE FAULT` — double fault exception
   - `ASSERTION FAILED` — assertion failure
   - `STACK SMASH` — stack canary violation
   - `*** ` — generic error marker
   If any found, mark boot status as FAIL.

6. **Check milestone messages** — for each source file that exists, expect a corresponding serial message:

   | Source file exists | Expected serial output (substring) |
   |---|---|
   | `kernel/boot/*.c` | `Booted` or `Boot` |
   | `kernel/arch/x86_64/gdt.c` | `GDT` |
   | `kernel/arch/x86_64/idt.c` | `IDT` |
   | `kernel/arch/x86_64/isr_stubs.asm` | `ISR` or `Interrupt` |
   | `kernel/drivers/serial.c` | `Serial` or `serial` |
   | `kernel/arch/x86_64/pic.c` | `PIC` |
   | `kernel/arch/x86_64/pit.c` | `PIT` or `Timer` |
   | `kernel/drivers/ps2_keyboard.c` | `Keyboard` or `PS2` |
   | `kernel/mm/pmm.c` | `PMM` or `Physical` |
   | `kernel/mm/vmm.c` | `VMM` or `Virtual` or `Paging` |
   | `kernel/mm/kmalloc.c` | `kmalloc` or `Heap` |
   | `kernel/proc/sched.c` | `Scheduler` or `Sched` |
   | `kernel/proc/thread.c` | `Threading` or `Thread` |
   | `kernel/proc/process.c` | `Process` or `PROC` |
   | `kernel/drivers/pci.c` | `PCI` |
   | `kernel/drivers/virtio_blk.c` | `VIRTIO-BLK` |
   | `kernel/fs/vfs.c` | `VFS` |

   For each: PASS if substring found, FAIL if source exists but message missing, SKIP if source doesn't exist.

7. **Report**:
   ```
   ## Boot Test Results

   **Build**: PASS/FAIL
   **Boot**: PASS/FAIL/TIMEOUT (with exit code)
   **Panic check**: CLEAN / FOUND: <pattern>

   ### Milestones
   - [PASS] GDT loaded
   - [PASS] IDT loaded
   - [FAIL] PMM initialized — expected "PMM" in output
   - [SKIP] VFS — source not present

   ### Serial Output (last 30 lines)
   <last 30 lines of captured output>

   ### Verdict
   PASS — all present milestones detected, no panics
   (or)
   FAIL — <reason>
   ```

## Common Failures

- **QEMU not installed**: `sudo apt-get install qemu-system-x86`
- **No ISO file**: Build failed or ISO target not configured — run `/build`
- **Immediate exit (exit code 1)**: Likely triple fault — no valid kernel entry, check bootloader config
- **Timeout with no output**: Kernel hung before serial init, or serial port not configured
- **Timeout with partial output**: Kernel reached a spin loop — usually OK for early phases
