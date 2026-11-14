/* arc_os — Power profiles and the idle readout (M1-21). Test ISOs only.
 *
 * Built with stock musl. Runs in QEMU (suite "power", -smp 4), where the CPU
 * has no MWAIT, HWP or RAPL, so the kernel idles with HLT. Checks:
 *  - profile: /proc/arc/power names the profile (balanced at boot), the idle
 *    method, HWP and energy lines and one residency line per CPU; writing
 *    "saver" / "performance" / "balanced" switches profile (with or without a
 *    newline); an unknown or over-long name gives EINVAL and changes nothing.
 *  - chunks: a reader taking 7 bytes per read() gets one whole, consistent
 *    text (the snapshot stays with the open file until it reads offset 0).
 *  - idle: over a 1 s sleep the CPUs' idle residency grows by at least 0.9 s
 *    in total (one CPU runs this program for part of it) and their idle
 *    entries grow (the HLT/MWAIT path counts).
 * Prints one FAIL line per failed check; exit status = failures. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POWER_FILE "/proc/arc/power"
#define TEXT_MAX   8192

static int failures;

#define CHECK(cond, ...)                \
    do {                                \
        if (!(cond)) {                  \
            printf("FAIL powertest: "); \
            printf(__VA_ARGS__);        \
            printf("\n");               \
            failures++;                 \
        }                               \
    } while (0)

/* Read the whole file with reads of `chunk` bytes. Returns the length or -1. */
static int read_power(char *buf, size_t chunk) {
    int fd = open(POWER_FILE, O_RDONLY);
    if (fd < 0) return -1;
    size_t len = 0;
    for (;;) {
        size_t want = chunk < TEXT_MAX - 1 - len ? chunk : TEXT_MAX - 1 - len;
        if (want == 0) break;
        ssize_t n = read(fd, buf + len, want);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    return (int)len;
}

/* Write s to the power file. Returns 0 or the errno. */
static int write_power(const char *s, size_t n) {
    int fd = open(POWER_FILE, O_WRONLY);
    if (fd < 0) return errno;
    ssize_t r = write(fd, s, n);
    int err = r < 0 ? errno : (r == (ssize_t)n ? 0 : EIO);
    close(fd);
    return err;
}

static int has_line(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    for (const char *p = text; p != NULL && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, prefix, n) == 0) return 1;
    }
    return 0;
}

/* CPUs online: "processor" lines in /proc/cpuinfo (musl's sysconf needs
 * sched_getaffinity, which comes with M1-17) */
static int online_cpus(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) return 1;
    char line[128];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL) n += strncmp(line, "processor", 9) == 0;
    fclose(f);
    return n > 0 ? n : 1;
}

/* Sum the residency and entries of every state of every cpuN line. Returns
 * how many cpu lines it found. */
static int sum_idle(const char *text, unsigned long long *ns, unsigned long long *entries) {
    int cpus = 0;
    *ns = *entries = 0;
    for (const char *p = text; p != NULL && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        char line[160];
        size_t len = strcspn(p, "\n");   /* sscanf must not run into the next line */
        if (len >= sizeof(line)) continue;
        memcpy(line, p, len);
        line[len] = '\0';
        unsigned cpu;
        char n1[8], n2[8];
        unsigned long long a, b, c = 0, d = 0;
        int got = sscanf(line, "cpu%u %7s %llu %llu %7s %llu %llu", &cpu, n1, &a, &b, n2, &c, &d);
        if (got != 4 && got != 7) continue;
        cpus++;
        *ns += a + c;
        *entries += b + d;
    }
    return cpus;
}

static void expect_profile(const char *name) {
    static char text[TEXT_MAX];
    char want[32];
    snprintf(want, sizeof(want), "profile: %s\n", name);
    CHECK(read_power(text, 4096) > 0 && strstr(text, want) == text, "profile should read %s; text starts: %.40s", name,
          text);
}

