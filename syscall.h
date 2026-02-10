#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// Initialize syscall mechanism (MSRs)
void syscall_init(void);

// Forward declaration
struct interrupt_frame;

// Syscall handler (C function called from assembly)
// regs is non-NULL when called via INT 0x80 (full register frame available for fork)
// regs is NULL when called via SYSCALL instruction
uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                          struct interrupt_frame *regs);

// Syscalls
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
#define SYS_SEC_GETCAPS  20
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

#endif
