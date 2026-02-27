#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

// Linux x86_64 Compatible Syscall Numbers
#define SYS_READ         0
#define SYS_WRITE        1
#define SYS_OPEN         2
#define SYS_CLOSE        3
#define SYS_STAT         4
#define SYS_FSTAT        5
#define SYS_LSTAT        6
#define SYS_POLL         7
#define SYS_LSEEK        8
#define SYS_MMAP         9
#define SYS_MPROTECT     10
#define SYS_MUNMAP       11
#define SYS_BRK          12
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_IOCTL        16
#define SYS_PREAD64      17
#define SYS_PWRITE64     18
#define SYS_READV        19
#define SYS_WRITEV       20
#define SYS_ACCESS       21
#define SYS_PIPE         22
#define SYS_SELECT       23
#define SYS_SCHED_YIELD  24
#define SYS_MREMAP       25
#define SYS_MSYNC        26
#define SYS_MINCORE      27
#define SYS_MADVISE      28
#define SYS_SHMGET       29
#define SYS_SHMAT        30
#define SYS_SHMCTL       31
#define SYS_DUP          32
#define SYS_DUP2         33
#define SYS_PAUSE        34
#define SYS_NANOSLEEP    35
#define SYS_GETITIMER    36
#define SYS_ALARM        37
#define SYS_SETITIMER    38
#define SYS_GETPID       39
#define SYS_SENDFILE     40
#define SYS_SOCKET       41
#define SYS_CONNECT      42
#define SYS_ACCEPT       43
#define SYS_SENDTO       44
#define SYS_RECVFROM     45
#define SYS_SENDMSG      46
#define SYS_RECVMSG      47
#define SYS_SHUTDOWN     48   // socket shutdown
#define SYS_REBOOT       169  // system power-off/reboot
#define SYS_BIND         49
#define SYS_LISTEN       50
#define SYS_GETSOCKNAME  51
#define SYS_GETPEERNAME  52
#define SYS_SOCKETPAIR   53
#define SYS_SETSOCKOPT   54
#define SYS_GETSOCKOPT   55
#define SYS_CLONE        56
#define SYS_FORK         57
#define SYS_VFORK        58
#define SYS_EXECVE       59
#define SYS_EXIT         60
#define SYS_WAIT4        61
#define SYS_KILL         62
#define SYS_UNAME        63
#define SYS_SEMGET       64
#define SYS_SEMOP        65
#define SYS_SEMCTL       66
#define SYS_SHMDT        67
#define SYS_MSGGET       68
#define SYS_MSGSND       69
#define SYS_MSGRCV       70
#define SYS_MSGCTL       71
#define SYS_FCNTL        72
#define SYS_FLOCK        73
#define SYS_FSYNC        74
#define SYS_FDATASYNC    75
#define SYS_TRUNCATE     76
#define SYS_FTRUNCATE    77
#define SYS_GETDENTS     78
#define SYS_GETCWD       79
#define SYS_CHDIR        80
#define SYS_FCHDIR       81
#define SYS_RENAME       82
#define SYS_MKDIR        83
#define SYS_RMDIR        84
#define SYS_CREAT        85
#define SYS_LINK         86
#define SYS_UNLINK       87
#define SYS_SYMLINK      88
#define SYS_READLINK     89
#define SYS_CHMOD        90
#define SYS_FCHMOD       91
#define SYS_CHOWN        92
#define SYS_FCHOWN       93
#define SYS_LCHOWN       94
#define SYS_UMASK        95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRLIMIT    97
#define SYS_GETRUSAGE    98
#define SYS_SYSINFO      99
#define SYS_TIMES        100
#define SYS_ARCH_PRCTL   158
#define SYS_FUTEX        202
#define SYS_GETDENTS64   217
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP   231

// Custom OS Syscalls (500+)
#define SYS_IPC_CREATE       500
#define SYS_IPC_SEND         501
#define SYS_IPC_RECV         502
#define SYS_IPC_LOOKUP       503
#define SYS_IPC_REGISTER     504
#define SYS_GET_FRAMEBUFFER  505
#define SYS_GET_INPUT_EVENT  506
#define SYS_IPC_SHMEM_CREATE 507
#define SYS_IPC_SHMEM_MAP    508
#define SYS_IPC_SHMEM_UNMAP  509
#define SYS_SEC_GETCAPS      510
#define SYS_SEC_GRANT        511
#define SYS_PROC_EXEC        512
#define SYS_THREAD_CREATE    513
#define SYS_THREAD_EXIT      514
#define SYS_THREAD_JOIN      515
#define SYS_SET_TLS          516
#define SYS_IOPORT           517
#define SYS_IRQ_WAIT         518
#define SYS_IRQ_ACK          519
#define SYS_GPU_UPDATE       520
#define SYS_MAP_PHYS         521
#define SYS_AIO_SUBMIT       522
#define SYS_AIO_WAIT         523

// IPC Constants
#define IPC_PORT_FLAG_RECEIVE (1 << 1)
#define IPC_PORT_FLAG_SEND    (1 << 2)
#define IPC_RECV_NONBLOCK     (1 << 0)

// Syscall Wrappers
static inline long syscall0(long num) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall1(long num, long arg1) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall2(long num, long arg1, long arg2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall3(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall4(long num, long arg1, long arg2, long arg3, long arg4) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall5(long num, long arg1, long arg2, long arg3, long arg4, long arg5) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#endif
