; arc_os — NASM entry stub for x86_64
; Limine provides: long mode, paging, valid stack

section .text
bits 64
global _start
extern kmain
extern _bss_start, _bss_end

_start:
    ; Zero BSS section
    lea rdi, [rel _bss_start]
    lea rcx, [rel _bss_end]
    sub rcx, rdi
    xor al, al
    rep stosb

    ; Enter C kernel
    call kmain

.halt:
    cli
    hlt
    jmp .halt
