#include <sys/stat.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/times.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "../syscalls.h"

// Environment
char *__env[1] = { 0 };
char **environ = __env;

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    while (1);
}

int _close(int file) {
    return syscall1(SYS_CLOSE, file);
}

int _execve(char *name, char **argv, char **env) {
    // Basic exec support (ignores argv/env for now)
    return syscall1(SYS_PROC_EXEC, (long)name);
}

int _fork(void) {
    return sys_fork();
}

int _fstat(int file, struct stat *st) {
    // TODO: Map ext2_stat to struct stat
    return 0;
}

int _getpid(void) {
    return syscall0(SYS_GETPID);
}

int _isatty(int file) {
    // TODO: Check if fd is a terminal
    return 1;
}

int _kill(int pid, int sig) {
    return syscall2(SYS_KILL, pid, sig);
}

int _link(char *old, char *new) {
    return -1; // Not supported
}

int _lseek(int file, int ptr, int dir) {
    return syscall3(SYS_LSEEK, file, ptr, dir);
}

int _open(const char *name, int flags, ...) {
    return syscall2(SYS_OPEN, (long)name, flags);
}

int _read(int file, char *ptr, int len) {
    return syscall3(SYS_READ, file, (long)ptr, len);
}

caddr_t _sbrk(int incr) {
    long current_brk = syscall1(SYS_BRK, 0);
    if (incr == 0) return (caddr_t)current_brk;
    
    long new_brk = syscall1(SYS_BRK, current_brk + incr);
    if (new_brk < current_brk + incr) {
        return (caddr_t)-1;
    }
    return (caddr_t)current_brk;
}

int _stat(const char *file, struct stat *st) {
    return syscall2(SYS_STAT, (long)file, (long)st);
}

clock_t _times(struct tms *buf) {
    return -1;
}

int _unlink(char *name) {
    return syscall1(SYS_UNLINK, (long)name);
}

int _wait(int *status) {
    return syscall1(SYS_WAIT, (long)status);
}

int _write(int file, char *ptr, int len) {
    return syscall3(SYS_WRITE, file, (long)ptr, len);
}

int _gettimeofday(struct timeval *tv, void *tz) {
    if (tv) {
        uint64_t ts[2];
        syscall1(SYS_CLOCK_GETTIME, (long)ts);
        tv->tv_sec = ts[0];
        tv->tv_usec = ts[1];
    }
    return 0;
}
