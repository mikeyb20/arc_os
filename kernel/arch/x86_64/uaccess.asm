; arc_os — user-memory copy with a page-fault fixup (M1-07)
;
; arch_copy_user(dst, src, n) copies n bytes with REP MOVSB and returns the
; bytes it did NOT copy: 0 on success. One side is user memory. If touching it
; faults and the fault handler can't map the page (unmapped, no permission,
; out of memory), the handler finds the faulting RIP in the exception table
; and resumes at the fixup, which returns RCX: what REP MOVSB had left.
;
; Exception table entries (section .ex_table, linker.ld): two quads each,
; the address that may fault and where to continue. isr.c looks them up.

section .text
bits 64

global arch_copy_user

arch_copy_user:
    mov rcx, rdx
.copy:
    rep movsb
    xor eax, eax
    ret
.fault:
    mov rax, rcx
    ret

section .ex_table alloc noexec nowrite progbits align=8
    dq arch_copy_user.copy, arch_copy_user.fault
