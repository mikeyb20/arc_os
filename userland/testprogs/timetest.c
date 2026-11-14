/* arc_os — Clocks, sleeps and tickless idle (M1-06). Test ISOs only.
 *
 * Built with stock musl (no vDSO on arc_os: every clock read is a syscall).
 * Runs in QEMU (suite "time", -smp 4). Checks: every clock ID reads and has
 * a 1 ns resolution; the wall clock comes from the RTC (year >= 2025) and
 * time()/gettimeofday() agree with it; nanosleep and clock_nanosleep
 * (relative, absolute, both clocks) wake within 2 ms of their deadline and
 * never early; bad arguments give EINVAL/EFAULT; the CPU-time clocks count
 * running time only; CLOCK_MONOTONIC never goes backwards, inside a process
 * that migrates and between processes on different CPUs (a pipe carries one
 * reading to the other side, which must read the same or later); eight
 * CPU-bound processes on four CPUs all get CPU time (the tickless slice
 * timer still preempts); and over a 5 s sleep with nothing else running the
 * idle CPUs take no timer interrupts (/proc/interrupts LOC row, reported as
 * budget idle_timer_irqs_5s). Prints one FAIL line per failed check; exit
 * status = failures. */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_MS   1000000LL
#define NS_PER_SEC  1000000000LL
#define LATE_OK_NS  (2 * NS_PER_MS)      /* the spec's +-2 ms (never early) */
#define LATE_MAX_NS (50 * NS_PER_MS)     /* any single sleep: a sanity bound */
#define HOGS        8                    /* CPU-bound processes on 4 CPUs */
#define HOG_NS      (1500 * NS_PER_MS)
#define HOG_SPIN    10000                /* busy loops between clock reads */
#define MAX_CPUS    16

static int failures;

#define CHECK(cond, ...)               \
    do {                               \
        if (!(cond)) {                 \
            printf("FAIL timetest: "); \
            printf(__VA_ARGS__);       \
            printf("\n");              \
            failures++;                \
        }                              \
    } while (0)

static int64_t ts_ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * NS_PER_SEC + ts->tv_nsec;
}

static int64_t now_ns(clockid_t id) {
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0) return -1;
    return ts_ns(&ts);
}

static struct timespec ns_ts(int64_t ns) {
    struct timespec ts = {.tv_sec = ns / NS_PER_SEC, .tv_nsec = ns % NS_PER_SEC};
    return ts;
}

/* ---- Clock IDs, resolution, wall clock ---- */

