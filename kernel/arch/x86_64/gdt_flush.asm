; arc_os — GDT flush routine
; Loads the GDT, reloads segment registers via far return, loads TSS.
;
; void gdt_flush(const GDTPointer *gdtr, uint16_t code_sel,
;                uint16_t data_sel, uint16_t tss_sel);
;
; System V AMD64 ABI:
;   rdi = gdtr pointer
;   rsi = kernel code selector (0x08)
;   rdx = kernel data selector (0x10)
;   rcx = TSS selector (0x28)

section .text
bits 64
global gdt_flush

gdt_flush:
    ; Load GDT
    lgdt [rdi]

    ; Reload data segments with kernel data selector
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Load TSS
    mov ax, cx
    ltr ax

    ; Far return to reload CS with kernel code selector
    pop rax            ; Return address
    push rsi           ; New CS
    push rax           ; Return address
    retfq
