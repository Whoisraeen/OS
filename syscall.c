#include "syscall.h"
#include "console.h"
#include "serial.h"
#include "io.h"
#include "gdt.h" // For GDT_KERNEL_CODE
#include "ipc.h"
#include "security.h"
#include "sched.h"
#include "vmm.h"

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
        
        // === Graphics/Input Syscalls ===
        case SYS_GET_FRAMEBUFFER: {
            // arg1 = struct fb_info *info
            if (!arg1) return -1;
            
            extern uint32_t *fb_ptr;
            extern uint64_t fb_width;
            extern uint64_t fb_height;
            
            // Map framebuffer to user space
            // For simplicity, we map it at 0x800000000 (32GB mark)
            // Ideally we should find a free region.
            // CAUTION: 0x800000000 might be in kernel space depending on layout?
            // User space is typically lower half (0 to 0x00007FFFFFFFFFFF).
            // 0x800000000 is 34GB, which is well within User space.
            uint64_t fb_user_base = 0x800000000;
            uint64_t fb_size = fb_width * fb_height * 4;
            // Align size to page
            fb_size = (fb_size + 4095) & ~4095;
            
            uint64_t phys_base = ((uint64_t)fb_ptr) - vmm_get_hhdm_offset();
            
            for (uint64_t off = 0; off < fb_size; off += 4096) {
                vmm_map_user_page(fb_user_base + off, phys_base + off);
            }
            
            // Fill info struct
            typedef struct {
                uint64_t addr;
                uint64_t width;
                uint64_t height;
                uint64_t pitch;
                uint32_t bpp;
            } fb_info_t;
            
            fb_info_t *info = (fb_info_t *)arg1;
            info->addr = fb_user_base;
            info->width = fb_width;
            info->height = fb_height;
            info->pitch = fb_width * 4;
            info->bpp = 32;
            
            return 0;
        }
        
        case SYS_GET_INPUT_EVENT: {
            // arg1 = struct input_event *event
            // Simple non-blocking event queue
            typedef struct {
                uint32_t type; // 1=key, 2=mouse
                uint32_t code; // key: scancode/char, mouse: buttons
                int32_t x;     // mouse x
                int32_t y;     // mouse y
            } input_event_t;
            
            if (!arg1) return -1;
            
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
