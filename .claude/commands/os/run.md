Build the arc_os kernel and launch it in QEMU with standard flags.

## Steps

1. Build the kernel (run `/build` workflow)
2. Launch QEMU with standard flags:
   ```bash
   qemu-system-x86_64 \
     -cdrom build/arc_os.iso \
     -serial stdio \
     -m 256M \
     -no-reboot \
     -no-shutdown \
     -boot d \
     -drive file=build/test_disk.img,format=raw,if=none,id=disk0 \
     -device virtio-blk-pci,drive=disk0
   ```
   Or use the launch script: `./tools/run.sh`
3. Monitor serial output for kernel messages
4. Report what happened: successful boot messages, panics, or crashes

If QEMU is not installed, suggest: `sudo apt-get install qemu-system-x86`
