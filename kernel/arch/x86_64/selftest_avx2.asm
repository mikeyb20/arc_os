; arc_os — AVX2 self-test routine (M1-04, ADR-0003 exception)
;
; The only kernel code that touches vector registers (Michael's OK,
; 2026-11-11). It runs under arc.selftest=1 only, before any user thread
; exists, so no user FPU state can be clobbered; it zeroes every YMM register
; before returning. tools/no-simd-check.py allowlists this one function.
;
; uint32_t selftest_avx2_sum(const uint32_t *buf, uint64_t count)
;   rdi = buf (32-byte aligned), rsi = count (a multiple of 8)
;   returns the sum of the count words, mod 2^32

bits 64
section .text
global selftest_avx2_sum

selftest_avx2_sum:
    vpxor ymm0, ymm0, ymm0
    test rsi, rsi
    jz .fold
.loop:
    vpaddd ymm0, ymm0, [rdi]
    add rdi, 32
    sub rsi, 8
    jnz .loop
.fold:
    vextracti128 xmm1, ymm0, 1
    vpaddd xmm0, xmm0, xmm1
    vpshufd xmm1, xmm0, 0x4E
    vpaddd xmm0, xmm0, xmm1
    vpshufd xmm1, xmm0, 0xB1
    vpaddd xmm0, xmm0, xmm1
    vmovd eax, xmm0
    vzeroall
    ret
