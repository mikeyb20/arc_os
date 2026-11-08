/* arc_os — AVX2 state survives preemption (M1-01, ADR-0003). Test ISOs only.
 *
 * Built with stock musl. Runs in QEMU (suite "fpu"). Two processes (parent
 * and a fork child) each load YMM0-15 with their own pattern and their own
 * MXCSR, spin with general registers only (no syscalls, so only the timer
 * interrupt takes the CPU away), then check every register. They run until
 * each has seen PREEMPTIONS_WANTED long gaps in its TSC readings, i.e. until
 * it has been switched out and back in that often while holding live AVX
 * state. Whoever gets there first keeps spinning until the other is done too,
 * so the slower one still has something to be preempted by: the parent tells
 * the child with SIGUSR1, the child tells the parent by exiting. Prints one
 * FAIL line per failed check; exit status = failures. */

#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define PREEMPTIONS_WANTED 20
#define MAX_ROUNDS         400000   /* give up: preemption never happened */
#define SPIN               2000     /* general-register loop per round */
#define GAP_FACTOR         50       /* a round this many times the fastest = switched out */

/* clang-format off */
struct round {
    uint8_t  in[16][32];    /*   0 */
    uint8_t  out[16][32];   /* 512 */
    uint32_t mxcsr_in;      /* 1024 */
    uint32_t mxcsr_out;     /* 1028 */
};
/* clang-format on */

/* Load YMM0-15 and MXCSR from r, spin, store them to r->out (fpu_test.S) */
void avx_round(struct round *r, uint64_t spin);

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static volatile sig_atomic_t parent_done;   /* child: SIGUSR1 seen */
static int child_status = -1;                /* parent: child's wait status once reaped */

static void on_usr1(int sig) {
    (void)sig;
    parent_done = 1;
}

/* The other process is done: the parent has reaped the child, the child has
 * had SIGUSR1 (delivered on syscall return, hence the getpid) */
static int other_done(int id, pid_t child) {
    if (id) {
        getpid();
        return parent_done;
    }
    int status;
    if (waitpid(child, &status, WNOHANG) != child) return 0;
    child_status = status;
    return 1;
}

/* Returns the number of failures for this process. Once it has its
 * preemptions it keeps running rounds until other_done(). */
static int worker(int id, pid_t child) {
    static struct round r;
    int failures = 0, gaps = 0;
    uint64_t fastest = UINT64_MAX;
    long rounds;
    r.mxcsr_in = id ? (0x1F80 | 0x8000 | 0x0040) : 0x1F80;   /* child: FZ and DAZ */
    for (rounds = 0; rounds < MAX_ROUNDS || gaps >= PREEMPTIONS_WANTED; rounds++) {
        if (gaps == PREEMPTIONS_WANTED && !id && !parent_done) {
            kill(child, SIGUSR1);
            parent_done = 1;   /* parent: "told" */
        }
        if (gaps >= PREEMPTIONS_WANTED && other_done(id, child)) break;
        for (int reg = 0; reg < 16; reg++)
            for (int i = 0; i < 32; i++) r.in[reg][i] = (uint8_t)(id * 131 + reg * 17 + i * 3 + rounds);
        uint64_t t0 = rdtsc();
        avx_round(&r, SPIN);
        uint64_t dt = rdtsc() - t0;
        if (memcmp(r.in, r.out, sizeof(r.in)) != 0 || r.mxcsr_out != r.mxcsr_in) {
            if (failures < 5)
                printf("FAIL fpu_avx: %s round %ld: registers changed under preemption\n", id ? "child" : "parent",
                       rounds);
            failures++;
        }
        if (dt < fastest)
            fastest = dt;
        else if (rounds > 100 && dt > fastest * GAP_FACTOR && gaps < PREEMPTIONS_WANTED)
            gaps++;
    }
    if (gaps < PREEMPTIONS_WANTED) {
        printf("FAIL fpu_avx: %s saw only %d preemptions in %ld rounds\n", id ? "child" : "parent", gaps, rounds);
        failures++;
    }
    printf("fpu_avx: %s %ld rounds, %d preemptions, %d failures\n", id ? "child" : "parent", rounds, gaps, failures);
    return failures;
}

int main(void) {
    signal(SIGUSR1, on_usr1);   /* before fork: the child inherits it */
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL fpu_avx: fork\n");
        return 1;
    }
    if (pid == 0) _exit(worker(1, 0));
    int failures = worker(0, pid);
    if (!parent_done) kill(pid, SIGUSR1);   /* gave up early: release the child */
    int status = child_status;
    if (status == -1 && waitpid(pid, &status, 0) != pid) status = -1;
    if (status == -1 || !WIFEXITED(status)) {
        printf("FAIL fpu_avx: child did not exit normally\n");
        failures++;
    } else {
        failures += WEXITSTATUS(status);
    }
    printf("fpu_avx: %d failures\n", failures);
    return failures;
}
