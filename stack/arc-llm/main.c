/*
 * arc-llm — the model service (ADR-0012, M2-04; arc_llm.h has the map of its parts).
 *
 *   arc-llm [--root DIR] [--socket PATH] [--threads N] [--ctx N] [--reserve-mb N]
 *
 * --root        system root holding models/roles.json (default /)
 * --socket      AF_UNIX socket path (default <root>/var/run/arc-llm.sock)
 * --threads     compute threads (default: the CPUs this process may run on)
 * --ctx         context cap in tokens; n_ctx = min(model's context_length, this) (default 4096)
 * --reserve-mb  RAM kept back for the OS and page cache in the fit check (default 1024)
 *
 * Serves one connection at a time and sleeps in accept() while idle: no timers, no polling.
 * SIGTERM/SIGINT finish the current token, close and unlink the socket, and exit 0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "api.h"
#include "arc_llm.h"
#include "engine.h"
#include "http.h"
#include "roles.h"

#define CLIENT_TIMEOUT_S 30 /* a client that sends or reads nothing for this long is dropped */

volatile sig_atomic_t arc_llm_stopping;
static int listen_fd = -1;

/* on_stop: SIGTERM/SIGINT. shutdown() makes a blocked or later accept() fail, so no signal is lost
 * between the flag check and the accept call. */
static void on_stop(int sig) {
    (void)sig;
    arc_llm_stopping = 1;
    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
    }
}

static void usage(void) {
    fputs("usage: arc-llm [--root DIR] [--socket PATH] [--threads N] [--ctx N] [--reserve-mb N]\n", stderr);
    exit(2);
}

/* number: a decimal argument in [min, max], or usage(). */
static unsigned long long number(const char *s, unsigned long long min, unsigned long long max) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || s[0] == '-' || v < min || v > max) {
        usage();
    }
    return v;
}

/* default_threads: CPUs in this process's affinity mask. */
static int default_threads(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return 1;
    }
    int n = CPU_COUNT(&set);
    return n > 0 ? n : 1;
}

/* open_socket: bind and listen on `path`; a stale socket file is removed, a live one is an error. */
static int open_socket(const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "arc-llm: socket path longer than %zu bytes: %s\n", sizeof(addr.sun_path) - 1, path);
        return -1;
    }
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("arc-llm: socket");
        return -1;
    }
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno == EADDRINUSE) {
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        bool live = probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
        if (probe >= 0) {
            close(probe);
        }
        if (live) {
            fprintf(stderr, "arc-llm: another arc-llm is serving %s\n", path);
            close(fd);
            return -1;
        }
        unlink(path); /* left behind by a process that died */
        rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    }
    if (rc != 0 || listen(fd, 16) != 0) {
        fprintf(stderr, "arc-llm: cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* serve: one connection: read the request, answer it, close. */
static void serve(int fd, llm_registry *r) {
    struct timeval timeout = {.tv_sec = CLIENT_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    static http_conn c; /* 16 KiB head buffer: static, not on the stack */
    c.fd = fd;
    c.head_len = 0;
    c.body = NULL;
    c.body_len = 0;
    c.gone = false;
    const char *why = NULL;
    int status = http_read_request(&c, &why);
    if (status != 0) {
        api_error(&c, status, why);
    } else {
        api_handle(&c, r);
    }
    http_free(&c);
    close(fd);
}

int main(int argc, char **argv) {
    arc_llm_config config = {.root = "/", .threads = 0, .ctx_cap = 4096, .reserve_mb = 1024};
    const char *socket_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (val == NULL) {
            usage();
        } else if (strcmp(arg, "--root") == 0) {
            config.root = val;
        } else if (strcmp(arg, "--socket") == 0) {
            socket_path = val;
        } else if (strcmp(arg, "--threads") == 0) {
            config.threads = (int)number(val, 1, 1024);
        } else if (strcmp(arg, "--ctx") == 0) {
            config.ctx_cap = (uint32_t)number(val, 16, 1u << 20);
        } else if (strcmp(arg, "--reserve-mb") == 0) {
            config.reserve_mb = number(val, 0, 1ull << 40);
        } else {
            usage();
        }
        i++;
    }
    if (config.threads == 0) {
        config.threads = default_threads();
    }
    char default_socket[PATH_MAX];
    if (socket_path == NULL) {
        snprintf(default_socket, sizeof(default_socket), "%s/var/run/arc-llm.sock",
                 strcmp(config.root, "/") == 0 ? "" : config.root);
        socket_path = default_socket;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop; /* no SA_RESTART: a blocked accept() returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    listen_fd = open_socket(socket_path);
    if (listen_fd < 0) {
        return 1;
    }
    engine_init(&config);
    llm_registry registry;
    roles_init(&registry, config.root);
    fprintf(stderr, "arc-llm: serving %s (root %s, %d threads, ctx cap %u, reserve %llu MiB)\n", socket_path,
            config.root, config.threads, config.ctx_cap, (unsigned long long)config.reserve_mb);

    while (!arc_llm_stopping) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EINTR && !arc_llm_stopping) {
                perror("arc-llm: accept");
            }
            continue;
        }
        serve(fd, &registry);
    }

    close(listen_fd);
    unlink(socket_path);
    roles_free(&registry);
    engine_shutdown();
    fprintf(stderr, "arc-llm: stopped\n");
    return 0;
}
