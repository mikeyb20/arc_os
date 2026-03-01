; context_switch(ThreadContext *old [rdi], ThreadContext *new [rsi])
;
; Saves callee-saved registers + RSP into old, loads from new, ret.
; RIP is implicit — the return address is on the stack.

global context_switch

section .text
context_switch:
    ; Save callee-saved registers into old ThreadContext
    mov [rdi + 0],  r15
    mov [rdi + 8],  r14
    mov [rdi + 16], r13
    mov [rdi + 24], r12
    mov [rdi + 32], rbx
    mov [rdi + 40], rbp
    mov [rdi + 48], rsp

    ; Load callee-saved registers from new ThreadContext
    mov r15, [rsi + 0]
    mov r14, [rsi + 8]
    mov r13, [rsi + 16]
    mov r12, [rsi + 24]
    mov rbx, [rsi + 32]
    mov rbp, [rsi + 40]
    mov rsp, [rsi + 48]

    ret                     ; Pops new thread's return address → resumes it
