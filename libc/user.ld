/* arc_os — Shared linker script for user-space programs */

ENTRY(_start)

SECTIONS {
    /* ELF and program headers sit in the first loaded page, so the kernel can
     * point AT_PHDR at them (System V process start, ADR-0002) */
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : {
        *(.data .data.*)
    }

    .bss ALIGN(4096) : {
        *(.bss .bss.*)
        *(COMMON)
    }

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
