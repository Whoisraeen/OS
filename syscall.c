#include "syscall.h"
#include "keyboard.h"
#include "mouse.h"
#include "aio.h"
#include "idt.h"
#include "console.h"
#include "serial.h"
#include "io.h"
#include "gdt.h"
#include "ipc.h"
#include "security.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "string.h"
#include "fd.h"
#include "pipe.h"
#include "signal.h"
#include "futex.h"
#include "vm_area.h"
#include "rtc.h"
#include "acpi.h"
#include "driver.h"
#include "ext2.h"

// MSR registers
#define MSR_EFER     0xC0000080
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_FMASK    0xC0000084

// EFER bits
#define EFER_SCE 0x01  // System Call Enable

#define EPERM   1
#define ENOENT  2
#define EBADF   9
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22

// External assembly handler
extern void syscall_entry(void);

// External Linux handler
extern uint64_t linux_syscall_handler(struct interrupt_frame *regs);

// Forward declaration if vmm.h fails
// int copy_from_user(void *kernel_dst, const void *user_src, size_t size);

static int local_copy_from_user(void *kernel_dst, const void *user_src, size_t size) {
    if (!is_user_address((uint64_t)user_src, size)) return -1;
    memcpy(kernel_dst, user_src, size);
    return 0;
}

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)0x13 << 48);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200);
    kprintf("[SYSCALL] Initialized\n");
}

