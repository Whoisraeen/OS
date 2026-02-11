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
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

// MSR registers
#define MSR_EFER     0xC0000080
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_FMASK    0xC0000084

// EFER bits
#define EFER_SCE 0x01  // System Call Enable

// External assembly handler
extern void syscall_entry(void);

// External Linux handler
extern uint64_t linux_syscall_handler(struct interrupt_frame *regs);

// Basic Socket Implementation (moved from internal switch)
typedef struct {
    struct tcp_pcb *pcb;
    int type; // 1=STREAM, 2=DGRAM
    int state;
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

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err; socket_t *sock = (socket_t *)arg; if (!sock || !p) return ERR_OK;
    uint8_t *data = (uint8_t *)p->payload;
    for (uint16_t i = 0; i < p->len; i++) {
        int next_tail = (sock->recv_tail + 1) % 1024;
        if (next_tail != sock->recv_head) {
            sock->recv_buf[sock->recv_tail] = data[i];
            sock->recv_tail = next_tail;
        }
    }
    pbuf_free(p); return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)err; socket_t *sock = (socket_t *)arg;
    if (sock) { sock->state = 1; pcb->recv = tcp_recv_cb; pcb->callback_arg = sock; }
    return ERR_OK;
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

    // kprintf("[SYSCALL] PID %u called #%lu (arg1=%lx, arg2=%lx, arg3=%lx) at RIP %lx\n", 
    //        current_pid, num, regs->rdi, regs->rsi, regs->rdx, regs->rip);

    if (current_task->abi == ABI_LINUX) {
        return linux_syscall_handler(regs);
    }

    // Mapping for convenience
    uint64_t arg1 = regs->rdi;
    uint64_t arg2 = regs->rsi;
    uint64_t arg3 = regs->rdx;
    // uint64_t arg4 = regs->rcx; // Note: syscall instruction clobbers RCX with RIP!
    // But our synthesized frame in interrupts.S puts User RCX into regs->rcx.

    switch (num) {
        case SYS_EXIT:
            security_destroy_context(current_pid);
            task_exit_code((int)arg1);
            return 0;

        case SYS_WRITE: {
            if (!is_user_address(arg2, arg3)) return (uint64_t)-1;
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-1;
            if (entry->type == FD_DEVICE && entry->dev->write)
                return entry->dev->write(entry->dev, (const uint8_t *)arg2, arg3);
            if (entry->type == FD_FILE && entry->node) {
                size_t written = vfs_write(entry->node, entry->offset, arg3, (uint8_t *)arg2);
                entry->offset += written; return written;
            }
            return (uint64_t)-1;
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
            extern uint32_t *fb_ptr; extern uint64_t fb_width; extern uint64_t fb_height;
            uint64_t fb_user_base = FB_USER_BASE; uint64_t fb_size = fb_width * fb_height * 4;
            fb_size = (fb_size + 4095) & ~4095;
            uint64_t phys_base = ((uint64_t)fb_ptr) - vmm_get_hhdm_offset();
            for (uint64_t off = 0; off < fb_size; off += 4096) vmm_map_user_page(fb_user_base + off, phys_base + off);
            typedef struct { uint64_t addr, width, height, pitch; uint32_t bpp; } fb_info_t;
            if (!is_user_address(arg1, sizeof(fb_info_t))) return (uint64_t)-1;
            fb_info_t *info = (fb_info_t *)arg1;
            info->addr = fb_user_base; info->width = fb_width; info->height = fb_height; info->pitch = fb_width * 4; info->bpp = 32;
            return 0;
        }

        case SYS_SOCKET: {
            if (arg1 != 2 || arg2 != 1) return (uint64_t)-1;
            int fd = fd_alloc(current_task->fd_table);
            if (fd < 0) return (uint64_t)-1;
            socket_t *sock = kmalloc(sizeof(socket_t));
            if (!sock) { fd_free(current_task->fd_table, fd); return (uint64_t)-1; }
            memset(sock, 0, sizeof(socket_t));
            sock->type = arg2; sock->pcb = tcp_new();
            if (!sock->pcb) { kfree(sock); fd_free(current_task->fd_table, fd); return (uint64_t)-1; }
            fd_entry_t *entry = fd_get(current_task->fd_table, fd);
            entry->type = FD_SOCKET; entry->socket = sock; entry->flags = O_RDWR;
            return (uint64_t)fd;
        }

        case SYS_CONNECT: {
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_SOCKET) return (uint64_t)-1;
            sockaddr_in_t addr;
            if (copy_from_user(&addr, (void*)arg2, sizeof(sockaddr_in_t)) < 0) return (uint64_t)-1;
            socket_t *sock = (socket_t*)entry->socket;
            tcp_connect(sock->pcb, addr.sin_addr, addr.sin_port, tcp_connected_cb);
            sock->pcb->callback_arg = sock;
            return 0;
        }

        case SYS_SEND: {
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_SOCKET) return (uint64_t)-1;
            socket_t *sock = (socket_t*)entry->socket;
            void *kbuf = kmalloc(arg3); if (!kbuf) return (uint64_t)-1;
            if (copy_from_user(kbuf, (void*)arg2, arg3) < 0) { kfree(kbuf); return (uint64_t)-1; }
            tcp_write(sock->pcb, kbuf, (uint16_t)arg3, 0); kfree(kbuf);
            return arg3;
        }

        case SYS_RECV: {
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_SOCKET) return (uint64_t)-1;
            socket_t *sock = (socket_t *)entry->socket;
            size_t bytes_read = 0; uint8_t *ubuf = (uint8_t *)arg2;
            while (bytes_read < arg3 && sock->recv_head != sock->recv_tail) {
                uint8_t c = (uint8_t)sock->recv_buf[sock->recv_head];
                if (copy_to_user(&ubuf[bytes_read], &c, 1) < 0) break;
                sock->recv_head = (sock->recv_head + 1) % 1024; bytes_read++;
            }
            return (uint64_t)bytes_read;
        }

        case SYS_GET_INPUT_EVENT: {
            // Stub: return 0 (no event available)
            return 0;
        }

        default:
            kprintf("[SYSCALL] Unknown #%lu from PID %u at RIP %lx\n", num, current_pid, regs->rip);
            return (uint64_t)-1;
    }
}