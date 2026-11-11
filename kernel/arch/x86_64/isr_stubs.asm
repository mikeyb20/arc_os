; arc_os — ISR stub generator and common interrupt dispatcher
;
; Generates 256 ISR stubs. Each stub pushes a uniform stack frame
; (dummy error code for vectors that don't push one), then jumps
; to isr_common which saves all GP registers and calls the C dispatcher.
;
; GS (M1-03): isr_common runs SWAPGS on entry when the interrupted code was in
; user mode (CS RPL 3) and again before IRETQ back to it. NMI, #DF, #GP and #MC
; go to isr_paranoid instead: they can arrive between SWAPGS and SYSRET/IRETQ,
; where CS says kernel but GS base is still the user's, so they read
; MSR_GS_BASE (a kernel address has the sign bit set; user GS base is always a
; user address because user space cannot set it) and swap only if needed.

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

; Paranoid variants: GS decided by MSR_GS_BASE (see above)
%macro ISR_NOERRCODE_PARANOID 1
isr_stub_%1:
    push 0
    push %1
    jmp isr_paranoid
%endmacro

%macro ISR_ERRCODE_PARANOID 1
isr_stub_%1:
    push %1
    jmp isr_paranoid
%endmacro

%define MSR_GS_BASE 0xC0000101

; Save / restore every GPR (InterruptFrame order, isr.h)
%macro PUSH_GPRS 0
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
%endmacro

%macro POP_GPRS 0
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
%endmacro

; --- Generate 256 ISR stubs ---

; Vectors 0-7: no error code (2 = NMI: paranoid, IST)
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE_PARANOID 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7

; Vector 8: Double fault (error code; paranoid, IST)
ISR_ERRCODE_PARANOID 8

; Vector 9: no error code
ISR_NOERRCODE 9

; Vectors 10-14: error code (13 = #GP: paranoid, an IRET to user mode can raise it)
ISR_ERRCODE 10
ISR_ERRCODE 11
ISR_ERRCODE 12
ISR_ERRCODE_PARANOID 13
ISR_ERRCODE 14

; Vectors 15-16: no error code
ISR_NOERRCODE 15
ISR_NOERRCODE 16

; Vector 17: Alignment check (error code)
ISR_ERRCODE 17

; Vectors 18-20: no error code (18 = #MC: paranoid, IST)
ISR_NOERRCODE_PARANOID 18
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
    test qword [rsp + 24], 3    ; interrupted CS: from user mode?
    jz .entry_kernel
    swapgs
.entry_kernel:
    PUSH_GPRS

    ; Pass pointer to InterruptFrame (stack pointer) as first argument
    mov rdi, rsp

    ; Align stack to 16 bytes (ABI requirement)
    mov rbp, rsp
    and rsp, ~0xF

    call isr_dispatch

    ; Restore stack
    mov rsp, rbp
    POP_GPRS

    ; Remove vector and error code from stack
    add rsp, 16

    test qword [rsp + 8], 3     ; returning to user mode?
    jz .exit_kernel
    cli                         ; no interrupt between SWAPGS and IRETQ
    swapgs
.exit_kernel:
    iretq

; --- Paranoid handler (NMI, #DF, #GP, #MC) ---
; EBX remembers whether entry swapped GS (callee-saved across isr_dispatch).

isr_paranoid:
    PUSH_GPRS
    xor ebx, ebx
    mov ecx, MSR_GS_BASE
    rdmsr                       ; EDX = high half of GS base
    test edx, edx
    js .gs_ok                   ; kernel address: already this CPU's block
    swapgs
    mov ebx, 1
.gs_ok:
    mov rdi, rsp
    mov rbp, rsp
    and rsp, ~0xF
    call isr_dispatch
    mov rsp, rbp
    test ebx, ebx
    jz .no_swap
    cli                         ; no interrupt between SWAPGS and IRETQ
    swapgs
.no_swap:
    POP_GPRS
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