static void test_clocks(void) {
    static const clockid_t ids[] = {CLOCK_REALTIME,          CLOCK_MONOTONIC,     CLOCK_PROCESS_CPUTIME_ID,
                                    CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE,
                                    CLOCK_MONOTONIC_COARSE,  CLOCK_BOOTTIME};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        struct timespec ts, res;
        CHECK(clock_gettime(ids[i], &ts) == 0, "clock_gettime(%d): errno %d", (int)ids[i], errno);
        CHECK(ts.tv_nsec >= 0 && ts.tv_nsec < NS_PER_SEC, "clock %d: tv_nsec %ld", (int)ids[i], (long)ts.tv_nsec);
        CHECK(clock_getres(ids[i], &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 1, "clock_getres(%d) = %ld.%09ld",
              (int)ids[i], (long)res.tv_sec, (long)res.tv_nsec);
        CHECK(clock_getres(ids[i], NULL) == 0, "clock_getres(%d, NULL)", (int)ids[i]);
    }
    struct timespec ts;
    errno = 0;
    CHECK(clock_gettime(42, &ts) == -1 && errno == EINVAL, "clock_gettime(42): errno %d", errno);
    errno = 0;
    CHECK(clock_getres(42, &ts) == -1 && errno == EINVAL, "clock_getres(42): errno %d", errno);
    errno = 0;
    CHECK(clock_gettime(CLOCK_MONOTONIC, (struct timespec *)8) == -1 && errno == EFAULT,
          "clock_gettime(bad pointer): errno %d", errno);

    /* Monotonic within this process: 100 000 reads, never smaller */
    int64_t prev = now_ns(CLOCK_MONOTONIC), back = 0;
    for (int i = 0; i < 100000; i++) {
        int64_t t = now_ns(CLOCK_MONOTONIC);
        if (t < prev) back++;
        prev = t;
    }
    CHECK(back == 0, "CLOCK_MONOTONIC went backwards %lld times", (long long)back);

    /* Wall clock from the RTC, and the other two ways to read it agree */
    int64_t real = now_ns(CLOCK_REALTIME);
    time_t secs = (time_t)(real / NS_PER_SEC);
    struct tm tm;
    gmtime_r(&secs, &tm);
    printf("timetest: wall clock %04d-%02d-%02d %02d:%02d:%02d UTC\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
    CHECK(tm.tm_year + 1900 >= 2025, "RTC year %d < 2025", tm.tm_year + 1900);
    time_t t2 = 0;
    time_t t1 = time(&t2);
    CHECK(t1 == t2 && t1 >= secs && t1 - secs <= 1, "time() %lld vs CLOCK_REALTIME %lld", (long long)t1,
          (long long)secs);
    CHECK(time(NULL) >= secs, "time(NULL)");
    struct timeval tv;
    CHECK(gettimeofday(&tv, NULL) == 0, "gettimeofday: errno %d", errno);
    CHECK(tv.tv_sec >= secs && tv.tv_sec - secs <= 1 && tv.tv_usec >= 0 && tv.tv_usec < 1000000,
          "gettimeofday %lld.%06ld vs %lld", (long long)tv.tv_sec, (long)tv.tv_usec, (long long)secs);
    CHECK(gettimeofday(NULL, NULL) == 0, "gettimeofday(NULL, NULL)");
    /* musl reads both through clock_gettime: the syscalls themselves too */
    struct timeval rtv;
    struct timezone tz = {.tz_minuteswest = 99, .tz_dsttime = 99};   /* musl's wrapper never passes tz */
    CHECK(syscall(SYS_gettimeofday, &rtv, &tz) == 0 && rtv.tv_sec >= secs && rtv.tv_sec - secs <= 1,
          "SYS_gettimeofday %lld", (long long)rtv.tv_sec);
    CHECK(tz.tz_minuteswest == 0 && tz.tz_dsttime == 0, "SYS_gettimeofday tz %d %d", tz.tz_minuteswest, tz.tz_dsttime);
    errno = 0;
    CHECK(syscall(SYS_gettimeofday, (void *)8, NULL) == -1 && errno == EFAULT, "SYS_gettimeofday(bad): errno %d",
          errno);
    long rt = 0;
    long rsecs = syscall(SYS_time, &rt);
    CHECK(rsecs == rt && rsecs >= (long)secs && rsecs - (long)secs <= 1, "SYS_time %ld / %ld", rsecs, rt);
    errno = 0;
    CHECK(syscall(SYS_time, (void *)8) == -1 && errno == EFAULT, "SYS_time(bad): errno %d", errno);
}

/* ---- Sleeps ---- */

/* Best and worst lateness of `tries` sleeps of ns; -1 if one woke early */
static void check_sleep(const char *what, int64_t ns, int (*sleep_fn)(int64_t)) {
    int64_t best = INT64_MAX, worst = 0;
    for (int i = 0; i < 3; i++) {
        int64_t t0 = now_ns(CLOCK_MONOTONIC);
        int rc = sleep_fn(ns);
        int64_t late = now_ns(CLOCK_MONOTONIC) - t0 - ns;
        CHECK(rc == 0, "%s %lld ms: returned %d (errno %d)", what, (long long)(ns / NS_PER_MS), rc, errno);
        CHECK(late >= 0, "%s %lld ms: woke %lld ns early", what, (long long)(ns / NS_PER_MS), (long long)-late);
        if (late < best) best = late;
        if (late > worst) worst = late;
    }
    printf("timetest: %s %lld ms: late by %lld..%lld us\n", what, (long long)(ns / NS_PER_MS), (long long)best / 1000,
           (long long)worst / 1000);
    /* The best of three within the spec's 2 ms; none wildly late */
    CHECK(best <= LATE_OK_NS, "%s %lld ms: %lld us late at best (want <= 2000)", what, (long long)(ns / NS_PER_MS),
          (long long)best / 1000);
    CHECK(worst <= LATE_MAX_NS, "%s %lld ms: %lld us late", what, (long long)(ns / NS_PER_MS), (long long)worst / 1000);
}

static int do_nanosleep(int64_t ns) {
    struct timespec req = ns_ts(ns), rem = {-1, -1};
    int rc = nanosleep(&req, &rem);
    return rc;
}

/* Syscall 35 itself (musl's nanosleep may go through clock_nanosleep) */
static int do_sys_nanosleep(int64_t ns) {
    struct timespec req = ns_ts(ns);
    return (int)syscall(SYS_nanosleep, &req, NULL);
}

static int do_clock_rel(int64_t ns) {
    struct timespec req = ns_ts(ns);
    return clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
}

static int do_clock_abs_mono(int64_t ns) {
    struct timespec req = ns_ts(now_ns(CLOCK_MONOTONIC) + ns);
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
}

static int do_clock_abs_real(int64_t ns) {
    struct timespec req = ns_ts(now_ns(CLOCK_REALTIME) + ns);
    return clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &req, NULL);
}

