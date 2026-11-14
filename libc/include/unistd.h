#ifndef ARCHOS_LIBC_UNISTD_H
#define ARCHOS_LIBC_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

/* The environment (set by crt0 from the initial stack) */
extern char **environ;

/* Seek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* File operations */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     open(const char *path, int flags, ...);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     unlink(const char *path);
int rmdir(const char *path);
int     pipe(int pipefd[2]);
void sync(void);

/* Process operations */
pid_t   fork(void);
int     execv(const char *path, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);
void    _exit(int status) __attribute__((noreturn));
pid_t   getpid(void);
pid_t   getppid(void);
uid_t   getuid(void);
gid_t   getgid(void);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   getpgid(pid_t pid);

/* Terminal foreground process group (ioctl TIOCSPGRP / TIOCGPGRP) */
int     tcsetpgrp(int fd, pid_t pgrp);
pid_t   tcgetpgrp(int fd);

/* Directory operations */
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);

/* Memory */
void   *sbrk(intptr_t increment);

#endif /* ARCHOS_LIBC_UNISTD_H */
