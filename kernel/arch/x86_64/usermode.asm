; arc_os — Jump to user-mode via IRETQ
;
; void jump_to_usermode(uint64_t entry_rip, uint64_t user_rsp)
;   RDI = entry_rip, RSI = user_rsp
;
; Builds an IRETQ frame on the stack:
;   [RSP+32] SS     = 0x1B (GDT_USER_DATA | RPL=3)
;   [RSP+24] RSP    = user_rsp
;   [RSP+16] RFLAGS = 0x202 (IF set)
;   [RSP+8]  CS     = 0x23 (GDT_USER_CODE | RPL=3)
;   [RSP+0]  RIP    = entry_rip

section .text
global jump_to_usermode

jump_to_usermode:
    ; Build IRETQ frame
    push 0x1B               ; SS = GDT_USER_DATA | RPL=3
    push rsi                ; RSP = user_rsp
    push 0x202              ; RFLAGS = IF set
    push 0x23               ; CS = GDT_USER_CODE | RPL=3
    push rdi                ; RIP = entry_rip

    ; Zero all general-purpose registers for clean user entry
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

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
;
; Returns to user mode with RAX=0 (fork child return value).
; Restores callee-saved registers so C code resumes correctly.
global fork_return_to_user

fork_return_to_user:
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

    ; Zero caller-saved registers
    xor rax, rax            ; RAX = 0 (fork returns 0 to child)
    xor rdx, rdx
    xor rsi, rsi
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor rdi, rdi            ; Zero RDI last (was struct pointer)

    o64 sysret
