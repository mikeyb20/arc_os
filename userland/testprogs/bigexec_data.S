/* arc_os — the payload of bigexec (M1-12): 8 MiB of read-only data and
 * 1 MiB of writable data, all initialized, so the program file is over
 * 9 MiB. Each 64-byte block of page k is the word (k << 8) | j (j the block)
 * then 15 copies of ((k * 64 + j) * 0x9E3779B1) mod 2^32, xor-ed with
 * 0xFFFFFFFF in the writable part; bigexec.c checks every word. */

.macro page k, mask
    .set j, 0
    .rept 64
        .long ((\k << 8) | j) ^ \mask
        .fill 15, 4, ((((\k * 64 + j) * 0x9E3779B1) & 0xFFFFFFFF) ^ \mask)
        .set j, j + 1
    .endr
.endm

    .section .rodata
    .balign 4096
    .globl big_ro
big_ro:
    .set k, 0
    .rept 2048
        page k, 0
        .set k, k + 1
    .endr

    .data
    .balign 4096
    .globl big_rw
big_rw:
    .set k, 0
    .rept 256
        page k, 0xFFFFFFFF
        .set k, k + 1
    .endr

    .section .note.GNU-stack, "", @progbits
