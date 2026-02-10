#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

// Syscall Numbers
#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_OPEN         2
#define SYS_READ         3
#define SYS_CLOSE        4
#define SYS_BRK          5
#define SYS_MMAP         6
#define SYS_MUNMAP       7
#define SYS_LSEEK        8
#define SYS_FORK         9
#define SYS_YIELD        24
#define SYS_DUP          32
#define SYS_DUP2         33
#define SYS_IPC_CREATE   10
#define SYS_IPC_SEND     11
#define SYS_IPC_RECV     12
#define SYS_IPC_LOOKUP   13
#define SYS_IPC_REGISTER 14
#define SYS_GET_FRAMEBUFFER 15
#define SYS_GET_INPUT_EVENT 16
#define SYS_IPC_SHMEM_CREATE 17
#define SYS_IPC_SHMEM_MAP    18
#define SYS_IPC_SHMEM_UNMAP  19
#define SYS_GETPID       39
#define SYS_GETPPID      40
#define SYS_PROC_EXEC    41
#define SYS_WAIT         42
#define SYS_WAITPID      43
#define SYS_KILL         44
#define SYS_PIPE         45
#define SYS_SIGNAL       46
#define SYS_THREAD_CREATE 50
#define SYS_THREAD_EXIT  51
#define SYS_THREAD_JOIN  52
#define SYS_FUTEX        53
#define SYS_SET_TLS      54
#define SYS_CLOCK_GETTIME 55
#define SYS_REBOOT       56
#define SYS_SHUTDOWN     57
#define SYS_IOCTL        58
#define SYS_STAT         59
#define SYS_MKDIR        60
#define SYS_RMDIR        61
#define SYS_UNLINK       62
#define SYS_RENAME       63
#define SYS_GETDENTS     64
#define SYS_IOPORT       65
#define SYS_IRQ_WAIT     66
#define SYS_IRQ_ACK      67
#define SYS_MAP_PHYS     68
#define SYS_AIO_SUBMIT   69
#define SYS_AIO_WAIT     70
#define SYS_SEC_GRANT    71

// Socket Syscalls
#define SYS_SOCKET       80
#define SYS_BIND         81
#define SYS_LISTEN       82
#define SYS_ACCEPT       83
#define SYS_CONNECT      84
#define SYS_SEND         85
#define SYS_RECV         86

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

// Fork wrapper — uses INT 0x80 (not SYSCALL) because fork needs full register frame
static inline long sys_fork(void) {
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"((long)SYS_FORK)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// Memory management wrappers
static inline long sys_brk(long addr) {
    return syscall1(SYS_BRK, addr);
}

static inline long sys_mmap(long addr, long size, long prot) {
    return syscall3(SYS_MMAP, addr, size, prot);
}

static inline long sys_munmap(long addr, long size) {
    return syscall2(SYS_MUNMAP, addr, size);
}

#endif // SYSCALLS_H
