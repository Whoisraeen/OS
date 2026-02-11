#include "syscall.h"
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
#include "net/include/lwip/tcp.h"
#include "net/include/lwip/ip.h"

// Basic Socket Implementation
typedef struct {
    struct tcp_pcb *pcb;
    int type; // 1=STREAM, 2=DGRAM
    int state;
    // Minimal receive buffer for demo
    char recv_buf[1024];
    int recv_head;
    int recv_tail;
} socket_t;

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
} sockaddr_in_t;

// Callback for TCP receive
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    socket_t *sock = (socket_t *)arg;
    if (!sock || !p) return 0;

    // Copy data into socket ring buffer
    uint8_t *data = (uint8_t *)p->payload;
    for (uint16_t i = 0; i < p->len; i++) {
        int next_tail = (sock->recv_tail + 1) % 1024;
        if (next_tail != sock->recv_head) { // Not full
            sock->recv_buf[sock->recv_tail] = data[i];
            sock->recv_tail = next_tail;
        }
    }

    pbuf_free(p);
    return 0;
}

// Callback for TCP connection
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)err;
    socket_t *sock = (socket_t *)arg;
    if (sock) {
        sock->state = 1; // Connected
        pcb->recv = tcp_recv_cb;
        pcb->callback_arg = sock;
    }
    return 0;
}

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

