Build the arc_os kernel and create a bootable ISO image.

## Steps

1. Run `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64.cmake` if `build/` directory doesn't exist
2. Run `cmake --build build` to compile the kernel
3. If the build succeeds, create the bootable ISO (run `cmake --build build --target iso` or the ISO creation script in `tools/make-iso.sh`)
4. Report build status: success with binary size, or failure with error details

If the build fails, analyze the error output and suggest fixes. Common issues:
- Missing cross-compiler: suggest installing `x86_64-elf-gcc`
- Missing NASM: `sudo apt-get install nasm`
- Linker errors: check linker script and symbol references
