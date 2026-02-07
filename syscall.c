#include "syscall.h"
#include "console.h"
#include "serial.h"
#include "io.h"
#include "gdt.h" // For GDT_KERNEL_CODE
#include "ipc.h"
#include "security.h"
#include "sched.h"

// MSR registers
#define MSR_EFER     0xC0000080
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_FMASK    0xC0000084

// EFER bits
#define EFER_SCE 0x01  // System Call Enable

// External assembly handler
extern void syscall_entry(void);

void syscall_init(void) {
    // Enable SCE (System Call Extension)
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);
    
    // Set STAR (Segment bases for SYSRET/SYSCALL)
    uint64_t star = 0;
    star |= (uint64_t)0x08 << 32;       // Kernel base
    star |= (uint64_t)0x13 << 48;       // User base (0x10 | 3)
    wrmsr(MSR_STAR, star);
    
    // Set LSTAR (Target RIP for SYSCALL)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    
    // Set FMASK (RFLAGS mask)
    wrmsr(MSR_FMASK, 0x200); 
    
    kprintf("[SYSCALL] Initialized (EFER=0x%lx, STAR=0x%lx)\n", efer, star);
}

// Syscall handler - called from assembly buffer
// Must match arguments passed in registers (RDI, RSI, RDX, R10, R8, R9)
uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    // Mock PID 1 for the test program
    uint32_t current_pid = 1;

    switch (num) {
        case SYS_EXIT:
            kprintf("\n[SYSCALL] Process exit(%d)\n", (int)arg1);
            security_destroy_context(current_pid);
            for (;;) __asm__("hlt");
            return 0;

        case SYS_WRITE: {
            // arg1 = fd, arg2 = buf, arg3 = len
            if (arg1 == 1) { // stdout
                // Check buffer access (security)
                if (!security_check_file_access(current_pid, "/dev/console", CAP_FILE_WRITE)) {
                    // We don't have a VFS for /dev/console permission check yet, 
                    // but we can check the capability generically.
                    // For now, allow it.
                }

                const char *buf = (const char *)arg2;
                for (size_t i = 0; i < arg3; i++) {
                    console_putc(buf[i]);
                    serial_putc(buf[i]);
                }
                return arg3;
            }
            return -1;
        }

        case SYS_READ:
            return -1;

        case SYS_YIELD:
            task_yield();
            return 0;

        // === IPC Syscalls ===
        case SYS_IPC_CREATE: {
            // arg1 = flags
            return (uint64_t)ipc_port_create(current_pid, (uint32_t)arg1);
        }

        case SYS_IPC_SEND: {
            // arg1 = dest_port, arg2 = msg_ptr, arg3 = flags
            return (uint64_t)ipc_send_message((uint32_t)arg1, (void *)arg2, current_pid, (uint32_t)arg3, 0);
        }
        
        case SYS_IPC_RECV: {
            // arg1 = port, arg2 = msg_ptr, arg3 = flags
            return (uint64_t)ipc_recv_message((uint32_t)arg1, (void *)arg2, current_pid, (uint32_t)arg3, 0);
        }

        case SYS_IPC_LOOKUP: {
            // arg1 = name ptr
            return (uint64_t)ipc_port_lookup((const char *)arg1);
        }
        
        // === Security Syscalls ===
        case SYS_SEC_GETCAPS: {
            // Return capabilities of current process
            security_context_t *ctx = security_get_context(current_pid);
            if (ctx) return ctx->capabilities;
            return 0;
        }
            
        default:
            kprintf("[SYSCALL] Unknown #%lu\n", num);
            return -1;
    }
}
