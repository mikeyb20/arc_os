; arc_os — MSR access that survives a missing register (M1-21)
;
; rdmsr_safe(msr, *out) and wrmsr_safe(msr, value) run RDMSR / WRMSR with an
; exception-table entry each. A register the CPU doesn't have (or a value it
; refuses) raises #GP; the #GP handler (isr.c) finds the faulting RIP in the
; table and resumes at the fixup, which returns -EIO (5) as Linux's
; rdmsr_safe does. On success they return 0. Probing optional registers (HWP,
; RAPL) this way works the same on real CPUs, under KVM and under TCG.
;
; Exception table entries (section .ex_table, linker.ld): two quads each,
; the address that may fault and where to continue.

section .text
bits 64

global rdmsr_safe
global wrmsr_safe

; int rdmsr_safe(uint32_t msr [edi], uint64_t *out [rsi])
rdmsr_safe:
    mov ecx, edi
.insn:
    rdmsr
    shl rdx, 32
    or rax, rdx             ; RDMSR zero-extends EAX into RAX
    mov [rsi], rax
    xor eax, eax
    ret
.fault:
    mov qword [rsi], 0
    mov eax, -5             ; -EIO
    ret

; int wrmsr_safe(uint32_t msr [edi], uint64_t value [rsi])
wrmsr_safe:
    mov ecx, edi
    mov eax, esi
    mov rdx, rsi
    shr rdx, 32
.insn:
    wrmsr
    xor eax, eax
    ret
.fault:
    mov eax, -5             ; -EIO
    ret

section .ex_table alloc noexec nowrite progbits align=8
    dq rdmsr_safe.insn, rdmsr_safe.fault
    dq wrmsr_safe.insn, wrmsr_safe.fault
