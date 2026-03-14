; context_switch(ThreadContext *old [rdi], ThreadContext *new [rsi])
;
; Saves callee-saved registers + RSP into old, loads from new, ret.
; RIP is implicit — the return address is on the stack.

global context_switch

; ThreadContext struct field offsets (must match thread.h)
%define CTX_R15  0
%define CTX_R14  8
%define CTX_R13  16
%define CTX_R12  24
%define CTX_RBX  32
%define CTX_RBP  40
%define CTX_RSP  48

section .text
context_switch:
    ; Save callee-saved registers into old ThreadContext
    mov [rdi + CTX_R15], r15
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_RSP], rsp

    ; Load callee-saved registers from new ThreadContext
    mov r15, [rsi + CTX_R15]
    mov r14, [rsi + CTX_R14]
    mov r13, [rsi + CTX_R13]
    mov r12, [rsi + CTX_R12]
    mov rbx, [rsi + CTX_RBX]
    mov rbp, [rsi + CTX_RBP]
    mov rsp, [rsi + CTX_RSP]

    ret                     ; Pops new thread's return address → resumes it
