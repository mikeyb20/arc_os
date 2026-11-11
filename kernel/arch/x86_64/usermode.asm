; arc_os — Jump to user-mode via IRETQ
;
; void jump_to_usermode(uint64_t entry_rip, uint64_t user_rsp)
;   RDI = entry_rip, RSI = user_rsp (points at argc on the System V stack)
;
; Builds an IRETQ frame on the stack:
;   [RSP+32] SS     = 0x1B (GDT_USER_DATA | RPL=3)
;   [RSP+24] RSP    = user_rsp
;   [RSP+16] RFLAGS = 0x202 (IF set)
;   [RSP+8]  CS     = 0x23 (GDT_USER_CODE | RPL=3)
;   [RSP+0]  RIP    = entry_rip

section .text
global jump_to_usermode

; GDT segment selectors with Ring 3 privilege
%define USER_DATA_RPL3  0x1B    ; GDT_USER_DATA | RPL=3
%define USER_CODE_RPL3  0x23    ; GDT_USER_CODE | RPL=3
%define RFLAGS_IF       0x202   ; RFLAGS with Interrupt Flag set

jump_to_usermode:
    cli                     ; no interrupt between SWAPGS and IRETQ (M1-03)
    ; Build IRETQ frame
    push USER_DATA_RPL3     ; SS
    push rsi                ; RSP = user_rsp
    push RFLAGS_IF          ; RFLAGS
    push USER_CODE_RPL3     ; CS
    push rdi                ; RIP = entry_rip

    ; Zero every GPR for a clean System V entry (RDX = 0: no atexit hook)
    xor rdi, rdi
    xor rsi, rsi
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    swapgs                  ; GS -> user's; this CPU's block waits in KERNEL_GS_BASE
    iretq

; void fork_return_to_user(const ForkContext *ctx)
;   RDI = pointer to ForkContext struct:
;     offset  0: user_rip
;     offset  8: user_rsp
;     offset 16: user_rflags
;     offset 24: user_rbp
;     offset 32: user_rbx
;     offset 40: user_r12
;     offset 48: user_r13
;     offset 56: user_r14
;     offset 64: user_r15
;     offset 72..112: user_rdi, rsi, rdx, r10, r8, r9
;
; Returns to user mode with RAX=0 (fork child return value) and every other
; register as the parent had it at the fork syscall (RCX, R11 aside).
global fork_return_to_user

fork_return_to_user:
    cli                     ; no interrupt between SWAPGS and SYSRET (M1-03)
    ; Load callee-saved registers from ForkContext (must finish before zeroing RDI)
    mov rbp, [rdi + 24]
    mov rbx, [rdi + 32]
    mov r12, [rdi + 40]
    mov r13, [rdi + 48]
    mov r14, [rdi + 56]
    mov r15, [rdi + 64]

    ; Load SYSRET context
    mov rcx, [rdi]          ; RCX = user RIP (SYSRET uses RCX)
    mov r11, [rdi + 16]     ; R11 = user RFLAGS (SYSRET uses R11)
    mov rsp, [rdi + 8]      ; RSP = user stack

    ; Argument registers as the parent had them (Linux preserves them)
    xor rax, rax            ; RAX = 0 (fork returns 0 to child)
    mov rsi, [rdi + 80]
    mov rdx, [rdi + 88]
    mov r10, [rdi + 96]
    mov r8,  [rdi + 104]
    mov r9,  [rdi + 112]
    mov rdi, [rdi + 72]     ; RDI last (was struct pointer)

    swapgs                  ; GS -> user's (RIP came from SYSCALL: canonical)
    o64 sysret