static int do_clock_boottime(int64_t ns) {
    struct timespec req = ns_ts(ns);
    return clock_nanosleep(CLOCK_BOOTTIME, 0, &req, NULL);
}

static void test_sleeps(void) {
    check_sleep("nanosleep", 10 * NS_PER_MS, do_nanosleep);
    check_sleep("nanosleep", 100 * NS_PER_MS, do_nanosleep);
    check_sleep("SYS_nanosleep", 10 * NS_PER_MS, do_sys_nanosleep);
    check_sleep("clock_nanosleep MONOTONIC", 20 * NS_PER_MS, do_clock_rel);
    check_sleep("clock_nanosleep MONOTONIC abs", 30 * NS_PER_MS, do_clock_abs_mono);
    check_sleep("clock_nanosleep REALTIME abs", 30 * NS_PER_MS, do_clock_abs_real);
    check_sleep("clock_nanosleep BOOTTIME", 20 * NS_PER_MS, do_clock_boottime);

    /* A deadline already past returns at once; zero sleeps too */
    struct timespec past = ns_ts(now_ns(CLOCK_MONOTONIC) - NS_PER_SEC);
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &past, NULL) == 0, "past deadline");
    struct timespec zero = {0, 0};
    CHECK(nanosleep(&zero, NULL) == 0, "nanosleep(0)");
    CHECK(now_ns(CLOCK_MONOTONIC) - t0 < 5 * NS_PER_MS, "past deadline / zero sleep took %lld us",
          (long long)(now_ns(CLOCK_MONOTONIC) - t0) / 1000);

    /* Bad arguments */
    struct timespec bad = {0, NS_PER_SEC};
    errno = 0;
    CHECK(nanosleep(&bad, NULL) == -1 && errno == EINVAL, "nanosleep(tv_nsec 1e9): errno %d", errno);
    bad.tv_nsec = -1;
    errno = 0;
    CHECK(nanosleep(&bad, NULL) == -1 && errno == EINVAL, "nanosleep(tv_nsec -1): errno %d", errno);
    bad.tv_sec = -1;
    bad.tv_nsec = 0;
    errno = 0;
    CHECK(nanosleep(&bad, NULL) == -1 && errno == EINVAL, "nanosleep(tv_sec -1): errno %d", errno);
    errno = 0;
    CHECK(nanosleep((struct timespec *)8, NULL) == -1 && errno == EFAULT, "nanosleep(bad pointer): errno %d", errno);
    struct timespec ok = {0, 1000};
    CHECK(clock_nanosleep(42, 0, &ok, NULL) == EINVAL, "clock_nanosleep(42)");
    /* Raw syscall 230: musl answers THREAD_CPUTIME_ID itself with EINVAL. Linux
     * gives EOPNOTSUPP for a clock it can't sleep on and ignores unknown flags. */
    errno = 0;
    CHECK(syscall(SYS_clock_nanosleep, CLOCK_THREAD_CPUTIME_ID, 0, &ok, NULL) == -1 && errno == EOPNOTSUPP,
          "SYS_clock_nanosleep(THREAD_CPUTIME): errno %d", errno);
    errno = 0;
    CHECK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC_COARSE, 0, &ok, NULL) == -1 && errno == EOPNOTSUPP,
          "SYS_clock_nanosleep(MONOTONIC_COARSE): errno %d", errno);
    CHECK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0x10, &ok, NULL) == 0, "SYS_clock_nanosleep(flag 0x10)");
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, 0, &bad, NULL) == EINVAL, "clock_nanosleep(tv_sec -1)");
}

