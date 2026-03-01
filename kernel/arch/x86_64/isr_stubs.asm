; arc_os — ISR stub generator and common interrupt dispatcher
;
; Generates 256 ISR stubs. Each stub pushes a uniform stack frame
; (dummy error code for vectors that don't push one), then jumps
; to isr_common which saves all GP registers and calls the C dispatcher.

section .text
bits 64

extern isr_dispatch

; --- Macros for ISR stubs ---

; ISR with no error code: push dummy 0, then vector number
%macro ISR_NOERRCODE 1
isr_stub_%1:
    push 0              ; dummy error code
    push %1             ; vector number
    jmp isr_common
%endmacro

; ISR with error code already pushed by CPU: push vector number
%macro ISR_ERRCODE 1
isr_stub_%1:
    push %1             ; vector number (error code already on stack)
    jmp isr_common
%endmacro

; --- Generate 256 ISR stubs ---

; Vectors 0-7: no error code
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7

; Vector 8: Double fault (error code)
ISR_ERRCODE 8

; Vector 9: no error code
ISR_NOERRCODE 9

; Vectors 10-14: error code
ISR_ERRCODE 10
ISR_ERRCODE 11
ISR_ERRCODE 12
ISR_ERRCODE 13
ISR_ERRCODE 14

; Vectors 15-16: no error code
ISR_NOERRCODE 15
ISR_NOERRCODE 16

; Vector 17: Alignment check (error code)
ISR_ERRCODE 17

; Vectors 18-20: no error code
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20

; Vector 21: Control protection (error code)
ISR_ERRCODE 21

; Vectors 22-28: no error code
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28

; Vector 29: VMM communication (error code)
ISR_ERRCODE 29

; Vector 30: Security exception (error code)
ISR_ERRCODE 30

; Vector 31: no error code
ISR_NOERRCODE 31

; Vectors 32-255: IRQs and software interrupts (no error code)
%assign i 32
%rep 224
ISR_NOERRCODE i
%assign i i+1
%endrep

; --- Common ISR handler ---
; Stack at entry: SS, RSP, RFLAGS, CS, RIP, error_code, vector

isr_common:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass pointer to InterruptFrame (stack pointer) as first argument
    mov rdi, rsp

    ; Align stack to 16 bytes (ABI requirement)
    mov rbp, rsp
    and rsp, ~0xF

    call isr_dispatch

    ; Restore stack
    mov rsp, rbp

    ; Restore all general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove vector and error code from stack
    add rsp, 16

    iretq

; --- ISR stub table ---
; Array of 256 function pointers, exported for IDT setup in C

section .data
global isr_stub_table

isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
