/* arc_os — Higher-half kernel linker script for x86_64
 *
 * Four page-aligned parts, each its own program header so the ELF says what
 * vmm_init maps (M1-19, W^X): Limine's requests (the bootloader writes their
 * answers before the kernel runs; the kernel only reads them: read-only, NX),
 * code (read + execute), read-only data (.rodata, the exception table, the
 * symbol table: read-only, NX) and data (.data, .bss: read-write, NX). */

ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS {
    requests PT_LOAD FLAGS(6);   /* RW: Limine fills in the answers */
    text     PT_LOAD FLAGS(5);   /* R X */
    rodata   PT_LOAD FLAGS(4);   /* R */
    data     PT_LOAD FLAGS(6);   /* RW */
}

SECTIONS {
    . = KERNEL_VMA;
    _kernel_start = .;

    .limine_requests ALIGN(4096) : {
        KEEP(*(.limine_requests))
    } :requests

    .text ALIGN(4096) : {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    } :text

    .rodata ALIGN(4096) : {
        _rodata_start = .;
        *(.rodata .rodata.*)
    } :rodata

    /* Exception table (M1-07): pairs of (instruction that may fault on user
     * memory, fixup address); uaccess.asm writes them, isr.c reads them */
    .ex_table ALIGN(8) : {
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
    } :rodata

    /* Function symbols for panic backtraces (tools/gen-ksyms.py). Placed after
     * all code so the second kernel link moves no function. */
    .ksyms ALIGN(16) : {
        KEEP(*(.ksyms))
    } :rodata

    .data ALIGN(4096) : {
        _data_start = .;
        *(.data .data.*)
    } :data

    .bss ALIGN(4096) : {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    } :data

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
