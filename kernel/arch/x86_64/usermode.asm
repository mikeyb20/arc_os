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
