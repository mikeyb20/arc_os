Build the arc_os kernel, launch QEMU in debug mode, and print GDB connection instructions.

## Steps

1. Build the kernel (run `/build` workflow)
2. Launch QEMU in debug mode (paused, waiting for GDB):
   ```bash
   qemu-system-x86_64 \
     -cdrom build/arc_os.iso \
     -serial stdio \
     -m 256M \
     -no-reboot \
     -no-shutdown \
     -s -S \
     -d int,cpu_reset \
     -D qemu.log
   ```
   Or use: `./tools/run.sh -s -S`
3. Print GDB connection instructions:
   ```
   In another terminal, run:
     gdb build/kernel.elf \
       -ex "target remote :1234" \
       -ex "break kmain" \
       -ex "continue"
   ```

## Debug Tips
- QEMU is paused at startup (`-S`), nothing happens until GDB connects and issues `continue`
- Set breakpoints before continuing: `break kmain`, `break page_fault_handler`
- Use `monitor info mem` in GDB to inspect page table mappings
- Check `qemu.log` for interrupt and CPU reset events