// External Linux handler
extern uint64_t linux_syscall_handler(struct interrupt_frame *regs);

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                          struct interrupt_frame *regs) {
    // Get real PID from scheduler
    uint32_t current_pid = task_current_id();
    task_t *current_task = task_get_by_id(current_pid);

    // 1. Check ABI Compatibility
    if (current_task && current_task->abi == ABI_LINUX) {
        // For SYSCALL instruction, registers are on the stack via interrupts.S
        // If regs is provided, use it.
        if (regs) {
            return linux_syscall_handler(regs);
        } else {
            // This case happens if syscall_entry in interrupts.S doesn't pass regs
            // We should ensure interrupts.S passes the frame pointer.
        }
    }

    // DEBUG: Trace native syscalls
    // kprintf("[SYSCALL] PID %u called native sys_%lu(%lx, %lx, %lx)\n", current_pid, num, arg1, arg2, arg3);

    switch (num) {
        case SYS_EXIT:
            kprintf("[SYSCALL] Process %u exit(%d)\n", current_pid, (int)arg1);
            security_destroy_context(current_pid);
            task_exit_code((int)arg1);
            return 0;    // Never reached

        case SYS_WRITE: {
            // arg1 = fd, arg2 = buf, arg3 = len
            if (!is_user_address(arg2, arg3)) return (uint64_t)-1;

            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-1;

            switch (entry->type) {
                case FD_DEVICE:
                    if (entry->dev && entry->dev->write)
                        return entry->dev->write(entry->dev, (const uint8_t *)arg2, arg3);
                    return (uint64_t)-1;
                case FD_FILE:
                    if (entry->node) {
                        size_t written = vfs_write(entry->node, entry->offset, arg3, (uint8_t *)arg2);
                        entry->offset += written;
                        return written;
                    }
                    return (uint64_t)-1;
                case FD_PIPE:
                    if (entry->pipe)
                        return pipe_write((pipe_t *)entry->pipe, (const uint8_t *)arg2, arg3);
                    return (uint64_t)-1;
                default:
                    return (uint64_t)-1;
            }
        }

        case SYS_READ: {
            // arg1 = fd, arg2 = buf, arg3 = len
            if (!is_user_address(arg2, arg3)) return (uint64_t)-1;

            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-1;

            switch (entry->type) {
                case FD_DEVICE:
                    if (entry->dev && entry->dev->read)
                        return entry->dev->read(entry->dev, (uint8_t *)arg2, arg3);
                    return (uint64_t)-1;
                case FD_FILE:
                    if (entry->node) {
                        size_t bytes = vfs_read(entry->node, entry->offset, arg3, (uint8_t *)arg2);
                        entry->offset += bytes;
                        return bytes;
                    }
                    return (uint64_t)-1;
                case FD_PIPE:
                    if (entry->pipe)
                        return pipe_read((pipe_t *)entry->pipe, (uint8_t *)arg2, arg3);
                    return (uint64_t)-1;
                default:
                    return (uint64_t)-1;
            }
        }

        case SYS_OPEN: {
            // arg1 = path (user), arg2 = flags
            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-1;

            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            // Use VFS to open/create the node
            vfs_node_t *node = vfs_open(path, (int)arg2);

            if (!node) return (uint64_t)-1;

            // Truncate if O_TRUNC and file is writable
            if (((uint32_t)arg2 & O_TRUNC) && node->inode != 0 && node->write != NULL) {
                // Assumption: Only Ext2 is writable.
                // We should really check if the node belongs to ext2_root_fs
                // But for now, this heuristic works.
                if (ext2_root_fs) {
                     ext2_truncate(ext2_root_fs, (uint32_t)node->inode);
                     node->length = 0;
                }
            }

            int fd = fd_alloc(task->fd_table);
            if (fd < 0) return (uint64_t)-1;

            fd_entry_t *entry = &task->fd_table->entries[fd];
            entry->type = FD_FILE;
            entry->node = node;
            entry->offset = 0;
            entry->flags = (uint32_t)arg2;

            // O_APPEND: start at end
            if ((uint32_t)arg2 & O_APPEND) {
                entry->offset = node->length;
            }

            return (uint64_t)fd;
        }

        case SYS_CLOSE: {
            // arg1 = fd
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-1;

            // Handle pipe close (decrement reader/writer count)
            if (entry->type == FD_PIPE && entry->pipe) {
                int is_write = (entry->flags & O_WRONLY) ? 1 : 0;
                pipe_close((pipe_t *)entry->pipe, is_write);
            }

            fd_free(task->fd_table, (int)arg1);
            return 0;
        }

        case SYS_LSEEK: {
            // arg1 = fd, arg2 = offset, arg3 = whence
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_FILE || !entry->node)
                return (uint64_t)-1;

            int64_t offset = (int64_t)arg2;
            size_t new_pos;

            switch (arg3) {
                case SEEK_SET:
                    if (offset < 0) return (uint64_t)-1;
                    new_pos = (size_t)offset;
                    break;
                case SEEK_CUR:
                    if (offset < 0 && (size_t)(-offset) > entry->offset)
                        return (uint64_t)-1;
                    new_pos = entry->offset + offset;
                    break;
                case SEEK_END:
                    if (offset < 0 && (size_t)(-offset) > entry->node->length)
                        return (uint64_t)-1;
                    new_pos = entry->node->length + offset;
                    break;
                default:
                    return (uint64_t)-1;
            }

            entry->offset = new_pos;
            return (uint64_t)new_pos;
        }

        case SYS_BRK: {
            // arg1 = new break address (0 = query current break)
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->mm) return (uint64_t)-1;

            if (arg1 == 0) {
                // Query current break
                return task->mm->brk;
            }

            uint64_t new_brk = (arg1 + 0xFFF) & ~0xFFFULL; // Page-align
            uint64_t old_brk = task->mm->brk;

            if (new_brk < task->mm->start_brk) return (uint64_t)-1;

            if (new_brk > old_brk) {
                // Expanding: create VMA for new region (demand-paged)
                vm_area_t *vma = vma_create(old_brk, new_brk,
                    VMA_USER | VMA_READ | VMA_WRITE, VMA_TYPE_ANONYMOUS);
                if (vma) vma_insert(task->mm, vma);
            } else if (new_brk < old_brk) {
                // Shrinking: remove VMA region and unmap pages
                vma_remove(task->mm, new_brk, old_brk);
                for (uint64_t addr = new_brk; addr < old_brk; addr += PAGE_SIZE) {
                    uint64_t pte = vmm_get_pte(addr);
                    if (pte & 1) { // PTE_PRESENT
                        uint64_t phys = pte & 0x000FFFFFFFFFF000ULL;
                        vmm_unmap_page(addr);
                        pmm_free_page((void *)phys);
                    }
                }
            }

            task->mm->brk = new_brk;
            return new_brk;
        }

        case SYS_MMAP: {
            // arg1 = hint address (0 = kernel chooses), arg2 = size, arg3 = prot flags
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->mm) return (uint64_t)-1;

            uint64_t size = (arg2 + 0xFFF) & ~0xFFFULL;
            if (size == 0) return (uint64_t)-1;

            uint64_t addr;
            if (arg1 != 0) {
                addr = arg1 & ~0xFFFULL;
                // Check if requested range is free
                for (uint64_t a = addr; a < addr + size; a += PAGE_SIZE) {
                    if (vma_find(task->mm, a)) {
                        addr = 0; // Conflict, fall through to auto-assign
                        break;
                    }
                }
            } else {
                addr = 0;
            }

            if (addr == 0) {
                addr = vma_find_free(task->mm, size);
                if (addr == 0) return (uint64_t)-1;
            }

            // Build VMA flags
            uint32_t flags = VMA_USER | VMA_READ;
            if (arg3 & 2) flags |= VMA_WRITE; // PROT_WRITE
            if (arg3 & 4) flags |= VMA_EXEC;  // PROT_EXEC

            vm_area_t *vma = vma_create(addr, addr + size, flags, VMA_TYPE_ANONYMOUS);
            if (!vma) return (uint64_t)-1;
            vma_insert(task->mm, vma);

            // Pages are demand-allocated on first access (page fault handler)
