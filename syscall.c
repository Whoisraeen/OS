#include "syscall.h"
#include "console.h"
#include "serial.h"
#include "io.h"
#include "gdt.h"
#include "ipc.h"
#include "security.h"
#include "sched.h"
#include "vmm.h"
#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "string.h"

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
    star |= (uint64_t)GDT_KERNEL_CODE << 32;  // Kernel base
    star |= (uint64_t)0x13 << 48;              // User base (0x10 | 3)
    wrmsr(MSR_STAR, star);

    // Set LSTAR (Target RIP for SYSCALL)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // Set FMASK (RFLAGS mask — clears IF on SYSCALL entry)
    wrmsr(MSR_FMASK, 0x200);

    kprintf("[SYSCALL] Initialized (EFER=0x%lx, STAR=0x%lx)\n", efer, star);
}

// Syscall handler — called from assembly
uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    // Get real PID from scheduler (not hardcoded!)
    uint32_t current_pid = task_current_id();

    switch (num) {
        case SYS_EXIT:
            kprintf("[SYSCALL] Process %u exit(%d)\n", current_pid, (int)arg1);
            security_destroy_context(current_pid);
            task_exit(); // Properly yields to scheduler instead of halting
            return 0;    // Never reached

        case SYS_WRITE: {
            // arg1 = fd, arg2 = buf, arg3 = len
            if (arg1 == 1) { // stdout
                // Validate user buffer
                if (!is_user_address(arg2, arg3)) {
                    kprintf("[SYSCALL] SYS_WRITE: bad user address 0x%lx\n", arg2);
                    return (uint64_t)-1;
                }

                // Check file write capability
                if (!security_check_file_access(current_pid, "/dev/console", CAP_FILE_WRITE)) {
                    return (uint64_t)-1;
                }

                const char *buf = (const char *)arg2;
                for (size_t i = 0; i < arg3; i++) {
                    console_putc(buf[i]);
                    serial_putc(buf[i]);
                }
                return arg3;
            }
            return (uint64_t)-1;
        }

        case SYS_READ:
            return (uint64_t)-1;

        case SYS_YIELD:
            task_yield();
            return 0;

        // === IPC Syscalls ===
        case SYS_IPC_CREATE: {
            if (!security_has_capability(current_pid, CAP_IPC_CREATE)) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_port_create(current_pid, (uint32_t)arg1);
        }

        case SYS_IPC_SEND: {
            if (!security_has_capability(current_pid, CAP_IPC_SEND)) {
                return (uint64_t)-1;
            }
            // Validate user message pointer
            if (!is_user_address(arg2, 128)) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_send_message((uint32_t)arg1, (void *)arg2, current_pid, (uint32_t)arg3, 0);
        }

        case SYS_IPC_RECV: {
            if (!security_has_capability(current_pid, CAP_IPC_RECV)) {
                return (uint64_t)-1;
            }
            // Validate user message buffer
            if (!is_user_address(arg2, 128)) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_recv_message((uint32_t)arg1, (void *)arg2, current_pid, (uint32_t)arg3, 0);
        }

        case SYS_IPC_LOOKUP: {
            // Validate user string pointer
            char kbuf[64];
            if (copy_string_from_user(kbuf, (const char *)arg1, sizeof(kbuf)) < 0) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_port_lookup(kbuf);
        }

        case SYS_IPC_REGISTER: {
            // Validate user string pointer
            char kbuf[64];
            if (copy_string_from_user(kbuf, (const char *)arg2, sizeof(kbuf)) < 0) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_port_register((uint32_t)arg1, kbuf);
        }

        case SYS_IPC_SHMEM_CREATE: {
            if (!security_has_capability(current_pid, CAP_IPC_SHMEM)) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_shmem_create((size_t)arg1, current_pid, (uint32_t)arg2);
        }

        case SYS_IPC_SHMEM_MAP: {
            if (!security_has_capability(current_pid, CAP_IPC_SHMEM)) {
                return (uint64_t)-1;
            }
            return (uint64_t)ipc_shmem_map((uint32_t)arg1, current_pid);
        }

        case SYS_IPC_SHMEM_UNMAP: {
            return (uint64_t)ipc_shmem_unmap((uint32_t)arg1, current_pid);
        }

        // === Process Management Syscalls ===
        case SYS_PROC_EXEC: {
            if (!arg1) return (uint64_t)-1;

            // Check exec capability
            if (!security_has_capability(current_pid, CAP_PROC_EXEC)) {
                kprintf("[SYSCALL] exec: permission denied (PID %u)\n", current_pid);
                return (uint64_t)-1;
            }

            // Copy path from user space
            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) {
                kprintf("[SYSCALL] exec: bad path pointer 0x%lx\n", arg1);
                return (uint64_t)-1;
            }

            // Find file in VFS (Initrd)
            vfs_node_t *node = initrd_find(path);
            if (!node) {
                kprintf("[SYSCALL] exec: file not found '%s'\n", path);
                return (uint64_t)-1;
            }

            // Allocate kernel buffer
            void *data = kmalloc(node->length);
            if (!data) return (uint64_t)-1;

            // Read file
            vfs_read(node, 0, node->length, (uint8_t*)data);

            // Create Task
            int pid = task_create_user(path, data, node->length);

            kfree(data);

            if (pid >= 0) {
                kprintf("[SYSCALL] exec: started '%s' (PID %d)\n", path, pid);
            }
            return (uint64_t)pid;
        }

        // === Graphics/Input Syscalls ===
        case SYS_GET_FRAMEBUFFER: {
            if (!arg1) return (uint64_t)-1;

            // Check video hardware capability
            if (!security_has_capability(current_pid, CAP_HW_VIDEO)) {
                kprintf("[SYSCALL] get_framebuffer: permission denied (PID %u)\n", current_pid);
                return (uint64_t)-1;
            }

            extern uint32_t *fb_ptr;
            extern uint64_t fb_width;
            extern uint64_t fb_height;

            // Map framebuffer to user space
            uint64_t fb_user_base = FB_USER_BASE;
            uint64_t fb_size = fb_width * fb_height * 4;
            fb_size = (fb_size + 4095) & ~4095;

            uint64_t phys_base = ((uint64_t)fb_ptr) - vmm_get_hhdm_offset();

            for (uint64_t off = 0; off < fb_size; off += 4096) {
                vmm_map_user_page(fb_user_base + off, phys_base + off);
            }

            // Validate user info struct pointer
            typedef struct {
                uint64_t addr;
                uint64_t width;
                uint64_t height;
                uint64_t pitch;
                uint32_t bpp;
            } fb_info_t;

            if (!is_user_address(arg1, sizeof(fb_info_t))) {
                return (uint64_t)-1;
            }

            fb_info_t *info = (fb_info_t *)arg1;
            info->addr = fb_user_base;
            info->width = fb_width;
            info->height = fb_height;
            info->pitch = fb_width * 4;
            info->bpp = 32;

            return 0;
        }

        case SYS_GET_INPUT_EVENT: {
            typedef struct {
                uint32_t type;
                uint32_t code;
                int32_t x;
                int32_t y;
            } input_event_t;

            if (!arg1) return (uint64_t)-1;

            // Validate user event struct pointer
            if (!is_user_address(arg1, sizeof(input_event_t))) {
                return (uint64_t)-1;
            }

            extern int get_keyboard_event(uint32_t *type, uint32_t *code);
            extern int get_mouse_event(uint32_t *type, uint32_t *buttons, int32_t *x, int32_t *y);

            input_event_t *evt = (input_event_t *)arg1;

            // Prioritize keyboard
            uint32_t type, code;
            if (get_keyboard_event(&type, &code)) {
                evt->type = type;
                evt->code = code;
                return 1;
            }

            // Check mouse
            uint32_t buttons;
            int32_t mx, my;
            if (get_mouse_event(&type, &buttons, &mx, &my)) {
                evt->type = type;
                evt->code = buttons;
                evt->x = mx;
                evt->y = my;
                return 1;
            }

            return 0; // No event
        }

        // === Security Syscalls ===
        case SYS_SEC_GETCAPS: {
            security_context_t *ctx = security_get_context(current_pid);
            if (ctx) return ctx->capabilities;
            return 0;
        }

        default:
            kprintf("[SYSCALL] Unknown #%lu from PID %u\n", num, current_pid);
            return (uint64_t)-1;
    }
}
