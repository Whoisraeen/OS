#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// Initialize syscall mechanism (MSRs)
void syscall_init(void);

// Syscall handler (C function called from assembly)
uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);

// Syscalls
#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_READ         3
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
#define SYS_SEC_GETCAPS  20
#define SYS_PROC_EXEC    41

#endif
