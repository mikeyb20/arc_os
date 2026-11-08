/* arc_os — signals with Linux rt_sigframes and XSAVE state (M1-01). Test ISOs only.
 *
 * Built with stock musl (musl-gcc -static), so musl's own sigaction and
 * __restore_rt are what the kernel sees. Runs in QEMU (suite "fpu"):
 *   - every general register, YMM0-15, MXCSR and the x87 control word survive
 *     a signal whose handler clobbers all of them;
 *   - the handler starts with RSP + 8 16-byte aligned and a clean FPU state,
 *     gets siginfo (signo, SI_USER, sender pid/uid) and a ucontext whose
 *     registers and XSAVE image (Linux magic numbers) match the interrupted
 *     code;
 *   - sa_mask and the signal itself are blocked while the handler runs and
 *     delivered after it returns; SA_NODEFER nests; SA_RESETHAND resets;
 *   - a fork child keeps the handler; AT_MINSIGSTKSZ covers the frame;
 *   - /proc/cpuinfo lists CPU 0.
 * Prints one FAIL line per failed check; exit status = number of failures. */

#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/auxv.h>
#include <sys/wait.h>

#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif
#define FP_XSTATE_MAGIC1 0x46505853U
#define FP_XSTATE_MAGIC2 0x46505845U
#define XSAVE_AVX_OFFSET 576   /* YMM upper halves in the standard XSAVE format */

static int failures;

