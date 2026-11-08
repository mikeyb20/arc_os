/* arc_os — assembly for the FPU tests (M1-01), linked into sig_fpu
 * (-DSIG_FPU) and fpu_avx (-DFPU_AVX). Test ISOs only. Each routine holds
 * YMM0-15, MXCSR and FCW in registers across the event under test, which C
 * code can't promise. */

        .text

/* Load YMM0-15 from 16 * 32 bytes at off(%rdi), or store them there */
.macro YMM_LOAD off
        .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        vmovdqu \off+32*\n(%rdi), %ymm\n
        .endr
.endm
.macro YMM_STORE off
        .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        vmovdqu %ymm\n, \off+32*\n(%rdi)
        .endr
.endm

#ifdef SIG_FPU
/* void sig_regs_test(struct regs_block *b)  (layout in sig_fpu.c)
 * Load YMM0-15, MXCSR, FCW and rdx r8 r9 r10 rbx rbp r12-r15 from b, send
 * b->signo to b->pid with a raw kill (the signal arrives on syscall return),
 * then store all of them, and RSI/RDI, back into b. */
        .globl sig_regs_test
        .type sig_regs_test, @function
sig_regs_test:
        push %rbx
        push %rbp
        push %r12
        push %r13
        push %r14
        push %r15
        mov %rdi, blk_ptr(%rip)
        ldmxcsr 1216(%rdi)
        fldcw 1220(%rdi)
        YMM_LOAD 0
        mov 512(%rdi), %rdx
        mov 520(%rdi), %r8
        mov 528(%rdi), %r9
        mov 536(%rdi), %r10
        mov 544(%rdi), %rbx
        mov 552(%rdi), %rbp
        mov 560(%rdi), %r12
        mov 568(%rdi), %r13
        mov 576(%rdi), %r14
        mov 584(%rdi), %r15
        mov %rsp, 1232(%rdi)
        mov 592(%rdi), %rsi
        mov 600(%rdi), %rdi
        mov $62, %eax                   /* kill(pid, signo) */
        syscall
        push %rdi
        mov blk_ptr(%rip), %rdi
        YMM_STORE 608
        mov %rdx, 1120(%rdi)
        mov %r8, 1128(%rdi)
        mov %r9, 1136(%rdi)
        mov %r10, 1144(%rdi)
        mov %rbx, 1152(%rdi)
        mov %rbp, 1160(%rdi)
        mov %r12, 1168(%rdi)
        mov %r13, 1176(%rdi)
        mov %r14, 1184(%rdi)
        mov %r15, 1192(%rdi)
        mov %rsi, 1200(%rdi)
        pop %rax
        mov %rax, 1208(%rdi)
        stmxcsr 1224(%rdi)
        fnstcw 1228(%rdi)
        vzeroupper
        pop %r15
        pop %r14
        pop %r13
        pop %r12
        pop %rbp
        pop %rbx
        ret

/* Signal handler entry for sig_fpu.c: record RSP, MXCSR and FCW before any C
 * code runs, then continue in regs_handler with RDI/RSI/RDX untouched */
        .globl regs_handler_entry
        .type regs_handler_entry, @function
regs_handler_entry:
        mov %rsp, entry_rsp(%rip)
        stmxcsr entry_mxcsr(%rip)
        fnstcw entry_fcw(%rip)
        jmp regs_handler

/* void clobber_fpu(void): YMM0-15 all ones, MXCSR round-toward-zero, FCW
 * 0x7F. The kernel must give the interrupted code its own values back. */
        .globl clobber_fpu
        .type clobber_fpu, @function
clobber_fpu:
        .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        vpcmpeqb %ymm\n, %ymm\n, %ymm\n
        .endr
        sub $8, %rsp
        movl $0x7F80, (%rsp)
        ldmxcsr (%rsp)
        movw $0x007F, 4(%rsp)
        fldcw 4(%rsp)
        add $8, %rsp
        ret

        .data
blk_ptr:
        .quad 0
        .text
#endif /* SIG_FPU */

#ifdef FPU_AVX
/* void avx_round(struct round *r, uint64_t spin)  (layout in fpu_avx.c)
 * Load YMM0-15 and MXCSR from r, spin `spin` times with general registers
 * only, store them to r->out */
        .globl avx_round
        .type avx_round, @function
avx_round:
        ldmxcsr 1024(%rdi)
        YMM_LOAD 0
1:      dec %rsi
        jnz 1b
        YMM_STORE 512
        stmxcsr 1028(%rdi)
        vzeroupper
        ret
#endif /* FPU_AVX */

        .section .note.GNU-stack, "", @progbits
