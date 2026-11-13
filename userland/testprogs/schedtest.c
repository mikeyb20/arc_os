/* arc_os — SMP scheduler tests (M1-05). Test ISOs only, suite "sched".
 *
 *   schedtest spread N     fork N CPU-bound children; every CPU must do a fair
 *                          share of the work (busy time per CPU from
 *                          /proc/sched_debug, at least 1/(2 x CPUs) of the total)
 *   schedtest forkexec N   fork, exec this program ("exit") and wait, N times;
 *                          init's leak budgets (pages_leaked, heap_leaked_bytes,
 *                          both 0) show the free page count is back to its baseline
 *   schedtest pingpong N   a parent and a child bounce a counter through two
 *                          pipes N times; their wakeups cross CPUs (the child
 *                          starts on an idle CPU and goes back to it), and at
 *                          least two CPUs must each do N/2 context switches
 *   schedtest exit         exit 0 at once (the forkexec child)
 *
 * Prints one FAIL line per failed check; exit status = number of failures. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define MAX_CPUS        64
#define SPIN_ITERATIONS 4000000UL   /* per spread child: tens of ms under TCG */

static int failures;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL schedtest:%d: %s\n", __LINE__, #cond); \
            failures++;                                         \
        }                                                       \
    } while (0)

typedef struct {
    int ncpu;
    unsigned long switches[MAX_CPUS];
    unsigned long busy_ns[MAX_CPUS];
    unsigned long migrations[MAX_CPUS];
} SchedSnap;

static const char *parse_u64(const char *p, unsigned long *out) {
    while (*p == ' ') p++;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned long)(*p++ - '0');
    *out = v;
    return p;
}

/* Read /proc/sched_debug: "cpu core nr_running nr_switches nr_migrations idle_ns busy_ns" */
static int sched_snap(SchedSnap *s) {
    static char text[4096];
    memset(s, 0, sizeof(*s));
    int fd = open("/proc/sched_debug", O_RDONLY);
    if (fd < 0) return -1;
    int len = 0, n;
    while (len < (int)sizeof(text) - 1 && (n = (int)read(fd, text + len, sizeof(text) - 1 - len)) > 0) len += n;
    close(fd);
    text[len] = '\0';
    const char *p = strchr(text, '\n');   /* skip the header */
    while (p != NULL && *++p != '\0') {
        unsigned long cpu, running, sw, mig, idle, busy;
        p = parse_u64(p, &cpu);
        while (*p == ' ') p++;
        while (*p != ' ' && *p != '\0') p++;   /* core type */
        p = parse_u64(p, &running);
        p = parse_u64(p, &sw);
        p = parse_u64(p, &mig);
        p = parse_u64(p, &idle);
        p = parse_u64(p, &busy);
        if (cpu < MAX_CPUS) {
            s->switches[cpu] = sw;
            s->busy_ns[cpu] = busy;
            s->migrations[cpu] = mig;
            if ((int)cpu + 1 > s->ncpu) s->ncpu = (int)cpu + 1;
        }
        p = strchr(p, '\n');
    }
    return s->ncpu > 0 ? 0 : -1;
}

static void spin(void) {
    volatile unsigned long x = 0;
    for (unsigned long i = 0; i < SPIN_ITERATIONS; i++) x += i;
}

static void test_spread(int n) {
    SchedSnap before, after;
    CHECK(sched_snap(&before) == 0);
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL schedtest: fork %d failed\n", i);
            failures++;
            break;
        }
        if (pid == 0) {
            spin();
            _exit(0);
        }
    }
    int status, reaped = 0;
    while (wait(&status) > 0) {
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        reaped++;
    }
    CHECK(reaped == n);
    CHECK(sched_snap(&after) == 0);
    CHECK(after.ncpu == before.ncpu);

    unsigned long total = 0, migrations = 0;
    for (int c = 0; c < after.ncpu; c++) {
        total += after.busy_ns[c] - before.busy_ns[c];
        migrations += after.migrations[c] - before.migrations[c];
    }
    for (int c = 0; c < after.ncpu; c++) {
        unsigned long busy = after.busy_ns[c] - before.busy_ns[c];
        printf("schedtest: spread cpu %d busy %lu ms (%lu%%)\n", c, busy / 1000000, total ? busy * 100 / total : 0);
        /* A fair share is 1/ncpu; accept half of it */
        CHECK(busy * 2 * (unsigned long)after.ncpu >= total);
    }
    printf("schedtest: spread %d processes over %d CPUs, %lu pulled by idle CPUs\n", reaped, after.ncpu, migrations);
}

static void test_forkexec(int n, const char *self) {
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL schedtest: fork %d of %d failed\n", i, n);
            failures++;
            return;
        }
        if (pid == 0) {
            char *argv[] = {(char *)self, "exit", NULL};
            execv(self, argv);
            _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("FAIL schedtest: fork/exec/wait %d: status 0x%x\n", i, status);
            failures++;
            return;
        }
    }
    printf("schedtest: forkexec %d rounds\n", n);
}

static void test_pingpong(int n) {
    int to_child[2], to_parent[2];
    CHECK(pipe(to_child) == 0 && pipe(to_parent) == 0);
    SchedSnap before, after;
    CHECK(sched_snap(&before) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(to_child[1]);
        close(to_parent[0]);
        for (;;) {
            int v;
            if (read(to_child[0], &v, sizeof(v)) != sizeof(v)) _exit(0);
            v++;
            if (write(to_parent[1], &v, sizeof(v)) != sizeof(v)) _exit(1);
        }
    }
    close(to_child[0]);
    close(to_parent[1]);
    int v = 0;
    for (int i = 0; i < n; i++) {
        int sent = v;
        if (write(to_child[1], &v, sizeof(v)) != sizeof(v) || read(to_parent[0], &v, sizeof(v)) != sizeof(v)) {
            printf("FAIL schedtest: pingpong broke at round %d\n", i);
            failures++;
            break;
        }
        if (v != sent + 1) {
            printf("FAIL schedtest: pingpong round %d: sent %d, got %d\n", i, sent, v);
            failures++;
            break;
        }
    }
    close(to_child[1]);   /* the child reads EOF and exits */
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(v == n);
    CHECK(sched_snap(&after) == 0);
    int busy_cpus = 0;
    for (int c = 0; c < after.ncpu; c++) {
        unsigned long sw = after.switches[c] - before.switches[c];
        printf("schedtest: pingpong cpu %d switches %lu\n", c, sw);
        if (sw >= (unsigned long)n / 2) busy_cpus++;
    }
    CHECK(busy_cpus >= 2);
    printf("schedtest: pingpong %d round trips, %d CPUs switching\n", v, busy_cpus);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "exit") == 0) return 0;
    int n = argc >= 3 ? atoi(argv[2]) : 0;
    if (argc < 3 || n <= 0) {
        printf("usage: schedtest spread|forkexec|pingpong N\n");
        return 1;
    }
    if (strcmp(argv[1], "spread") == 0)
        test_spread(n);
    else if (strcmp(argv[1], "forkexec") == 0)
        test_forkexec(n, argv[0]);
    else if (strcmp(argv[1], "pingpong") == 0)
        test_pingpong(n);
    else {
        printf("schedtest: unknown test %s\n", argv[1]);
        return 1;
    }
    return failures;
}