static void check_profiles(void) {
    static char text[TEXT_MAX];
    int len = read_power(text, 4096);
    CHECK(len > 0, "read %s: %s", POWER_FILE, strerror(errno));
    if (len <= 0) return;
    CHECK(strncmp(text, "profile: balanced\n", 18) == 0, "boot profile is not balanced: %.40s", text);
    CHECK(has_line(text, "profiles: performance balanced saver"), "no profiles line");
    CHECK(has_line(text, "hwp: "), "no hwp line");
    CHECK(has_line(text, "idle: hlt") || has_line(text, "idle: mwait"), "no idle line");
    CHECK(has_line(text, "energy_j: "), "no energy line");
    unsigned long long ns, n;
    int cpus = sum_idle(text, &ns, &n);
    CHECK(cpus == online_cpus(), "%d cpu lines for %d CPUs", cpus, online_cpus());

    CHECK(write_power("saver\n", 6) == 0, "write saver: %s", strerror(errno));
    expect_profile("saver");
    CHECK(write_power("performance", 11) == 0, "write performance (no newline) failed");
    expect_profile("performance");
    CHECK(write_power("turbo\n", 6) == EINVAL, "an unknown profile should give EINVAL");
    CHECK(write_power("balancedxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 36) == EINVAL, "an over-long write should give EINVAL");
    CHECK(write_power("balance", 7) == EINVAL, "a prefix of a name should give EINVAL");
    expect_profile("performance");
    CHECK(write_power("balanced\n", 9) == 0, "write balanced failed");
    expect_profile("balanced");
}

static void check_chunked_read(void) {
    static char text[TEXT_MAX];
    int len = read_power(text, 7);
    CHECK(len > 0, "chunked read failed");
    if (len <= 0) return;
    CHECK(strncmp(text, "profile: ", 9) == 0, "chunked text starts wrong: %.20s", text);
    CHECK(text[len - 1] == '\n', "chunked text does not end with a newline");
    unsigned long long ns, n;
    CHECK(sum_idle(text, &ns, &n) == online_cpus(), "chunked text lacks cpu lines");
    CHECK(strstr(text, "\nprofile: ") == NULL, "chunked text has a second snapshot inside it");

    /* Seek back to 0: a fresh snapshot, same shape */
    int fd = open(POWER_FILE, O_RDONLY);
    char a[64], b[64];
    CHECK(fd >= 0 && read(fd, a, sizeof(a)) == (ssize_t)sizeof(a) && lseek(fd, 0, SEEK_SET) == 0 &&
              read(fd, b, sizeof(b)) == (ssize_t)sizeof(b) && memcmp(a, b, 18) == 0,
          "re-reading from offset 0 failed");
    if (fd >= 0) close(fd);
}

static void check_idle_residency(void) {
    static char before[TEXT_MAX], after[TEXT_MAX];
    unsigned long long ns0, n0, ns1, n1;
    CHECK(read_power(before, 4096) > 0, "read before");
    sum_idle(before, &ns0, &n0);
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    CHECK(read_power(after, 4096) > 0, "read after");
    sum_idle(after, &ns1, &n1);
    /* At least 0.9 s: with one CPU, this program runs for part of the second */
    CHECK(ns1 >= ns0 + 900000000ULL, "idle residency grew only %llu ns over a 1 s sleep", ns1 - ns0);
    CHECK(n1 > n0, "idle entries did not grow (%llu -> %llu)", n0, n1);
    printf("powertest: idle residency +%llu ms, entries +%llu over 1 s on %d CPUs\n", (ns1 - ns0) / 1000000, n1 - n0,
           online_cpus());
}

int main(int argc, char **argv) {
    const char *part = argc > 1 ? argv[1] : "all";
    int all = strcmp(part, "all") == 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (all || strcmp(part, "profile") == 0) check_profiles();
    if (all || strcmp(part, "chunks") == 0) check_chunked_read();
    if (all || strcmp(part, "idle") == 0) check_idle_residency();
    printf("powertest: %s: %d failures\n", part, failures);
    return failures;
}
