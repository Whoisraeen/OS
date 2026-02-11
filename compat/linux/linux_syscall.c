#include "linux_syscalls.h"
#include "../../sched.h"
#include "../../syscall.h"
#include "../../idt.h"
#include "../../console.h"
#include "../../serial.h"
#include "../../vfs.h"
#include "../../fd.h"
#include "../../string.h"
#include "../../heap.h"
#include "../../vmm.h"
#include "../../pmm.h"
#include "../../vm_area.h"

// External prototypes for RaeenOS syscall logic (implemented in syscall.c or elsewhere)
extern uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, struct interrupt_frame *regs);

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

uint64_t linux_syscall_handler(struct interrupt_frame *regs) {
    uint64_t num = regs->rax;
    uint32_t pid = task_current_id();

    switch (num) {
        case LINUX_SYS_READ:
            return syscall_handler(SYS_READ, regs->rdi, regs->rsi, regs->rdx, NULL);

        case LINUX_SYS_WRITE:
            return syscall_handler(SYS_WRITE, regs->rdi, regs->rsi, regs->rdx, NULL);

        case LINUX_SYS_OPEN:
            return syscall_handler(SYS_OPEN, regs->rdi, regs->rsi, 0, NULL);

        case LINUX_SYS_CLOSE:
            return syscall_handler(SYS_CLOSE, regs->rdi, 0, 0, NULL);

        case LINUX_SYS_LSEEK:
            return syscall_handler(SYS_LSEEK, regs->rdi, regs->rsi, regs->rdx, NULL);

        case LINUX_SYS_MMAP: {
            // Linux mmap: arg1=addr, arg2=len, arg3=prot, arg4=flags, arg5=fd, arg6=offset
            // Native SYS_MMAP: arg1=hint, arg2=size, arg3=prot
            // For now, map simple anonymous memory
            return syscall_handler(SYS_MMAP, regs->rdi, regs->rsi, regs->rdx, NULL);
        }

        case LINUX_SYS_MUNMAP:
            return syscall_handler(SYS_MUNMAP, regs->rdi, regs->rsi, 0, NULL);

        case LINUX_SYS_BRK:
            return syscall_handler(SYS_BRK, regs->rdi, 0, 0, NULL);

        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP:
            syscall_handler(SYS_EXIT, regs->rdi, 0, 0, NULL);
            return 0;

        case LINUX_SYS_GETPID:
            return (uint64_t)pid;

        case LINUX_SYS_GETPPID: {
            task_t *task = task_get_by_id(pid);
            return task ? (uint64_t)task->parent_pid : 0;
        }

        case LINUX_SYS_ARCH_PRCTL: {
            // Linux uses this to set FS/GS base
            // arg1 = code, arg2 = addr
            #define ARCH_SET_GS 0x1001
            #define ARCH_SET_FS 0x1002
            #define ARCH_GET_FS 0x1003
            #define ARCH_GET_GS 0x1004

            if (regs->rdi == ARCH_SET_FS) {
                return syscall_handler(SYS_SET_TLS, regs->rsi, 0, 0, NULL);
            }
            return (uint64_t)-1;
        }

        case LINUX_SYS_UNAME: {
            // Fill utsname struct
            linux_utsname_t *name = (linux_utsname_t *)regs->rdi;
            // TODO: check_user_pointer
            strcpy(name->sysname, "Linux"); // Trick them!
            strcpy(name->nodename, "raeenos");
            strcpy(name->release, "5.15.0-raeenos");
            strcpy(name->version, "#1 SMP 2026");
            strcpy(name->machine, "x86_64");
            return 0;
        }

        case LINUX_SYS_SET_TID_ADDRESS:
            // Often called by Glibc on startup. Just return PID for now.
            return (uint64_t)pid;

        default:
            kprintf("[LINUX] Unsupported syscall #%lu from PID %u
", num, pid);
            return (uint64_t)-1;
    }
}
