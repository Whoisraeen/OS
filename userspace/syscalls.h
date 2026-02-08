#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

// Syscall Numbers
#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_YIELD        24
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
#define SYS_PROC_EXEC    41

// IPC Constants
#define IPC_PORT_FLAG_RECEIVE (1 << 1)
#define IPC_PORT_FLAG_SEND    (1 << 2)
#define IPC_RECV_NONBLOCK     (1 << 0)

// Syscall Wrappers
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

#endif // SYSCALLS_H
