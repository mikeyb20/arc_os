; arc_os — SYSCALL fast-path entry stub (per CPU, M1-03)
;
; On SYSCALL:
;   RCX = return RIP, R11 = return RFLAGS
;   RSP = user stack (not swapped by hardware), GS base = user's
;   RAX = syscall number
;   RDI, RSI, RDX, R10, R8, R9 = args 0-5
;   IF, DF, TF, AC and NT are clear (SFMASK)
;
; SWAPGS comes first, before any GS access; the user RSP waits in this CPU's
; PerCpu scratch slot until it is pushed. The kernel stack is this CPU's
; kernel_rsp (the current thread's stack top, set on every context switch).
;
; Linux ABI: every register except RAX (result), RCX and R11 is preserved,
; including the argument registers (musl's inline syscalls rely on it).

section .text
global syscall_entry
global syscall_iret_user
extern syscall_dispatch
extern syscall_exit_work

; PerCpu field offsets (percpu.h, checked by _Static_assert in percpu.c)
%define PERCPU_KERNEL_RSP 8
%define PERCPU_USER_RSP   16

; SyscallFrame offsets (proc/signal.h): ret_rcx at the lowest address
%define FRAME_RET_RCX 0
%define FRAME_RET_R11 8
%define FRAME_R9      16
%define FRAME_RIP     112
%define FRAME_RFLAGS  120
%define FRAME_RSP     128

; User selectors with RPL 3 (gdt.h)
%define USER_DATA_RPL3  0x1B
%define USER_CODE_RPL3  0x23

syscall_entry:
    swapgs                              ; GS -> this CPU's PerCpu
    mov [gs:PERCPU_USER_RSP], rsp       ; user RSP to per-CPU scratch
    mov rsp, [gs:PERCPU_KERNEL_RSP]     ; this thread's kernel stack

    ; Build the SyscallFrame (proc/signal.h): user context first, r9 last
    push qword [gs:PERCPU_USER_RSP]     ; user RSP
    push r11                            ; user RFLAGS
    push rcx                            ; user RIP
    push rbx                            ; callee-saved regs
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rdi                            ; argument registers, restored on return
    push rsi
    push rdx
    push r10
    push r8
    push r9
    push r11                            ; ret_r11: R11 and RCX on return (SYSRET
    push rcx                            ; ret_rcx:  gives RFLAGS and RIP back)

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

    ; --- Signal delivery and rt_sigreturn, then the kernel lock goes ---
    ; syscall_exit_work(frame, syscall_ret) may rewrite any frame field
    ; (handler entry: RIP, RSP, RDI/RSI/RDX; sigreturn: every register)
    mov rdi, rsp            ; SyscallFrame pointer
    mov rsi, rax            ; syscall return value
    call syscall_exit_work
    ; RAX = value for user RAX

    ; Nothing may interrupt between SWAPGS and SYSRET: an interrupt there would
    ; see kernel CS and keep the user's GS base
    cli

    ; SYSRET to a non-canonical RIP faults in ring 0 on Intel, with the user's
    ; RSP already loaded. rt_sigreturn can set any RIP, so check it here and
    ; leave through IRET instead (its fault, if any, comes on the kernel stack).
    ; IRET too when RCX/R11 must differ from RIP/RFLAGS (rt_sigreturn to an
    ; interrupted context, M1-07): SYSRET can't give them back.
    mov rcx, [rsp + FRAME_RIP]
    mov r11, rcx
    shl r11, 16
    sar r11, 16
    cmp r11, rcx
    jne .return_iret
    cmp rcx, [rsp + FRAME_RET_RCX]
    jne .return_iret
    mov r11, [rsp + FRAME_RFLAGS]
    cmp r11, [rsp + FRAME_RET_R11]
    jne .return_iret

    add rsp, 16             ; ret_rcx, ret_r11: equal to RIP, RFLAGS
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop rcx                 ; user RIP
    pop r11                 ; user RFLAGS
    pop rsp                 ; user RSP: from here on no memory access
    swapgs                  ; GS -> user's (NMI/#MC here decide by MSR_GS_BASE)
    o64 sysret              ; return to user mode (64-bit SYSRET)

.return_iret:
    ; Rare path: build the IRET frame, restore registers from the frame with
    ; RCX/R11 from ret_rcx/ret_r11
    push USER_DATA_RPL3                     ; SS
    push qword [rsp + 8 + FRAME_RSP]        ; RSP (address computed before the push)
    push qword [rsp + 16 + FRAME_RFLAGS]    ; RFLAGS
    push USER_CODE_RPL3                     ; CS
    push qword [rsp + 32 + FRAME_RIP]       ; RIP
    mov rcx, [rsp + 40 + FRAME_RET_RCX]
    mov r11, [rsp + 40 + FRAME_RET_R11]
    mov r9,  [rsp + 40 + FRAME_R9 + 0]
    mov r8,  [rsp + 40 + FRAME_R9 + 8]
    mov r10, [rsp + 40 + FRAME_R9 + 16]
    mov rdx, [rsp + 40 + FRAME_R9 + 24]
    mov rsi, [rsp + 40 + FRAME_R9 + 32]
    mov rdi, [rsp + 40 + FRAME_R9 + 40]
    mov r15, [rsp + 40 + FRAME_R9 + 48]
    mov r14, [rsp + 40 + FRAME_R9 + 56]
    mov r13, [rsp + 40 + FRAME_R9 + 64]
    mov r12, [rsp + 40 + FRAME_R9 + 72]
    mov rbp, [rsp + 40 + FRAME_R9 + 80]
    mov rbx, [rsp + 40 + FRAME_R9 + 88]
    swapgs
syscall_iret_user:                      ; a #GP here is the user's (M1-07)
    iretq