uint64_t syscall_handler(uint64_t num, struct interrupt_frame *regs) {
    uint32_t current_pid = task_current_id();
    task_t *current_task = task_get_by_id(current_pid);

    if (!current_task) {
        return (uint64_t)-1;
    }

    // Mapping for convenience
    uint64_t arg1 = regs->rdi;
    uint64_t arg2 = regs->rsi;
    uint64_t arg3 = regs->rdx;

    if (num != 16) { // SYS_GET_INPUT_EVENT
        kprintf("[SYSCALL] PID %u called #%lu (arg1=%lx, arg2=%lx, arg3=%lx) at RIP %lx\n", 
                current_pid, num, arg1, arg2, arg3, regs->rip);
    }

    // uint64_t arg4 = regs->rcx; // Note: syscall instruction clobbers RCX with RIP!
    // But our synthesized frame in interrupts.S puts User RCX into regs->rcx.

    switch (num) {
        case SYS_EXIT:
            security_destroy_context(current_pid);
            task_exit_code((int)arg1);
            return 0;

        case SYS_WRITE: {
            if (current_pid == 2) kprintf("[SYSCALL] PID 2 SYS_WRITE len=%lu\n", arg3);

            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            
            // Limit write size to avoid large allocations
            if (arg3 > 1024 * 1024) arg3 = 1024 * 1024;
            
            void *kbuf = kmalloc((size_t)arg3);
            if (!kbuf) return (uint64_t)-ENOMEM;
            
            if (local_copy_from_user(kbuf, (void*)arg2, (size_t)arg3) < 0) {
                kfree(kbuf);
                return (uint64_t)-EFAULT;
            }

            uint64_t ret = (uint64_t)-EBADF;
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (entry) {
                 if (entry->type == FD_DEVICE && entry->dev->write) {
                     ret = entry->dev->write(entry->dev, (const uint8_t *)kbuf, (size_t)arg3);
                 } else if (entry->type == FD_FILE && entry->node) {
                     size_t written = vfs_write(entry->node, entry->offset, (size_t)arg3, (uint8_t *)kbuf);
                     entry->offset += written;
                     ret = written;
                 }
            }
            kfree(kbuf);
            if ((int64_t)ret < 0) {
                 kprintf("[SYSCALL] SYS_WRITE ret=%ld\n", (int64_t)ret);
            }
            return ret;
        }

        case SYS_READ: {
            if (!is_user_address(arg2, arg3)) return (uint64_t)-1;
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-1;
            if (entry->type == FD_DEVICE && entry->dev->read)
                return entry->dev->read(entry->dev, (uint8_t *)arg2, arg3);
            if (entry->type == FD_FILE && entry->node) {
                size_t bytes = vfs_read(entry->node, entry->offset, arg3, (uint8_t *)arg2);
                entry->offset += bytes; return bytes;
            }
            return (uint64_t)-1;
        }

        case SYS_OPEN: {
            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-1;
            vfs_node_t *node = vfs_open(path, (int)arg2);
            if (!node) return (uint64_t)-1;
            int fd = fd_alloc(current_task->fd_table);
            if (fd < 0) return (uint64_t)-1;
            fd_entry_t *entry = &current_task->fd_table->entries[fd];
            entry->type = FD_FILE; entry->node = node; entry->offset = 0; entry->flags = (uint32_t)arg2;
            if ((uint32_t)arg2 & O_APPEND) entry->offset = node->length;
            return (uint64_t)fd;
        }

        case SYS_CLOSE: {
            fd_free(current_task->fd_table, (int)arg1);
            return 0;
        }

        case SYS_BRK: {
            if (!current_task->mm) return (uint64_t)-1;
            if (arg1 == 0) return current_task->mm->brk;
            uint64_t new_brk = (arg1 + 0xFFF) & ~0xFFFULL;
            uint64_t old_brk = current_task->mm->brk;
            if (new_brk < current_task->mm->start_brk) return (uint64_t)-1;
            if (new_brk > old_brk) {
                vm_area_t *vma = vma_create(old_brk, new_brk, VMA_USER | VMA_READ | VMA_WRITE, VMA_TYPE_ANONYMOUS);
                if (vma) vma_insert(current_task->mm, vma);
            }
            current_task->mm->brk = new_brk;
            return new_brk;
        }

        case SYS_FORK: {
            return (uint64_t)task_fork((registers_t *)regs);
        }

        case SYS_YIELD:
            task_yield();
            return 0;


        case SYS_IPC_CREATE: {
            return (uint64_t)ipc_port_create(current_pid, (uint32_t)arg1);
        }

        case SYS_IPC_SEND: {
            ipc_message_t kmsg;
            if (local_copy_from_user(&kmsg, (void*)arg2, sizeof(ipc_message_t)) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)ipc_send_message((ipc_port_t)arg1, &kmsg, current_pid, (uint32_t)arg3, 0);
        }

        case SYS_IPC_RECV: {
            ipc_message_t kmsg;
            int ret = ipc_recv_message((ipc_port_t)arg1, &kmsg, current_pid, (uint32_t)arg3, 0);
            if (ret == 0) {
                if (copy_to_user((void*)arg2, &kmsg, sizeof(ipc_message_t)) < 0) return (uint64_t)-EFAULT;
            }
            return (uint64_t)ret;
        }

        case SYS_IPC_LOOKUP: {
            char name[64];
            if (copy_string_from_user(name, (const char *)arg1, sizeof(name)) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)ipc_port_lookup(name);
        }

        case SYS_IPC_REGISTER: {
            char name[64];
            if (copy_string_from_user(name, (const char *)arg2, sizeof(name)) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)ipc_port_register((ipc_port_t)arg1, name);
        }

        case SYS_IPC_SHMEM_CREATE: {
            return (uint64_t)ipc_shmem_create((size_t)arg1, current_pid, (uint32_t)arg2);
        }

        case SYS_IPC_SHMEM_MAP: {
            return (uint64_t)ipc_shmem_map((uint32_t)arg1, current_pid);
        }

        case SYS_IPC_SHMEM_UNMAP: {
            return (uint64_t)ipc_shmem_unmap((uint32_t)arg1, current_pid);
        }

        case SYS_IRQ_WAIT: {
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) return (uint64_t)-1;
            irq_register_waiter((int)arg1, current_task);
            task_block();
            return 0;
        }

        case SYS_IRQ_ACK: {
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) return (uint64_t)-1;
            // For now, identity map IRQ to PIC/IOAPIC EOI logic if needed
            // But usually the kernel handles EOI, drivers just need to clear hardware state
            return 0;
        }

        case SYS_CLOCK_GETTIME: {
            uint64_t ts[2];
            ts[0] = rtc_get_timestamp();
            ts[1] = 0; // nsec
            if (copy_to_user((void*)arg1, ts, sizeof(ts)) < 0) return (uint64_t)-1;
            return 0;
        }

        case SYS_SHUTDOWN: {
            if (!security_has_capability(current_pid, CAP_SYS_REBOOT)) return (uint64_t)-1;
            kprintf("[SYSCALL] Powering off...\n");
            // ACPI shutdown or QEMU debug exit
            outw(0x604, 0x2000); // QEMU/VirtualBox
            return 0;
        }

        case SYS_GETDENTS: {
            // arg1=fd, arg2=buf, arg3=count
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_FILE || !(entry->node->flags & VFS_DIRECTORY)) return (uint64_t)-1;
            
            // Simplified: return one entry at a time for now to keep it easy
            // We use entry->offset as the index
            vfs_node_t *node = vfs_readdir(entry->node, entry->offset);
            if (!node) return 0; // EOF
            
            // linux-style dirent64 (simplified)
            struct {
                uint64_t d_ino;
                int64_t  d_off;
                uint16_t d_reclen;
                uint8_t  d_type;
                char     d_name[256];
            } de;
            memset(&de, 0, sizeof(de));
            de.d_ino = node->inode;
            de.d_off = entry->offset + 1;
            de.d_type = (node->flags & VFS_DIRECTORY) ? 4 : 8; // DT_DIR : DT_REG
            strncpy(de.d_name, node->name, 255);
            de.d_reclen = sizeof(de); // Fix later for variable size
            
            if (copy_to_user((void*)arg2, &de, sizeof(de)) < 0) return (uint64_t)-1;
            entry->offset++;
            return (uint64_t)sizeof(de);
        }

        case SYS_IOPORT: {
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) return (uint64_t)-1;
            uint16_t port = (uint16_t)arg1;
            uint16_t val = (uint16_t)arg2;
            int write = (int)arg3;
            
            if (write) {
                if (val > 0xFF) outw(port, val);
                else outb(port, (uint8_t)val);
                return 0;
            } else {
                return (uint64_t)inb(port);
            }
        }

        case SYS_PROC_EXEC: {
            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-1;
            vfs_node_t *node = vfs_open(path, 0);
            if (!node) {
                kprintf("[SYSCALL] SYS_PROC_EXEC: Failed to open %s\n", path);
                return (uint64_t)-1;
            }
            uint8_t *buf = kmalloc(node->length);
            if (!buf) return (uint64_t)-1;
            vfs_read(node, 0, node->length, buf);
            kprintf("[SYSCALL] SYS_PROC_EXEC: Loading %s (%lu bytes)\n", path, node->length);
            int slot = task_create_user(path, buf, node->length, current_pid, ABI_NATIVE);
            kfree(buf);
            return (uint64_t)slot;
        }

        case SYS_STAT: {
            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-1;
            vfs_node_t *node = vfs_open(path, 0);
            if (!node) return (uint64_t)-ENOENT;

            struct {
                uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
                uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t st_size;
                uint64_t st_blksize; uint64_t st_blocks; uint64_t st_atime; uint64_t st_mtime;
                uint64_t st_ctime;
            } kst;
            memset(&kst, 0, sizeof(kst));
            kst.st_ino = node->inode;
            kst.st_size = node->length;
            kst.st_mode = (node->flags & VFS_DIRECTORY) ? 0040000 : 0100000; // S_IFDIR : S_IFREG
            kst.st_mode |= 0755; // Default permissions

            if (copy_to_user((void*)arg2, &kst, sizeof(kst)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }

        case SYS_WAIT: {
            int status = 0;
            int child_pid = task_wait(&status);
            if (child_pid > 0 && arg1) {
                if (copy_to_user((void*)arg1, &status, sizeof(int)) < 0) return (uint64_t)-1;
            }
            return (uint64_t)child_pid;
        }

        case SYS_SEC_GRANT: {
            uint32_t target_pid = (uint32_t)arg1;
            uint64_t caps = arg2;
            return (uint64_t)security_grant_capability(current_pid, target_pid, caps);
        }

        case SYS_GET_FRAMEBUFFER: {
            if (!security_has_capability(current_pid, CAP_HW_VIDEO)) return (uint64_t)-1;
            console_set_enabled(0);
            
            extern uint32_t *fb_ptr; 
            extern uint64_t fb_width; 
            extern uint64_t fb_height;
            
            uint64_t fb_user_base = FB_USER_BASE; 
            uint64_t fb_size = fb_width * fb_height * 4;
            fb_size = (fb_size + 4095) & ~4095;
            
            // Determine physical address (logic from vmm_init)
            uint64_t fb_virt_kernel = (uint64_t)fb_ptr;
            uint64_t fb_phys;
            
            // We need kernel_addr_request.response to check kernel range
            // For now, assume it's either in Kernel or HHDM
            // Most likely HHDM if it's large.
            if (fb_virt_kernel >= 0xffffffff80000000ULL) {
                // Kernel range (rough estimate)
                fb_phys = fb_virt_kernel - 0xffffffff80000000ULL; // Just a guess without the response struct
                // Actually, let's use the HHDM offset if it's in higher half but not kernel range
            } else {
                fb_phys = fb_virt_kernel - vmm_get_hhdm_offset();
            }
            
            // Map the pages to user space
            for (uint64_t off = 0; off < fb_size; off += 4096) {
                vmm_map_user_page(fb_user_base + off, fb_phys + off);
            }
            
            typedef struct { uint64_t addr, width, height, pitch; uint32_t bpp; } fb_info_t;
            if (!is_user_address(arg1, sizeof(fb_info_t))) return (uint64_t)-1;
            
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
                uint32_t type; // 1=Key, 2=Mouse
                uint32_t code; // Keycode or Buttons
                int32_t x;     // Mouse X
                int32_t y;     // Mouse Y
            } input_event_t;

            input_event_t *uevt = (input_event_t *)arg1;
            if (!is_user_address(arg1, sizeof(input_event_t))) return (uint64_t)-1;

            uint32_t type, code, buttons;
            int32_t x, y;

            // Check mouse first
            if (get_mouse_event(&type, &buttons, &x, &y)) {
                input_event_t kevt = { .type = type, .code = buttons, .x = x, .y = y };
                copy_to_user(uevt, &kevt, sizeof(input_event_t));
                return 1;
            }

            // Then keyboard
            if (get_keyboard_event(&type, &code)) {
                input_event_t kevt = { .type = type, .code = code, .x = 0, .y = 0 };
                copy_to_user(uevt, &kevt, sizeof(input_event_t));
                return 1;
            }

            return 0;
        }

        default:
            kprintf("[SYSCALL] Unknown #%lu from PID %u at RIP %lx\n", num, current_pid, regs->rip);
            return (uint64_t)-1;
    }
}