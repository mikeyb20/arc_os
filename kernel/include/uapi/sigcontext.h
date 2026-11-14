#ifndef ARCHOS_UAPI_SIGCONTEXT_H
#define ARCHOS_UAPI_SIGCONTEXT_H

#include <stdint.h>

/* Linux x86_64 signal frame layout (ADR-0001): what a handler finds on its
 * stack and what rt_sigreturn reads back. Same names, sizes and offsets as
 * Linux's asm/sigcontext.h, asm/ucontext.h and asm/siginfo.h (checked by the
 * host test test_abi_layout), plus struct rt_sigframe, which Linux keeps
 * kernel-internal (arch/x86/include/asm/sigframe.h). */

/* fpstate->sw_reserved: tells rt_sigreturn the XSAVE image follows the legacy
 * 512 bytes; FP_XSTATE_MAGIC2 sits right after the XSAVE image. */
#define FP_XSTATE_MAGIC1      0x46505853U
#define FP_XSTATE_MAGIC2      0x46505845U
#define FP_XSTATE_MAGIC2_SIZE 4

struct _fpx_sw_bytes {
    uint32_t magic1;          /* FP_XSTATE_MAGIC1 */
    uint32_t extended_size;   /* xstate_size + FP_XSTATE_MAGIC2_SIZE */
    uint64_t xfeatures;       /* XCR0 bits present in the image */
    uint32_t xstate_size;     /* bytes of the XSAVE image (legacy + header + ext) */
    uint32_t padding[7];
};

/* The FXSAVE-format legacy area at the start of every XSAVE image */
struct _fpstate {
    uint16_t cwd;
    uint16_t swd;
    uint16_t twd;
    uint16_t fop;
    uint64_t rip;
    uint64_t rdp;
    uint32_t mxcsr;
    uint32_t mxcsr_mask;
    uint32_t st_space[32];    /* 8 x87 registers, 16 bytes each */
    uint32_t xmm_space[64];   /* XMM0-15, 16 bytes each */
    uint32_t reserved2[12];
    union {
        uint32_t reserved3[12];
        struct _fpx_sw_bytes sw_reserved;   /* bytes 464..511 */
    };
};

/* The XSAVE header follows the legacy area */
#define XSAVE_LEGACY_SIZE 512
#define XSAVE_HEADER_SIZE 64

struct sigcontext {
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t eflags;
    uint16_t cs;
    uint16_t gs;
    uint16_t fs;
    uint16_t ss;
    uint64_t err;
    uint64_t trapno;
    uint64_t oldmask;
    uint64_t cr2;
    uint64_t fpstate;         /* user address of struct _fpstate, 0 = none */
    uint64_t reserved1[8];
};

/* sigaltstack's stack_t */
#define SS_ONSTACK 1
#define SS_DISABLE 2

typedef struct sigaltstack {
    uint64_t ss_sp;
    int32_t ss_flags;
    uint64_t ss_size;
} stack_t;

/* uc_flags */
#define UC_FP_XSTATE         0x1
#define UC_SIGCONTEXT_SS     0x2
#define UC_STRICT_RESTORE_SS 0x4

struct ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    stack_t uc_stack;
    struct sigcontext uc_mcontext;
    uint64_t uc_sigmask;   /* kernel sigset_t: bit (n - 1) = signal n */
};

/* si_code values */
#define SI_USER     0
#define SI_KERNEL   0x80
#define SEGV_MAPERR 1   /* SIGSEGV: no mapping at the address */
#define SEGV_ACCERR 2   /* SIGSEGV: mapped, but not for this access */
#define ILL_ILLOPN  2   /* SIGILL: illegal operand (#UD) */
#define FPE_INTDIV  1   /* SIGFPE: integer divide by zero (#DE) */
#define FPE_FLTDIV  3   /* SIGFPE: floating-point divide by zero (#MF, #XM) */
#define FPE_FLTOVF  4   /* SIGFPE: floating-point overflow */
#define FPE_FLTUND  5   /* SIGFPE: floating-point underflow (or denormal) */
#define FPE_FLTRES  6   /* SIGFPE: inexact result */
#define FPE_FLTINV  7   /* SIGFPE: invalid operation */
#define BUS_ADRALN  1   /* SIGBUS: alignment check (#AC) */
#define TRAP_BRKPT  1   /* SIGTRAP: breakpoint (#BP) */
#define TRAP_TRACE  2   /* SIGTRAP: single step (#DB) */

/* siginfo_t: 128 bytes; arc_os fills the kill() fields (si_pid, si_uid) or,
 * for a fault, the address (si_addr, M1-07); they share the same bytes */
typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t __pad0;
    union {
        struct {
            uint32_t si_pid;
            uint32_t si_uid;
        };
        uint64_t si_addr;
    };
    uint8_t __rest[104];
} siginfo_t;

/* What signal delivery puts on the user stack (Linux's struct rt_sigframe).
 * The handler starts with RSP = &frame, so pretcode is its return address (the
 * sa_restorer) and RSP + 8 is 16-byte aligned. The XSAVE image (64-byte
 * aligned, uc_mcontext.fpstate points to it) lies above the frame. */
struct rt_sigframe {
    uint64_t pretcode;
    struct ucontext uc;
    siginfo_t info;
};

_Static_assert(sizeof(struct _fpx_sw_bytes) == 48, "_fpx_sw_bytes is 48 bytes");
_Static_assert(sizeof(struct _fpstate) == 512, "_fpstate is the 512-byte legacy area");
_Static_assert(__builtin_offsetof(struct _fpstate, sw_reserved) == 464, "sw_reserved at 464");
_Static_assert(sizeof(struct sigcontext) == 256, "sigcontext is 256 bytes");
_Static_assert(sizeof(stack_t) == 24, "stack_t is 24 bytes");
_Static_assert(sizeof(struct ucontext) == 304, "ucontext is 304 bytes");
_Static_assert(sizeof(siginfo_t) == 128, "siginfo_t is 128 bytes");
_Static_assert(__builtin_offsetof(struct rt_sigframe, uc) == 8, "uc follows pretcode");
_Static_assert(__builtin_offsetof(struct rt_sigframe, info) == 312, "info follows uc");

#endif /* ARCHOS_UAPI_SIGCONTEXT_H */
