; arc_os — SYSCALL fast-path entry stub
;
; On SYSCALL:
;   RCX = return RIP, R11 = return RFLAGS
;   RSP = user stack (not swapped by hardware)
;   RAX = syscall number
;   RDI, RSI, RDX, R10, R8, R9 = args 0-5

section .text
global syscall_entry
extern syscall_dispatch
extern syscall_kernel_rsp

syscall_entry:
    ; Save user RSP to scratch space (can't trust user stack)
    mov [rel syscall_user_rsp], rsp
    ; Load kernel stack (set by scheduler on context switch)
    mov rsp, [rel syscall_kernel_rsp]

    ; Build frame: push user context (callee-saved + return info)
    push qword [rel syscall_user_rsp]   ; user RSP
    push r11                             ; user RFLAGS
    push rcx                             ; user RIP
    push rbx                             ; callee-saved regs
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Shuffle registers for C calling convention:
    ;   syscall_dispatch(num, a0, a1, a2, a3, a4, a5)
    ;   RDI=num  RSI=a0  RDX=a1  RCX=a2  R8=a3  R9=a4  [rsp]=a5
    mov r12, rdi            ; save user arg0 (RDI)
    mov r13, rsi            ; save user arg1 (RSI)
    push r9                 ; 7th C arg: user arg5 (on stack)
    mov r9, r8              ; C r9 = user arg4
    mov r8, r10             ; C r8 = user arg3 (R10, since RCX is clobbered)
    mov rcx, rdx            ; C rcx = user arg2
    mov rdx, r13            ; C rdx = user arg1
    mov rsi, r12            ; C rsi = user arg0
    mov rdi, rax            ; C rdi = syscall number

    call syscall_dispatch
    add rsp, 8              ; pop 7th arg

    ; Return value in RAX (already set by C function)

    ; Restore callee-saved registers and user context
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop rcx                 ; user RIP
    pop r11                 ; user RFLAGS
    pop rsp                 ; user RSP

    o64 sysret              ; return to user mode (64-bit SYSRET)

section .bss
syscall_user_rsp: resq 1   ; scratch for user RSP (single-CPU only)