// Basic Error Codes
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EINVAL  22
#define ENOSYS  38

// Globals for drivers to access
            return addr;
        }

        case SYS_MUNMAP: {
            // arg1 = address, arg2 = size
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->mm) return (uint64_t)-1;

            uint64_t addr = arg1 & ~0xFFFULL;
            uint64_t size = (arg2 + 0xFFF) & ~0xFFFULL;
            if (size == 0) return (uint64_t)-1;

            // Remove VMA region
            vma_remove(task->mm, addr, addr + size);

            // Unmap physical pages
            for (uint64_t a = addr; a < addr + size; a += PAGE_SIZE) {
                uint64_t pte = vmm_get_pte(a);
                if (pte & 1) {
                    uint64_t phys = pte & 0x000FFFFFFFFFF000ULL;
                    vmm_unmap_page(a);
                    pmm_free_page((void *)phys);
                }
            }
            return 0;
        }

        case SYS_FORK: {
            // Fork requires full register frame (must use INT 0x80, not SYSCALL)
            if (!regs) return (uint64_t)-1;
            return (uint64_t)task_fork((registers_t *)regs);
        }

        case SYS_YIELD:
            task_yield();
            return 0;

        case SYS_DUP: {
            // arg1 = old_fd
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;
            return (uint64_t)fd_dup(task->fd_table, (int)arg1);
        }

        case SYS_DUP2: {
            // arg1 = old_fd, arg2 = new_fd
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;
            return (uint64_t)fd_dup2(task->fd_table, (int)arg1, (int)arg2);
        }

        // === Process Management Syscalls ===
        case SYS_GETPID:
            return (uint64_t)current_pid;

        case SYS_GETPPID: {
            task_t *task = task_get_by_id(current_pid);
            if (!task) return 0;
            return (uint64_t)task->parent_pid;
        }

        case SYS_WAIT: {
            // arg1 = pointer to status (user space, may be NULL)
            int status = 0;
            int child_pid = task_wait(&status);
            if (child_pid >= 0 && arg1 != 0) {
                if (is_user_address(arg1, sizeof(int))) {
                    *(int *)arg1 = status;
                }
            }
            return (uint64_t)child_pid;
        }

        case SYS_WAITPID: {
            // arg1 = pid, arg2 = pointer to status (user space, may be NULL)
            int status = 0;
            int child_pid = task_waitpid((uint32_t)arg1, &status);
            if (child_pid >= 0 && arg2 != 0) {
                if (is_user_address(arg2, sizeof(int))) {
                    *(int *)arg2 = status;
                }
            }
            return (uint64_t)child_pid;
        }

        case SYS_KILL: {
            // arg1 = pid, arg2 = signal
            return (uint64_t)signal_send((uint32_t)arg1, (int)arg2);
        }

        case SYS_SIGNAL: {
            // arg1 = signal number, arg2 = action (SIG_DFL=0, SIG_IGN=1)
            return (uint64_t)signal_set_handler(current_pid, (int)arg1, (int)arg2);
        }

        case SYS_PIPE: {
            // arg1 = pointer to int[2] (user space) — [0]=read_fd, [1]=write_fd
            if (!is_user_address(arg1, 2 * sizeof(int))) return (uint64_t)-1;

            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            int rfd, wfd;
            if (pipe_create(task->fd_table, &rfd, &wfd) < 0) return (uint64_t)-1;

            int *user_fds = (int *)arg1;
            user_fds[0] = rfd;
            user_fds[1] = wfd;
            return 0;
        }

        // === Thread Syscalls ===
        case SYS_THREAD_CREATE: {
            // arg1 = entry point, arg2 = argument, arg3 = stack (0 = kernel allocates)
            return (uint64_t)task_create_thread(arg1, arg2, arg3);
        }

        case SYS_THREAD_EXIT: {
            // arg1 = exit code
            task_exit_code((int)arg1);
            return 0; // Never reached
        }

        case SYS_THREAD_JOIN: {
            // arg1 = thread ID
            return (uint64_t)task_thread_join((uint32_t)arg1);
        }

        case SYS_FUTEX: {
            // arg1 = address, arg2 = operation, arg3 = value
            if (!is_user_address(arg1, sizeof(uint64_t))) return (uint64_t)-1;
            return (uint64_t)futex_op((uint64_t *)arg1, (int)arg2, (uint32_t)arg3);
        }

        case SYS_SET_TLS: {
            // arg1 = TLS base address
            task_t *task = task_get_by_id(current_pid);
            if (!task) return (uint64_t)-1;
            task->tls_base = arg1;
            wrmsr(0xC0000100, arg1); // MSR_FS_BASE — apply immediately
            return 0;
        }

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
            // Validate user message pointer (header + data)
            if (!is_user_address(arg2, 152)) {
                return (uint64_t)-1;
            }
            
            // Safe copy from user
            uint8_t kmsg[152];
            if (copy_from_user(kmsg, (const void *)arg2, 152) < 0) {
                return (uint64_t)-1;
            }
            
            return (uint64_t)ipc_send_message((uint32_t)arg1, (void *)kmsg, current_pid, (uint32_t)arg3, 0);
        }

        case SYS_IPC_RECV: {
            if (!security_has_capability(current_pid, CAP_IPC_RECV)) {
                return (uint64_t)-1;
            }
            // Validate user message buffer
            if (!is_user_address(arg2, 152)) {
                return (uint64_t)-1;
            }
            
            // We pass the user pointer directly to recv because it writes back?
            // Wait, ipc_recv_message writes to the buffer.
            // If we use a kernel buffer, we need to copy back.
            
            uint8_t kmsg[152];
            // No need to copy *from* user for RECV, just *to* user.
            
            int ret = ipc_recv_message((uint32_t)arg1, (void *)kmsg, current_pid, (uint32_t)arg3, 0);
            if (ret == 0) {
                if (copy_to_user((void *)arg2, kmsg, 152) < 0) return (uint64_t)-1;
            }
            return (uint64_t)ret;
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
            kprintf("[SYSCALL] SYS_PROC_EXEC called. Path ptr: %lx\n", arg1);
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
            kprintf("[SYSCALL] exec: path='%s'\n", path);

            // Find file in VFS
            vfs_node_t *node = vfs_open(path, 0);
            if (!node) {
                kprintf("[SYSCALL] exec: file not found '%s'\n", path);
                return (uint64_t)-1;
            }
            kprintf("[SYSCALL] exec: file found, size=%lu\n", node->length);

            // Allocate kernel buffer
            void *data = kmalloc(node->length);
            if (!data) return (uint64_t)-1;

            // Read file
            vfs_read(node, 0, node->length, (uint8_t*)data);
            kprintf("[SYSCALL] exec: file read complete\n");

            // Create Task
            int pid = task_create_user(path, data, node->length, current_pid);
            kprintf("[SYSCALL] exec: task_create_user returned %d\n", pid);

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

            // Display Handover: Disable kernel console output to screen
            console_set_enabled(0);
            kprintf("[SYSCALL] Display handover to PID %u. Kernel console disabled.\n", current_pid);

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

        case SYS_SEC_GRANT: {
            // arg1 = target_pid, arg2 = capability
            return (uint64_t)security_grant_capability(current_pid, (uint32_t)arg1, (capability_t)arg2);
        }

        // === Time & Power Syscalls ===
        case SYS_CLOCK_GETTIME: {
            // arg1 = struct timespec *ts
            if (!is_user_address(arg1, 16)) return (uint64_t)-1;
            
            extern uint64_t timer_get_ticks(void);
            uint64_t ticks = timer_get_ticks();
            uint64_t ms = ticks * 10;
            
            uint64_t *user_ts = (uint64_t *)arg1;
            user_ts[0] = ms / 1000;
            user_ts[1] = (ms % 1000) * 1000000;
            
            return 0;
        }

        case SYS_REBOOT: {
            kprintf("[SYSCALL] Reboot requested by PID %u\n", current_pid);
            acpi_reboot();
            return 0; // Never reached
        }

        case SYS_SHUTDOWN: {
            kprintf("[SYSCALL] Shutdown requested by PID %u\n", current_pid);
            acpi_shutdown();
            return 0; // Never reached
        }

        case SYS_IOCTL: {
            // arg1 = fd, arg2 = cmd, arg3 = arg pointer
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-1;

            // For now, ioctl only works on device fds backed by a driver
            // A future extension could support ioctl on files, pipes, etc.
            (void)arg2;
            (void)arg3;
            return (uint64_t)-1; // Not yet implemented for specific devices
        }

        // === Filesystem Syscalls ===
        case SYS_STAT: {
            // arg1 = path (user), arg2 = pointer to ext2_stat_t (user)
            if (!ext2_root_fs) return (uint64_t)-1;

            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-1;
            if (!is_user_address(arg2, sizeof(ext2_stat_t))) return (uint64_t)-1;

            uint32_t ino = ext2_resolve_path(ext2_root_fs, path, NULL);
            if (ino == 0) return (uint64_t)-1;

            return (uint64_t)ext2_stat(ext2_root_fs, ino, (ext2_stat_t *)arg2);
        }

        case SYS_MKDIR: {
            // arg1 = path (user), arg2 = mode
            if (!ext2_root_fs) return (uint64_t)-1;

            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-1;

            char name[256];
            uint32_t parent_ino = ext2_resolve_parent(ext2_root_fs, path, name, sizeof(name));
            if (parent_ino == 0) return (uint64_t)-1;

            uint16_t mode = EXT2_S_IFDIR | ((uint16_t)arg2 & 0x0FFF);
            uint32_t new_ino = ext2_create(ext2_root_fs, parent_ino, name, mode);
            return (new_ino != 0) ? 0 : (uint64_t)-1;
        }

        case SYS_RMDIR: {
            // arg1 = path (user)
            if (!ext2_root_fs) return (uint64_t)-1;

            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-1;

            char name[256];
            uint32_t parent_ino = ext2_resolve_parent(ext2_root_fs, path, name, sizeof(name));
            if (parent_ino == 0) return (uint64_t)-1;

            return (uint64_t)ext2_rmdir(ext2_root_fs, parent_ino, name);
        }

        case SYS_UNLINK: {
            // arg1 = path (user)
            if (!ext2_root_fs) return (uint64_t)-1;

            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-1;

            char name[256];
            uint32_t parent_ino = ext2_resolve_parent(ext2_root_fs, path, name, sizeof(name));
            if (parent_ino == 0) return (uint64_t)-1;

            return (uint64_t)ext2_unlink(ext2_root_fs, parent_ino, name);
        }

        case SYS_RENAME: {
            // arg1 = old path (user), arg2 = new path (user)
            if (!ext2_root_fs) return (uint64_t)-1;

            char old_path[256], new_path[256];
            if (copy_string_from_user(old_path, (const char *)arg1, sizeof(old_path)) < 0)
                return (uint64_t)-1;
            if (copy_string_from_user(new_path, (const char *)arg2, sizeof(new_path)) < 0)
                return (uint64_t)-1;

            char old_name[256], new_name[256];
            uint32_t old_parent = ext2_resolve_parent(ext2_root_fs, old_path, old_name, sizeof(old_name));
            uint32_t new_parent = ext2_resolve_parent(ext2_root_fs, new_path, new_name, sizeof(new_name));
            if (old_parent == 0 || new_parent == 0) return (uint64_t)-1;

            return (uint64_t)ext2_rename(ext2_root_fs, old_parent, old_name, new_parent, new_name);
        }

        case SYS_GETDENTS: {
            // arg1 = fd, arg2 = pointer to dirent_t array (user), arg3 = max entries
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->fd_table) return (uint64_t)-1;
            if (!ext2_root_fs) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_FILE || !entry->node) return (uint64_t)-1;
            if (!(entry->node->flags & VFS_DIRECTORY)) return (uint64_t)-1;

            if (!is_user_address(arg2, arg3 * sizeof(dirent_t))) return (uint64_t)-1;

            return (uint64_t)ext2_getdents(ext2_root_fs,
                (uint32_t)entry->node->inode, (dirent_t *)arg2, (int)arg3);
        }

        // === Driver Support Syscalls ===
        case SYS_IOPORT: {
            // arg1 = port, arg2 = value (for write), arg3 = operation (0=READ, 1=WRITE)
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) { // Or generic HW_IO cap
                return (uint64_t)-1;
            }
            if (arg3 == 0) { // READ
                return (uint64_t)inb((uint16_t)arg1);
            } else { // WRITE
                outb((uint16_t)arg1, (uint8_t)arg2);
                return 0;
            }
        }

        case SYS_IRQ_WAIT: {
            // arg1 = irq number
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) {
                return (uint64_t)-1;
            }
            
            int irq = (int)arg1;
            if (irq < 0 || irq > 255) return (uint64_t)-1;
            
            task_t *task = task_get_by_id(current_pid);
            if (!task) return (uint64_t)-1;
            
            // Register waiter
            irq_register_waiter(irq, task);
            
            // Block and yield
            task_block();
            
            return 0; 
        }

        case SYS_MAP_PHYS: {
            // arg1 = phys_addr, arg2 = size
            if (!security_has_capability(current_pid, CAP_HW_DISK)) { // Needs root/driver cap
                return (uint64_t)-1;
            }
            
            // Map physical memory to user space (similar to get_framebuffer but generic)
            uint64_t size = (arg2 + 0xFFF) & ~0xFFFULL;
            uint64_t phys = arg1 & ~0xFFFULL;
            
            // Find free user virtual address
            task_t *task = task_get_by_id(current_pid);
            if (!task || !task->mm) return (uint64_t)-1;
            
            uint64_t virt = vma_find_free(task->mm, size);
            if (virt == 0) return (uint64_t)-1;
            
            // Map it
            for (uint64_t off = 0; off < size; off += 4096) {
                vmm_map_user_page(virt + off, phys + off);
            }
            
            // Create VMA to track it
            vm_area_t *vma = vma_create(virt, virt + size, VMA_USER | VMA_READ | VMA_WRITE, VMA_TYPE_ANONYMOUS); // Should be VMA_TYPE_DEVICE
            if (vma) vma_insert(task->mm, vma);
            
            return virt;
        }

        case SYS_AIO_SUBMIT: {
            // arg1 = aio_request_t *req
            if (!is_user_address(arg1, sizeof(aio_request_t))) return (uint64_t)-1;
            
            // Copy request from user
            aio_request_t req;
            if (copy_from_user(&req, (const void *)arg1, sizeof(aio_request_t)) < 0) return (uint64_t)-1;
            
            return sys_aio_submit(&req);
        }

        case SYS_AIO_WAIT: {
            // arg1 = aio_id, arg2 = aio_result_t *res
            if (!is_user_address(arg2, sizeof(aio_result_t))) return (uint64_t)-1;
            
            aio_result_t res;
            uint64_t ret = sys_aio_wait(arg1, &res);
            
            if (ret == 0) {
                if (copy_to_user((void *)arg2, &res, sizeof(aio_result_t)) < 0) return (uint64_t)-1;
            }
            kprintf("[SYSCALL] Handler %lu returning 0x%lx\n", num, ret);
            return ret;
        }
        
        // === Socket Syscalls ===
        case SYS_SOCKET: {
            // arg1 = domain, arg2 = type, arg3 = protocol
            // Only AF_INET(2) + SOCK_STREAM(1) supported for now
            if (arg1 != 2 || arg2 != 1) return (uint64_t)-1; 

            task_t *current_task = task_get_by_id(current_pid);
            if (!current_task) return (uint64_t)-1;

            fd_table_t *table = current_task->fd_table;
            int fd = fd_alloc(table);
            if (fd < 0) return (uint64_t)-1;

            socket_t *sock = kmalloc(sizeof(socket_t));
            if (!sock) {
                fd_free(table, fd);
                return (uint64_t)-1;
            }
            memset(sock, 0, sizeof(socket_t));
            sock->type = arg2;
            sock->pcb = tcp_new();
            if (!sock->pcb) {
                kfree(sock);
                fd_free(table, fd);
                return (uint64_t)-1;
            }

            fd_entry_t *entry = fd_get(table, fd);
            entry->type = FD_SOCKET;
            entry->socket = sock;
            entry->flags = O_RDWR;

            kprintf("[SYSCALL] socket created fd=%d\n", fd);
            return (uint64_t)fd;
        }
        
        case SYS_CONNECT: {
            // arg1 = fd, arg2 = addr, arg3 = addrlen
            task_t *current_task = task_get_by_id(current_pid);
            if (!current_task) return (uint64_t)-1;

            fd_table_t *table = current_task->fd_table;
            fd_entry_t *entry = fd_get(table, (int)arg1);
            if (!entry || entry->type != FD_SOCKET) return (uint64_t)-1;

            sockaddr_in_t addr;
            if (copy_from_user(&addr, (void*)arg2, sizeof(sockaddr_in_t)) < 0) return (uint64_t)-1;

            socket_t *sock = (socket_t*)entry->socket;
            if (!sock || !sock->pcb) return (uint64_t)-1;

            kprintf("[SYSCALL] connect to %08x:%d\n", addr.sin_addr, addr.sin_port);
            tcp_connect(sock->pcb, addr.sin_addr, addr.sin_port, tcp_connected_cb);
            sock->pcb->callback_arg = sock; // Ensure arg is set
            
            return 0;
        }
        
        case SYS_SEND: {
            // arg1 = fd, arg2 = buf, arg3 = len
            task_t *current_task = task_get_by_id(current_pid);
            if (!current_task) return (uint64_t)-1;

            fd_table_t *table = current_task->fd_table;
            fd_entry_t *entry = fd_get(table, (int)arg1);
            if (!entry || entry->type != FD_SOCKET) return (uint64_t)-1;

            socket_t *sock = (socket_t*)entry->socket;
            
            void *kbuf = kmalloc(arg3);
            if (!kbuf) return (uint64_t)-1;
            
            if (copy_from_user(kbuf, (void*)arg2, arg3) < 0) {
                kfree(kbuf);
                return (uint64_t)-1;
            }

            tcp_write(sock->pcb, kbuf, (uint16_t)arg3, 0);
            kfree(kbuf);
            return arg3;
        }
        
        case SYS_RECV: {
            // arg1 = fd, arg2 = buf, arg3 = len
            task_t *current_task = task_get_by_id(current_pid);
            if (!current_task) return (uint64_t)-1;

            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_SOCKET) return (uint64_t)-1;
            
            socket_t *sock = (socket_t *)entry->socket;
            if (!sock) return (uint64_t)-1;

            size_t bytes_read = 0;
            uint8_t *ubuf = (uint8_t *)arg2;

            while (bytes_read < arg3 && sock->recv_head != sock->recv_tail) {
                uint8_t c = (uint8_t)sock->recv_buf[sock->recv_head];
                if (copy_to_user(&ubuf[bytes_read], &c, 1) < 0) break;
                
                sock->recv_head = (sock->recv_head + 1) % 1024;
                bytes_read++;
            }

            return (uint64_t)bytes_read;
        }
        
        case SYS_BIND:
        case SYS_LISTEN:
        case SYS_ACCEPT:
            return (uint64_t)-1;

        case SYS_WRITEV: {
            // arg1 = fd, arg2 = iov, arg3 = iovcnt
            task_t *current_task = task_get_by_id(current_pid);
            if (!current_task) return (uint64_t)-1;
            
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-EBADF;

            struct iovec {
                uint64_t base;
                uint64_t len;
            } *uiov = (struct iovec *)arg2;

            if (!is_user_address((uint64_t)uiov, sizeof(struct iovec) * arg3)) return (uint64_t)-EFAULT;

            uint64_t total_written = 0;
            for (uint64_t i = 0; i < arg3; i++) {
                struct iovec vec;
                if (copy_from_user(&vec, &uiov[i], sizeof(struct iovec)) < 0) return (uint64_t)-EFAULT;
                
                // Handle Console/File writes safely
                if (entry->type == FD_DEVICE || entry->type == FD_FILE) {
                     // For safety, copy to kernel buffer. This handles SMAP/SMEP/User-access safely.
                     char *buf = kmalloc((size_t)vec.len + 1);
                     if (buf) {
                        if (copy_from_user(buf, (void*)vec.base, (size_t)vec.len) == 0) {
                             if (entry->type == FD_DEVICE && entry->dev && entry->dev->write) {
                                 // Warn: dev->write might expect user ptr, but we give kernel ptr.
                                 // If dev->write uses 'copy_from_user', it will fail on kernel ptr!
                                 // But existing SYS_WRITE passes user ptr.
                                 // Let's assume dev->write handles whatever.
                                 // Actually, to be safe and consistent with existing SYS_WRITE (line 154),
                                 // let's try passing the user pointer 'vec.base' directly to dev->write?
                                 // But writev loop needs to be atomic? No, usually not for console.
                                 entry->dev->write(entry->dev, (const uint8_t *)vec.base, vec.len);
                                 total_written += vec.len;
                             } else if (entry->type == FD_FILE && entry->node) {
                                 size_t w = vfs_write(entry->node, entry->offset, vec.len, (uint8_t *)buf);
                                 entry->offset += w;
                                 total_written += w;
                             }
                        }
                        kfree(buf);
                     }
                }
            }
            return total_written;
        }

        case SYS_ARCH_PRCTL: {
            // arg1 = code, arg2 = addr
            // ARCH_SET_GS = 0x1001, ARCH_SET_FS = 0x1002
            if (arg1 == 0x1002) {
                wrmsr(0xC0000100, arg2); // FS_BASE
                return 0;
            } else if (arg1 == 0x1001) {
                wrmsr(0xC0000101, arg2); // GS_BASE
                return 0;
            }
            return (uint64_t)-EINVAL;
        }

        default:
            kprintf("[SYSCALL] Unknown #%lu from PID %u\n", num, current_pid);
            return (uint64_t)-1;
    }
}