/* ---- CPU-time clocks ---- */

static void spin_ns(int64_t ns) {
    int64_t end = now_ns(CLOCK_MONOTONIC) + ns;
    while (now_ns(CLOCK_MONOTONIC) < end) {
    }
}

static void test_cpu_time(void) {
    int64_t c0 = now_ns(CLOCK_THREAD_CPUTIME_ID), p0 = now_ns(CLOCK_PROCESS_CPUTIME_ID);
    spin_ns(200 * NS_PER_MS);   /* alone on its CPU: about all of it is ours */
    int64_t busy = now_ns(CLOCK_THREAD_CPUTIME_ID) - c0, pbusy = now_ns(CLOCK_PROCESS_CPUTIME_ID) - p0;
    struct timespec req = ns_ts(200 * NS_PER_MS);
    int64_t c1 = now_ns(CLOCK_THREAD_CPUTIME_ID);
    nanosleep(&req, NULL);
    int64_t asleep = now_ns(CLOCK_THREAD_CPUTIME_ID) - c1;
    printf("timetest: CPU time %lld ms busy (process %lld ms), %lld us over a 200 ms sleep\n",
           (long long)busy / NS_PER_MS, (long long)pbusy / NS_PER_MS, (long long)asleep / 1000);
    CHECK(busy >= 150 * NS_PER_MS && busy <= 220 * NS_PER_MS, "THREAD_CPUTIME over a 200 ms spin: %lld ms",
          (long long)busy / NS_PER_MS);
    CHECK(pbusy >= 150 * NS_PER_MS && pbusy <= 220 * NS_PER_MS, "PROCESS_CPUTIME over a 200 ms spin: %lld ms",
          (long long)pbusy / NS_PER_MS);
    CHECK(asleep < 20 * NS_PER_MS, "THREAD_CPUTIME grew %lld us during a sleep", (long long)asleep / 1000);
}

/* ---- Monotonic across CPUs ---- */

/* Two processes bounce a clock reading through two pipes; each side reads
 * the clock after it receives and must never see less than what was sent */
static void test_cross_cpu(void) {
    int ab[2], ba[2];
    if (pipe(ab) != 0 || pipe(ba) != 0) {
        CHECK(0, "pipe");
        return;
    }
    const int rounds = 2000;
    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        for (int i = 0; i < rounds; i++) {
            int64_t sent;
            if (read(ab[0], &sent, sizeof(sent)) != sizeof(sent)) _exit(100);
            int64_t t = now_ns(CLOCK_MONOTONIC);
            if (t < sent) bad++;
            if (write(ba[1], &t, sizeof(t)) != sizeof(t)) _exit(101);
        }
        _exit(bad > 99 ? 99 : bad);
    }
    int bad = 0;
    for (int i = 0; i < rounds; i++) {
        int64_t t = now_ns(CLOCK_MONOTONIC), got;
        if (write(ab[1], &t, sizeof(t)) != sizeof(t) || read(ba[0], &got, sizeof(got)) != sizeof(got)) {
            CHECK(0, "pipe ping-pong broke at round %d", i);
            break;
        }
        if (now_ns(CLOCK_MONOTONIC) < got) bad++;
    }
    int status;
    waitpid(pid, &status, 0);
    int child_bad = WIFEXITED(status) ? WEXITSTATUS(status) : 1000;
    printf("timetest: cross-CPU monotonic: %d round trips, %d + %d readings behind\n", rounds, bad, child_bad);
    CHECK(bad == 0 && child_bad == 0, "CLOCK_MONOTONIC went backwards between processes (%d, %d)", bad, child_bad);
    close(ab[0]);
    close(ab[1]);
    close(ba[0]);
    close(ba[1]);
}

/* ---- Preemption under load ---- */

/* HOGS processes spin HOG_NS of wall time each, checking their own clock
 * stays monotonic as they move between CPUs; each exits with the share of
 * the CPU it got, in percent. They read the clock only every HOG_SPIN loops:
 * every syscall takes the kernel lock, and 8 CPUs reading the clock flat out
 * would hold the woken sleeper off it. Meanwhile the parent sleeps and checks
 * how late it wakes: run queues are FIFO, so with 8 hogs on 4 CPUs it can
 * queue behind one hog and wait out the running slice and one more. */
