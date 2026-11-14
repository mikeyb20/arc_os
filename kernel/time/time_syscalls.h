#ifndef ARCHOS_TIME_TIME_SYSCALLS_H
#define ARCHOS_TIME_TIME_SYSCALLS_H

/* Register the time syscalls (clock_gettime, clock_getres, clock_nanosleep,
 * nanosleep, gettimeofday, time) with the dispatcher (M1-06). */
void time_syscalls_init(void);

#endif /* ARCHOS_TIME_TIME_SYSCALLS_H */
