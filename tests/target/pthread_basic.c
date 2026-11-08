/* arc_os target program (M0-04): POSIX threads under musl.
 *
 * Thread create/join, mutex, condition variable with a timed wait, rwlock,
 * thread-local storage and pthread_once: what musl turns into clone, futex,
 * set_tid_address and friends (M1-16, M1-17). Exit status 0 = pass. */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define THREADS 4
#define ITERS   10000

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static long counter;
static int ready;
static int once_runs;
static _Thread_local int tls_value;

static void once_fn(void) { once_runs++; }

static void *worker(void *arg) {
    long id = (long)arg;
    tls_value = (int)id;
    pthread_once(&once, once_fn);
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    pthread_rwlock_rdlock(&rw);
    pthread_rwlock_unlock(&rw);

    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return (void *)(long)(tls_value * 10);
}

int main(void) {
    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++) {
        CHECK(pthread_create(&t[i], NULL, worker, (void *)i) == 0);
    }

    /* Timed wait that must time out (nobody signals cond2) */
    pthread_cond_t cond2 = PTHREAD_COND_INITIALIZER;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 20 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&lock);
    CHECK(pthread_cond_timedwait(&cond2, &lock, &ts) == ETIMEDOUT);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);

    for (long i = 0; i < THREADS; i++) {
        void *ret = NULL;
        CHECK(pthread_join(t[i], &ret) == 0);
        CHECK((long)ret == i * 10);
    }
    CHECK(counter == (long)THREADS * ITERS);
    CHECK(once_runs == 1);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t d;
    CHECK(pthread_create(&d, &attr, worker, (void *)0) == 0);
    pthread_attr_destroy(&attr);

    printf("pthread_basic: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
