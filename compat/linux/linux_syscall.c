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

// External prototypes for RaeenOS syscall logic
extern uint64_t syscall_handler(uint64_t num, struct interrupt_frame *regs);

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

    // Linux Syscall Args: RDI, RSI, RDX, R10, R8, R9
    uint64_t arg1 = regs->rdi;
    uint64_t arg2 = regs->rsi;
    uint64_t arg3 = regs->rdx;
    uint64_t arg4 = regs->r10;
    uint64_t arg5 = regs->r8;
    uint64_t arg6 = regs->r9;

    switch (num) {
        case LINUX_SYS_READ: {
            regs->rdi = arg1; regs->rsi = arg2; regs->rdx = arg3;
            return syscall_handler(SYS_READ, regs);
        }

        case LINUX_SYS_WRITE: {
            regs->rdi = arg1; regs->rsi = arg2; regs->rdx = arg3;
            return syscall_handler(SYS_WRITE, regs);
        }

        case LINUX_SYS_OPEN: {
            regs->rdi = arg1; regs->rsi = arg2; 
            return syscall_handler(SYS_OPEN, regs);
        }

        case LINUX_SYS_CLOSE: {
            regs->rdi = arg1;
            return syscall_handler(SYS_CLOSE, regs);
        }

        case LINUX_SYS_LSEEK: {
            regs->rdi = arg1; regs->rsi = arg2; regs->rdx = arg3;
            return syscall_handler(SYS_LSEEK, regs);
        }

        case LINUX_SYS_MMAP: {
            // Linux mmap: arg1=addr, arg2=len, arg3=prot, arg4=flags, arg5=fd, arg6=offset
            // Native SYS_MMAP: arg1=hint, arg2=size, arg3=prot
            regs->rdi = arg1; regs->rsi = arg2; regs->rdx = arg3;
            return syscall_handler(SYS_MMAP, regs);
        }

        case LINUX_SYS_MUNMAP: {
            regs->rdi = arg1; regs->rsi = arg2;
            return syscall_handler(SYS_MUNMAP, regs);
        }

        case LINUX_SYS_BRK: {
            regs->rdi = arg1;
            return syscall_handler(SYS_BRK, regs);
        }

        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP: {
            regs->rdi = arg1;
            syscall_handler(SYS_EXIT, regs);
            return 0;
        }

        case LINUX_SYS_GETPID:
            return (uint64_t)pid;

        case LINUX_SYS_GETPPID: {
            task_t *task = task_get_by_id(pid);
            return task ? (uint64_t)task->parent_pid : 0;
        }

        case LINUX_SYS_ARCH_PRCTL: {
            // arg1 = code, arg2 = addr
            #define ARCH_SET_GS 0x1001
            #define ARCH_SET_FS 0x1002
            if (arg1 == ARCH_SET_FS) {
                regs->rdi = arg2;
                return syscall_handler(SYS_SET_TLS, regs);
            }
            return (uint64_t)-1;
        }

        case LINUX_SYS_UNAME: {
            linux_utsname_t *name = (linux_utsname_t *)arg1;
            if (!is_user_address(arg1, sizeof(linux_utsname_t))) return (uint64_t)-1;
            strcpy(name->sysname, "Linux");
            strcpy(name->nodename, "raeenos");
            strcpy(name->release, "5.15.0-raeenos");
            strcpy(name->version, "#1 SMP 2026");
            strcpy(name->machine, "x86_64");
            return 0;
        }

        case LINUX_SYS_SET_TID_ADDRESS:
            return (uint64_t)pid;

        case LINUX_SYS_IOCTL:
            // Just return success for common terminal ioctls
            return 0;

        case LINUX_SYS_FUTEX:
            // Minimal futex (likely used by Glibc/Wine)
            // arg1=uaddr, arg2=op, arg3=val
            regs->rdi = arg1; regs->rsi = arg2; regs->rdx = arg3;
            return syscall_handler(SYS_FUTEX, regs);

        default:
            kprintf("[LINUX] Unsupported syscall #%lu (arg1=%lx) from PID %u\n", num, arg1, pid);
            return (uint64_t)-1;
    }
}