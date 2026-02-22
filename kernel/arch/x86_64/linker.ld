/* arc_os — Higher-half kernel linker script for x86_64 */

ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS {
    . = KERNEL_VMA;
    _kernel_start = .;

    .limine_requests ALIGN(4096) : {
        KEEP(*(.limine_requests))
    }

    .text ALIGN(4096) : {
        *(.text .text.*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : {
        *(.data .data.*)
    }

    .bss ALIGN(4096) : {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
