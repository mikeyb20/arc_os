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
extern sig_maybe_deliver
extern sig_pending_arg

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

    ; Save user context to globals for fork()
    mov [rel syscall_saved_user_rip], rcx
    mov [rel syscall_saved_user_rflags], r11
    push qword [rel syscall_user_rsp]
    pop qword [rel syscall_saved_user_rsp]
    mov [rel syscall_saved_user_rbp], rbp
    mov [rel syscall_saved_user_rbx], rbx
    mov [rel syscall_saved_user_r12], r12
    mov [rel syscall_saved_user_r13], r13
    mov [rel syscall_saved_user_r14], r14
    mov [rel syscall_saved_user_r15], r15

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

    ; --- Signal delivery check ---
    ; sig_maybe_deliver(frame_ptr=rsp, syscall_ret=rax)
    mov rdi, rsp            ; SyscallFrame pointer
    mov rsi, rax            ; syscall return value
    mov qword [rel sig_pending_arg], 0
    call sig_maybe_deliver
    ; RAX = (possibly modified) return value
    ; sig_pending_arg = signo for handler RDI, or 0

    ; Restore callee-saved registers and user context
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop rcx                 ; user RIP
    pop r11                 ; user RFLAGS

    ; Load signal handler arg into RDI (0 if no signal)
    mov rdi, [rel sig_pending_arg]

    pop rsp                 ; user RSP

    o64 sysret              ; return to user mode (64-bit SYSRET)

section .bss
syscall_user_rsp: resq 1   ; scratch for user RSP (single-CPU only)

global syscall_saved_user_rip
global syscall_saved_user_rsp
global syscall_saved_user_rflags
global syscall_saved_user_rbp
global syscall_saved_user_rbx
global syscall_saved_user_r12
global syscall_saved_user_r13
global syscall_saved_user_r14
global syscall_saved_user_r15
syscall_saved_user_rip:    resq 1
syscall_saved_user_rsp:    resq 1
syscall_saved_user_rflags: resq 1
syscall_saved_user_rbp:    resq 1
syscall_saved_user_rbx:    resq 1
syscall_saved_user_r12:    resq 1
syscall_saved_user_r13:    resq 1
syscall_saved_user_r14:    resq 1
syscall_saved_user_r15:    resq 1