#define CHECK(cond)                                           \
    do {                                                      \
        if (!(cond)) {                                        \
            printf("FAIL sig_fpu:%d: %s\n", __LINE__, #cond); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* --- 1. Registers across a signal ---
 * sig_regs_test(&blk) loads YMM0-15, MXCSR, FCW and ten general registers
 * from blk, sends itself blk.signo with a raw kill (signals are delivered on
 * syscall return), then stores the same registers back. The offsets below
 * are fixed by fpu_test.S. */
/* clang-format off */
struct regs_block {
    uint8_t  ymm_in[16][32];    /*    0 */
    uint64_t gpr_in[10];        /*  512: rdx r8 r9 r10 rbx rbp r12 r13 r14 r15 */
    uint64_t signo;             /*  592 */
    uint64_t pid;               /*  600 */
    uint8_t  ymm_out[16][32];   /*  608 */
    uint64_t gpr_out[10];       /* 1120 */
    uint64_t rsi_out;           /* 1200 */
    uint64_t rdi_out;           /* 1208 */
    uint32_t mxcsr_in;          /* 1216 */
    uint16_t fcw_in;            /* 1220 */
    uint16_t pad0;
    uint32_t mxcsr_out;         /* 1224 */
    uint16_t fcw_out;           /* 1228 */
    uint16_t pad1;
    uint64_t rsp_at_syscall;    /* 1232 */
};
/* clang-format on */
_Static_assert(__builtin_offsetof(struct regs_block, gpr_in) == 512, "layout");
_Static_assert(__builtin_offsetof(struct regs_block, ymm_out) == 608, "layout");
_Static_assert(__builtin_offsetof(struct regs_block, gpr_out) == 1120, "layout");
_Static_assert(__builtin_offsetof(struct regs_block, mxcsr_in) == 1216, "layout");
_Static_assert(__builtin_offsetof(struct regs_block, rsp_at_syscall) == 1232, "layout");

void sig_regs_test(struct regs_block *b);   /* fpu_test.S */

/* What regs_handler_entry records before any C code runs */
uint64_t entry_rsp;
uint32_t entry_mxcsr;
uint16_t entry_fcw;
void regs_handler_entry(int, siginfo_t *, void *);   /* fpu_test.S */

static struct regs_block blk;
static volatile int handler_runs;
static volatile int handler_ok;

/* Fill every vector register with ones and change MXCSR and FCW (fpu_test.S) */
void clobber_fpu(void);

void regs_handler(int sig, siginfo_t *si, void *ucv);
void regs_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    int ok = 1;
    handler_runs++;
    ok &= sig == SIGUSR1 && si->si_signo == SIGUSR1 && si->si_code == SI_USER;
    ok &= si->si_pid == (pid_t)blk.pid && si->si_uid == getuid();
    /* The interrupted registers, as the frame recorded them */
    greg_t *g = uc->uc_mcontext.gregs;
    ok &= (uint64_t)g[REG_RDX] == blk.gpr_in[0] && (uint64_t)g[REG_R8] == blk.gpr_in[1];
    ok &= (uint64_t)g[REG_R15] == blk.gpr_in[9] && (uint64_t)g[REG_RBP] == blk.gpr_in[5];
    ok &= (uint64_t)g[REG_RDI] == blk.pid && (uint64_t)g[REG_RSI] == blk.signo;
    ok &= (uint64_t)g[REG_RSP] == blk.rsp_at_syscall;
    /* The XSAVE image: Linux's magic numbers, XMM0 and the upper half of YMM0 */
    const uint8_t *fp = (const uint8_t *)uc->uc_mcontext.fpregs;
    ok &= fp != NULL && ((uintptr_t)fp & 63) == 0;
    if (fp != NULL) {
        uint32_t magic1, xstate_size, magic2;
        memcpy(&magic1, fp + 464, 4);
        memcpy(&xstate_size, fp + 464 + 16, 4);
        memcpy(&magic2, fp + xstate_size, 4);
        ok &= magic1 == FP_XSTATE_MAGIC1 && xstate_size >= 832 && magic2 == FP_XSTATE_MAGIC2;
        ok &= memcmp(fp + 160, blk.ymm_in[0], 16) == 0;                       /* XMM0 */
        ok &= memcmp(fp + XSAVE_AVX_OFFSET, blk.ymm_in[0] + 16, 16) == 0;     /* YMM0 high */
        uint32_t saved_mxcsr;
        memcpy(&saved_mxcsr, fp + 24, 4);
        ok &= saved_mxcsr == blk.mxcsr_in;
    }
    handler_ok = ok;
    clobber_fpu();
}

static void test_registers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = regs_handler_entry;
    sa.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    for (int r = 0; r < 16; r++)
        for (int i = 0; i < 32; i++) blk.ymm_in[r][i] = (uint8_t)(r * 32 + i + 1);
    for (int i = 0; i < 10; i++) blk.gpr_in[i] = 0x1111111111111111ULL * (uint64_t)(i + 1);
    blk.signo = SIGUSR1;
    blk.pid = (uint64_t)getpid();
    blk.mxcsr_in = 0x1F80 | 0x8000 | 0x0040;   /* default plus FZ and DAZ */
    blk.fcw_in = 0x027F;                       /* 53-bit precision */
    sig_regs_test(&blk);

    CHECK(handler_runs == 1);
    CHECK(handler_ok);
    CHECK((entry_rsp + 8) % 16 == 0);          /* as after a call */
    CHECK(entry_mxcsr == 0x1F80);              /* handler starts clean */
    CHECK(entry_fcw == 0x037F);
    CHECK(memcmp(blk.ymm_out, blk.ymm_in, sizeof(blk.ymm_in)) == 0);
    CHECK(memcmp(blk.gpr_out, blk.gpr_in, sizeof(blk.gpr_in)) == 0);
    CHECK(blk.rsi_out == blk.signo && blk.rdi_out == blk.pid);   /* argument registers too */
    CHECK(blk.mxcsr_out == blk.mxcsr_in);
    CHECK(blk.fcw_out == blk.fcw_in);
    unsigned long minsig = getauxval(AT_MINSIGSTKSZ);
    CHECK(minsig >= 1024);
    CHECK(blk.rsp_at_syscall - entry_rsp <= 128 + minsig);    /* frame fits the bound */
    CHECK(getauxval(AT_HWCAP2) == 0);

    uint32_t def = 0x1F80;
    uint16_t fcw = 0x037F;
    __asm__ volatile("ldmxcsr %0\n fldcw %1" : : "m"(def), "m"(fcw));
}

/* --- 2. Masks --- */
static volatile int depth, max_depth, usr1_runs, usr2_runs, usr2_before_usr1_done;

static void mask_usr1(int sig) {
    (void)sig;
    depth++;
    if (depth > max_depth) max_depth = depth;
    if (++usr1_runs == 1) {
        kill(getpid(), SIGUSR2);   /* in sa_mask: waits */
        kill(getpid(), SIGUSR1);   /* itself: waits */
        if (usr2_runs != 0) usr2_before_usr1_done = 1;
    }
    depth--;
}

static void mask_usr2(int sig) {
    (void)sig;
    usr2_runs++;
}

static void nodefer_usr1(int sig) {
    (void)sig;
    depth++;
    if (depth > max_depth) max_depth = depth;
    if (++usr1_runs == 1) kill(getpid(), SIGUSR1);   /* SA_NODEFER: nests at once */
    depth--;
}

static void test_masks(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mask_usr2;
    CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);
    sa.sa_handler = mask_usr1;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    depth = max_depth = usr1_runs = usr2_runs = usr2_before_usr1_done = 0;
    kill(getpid(), SIGUSR1);
    CHECK(max_depth == 1);
    CHECK(usr1_runs == 2);
    CHECK(usr2_runs == 1);
    CHECK(!usr2_before_usr1_done);

    sa.sa_handler = nodefer_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    depth = max_depth = usr1_runs = 0;
    kill(getpid(), SIGUSR1);
    CHECK(max_depth == 2);
    CHECK(usr1_runs == 2);

    sa.sa_handler = mask_usr2;
    sa.sa_flags = SA_RESETHAND;
    CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);
    usr2_runs = 0;
    kill(getpid(), SIGUSR2);
    CHECK(usr2_runs == 1);
    struct sigaction old;
    CHECK(sigaction(SIGUSR2, NULL, &old) == 0 && old.sa_handler == SIG_DFL);
}

/* --- 3. fork keeps the handler --- */
static volatile int child_got;
static void child_handler(int sig) {
    child_got = sig;
}

static void test_fork_inherits(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_handler;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        kill(getpid(), SIGUSR1);
        _exit(child_got == SIGUSR1 ? 0 : 1);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* --- 4. /proc/cpuinfo --- */
static void test_cpuinfo(void) {
    char buf[512];
    int fd = open("/proc/cpuinfo", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    CHECK(n > 0);
    if (n <= 0) return;
    buf[n] = '\0';
    CHECK(strstr(buf, "processor\t: 0\n") != NULL);
    CHECK(strstr(buf, "core type\t: ") != NULL);
}

int main(void) {
    test_registers();
    test_masks();
    test_fork_inherits();
    test_cpuinfo();
    printf("sig_fpu: %d failures\n", failures);
    return failures;
}