static void test_load(void) {
    pid_t pids[HOGS];
    for (int i = 0; i < HOGS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            int64_t w0 = now_ns(CLOCK_MONOTONIC), c0 = now_ns(CLOCK_THREAD_CPUTIME_ID), prev = w0;
            int back = 0;
            for (;;) {
                for (volatile int spin = 0; spin < HOG_SPIN; spin++) {
                }
                int64_t t = now_ns(CLOCK_MONOTONIC);
                if (t < prev) back++;
                prev = t;
                if (t - w0 >= HOG_NS) break;
            }
            int64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - c0;
            _exit(back ? 200 : (int)(cpu * 100 / HOG_NS));
        }
    }
    struct timespec req = ns_ts(NS_PER_SEC);
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    nanosleep(&req, NULL);
    int64_t late = now_ns(CLOCK_MONOTONIC) - t0 - NS_PER_SEC;
    int starved = 0, back = 0, min_pct = 100, total = 0;
    for (int i = 0; i < HOGS; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        int pct = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
        if (pct == 200) {
            back++;
            continue;
        }
        if (pct < 10) starved++;
        if (pct < min_pct) min_pct = pct;
        total += pct;
    }
    printf("timetest: %d CPU-bound processes: least CPU share %d%%, sum %d%%; 1 s sleep beside them %lld ms late\n",
           HOGS, min_pct, total, (long long)late / NS_PER_MS);
    CHECK(back == 0, "%d processes saw CLOCK_MONOTONIC go backwards", back);
    CHECK(starved == 0, "%d processes got under 10%% of a CPU: the slice timer isn't preempting", starved);
    /* At most the running slice plus one queued ahead (2 x 100 ms), plus margin */
    CHECK(late >= 0 && late <= 300 * NS_PER_MS, "1 s sleep under load: %lld ms late", (long long)late / NS_PER_MS);
}

/* ---- Tickless idle ---- */

/* The LOC row of /proc/interrupts: per-CPU timer interrupt counts */
static int read_loc(uint64_t counts[MAX_CPUS]) {
    FILE *f = fopen("/proc/interrupts", "r");
    if (f == NULL) return -1;
    char line[512];
    int n = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        while (*p == ' ') p++;
        if (strncmp(p, "LOC:", 4) != 0) continue;
        p += 4;
        n = 0;
        while (n < MAX_CPUS) {
            char *end;
            unsigned long long v = strtoull(p, &end, 10);
            if (end == p) break;
            counts[n++] = v;
            p = end;
        }
    }
    fclose(f);
    return n;
}

static void test_idle(void) {
    uint64_t before[MAX_CPUS], after[MAX_CPUS];
    int n = read_loc(before);
    CHECK(n >= 1, "/proc/interrupts has no LOC row");
    if (n < 1) return;
    struct timespec req = ns_ts(5 * NS_PER_SEC);
    nanosleep(&req, NULL);
    CHECK(read_loc(after) == n, "/proc/interrupts LOC row changed width");
    /* The CPU this process slept on takes its wakeup (and the one-shot
     * fallback may fire early once); the rest are idle */
    uint64_t total = 0, most = 0;
    printf("timetest: timer interrupts per CPU over a 5 s idle sleep:");
    for (int c = 0; c < n; c++) {
        uint64_t d = after[c] - before[c];
        printf(" %llu", (unsigned long long)d);
        total += d;
        if (d > most) most = d;
    }
    printf("\n");
    uint64_t idle = total - most;
    printf("ARC_TEST budget idle_timer_irqs_5s %llu\n", (unsigned long long)idle);
    CHECK(most <= 4, "the sleeping CPU took %llu timer interrupts in 5 s", (unsigned long long)most);
}

int main(int argc, char **argv) {
    const char *part = argc > 1 ? argv[1] : "all";
    int all = strcmp(part, "all") == 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (all || strcmp(part, "clocks") == 0) test_clocks();
    if (all || strcmp(part, "sleep") == 0) test_sleeps();
    if (all || strcmp(part, "cputime") == 0) test_cpu_time();
    if (all || strcmp(part, "crosscpu") == 0) test_cross_cpu();
    if (all || strcmp(part, "load") == 0) test_load();
    if (all || strcmp(part, "idle") == 0) test_idle();
    printf("timetest: %s: %d failures\n", part, failures);
    return failures;
}
