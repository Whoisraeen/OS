#include "syscall.h"
#include "net/net.h"
#include "keyboard.h"
#include "mouse.h"
#include "drivers/usb/hid_gamepad.h"
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
#include "timer.h"
#include "signal.h"
#include "pty.h"

// MSR registers
#define MSR_EFER     0xC0000080
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_FMASK    0xC0000084
#define MSR_FS_BASE  0xC0000100
#define MSR_GS_BASE  0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

// EFER bits
#define EFER_SCE 0x01  // System Call Enable

#define EPERM    1
#define ENOENT   2
#define EBADF    9
#define ECHILD  10
#define ENOMEM  12
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EINVAL  22
#define EMFILE  24
#define EPIPE   32
#define ESPIPE  29
#define EACCES  13
#define ERANGE  34
#define ENOSYS  38
#define ENOTEMPTY 39
#define ENOTTY  25
#define EISDIR  21
#define ENOSPC  28
#define ELOOP   40
#define ENXIO    6

/* mmap protection flags (match Linux) */
#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

/* mmap flags (match Linux x86-64) */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        0x20
#define MAP_GROWSDOWN   0x0100
#define MAP_POPULATE    0x08000
#define MAP_HUGETLB     0x40000

/* AT_FDCWD for openat/fstatat family */
#define AT_FDCWD  (-100)

// External assembly handler
extern void syscall_entry(void);

// External Linux handler
extern uint64_t linux_syscall_handler(struct interrupt_frame *regs);

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)0x13 << 48);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200);
    kprintf("[SYSCALL] Initialized\n");
}

uint64_t native_syscall_handler(uint64_t num, struct interrupt_frame *regs) {
    uint32_t current_pid = task_current_id();
    task_t *current_task = task_get_by_id(current_pid);

    if (!current_task) {
        return (uint64_t)-1;
    }

    // Mapping for convenience
    // Linux ABI: RDI, RSI, RDX, R10, R8, R9
    uint64_t arg1 = regs->rdi;
    uint64_t arg2 = regs->rsi;
    uint64_t arg3 = regs->rdx;
    uint64_t arg4 = regs->r10;
    uint64_t arg5 = regs->r8; 
    uint64_t arg6 = regs->r9;

    (void)arg4; (void)arg5; (void)arg6;

    if (num != SYS_GET_INPUT_EVENT && num != SYS_WRITE && num != SYS_SCHED_YIELD && num != SYS_POLL) {
        kprintf("[SYSCALL] PID %u called #%lu (arg1=%lx, arg2=%lx, arg3=%lx) at RIP %lx\n", 
                current_pid, num, arg1, arg2, arg3, regs->rip);
    }

    switch (num) {
        case SYS_EXIT: // 60 — exit this thread only
            security_destroy_context(current_pid);
            task_exit_code((int)arg1);
            return 0;

        case 231: // SYS_EXIT_GROUP — kill all threads in the process group
            security_destroy_context(current_pid);
            task_exit_group((int)arg1);
            return 0;

        case SYS_WRITE: { // 1
            if (arg3 > 1024 * 1024) arg3 = 1024 * 1024;
            void *kbuf = kmalloc((size_t)arg3);
            if (!kbuf) return (uint64_t)-ENOMEM;
            
            if (copy_from_user(kbuf, (void*)arg2, (size_t)arg3) < 0) {
                kfree(kbuf);
                return (uint64_t)-EFAULT;
            }

            uint64_t ret = (uint64_t)-EBADF;
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (entry) {
                if (entry->type == FD_DEVICE && entry->dev && entry->dev->write) {
                    ret = entry->dev->write(entry->dev, (const uint8_t *)kbuf, (size_t)arg3);
                } else if (entry->type == FD_FILE && entry->node) {
                    size_t written = vfs_write(entry->node, entry->offset, (size_t)arg3, (uint8_t *)kbuf);
                    entry->offset += written;
                    ret = written;
                } else if (entry->type == FD_PIPE && entry->pipe) {
                    size_t w = pipe_write((pipe_t *)entry->pipe, (const uint8_t *)kbuf, (size_t)arg3);
                    ret = (w == (size_t)-1) ? (uint64_t)-EPIPE : w;
                } else if (entry->type == FD_SOCKET && entry->socket) {
                    ret = (uint64_t)net_socket_send((net_socket_t *)entry->socket,
                                                    kbuf, (int)arg3);
                } else if (entry->type == FD_PTY_MASTER && entry->pipe) {
                    ret = pty_master_write((pty_t *)entry->pipe, (const uint8_t *)kbuf, (size_t)arg3);
                } else if (entry->type == FD_PTY_SLAVE && entry->pipe) {
                    ret = pty_slave_write((pty_t *)entry->pipe, (const uint8_t *)kbuf, (size_t)arg3);
                }
            }
            kfree(kbuf);
            return ret;
        }

        case SYS_READ: { // 0
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry) return (uint64_t)-EBADF;
            if (arg3 == 0) return 0;
            if (arg3 > 1024 * 1024) arg3 = 1024 * 1024; // cap at 1MB

            uint8_t *kbuf = kmalloc((size_t)arg3);
            if (!kbuf) return (uint64_t)-ENOMEM;

            int64_t r = (int64_t)-EBADF;
            if (entry->type == FD_DEVICE && entry->dev && entry->dev->read)
                r = (int64_t)entry->dev->read(entry->dev, kbuf, arg3);
            else if (entry->type == FD_FILE && entry->node) {
                r = (int64_t)vfs_read(entry->node, entry->offset, arg3, kbuf);
                if (r > 0) entry->offset += (size_t)r;
            }
            else if (entry->type == FD_PIPE && entry->pipe)
                r = (int64_t)pipe_read((pipe_t *)entry->pipe, kbuf, arg3);
            else if (entry->type == FD_SOCKET && entry->socket)
                r = (int64_t)net_socket_recv((net_socket_t *)entry->socket, kbuf, (int)arg3, 0);
            else if (entry->type == FD_PTY_MASTER && entry->pipe)
                r = (int64_t)pty_master_read((pty_t *)entry->pipe, kbuf, arg3);
            else if (entry->type == FD_PTY_SLAVE && entry->pipe)
                r = (int64_t)pty_slave_read((pty_t *)entry->pipe, kbuf, arg3);

            if (r > 0) copy_to_user((void *)arg2, kbuf, (size_t)r);
            kfree(kbuf);
            return (uint64_t)r;
        }

        case SYS_OPEN: { // 2
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));

            /* /dev/ptmx — allocate a new PTY master */
            if (strcmp(abs, "/dev/ptmx") == 0) {
                pty_t *pty = pty_alloc();
                if (!pty) return (uint64_t)-ENOMEM;
                int fd = fd_alloc(current_task->fd_table);
                if (fd < 0) return (uint64_t)-ENOMEM;
                fd_entry_t *e = &current_task->fd_table->entries[fd];
                e->type  = FD_PTY_MASTER;
                e->flags = (uint32_t)arg2;
                e->pipe  = pty;
                e->offset = 0;
                return (uint64_t)fd;
            }

            /* /dev/pts/N — open PTY slave */
            if (strncmp(abs, "/dev/pts/", 9) == 0) {
                int idx = 0;
                const char *np = abs + 9;
                while (*np >= '0' && *np <= '9') idx = idx * 10 + (*np++ - '0');
                pty_t *pty = pty_get(idx);
                if (!pty || !pty->in_use) return (uint64_t)-ENOENT;
                if (pty->locked) return (uint64_t)-EACCES; /* not yet unlocked */
                int fd = fd_alloc(current_task->fd_table);
                if (fd < 0) return (uint64_t)-ENOMEM;
                fd_entry_t *e = &current_task->fd_table->entries[fd];
                e->type  = FD_PTY_SLAVE;
                e->flags = (uint32_t)arg2;
                e->pipe  = pty;
                e->offset = 0;
                pty->slave_open = 1;
                return (uint64_t)fd;
            }

            vfs_node_t *node = vfs_open(abs, (int)arg2);
            if (!node) return (uint64_t)-ENOENT;
            int fd = fd_alloc(current_task->fd_table);
            if (fd < 0) return (uint64_t)-1;
            fd_entry_t *entry = &current_task->fd_table->entries[fd];
            entry->type = FD_FILE; entry->node = node; entry->offset = 0; entry->flags = (uint32_t)arg2;
            if ((uint32_t)arg2 & O_APPEND) entry->offset = node->length;
            /* Store absolute path for directories (used by SYS_FCHDIR) */
            if (node->flags & VFS_DIRECTORY)
                strncpy(entry->dir_path, abs, sizeof(entry->dir_path) - 1);
            return (uint64_t)fd;
        }

        case SYS_CLOSE: { // 3
            fd_entry_t *ce = fd_get(current_task->fd_table, (int)arg1);
            if (ce) {
                if (ce->type == FD_PIPE && ce->pipe)
                    pipe_close((pipe_t *)ce->pipe, (ce->flags & O_WRONLY) ? 1 : 0);
                else if (ce->type == FD_PTY_MASTER && ce->pipe)
                    pty_close_master((pty_t *)ce->pipe);
                else if (ce->type == FD_PTY_SLAVE && ce->pipe)
                    pty_close_slave((pty_t *)ce->pipe);
                else if (ce->type == FD_SOCKET && ce->socket)
                    net_socket_close((net_socket_t *)ce->socket);
            }
            fd_free(current_task->fd_table, (int)arg1);
            return 0;
        }

        case SYS_LSTAT: // 6 — same as STAT (no symlinks yet)
        /* fall through */

        case SYS_POLL: { // 7 — arg1=pollfd[], arg2=nfds, arg3=timeout_ms
            if (num == SYS_LSTAT) {
                /* SYS_LSTAT: same as SYS_STAT since we have no symlinks */
                char path[512], abs[512];
                if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                    return (uint64_t)-EFAULT;
                vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
                vfs_node_t *node = vfs_open(abs, 0);
                if (!node) return (uint64_t)-ENOENT;
                struct {
                    uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink;
                    uint32_t st_uid, st_gid; uint64_t st_rdev, st_size;
                    uint64_t st_blksize, st_blocks, st_atime, st_mtime, st_ctime;
                } kst;
                memset(&kst, 0, sizeof(kst));
                kst.st_ino  = node->inode;
                kst.st_size = node->length;
                kst.st_mode = (node->flags & VFS_DIRECTORY) ? 0040755 : 0100644;
                if (copy_to_user((void *)arg2, &kst, sizeof(kst)) < 0) return (uint64_t)-EFAULT;
                return 0;
            }
            /* SYS_POLL */
            int nfds = (int)arg2;
            int timeout_ms = (int)(int64_t)arg3;
            if (nfds <= 0 || nfds > 1024) return (uint64_t)-EINVAL;
            struct pollfd_s { int fd; short events; short revents; };
            size_t pfds_size = (size_t)nfds * sizeof(struct pollfd_s);
            if (!is_user_address(arg1, pfds_size)) return (uint64_t)-EFAULT;

            /* Copy pollfd array into kernel buffer */
            struct pollfd_s *kpfds = kmalloc(pfds_size);
            if (!kpfds) return (uint64_t)-ENOMEM;
            if (copy_from_user(kpfds, (void *)arg1, pfds_size) < 0) {
                kfree(kpfds);
                return (uint64_t)-EFAULT;
            }

            int ready = 0;
            for (int i = 0; i < nfds; i++) {
                kpfds[i].revents = 0;
                if (kpfds[i].fd < 0) continue;
                fd_entry_t *e = fd_get(current_task->fd_table, kpfds[i].fd);
                if (!e) { kpfds[i].revents = 0x20 /* POLLNVAL */; ready++; continue; }
                short rev = 0;
                if (kpfds[i].events & 0x0001) { /* POLLIN */
                    int ok = 0;
                    if (e->type == FD_FILE || e->type == FD_DEVICE) ok = 1;
                    else if (e->type == FD_PIPE && e->pipe)
                        ok = (pipe_bytes_available((pipe_t *)e->pipe) > 0) ||
                             (((pipe_t *)e->pipe)->writers == 0);
                    else if (e->type == FD_PTY_SLAVE  && e->pipe) ok = pty_slave_avail((pty_t *)e->pipe);
                    else if (e->type == FD_PTY_MASTER && e->pipe) ok = pty_master_avail((pty_t *)e->pipe);
                    else if (e->type == FD_SOCKET && e->socket)
                        ok = (net_socket_rx_avail((net_socket_t *)e->socket) > 0) ||
                             !net_socket_is_connected((net_socket_t *)e->socket);
                    if (ok) rev |= 0x0001;
                }
                if (kpfds[i].events & 0x0004) { /* POLLOUT */
                    int ok = 1; /* always writable unless socket is not connected */
                    if (e->type == FD_SOCKET && e->socket)
                        ok = net_socket_is_connected((net_socket_t *)e->socket);
                    if (ok) rev |= 0x0004;
                }
                if (rev) ready++;
                kpfds[i].revents = rev;
            }

            /* Copy results back to userspace */
            copy_to_user((void *)arg1, kpfds, pfds_size);
            kfree(kpfds);

            if (ready > 0 || timeout_ms == 0) return (uint64_t)ready;
            /* Simple timeout: sleep then return 0 */
            if (timeout_ms > 0 && timeout_ms < 10000) timer_sleep((uint32_t)timeout_ms);
            return 0;
        }

        case SYS_IOCTL: { // 16 — arg1=fd, arg2=request, arg3=arg
            fd_entry_t *ie = fd_get(current_task->fd_table, (int)arg1);
            if (!ie) return (uint64_t)-EBADF;

            unsigned long req = (unsigned long)arg2;

            /* TCGETS / TCSETS — termios (36 bytes) */
            if (req == 0x5401) { /* TCGETS */
                if (ie->type == FD_PTY_MASTER || ie->type == FD_PTY_SLAVE) {
                    pty_t *pt = (pty_t *)ie->pipe;
                    if (!is_user_address(arg3, sizeof(pty_termios_t))) return (uint64_t)-EFAULT;
                    copy_to_user((void *)arg3, &pt->termios, sizeof(pty_termios_t));
                    return 0;
                }
                /* Non-TTY fd: ENOTTY */
                return (uint64_t)-25; /* ENOTTY */
            }
            if (req == 0x5402 || req == 0x5403 || req == 0x5404) { /* TCSETS/W/F */
                if (ie->type == FD_PTY_MASTER || ie->type == FD_PTY_SLAVE) {
                    pty_t *pt = (pty_t *)ie->pipe;
                    if (!is_user_address(arg3, sizeof(pty_termios_t))) return (uint64_t)-EFAULT;
                    copy_from_user(&pt->termios, (void *)arg3, sizeof(pty_termios_t));
                    return 0;
                }
                return 0; /* Accept silently for other fds */
            }

            /* TIOCGWINSZ */
            if (req == 0x5413) {
                struct { uint16_t rows, cols, xpix, ypix; } ws = {24, 80, 0, 0};
                if (ie->type == FD_PTY_MASTER || ie->type == FD_PTY_SLAVE) {
                    pty_t *pt = (pty_t *)ie->pipe;
                    ws.rows = pt->rows; ws.cols = pt->cols;
                }
                if (!is_user_address(arg3, sizeof(ws))) return (uint64_t)-EFAULT;
                copy_to_user((void *)arg3, &ws, sizeof(ws));
                return 0;
            }
            /* TIOCSWINSZ */
            if (req == 0x5414) {
                struct { uint16_t rows, cols, xpix, ypix; } ws;
                if (!is_user_address(arg3, sizeof(ws))) return (uint64_t)-EFAULT;
                copy_from_user(&ws, (void *)arg3, sizeof(ws));
                if (ie->type == FD_PTY_MASTER || ie->type == FD_PTY_SLAVE) {
                    pty_t *pt = (pty_t *)ie->pipe;
                    pt->rows = ws.rows; pt->cols = ws.cols;
                }
                return 0;
            }

            /* TIOCGPTN — get slave PTY number (master only) */
            if (req == TIOCGPTN) {
                if (ie->type != FD_PTY_MASTER) return (uint64_t)-25; /* ENOTTY */
                if (!is_user_address(arg3, sizeof(unsigned int))) return (uint64_t)-EFAULT;
                unsigned int n = (unsigned int)((pty_t *)ie->pipe)->index;
                copy_to_user((void *)arg3, &n, sizeof(n));
                return 0;
            }
            /* TIOCSPTLCK — set lock state */
            if (req == TIOCSPTLCK) {
                if (ie->type != FD_PTY_MASTER) return (uint64_t)-25;
                if (!is_user_address(arg3, sizeof(int))) return (uint64_t)-EFAULT;
                int lock_val;
                copy_from_user(&lock_val, (void *)arg3, sizeof(lock_val));
                ((pty_t *)ie->pipe)->locked = lock_val;
                return 0;
            }
            /* TIOCGPTLCK — get lock state */
            if (req == TIOCGPTLCK) {
                if (ie->type != FD_PTY_MASTER) return (uint64_t)-25;
                if (!is_user_address(arg3, sizeof(int))) return (uint64_t)-EFAULT;
                int lock_val = ((pty_t *)ie->pipe)->locked;
                copy_to_user((void *)arg3, &lock_val, sizeof(lock_val));
                return 0;
            }

            /* TIOCGPGRP / TIOCSPGRP */
            if (req == 0x540F) { /* TIOCGPGRP */
                uint32_t pgrp = current_pid;
                if (is_user_address(arg3, 4)) copy_to_user((void *)arg3, &pgrp, 4);
                return 0;
            }
            if (req == 0x5410) return 0; /* TIOCSPGRP */

            /* FIONREAD */
            if (req == 0x541B) {
                int avail = 0;
                if (ie->type == FD_PTY_SLAVE && ie->pipe) {
                    pty_t *pt = (pty_t *)ie->pipe;
                    spinlock_acquire(&pt->lock);
                    avail = (int)((pt->m2s_head - pt->m2s_tail + PTY_BUF_SIZE) % PTY_BUF_SIZE);
                    spinlock_release(&pt->lock);
                }
                if (is_user_address(arg3, 4)) copy_to_user((void *)arg3, &avail, 4);
                return 0;
            }

            /* Unknown ioctl — return 0 (permissive) */
            return 0;
        }

        case SYS_SELECT: { // 23 — arg1=nfds, arg2=readfds, arg3=writefds, arg4=exceptfds, arg5=timeout
            /* fd_set is 128 bytes (1024 bits) on Linux x86-64 */
            int nfds = (int)arg1;
            if (nfds < 0 || nfds > 1024) return (uint64_t)-EINVAL;

            uint8_t rset[128];
            memset(rset, 0, sizeof(rset));
            if (arg2 && is_user_address(arg2, (size_t)((nfds + 7) / 8)))
                copy_from_user(rset, (void *)arg2, (size_t)((nfds + 7) / 8));

            /* timeout: struct timeval { int64_t tv_sec; int64_t tv_usec; } */
            int64_t timeout_ms = -1; /* -1 = blocking (not supported; we'll just check once) */
            if (arg5 && is_user_address(arg5, 16)) {
                struct { int64_t tv_sec; int64_t tv_usec; } ktv;
                if (copy_from_user(&ktv, (void *)arg5, sizeof(ktv)) == 0)
                    timeout_ms = ktv.tv_sec * 1000 + ktv.tv_usec / 1000;
            }

            int ready = 0;
            uint8_t out_r[128];
            memset(out_r, 0, sizeof(out_r));

            for (int fd = 0; fd < nfds; fd++) {
                int byte = fd / 8, bit = fd % 8;
                if (!(rset[byte] & (1 << bit))) continue;
                fd_entry_t *e = fd_get(current_task->fd_table, fd);
                if (!e) continue;
                int ok = 0;
                if (e->type == FD_FILE || e->type == FD_DEVICE) ok = 1;
                else if (e->type == FD_PIPE && e->pipe)
                    ok = (pipe_bytes_available((pipe_t *)e->pipe) > 0) ||
                         (((pipe_t *)e->pipe)->writers == 0);
                else if (e->type == FD_PTY_SLAVE  && e->pipe) ok = pty_slave_avail((pty_t *)e->pipe);
                else if (e->type == FD_PTY_MASTER && e->pipe) ok = pty_master_avail((pty_t *)e->pipe);
                if (ok) { out_r[byte] |= (uint8_t)(1 << bit); ready++; }
            }

            if (arg2) copy_to_user((void *)arg2, out_r, (size_t)((nfds + 7) / 8));
            if (arg3 && is_user_address(arg3, (size_t)((nfds + 7) / 8))) {
                /* writefds: always clear (no write-readiness check) */
                uint8_t tmp[128] = {0};
                copy_to_user((void *)arg3, tmp, (size_t)((nfds + 7) / 8));
            }

            if (ready > 0 || timeout_ms == 0) return (uint64_t)ready;
            if (timeout_ms > 0 && timeout_ms < 10000) timer_sleep((uint32_t)timeout_ms);
            return 0;
        }

        case SYS_MMAP: { // 9 — arg1=addr, arg2=len, arg3=prot, arg4=flags, arg5=fd, arg6=off
            uint64_t hint   = arg1;
            size_t   len    = (size_t)arg2;
            int      prot   = (int)arg3;
            int      flags  = (int)arg4;
            int      mapfd  = (int)arg5;
            size_t   offset = (size_t)regs->r9; /* arg6 via r9 */

            if (len == 0) return (uint64_t)-EINVAL;
            if (!current_task->mm) return (uint64_t)-ENOMEM;

            /* Align length to page boundary */
            size_t aligned = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

            /* Choose virtual address */
            uint64_t map_addr;
            if ((flags & MAP_FIXED) && hint != 0)
                map_addr = hint & ~(uint64_t)(PAGE_SIZE - 1);
            else
                map_addr = vma_find_free(current_task->mm, aligned);
            if (map_addr == 0) return (uint64_t)-ENOMEM;

            /* VMA permission flags */
            uint32_t vflags = VMA_USER;
            if (prot & PROT_READ)  vflags |= VMA_READ;
            if (prot & PROT_WRITE) vflags |= VMA_WRITE;
            if (prot & PROT_EXEC)  vflags |= VMA_EXEC;

            if ((flags & MAP_ANONYMOUS) || mapfd < 0) {
                /* Anonymous: create VMA, let page fault handler zero-fill lazily */
                vm_area_t *vma = vma_create(map_addr, map_addr + aligned, vflags, VMA_TYPE_ANONYMOUS);
                if (!vma) return (uint64_t)-ENOMEM;
                vma_insert(current_task->mm, vma);
                return map_addr;
            }

            /* File-backed mmap: read file data into pages eagerly */
            fd_entry_t *mfe = fd_get(current_task->fd_table, mapfd);
            if (!mfe || mfe->type != FD_FILE || !mfe->node) return (uint64_t)-EBADF;

            uint64_t hhdm = vmm_get_hhdm_offset();
            size_t pages  = aligned / PAGE_SIZE;
            for (size_t i = 0; i < pages; i++) {
                uint64_t page_virt = map_addr + i * PAGE_SIZE;
                void *phys = pmm_alloc_page();
                if (!phys) {
                    /* On failure: unmap already-mapped pages */
                    for (size_t j = 0; j < i; j++) {
                        uint64_t pv = map_addr + j * PAGE_SIZE;
                        uint64_t pte = vmm_get_pte(pv);
                        if (pte & PTE_PRESENT) {
                            pmm_page_unref((void *)(pte & PTE_ADDR_MASK));
                            vmm_unmap_page(pv);
                        }
                    }
                    return (uint64_t)-ENOMEM;
                }
                uint8_t *kp = (uint8_t *)((uint64_t)phys + hhdm);
                memset(kp, 0, PAGE_SIZE);

                size_t file_off = offset + i * PAGE_SIZE;
                if (file_off < mfe->node->length) {
                    size_t to_read = PAGE_SIZE;
                    if (file_off + to_read > mfe->node->length)
                        to_read = mfe->node->length - file_off;
                    vfs_read(mfe->node, file_off, to_read, kp);
                }

                uint64_t pte_flags = PTE_PRESENT | PTE_USER;
                if (prot & PROT_WRITE) pte_flags |= PTE_WRITABLE;
                vmm_map_page(page_virt, (uint64_t)phys, pte_flags);
            }

            /* Register VMA for munmap/fork tracking */
            vm_area_t *vma = vma_create(map_addr, map_addr + aligned, vflags, VMA_TYPE_FILE);
            if (vma) vma_insert(current_task->mm, vma);
            return map_addr;
        }

        case SYS_MUNMAP: { // 11 — arg1=addr, arg2=len
            uint64_t addr   = arg1 & ~(uint64_t)(PAGE_SIZE - 1);
            size_t   len    = (arg2 + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
            if (!current_task->mm || len == 0) return 0;

            for (uint64_t a = addr; a < addr + len; a += PAGE_SIZE) {
                uint64_t pte = vmm_get_pte(a);
                if (pte & PTE_PRESENT) {
                    pmm_page_unref((void *)(pte & PTE_ADDR_MASK));
                    vmm_unmap_page(a);
                }
            }
            vma_remove(current_task->mm, addr, addr + len);
            return 0;
        }

        case SYS_PREAD64: { // 17 — arg1=fd, arg2=buf, arg3=count, arg4=offset
            fd_entry_t *pe = fd_get(current_task->fd_table, (int)arg1);
            if (!pe || pe->type != FD_FILE || !pe->node) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            uint8_t *kbuf = kmalloc(arg3);
            if (!kbuf) return (uint64_t)-ENOMEM;
            size_t n = vfs_read(pe->node, (size_t)arg4, arg3, kbuf);
            copy_to_user((void *)arg2, kbuf, n);
            kfree(kbuf);
            return (uint64_t)n;
        }

        case SYS_PWRITE64: { // 18 — arg1=fd, arg2=buf, arg3=count, arg4=offset
            fd_entry_t *pw = fd_get(current_task->fd_table, (int)arg1);
            if (!pw || pw->type != FD_FILE || !pw->node) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            uint8_t *kbuf = kmalloc(arg3);
            if (!kbuf) return (uint64_t)-ENOMEM;
            copy_from_user(kbuf, (void *)arg2, arg3);
            size_t n = vfs_write(pw->node, (size_t)arg4, arg3, kbuf);
            kfree(kbuf);
            return (uint64_t)n;
        }

        case SYS_READLINK: { // 89 — arg1=path, arg2=buf, arg3=bufsiz
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-EFAULT;
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            char result[512];
            result[0] = '\0';

            /* /proc/self/exe → executable path */
            if (strcmp(path, "/proc/self/exe") == 0 ||
                strcmp(path, "/proc/thread-self/exe") == 0) {
                strncpy(result, current_task->exec_path[0] ? current_task->exec_path : "/", sizeof(result) - 1);
            }
            /* /proc/self/fd/N → path of open fd N */
            else if (strncmp(path, "/proc/self/fd/", 14) == 0 ||
                     strncmp(path, "/proc/thread-self/fd/", 21) == 0) {
                const char *ns = path + (path[6] == 's' ? 14 : 21); /* skip prefix */
                int fdnum = 0;
                while (*ns >= '0' && *ns <= '9') fdnum = fdnum * 10 + (*ns++ - '0');
                fd_entry_t *rfe = fd_get(current_task->fd_table, fdnum);
                if (rfe && rfe->type == FD_FILE && rfe->node)
                    strncpy(result, rfe->node->name, sizeof(result) - 1);
                else if (rfe && rfe->dir_path[0])
                    strncpy(result, rfe->dir_path, sizeof(result) - 1);
                else
                    ksnprintf(result, sizeof(result), "/proc/self/fd/%d", fdnum);
            }
            /* /proc/self → /proc/<pid> */
            else if (strcmp(path, "/proc/self") == 0) {
                ksnprintf(result, sizeof(result), "/proc/%u", current_pid);
            }
            else {
                /* Try VFS node */
                char abs[512];
                vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
                vfs_node_t *rln = vfs_open(abs, 0);
                if (!rln) return (uint64_t)-ENOENT;
                strncpy(result, rln->name, sizeof(result) - 1);
            }

            result[sizeof(result) - 1] = '\0';
            size_t rlen = strlen(result);
            size_t copy_len = rlen < (size_t)arg3 ? rlen : (size_t)arg3;
            copy_to_user((void *)arg2, result, copy_len);
            return (uint64_t)copy_len;
        }

        case SYS_BRK: { // 12
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

        case SYS_FORK: // 57
            return (uint64_t)task_fork((registers_t *)regs);
            
        case SYS_CLONE: { // 56 — arg1=flags, arg2=child_stack, arg3=ptid, arg4=ctid, arg5=tls
            uint64_t clone_flags = arg1;
            uint64_t child_stack = arg2;
            #define CLONE_VM       0x00000100
            #define CLONE_THREAD   0x00010000
            #define CLONE_SETTLS   0x00080000
            #define CLONE_PARENT_SETTID  0x00100000
            #define CLONE_CHILD_CLEARTID 0x00200000
            #define CLONE_CHILD_SETTID   0x01000000

            if (clone_flags & CLONE_THREAD) {
                /* Create a thread sharing address space */
                int tid = task_create_thread(regs->rip, 0, child_stack);
                if (tid < 0) return (uint64_t)-ENOMEM;
                task_t *child = task_get_by_id((uint32_t)tid);
                if (child) {
                    if ((clone_flags & CLONE_SETTLS) && arg5)
                        child->tls_base = arg5;
                    if ((clone_flags & CLONE_CHILD_CLEARTID) && arg4)
                        child->clear_tid_addr = arg4;
                    if ((clone_flags & CLONE_PARENT_SETTID) && arg3 && is_user_address(arg3, 4)) {
                        uint32_t t32 = (uint32_t)tid;
                        copy_to_user((void *)arg3, &t32, 4);
                    }
                    if ((clone_flags & CLONE_CHILD_SETTID) && arg4 && is_user_address(arg4, 4)) {
                        uint32_t t32 = (uint32_t)tid;
                        copy_to_user((void *)arg4, &t32, 4);
                    }
                }
                return (uint64_t)tid;
            }
            /* Fork-like clone */
            return (uint64_t)task_fork((registers_t *)regs);
        }

        case SYS_SCHED_YIELD: // 24
            task_yield();
            return 0;
            
        case SYS_WAIT4: { // 61 — wait4(pid, *status, options, *rusage)
            int status = 0;
            int wpid = (int)(int64_t)arg1; // -1 = any, >0 = specific
            int wopts = (int)arg3;
            int child_pid = task_waitpid(wpid, &status, wopts);
            if (child_pid > 0 && arg2) {
                if (copy_to_user((void*)arg2, &status, sizeof(int)) < 0) return (uint64_t)-EFAULT;
            }
            if (child_pid < 0) return (uint64_t)-ECHILD;
            return (uint64_t)child_pid;
        }

        // Custom IPC Syscalls (500+)
        case SYS_IPC_CREATE:
            return (uint64_t)ipc_port_create(current_pid, (uint32_t)arg1);

        case SYS_IPC_SEND: {
            ipc_message_t kmsg;
            if (copy_from_user(&kmsg, (void*)arg2, sizeof(ipc_message_t)) < 0) return (uint64_t)-EFAULT;
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

        case SYS_IPC_SHMEM_CREATE:
            return (uint64_t)ipc_shmem_create((size_t)arg1, current_pid, (uint32_t)arg2);

        case SYS_IPC_SHMEM_MAP:
            return (uint64_t)ipc_shmem_map((uint32_t)arg1, current_pid);

        case SYS_IPC_SHMEM_UNMAP:
            return (uint64_t)ipc_shmem_unmap((uint32_t)arg1, current_pid);

        case SYS_IRQ_WAIT: { // 518
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) return (uint64_t)-1;
            irq_register_waiter((int)arg1, current_task);
            task_block();
            return 0;
        }

        case SYS_IRQ_ACK: { // 519
            if (!security_has_capability(current_pid, CAP_HW_INPUT)) return (uint64_t)-1;
            return 0;
        }

        case SYS_CLOCK_GETTIME: { // 228
            uint64_t ts[2];
            ts[0] = rtc_get_timestamp();
            ts[1] = 0; // nsec
            if (copy_to_user((void*)arg1, ts, sizeof(ts)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }

        case SYS_SHUTDOWN: { // 48 — socket shutdown (arg1=fd, arg2=how)
            fd_entry_t *she = fd_get(current_task->fd_table, (int)arg1);
            if (!she || she->type != FD_SOCKET) return (uint64_t)-EBADF;
            net_socket_close((net_socket_t *)she->socket);
            she->socket = NULL;
            she->type   = FD_NONE;
            return 0;
        }

        case 169: { // SYS_REBOOT — system power-off / reboot
            if (!security_has_capability(current_pid, CAP_SYS_REBOOT)) return (uint64_t)-1;
            kprintf("[SYSCALL] Powering off...\n");
            outw(0x604, 0x2000); // QEMU/VirtualBox ACPI power-off
            return 0;
        }

        case SYS_GETDENTS64: { // 217 — fill buffer with multiple entries
            fd_entry_t *entry = fd_get(current_task->fd_table, (int)arg1);
            if (!entry || entry->type != FD_FILE || !(entry->node->flags & VFS_DIRECTORY))
                return (uint64_t)-EBADF;
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;

            struct {
                uint64_t d_ino;
                int64_t  d_off;
                uint16_t d_reclen;
                uint8_t  d_type;
                char     d_name[256];
            } de;

            size_t total = 0;
            for (;;) {
                vfs_node_t *node = vfs_readdir(entry->node, entry->offset);
                if (!node) break; // EOF

                size_t name_len = strlen(node->name);
                /* reclen: fixed fields (8+8+2+1=19) + name + NUL, rounded to 8 bytes */
                size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;
                if (reclen > sizeof(de)) reclen = sizeof(de);

                if (total + reclen > (size_t)arg3) {
                    if (total == 0) return (uint64_t)-EINVAL; // buffer too small
                    break;
                }

                memset(&de, 0, reclen);
                de.d_ino    = node->inode ? node->inode : (uint64_t)(entry->offset + 1);
                de.d_off    = (int64_t)(entry->offset + 1);
                de.d_type   = (node->flags & VFS_DIRECTORY) ? 4 : 8; // DT_DIR : DT_REG
                de.d_reclen = (uint16_t)reclen;
                size_t cp = name_len < 255 ? name_len : 255;
                memcpy(de.d_name, node->name, cp);
                de.d_name[cp] = '\0';

                if (copy_to_user((void *)(arg2 + total), &de, reclen) < 0)
                    return (uint64_t)-EFAULT;

                total += reclen;
                entry->offset++;
            }
            return (uint64_t)total; // 0 = EOF
        }

        case SYS_IOPORT: { // 517
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

        case SYS_PROC_EXEC: { // 512
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            if (!node) {
                kprintf("[SYSCALL] SYS_PROC_EXEC: Failed to open %s\n", path);
                return (uint64_t)-ENOENT;
            }
            uint8_t *buf = kmalloc(node->length);
            if (!buf) return (uint64_t)-ENOMEM;
            vfs_read(node, 0, node->length, buf);
            kprintf("[SYSCALL] SYS_PROC_EXEC: Loading %s (%lu bytes)\n", path, node->length);
            int slot = task_create_user(path, buf, node->length, current_pid, ABI_NATIVE);
            kfree(buf);
            return (uint64_t)slot;
        }

        case SYS_STAT: { // 4
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
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
            kst.st_mode = (node->flags & VFS_DIRECTORY) ? 0040000 : 0100000;
            kst.st_mode |= 0755;

            if (copy_to_user((void*)arg2, &kst, sizeof(kst)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }

        case SYS_SEC_GRANT: // 511
            return (uint64_t)security_grant_capability(current_pid, (uint32_t)arg1, arg2);

        case SYS_GET_FRAMEBUFFER: { // 505
            if (!security_has_capability(current_pid, CAP_HW_VIDEO)) return (uint64_t)-1;
            console_set_enabled(0);
            
            extern uint32_t *fb_ptr; 
            extern uint64_t fb_width; 
            extern uint64_t fb_height;
            
            uint64_t fb_user_base = FB_USER_BASE; 
            uint64_t fb_size = fb_width * fb_height * 4;
            fb_size = (fb_size + 4095) & ~4095;
            
            uint64_t fb_virt_kernel = (uint64_t)fb_ptr;
            uint64_t fb_phys;
            if (fb_virt_kernel >= 0xffffffff80000000ULL) {
                fb_phys = fb_virt_kernel - 0xffffffff80000000ULL; 
            } else {
                fb_phys = fb_virt_kernel - vmm_get_hhdm_offset();
            }
            
            for (uint64_t off = 0; off < fb_size; off += 4096) {
                vmm_map_user_page(fb_user_base + off, fb_phys + off);
            }
            
            typedef struct { uint64_t addr, width, height, pitch; uint32_t bpp; } fb_info_t;
            if (!is_user_address(arg1, sizeof(fb_info_t))) return (uint64_t)-EFAULT;
            
            fb_info_t *info = (fb_info_t *)arg1;
            info->addr = fb_user_base; 
            info->width = fb_width; 
            info->height = fb_height; 
            info->pitch = fb_width * 4; 
            info->bpp = 32;
            
            return 0;
        }

        case SYS_GET_INPUT_EVENT: { // 506
            typedef struct {
                uint32_t type; 
                uint32_t code; 
                int32_t x;     
                int32_t y;     
            } input_event_t;

            input_event_t *uevt = (input_event_t *)arg1;
            if (!is_user_address(arg1, sizeof(input_event_t))) return (uint64_t)-EFAULT;

            uint32_t type, code, buttons;
            int32_t x, y;

            if (get_mouse_event(&type, &buttons, &x, &y)) {
                input_event_t kevt = { .type = type, .code = buttons, .x = x, .y = y };
                copy_to_user(uevt, &kevt, sizeof(input_event_t));
                return 1;
            }

            if (get_keyboard_event(&type, &code)) {
                input_event_t kevt = { .type = type, .code = code, .x = 0, .y = 0 };
                copy_to_user(uevt, &kevt, sizeof(input_event_t));
                return 1;
            }

            // Gamepad events: type=3, code=buttons, x=lx|ly packed, y=rx|ry packed
            {
                int gp_idx;
                gamepad_state_t gstate;
                if (gamepad_get_event(&gp_idx, &gstate)) {
                    // Pack axes: x = (lx << 16) | (uint16_t)ly
                    //            y = (rx << 16) | (uint16_t)ry
                    int32_t packed_x = ((int32_t)(uint16_t)gstate.lx << 16) |
                                        (uint16_t)gstate.ly;
                    int32_t packed_y = ((int32_t)(uint16_t)gstate.rx << 16) |
                                        (uint16_t)gstate.ry;
                    input_event_t kevt = {
                        .type = INPUT_TYPE_GAMEPAD,
                        .code = gstate.buttons,
                        .x    = packed_x,
                        .y    = packed_y,
                    };
                    copy_to_user(uevt, &kevt, sizeof(input_event_t));
                    return 1;
                }
            }

            return 0;
        }
        
        case SYS_GPU_UPDATE: { // 520
            // arg1=x, arg2=y, arg3=w, arg4=h
            
            extern void virtio_gpu_transfer(int x, int y, int w, int h);
            extern void virtio_gpu_flush(int x, int y, int w, int h);
            
            int x = (int)arg1;
            int y = (int)arg2;
            int w = (int)arg3;
            int h = (int)arg4;
            
            // kprintf("[SYS] GPU Update: %d,%d %dx%d\n", x, y, w, h);
            virtio_gpu_transfer(x, y, w, h);
            virtio_gpu_flush(x, y, w, h);
            return 0;
        }

        case SYS_AUDIO_WRITE: { // 524 — write PCM to HDA ring buffer
            // arg1 = user buffer, arg2 = length in bytes
            extern int hda_write_audio(const void *pcm, int len);
            int len = (int)arg2;
            if (len <= 0 || len > 65536) return (uint64_t)-EINVAL;
            if (!is_user_address(arg1, (size_t)len)) return (uint64_t)-EFAULT;
            // Copy from userspace into a temporary kernel buffer, then write
            void *kbuf = kmalloc((size_t)len);
            if (!kbuf) return (uint64_t)-ENOMEM;
            copy_from_user(kbuf, (const void *)arg1, (size_t)len);
            int written = hda_write_audio(kbuf, len);
            kfree(kbuf);
            return (uint64_t)written;
        }

        case SYS_AUDIO_AVAIL: { // 525 — free bytes in ring buffer
            extern uint32_t hda_ring_free(void);
            return (uint64_t)hda_ring_free();
        }

        case SYS_AUDIO_SET_VOL: { // 526 — set volume 0-100
            extern void hda_set_volume(int vol_pct);
            hda_set_volume((int)arg1);
            return 0;
        }

        case SYS_ARCH_PRCTL: { // 158

            uint32_t code = (uint32_t)arg1;
            uint64_t addr = arg2;

            if (code == 0x1002) { // ARCH_SET_FS
                if (!is_user_address(addr, 8)) return (uint64_t)-EFAULT;
                current_task->tls_base = addr;
                wrmsr(MSR_FS_BASE, addr);
                return 0;
            } 
            else if (code == 0x1001) { // ARCH_SET_GS
                if (!is_user_address(addr, 8)) return (uint64_t)-EFAULT;
                wrmsr(MSR_KERNEL_GS_BASE, addr);
                return 0;
            }
            else if (code == 0x1003) { // ARCH_GET_FS
                 if (!is_user_address(addr, 8)) return (uint64_t)-EFAULT;
                 uint64_t fs_base = rdmsr(MSR_FS_BASE);
                 copy_to_user((void*)addr, &fs_base, sizeof(uint64_t));
                 return 0;
            }
            else if (code == 0x1004) { // ARCH_GET_GS
                 if (!is_user_address(addr, 8)) return (uint64_t)-EFAULT;
                 uint64_t gs_base = rdmsr(MSR_KERNEL_GS_BASE);
                 copy_to_user((void*)addr, &gs_base, sizeof(uint64_t));
                 return 0;
            }
            
            return (uint64_t)-EINVAL;
        }

        // ── Networking ────────────────────────────────────────────────────────

        case SYS_SOCKET: { // 41 — arg1=domain, arg2=type, arg3=proto
            if ((int)arg1 != AF_INET) return (uint64_t)-EINVAL;
            int sock_type = ((int)arg2 == SOCK_STREAM) ? SOCK_STREAM : SOCK_DGRAM;
            net_socket_t *s = net_socket_create(sock_type);
            if (!s) return (uint64_t)-ENOMEM;
            int fd = fd_alloc(current_task->fd_table);
            if (fd < 0) { net_socket_close(s); return (uint64_t)-ENOMEM; }
            fd_entry_t *e = &current_task->fd_table->entries[fd];
            e->type   = FD_SOCKET;
            e->socket = s;
            return (uint64_t)fd;
        }

        case SYS_BIND: { // 49 — arg1=fd, arg2=*sockaddr_in, arg3=len
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, sizeof(sockaddr_in_t))) return (uint64_t)-EFAULT;
            sockaddr_in_t sa;
            copy_from_user(&sa, (void*)arg2, sizeof(sa));
            return (uint64_t)net_socket_bind((net_socket_t*)e->socket,
                                             net_ntohl(sa.sin_addr),
                                             net_ntohs(sa.sin_port));
        }

        case SYS_LISTEN: { // 50
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            return (uint64_t)net_socket_listen((net_socket_t*)e->socket, (int)arg2);
        }

        case SYS_CONNECT: { // 42 — arg1=fd, arg2=*sockaddr_in, arg3=len
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, sizeof(sockaddr_in_t))) return (uint64_t)-EFAULT;
            sockaddr_in_t sa;
            copy_from_user(&sa, (void*)arg2, sizeof(sa));
            return (uint64_t)net_socket_connect((net_socket_t*)e->socket,
                                                net_ntohl(sa.sin_addr),
                                                net_ntohs(sa.sin_port));
        }

        case SYS_ACCEPT: { // 43 — arg1=fd, arg2=*sockaddr_in, arg3=*addrlen
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            uint32_t rip = 0; uint16_t rport = 0;
            net_socket_t *child = net_socket_accept((net_socket_t*)e->socket, &rip, &rport);
            if (!child) return (uint64_t)-EINVAL;
            int fd = fd_alloc(current_task->fd_table);
            if (fd < 0) { net_socket_close(child); return (uint64_t)-ENOMEM; }
            fd_entry_t *ce = &current_task->fd_table->entries[fd];
            ce->type   = FD_SOCKET;
            ce->socket = child;
            if (arg2 && is_user_address(arg2, sizeof(sockaddr_in_t))) {
                sockaddr_in_t sa = {AF_INET, net_htons(rport), net_htonl(rip), {0}};
                copy_to_user((void*)arg2, &sa, sizeof(sa));
            }
            if (arg3 && is_user_address(arg3, 4)) {
                uint32_t sz = sizeof(sockaddr_in_t);
                copy_to_user((void*)arg3, &sz, 4);
            }
            return (uint64_t)fd;
        }

        case SYS_SENDTO: { // 44 — arg1=fd, arg2=buf, arg3=len, arg4=flags, arg5=addr, arg6=addrlen
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            net_socket_t *s = (net_socket_t*)e->socket;
            // If addr provided (UDP), set destination
            if (arg5 && is_user_address(arg5, sizeof(sockaddr_in_t))) {
                sockaddr_in_t sa;
                copy_from_user(&sa, (void*)arg5, sizeof(sa));
                net_socket_set_remote(s, net_ntohl(sa.sin_addr), net_ntohs(sa.sin_port));
            }
            void *kbuf = kmalloc((size_t)arg3);
            if (!kbuf) return (uint64_t)-ENOMEM;
            copy_from_user(kbuf, (void*)arg2, (size_t)arg3);
            int r = net_socket_send(s, kbuf, (int)arg3);
            kfree(kbuf);
            return (uint64_t)r;
        }

        case SYS_RECVFROM: { // 45 — arg1=fd, arg2=buf, arg3=len, arg4=flags, arg5=src_addr*, arg6=addrlen*
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, arg3)) return (uint64_t)-EFAULT;
            int r = net_socket_recv((net_socket_t*)e->socket, (void*)arg2, (int)arg3, 0);
            if (r >= 0 && arg5 && is_user_address(arg5, sizeof(sockaddr_in_t))) {
                net_socket_t *s = (net_socket_t *)e->socket;
                sockaddr_in_t sa = {AF_INET, net_htons(net_socket_get_remote_port(s)),
                                    net_htonl(net_socket_get_remote_ip(s)), {0}};
                copy_to_user((void *)arg5, &sa, sizeof(sa));
                if (arg6 && is_user_address(arg6, 4)) {
                    uint32_t sz = sizeof(sockaddr_in_t);
                    copy_to_user((void *)arg6, &sz, 4);
                }
            }
            return (uint64_t)r;
        }

        case 288: { // SYS_ACCEPT4 — arg1=fd, arg2=addr*, arg3=addrlen*, arg4=flags
            /* Reuse ACCEPT logic; ignore flags (SOCK_NONBLOCK, SOCK_CLOEXEC) for now */
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            uint32_t rip = 0; uint16_t rport = 0;
            net_socket_t *child = net_socket_accept((net_socket_t*)e->socket, &rip, &rport);
            if (!child) return (uint64_t)-EINVAL;
            int nfd = fd_alloc(current_task->fd_table);
            if (nfd < 0) { net_socket_close(child); return (uint64_t)-ENOMEM; }
            fd_entry_t *ce = &current_task->fd_table->entries[nfd];
            ce->type   = FD_SOCKET;
            ce->flags  = (uint32_t)arg4 & O_CLOEXEC;
            ce->socket = child;
            if (arg2 && is_user_address(arg2, sizeof(sockaddr_in_t))) {
                sockaddr_in_t sa = {AF_INET, net_htons(rport), net_htonl(rip), {0}};
                copy_to_user((void*)arg2, &sa, sizeof(sa));
            }
            if (arg3 && is_user_address(arg3, 4)) {
                uint32_t sz = sizeof(sockaddr_in_t);
                copy_to_user((void*)arg3, &sz, 4);
            }
            return (uint64_t)nfd;
        }

        case 46: // SYS_SENDMSG — arg1=fd, arg2=msghdr*, arg3=flags
        case 47: { // SYS_RECVMSG — arg1=fd, arg2=msghdr*, arg3=flags
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            if (!is_user_address(arg2, 56)) return (uint64_t)-EFAULT;
            /* msghdr layout (x86-64): name(8), namelen(4), pad(4),
               iov*(8), iovlen(8), control*(8), controllen(8), flags(4) */
            struct { uint64_t name; uint32_t namelen; uint32_t pad;
                     uint64_t iov_ptr; uint64_t iovlen;
                     uint64_t ctrl; uint64_t ctrllen; int32_t flags; } mhdr;
            copy_from_user(&mhdr, (void*)arg2, sizeof(mhdr));
            if (mhdr.iovlen == 0) return 0;
            if (mhdr.iovlen > 16) mhdr.iovlen = 16;
            /* struct iovec: base(8) + len(8) = 16 bytes each */
            struct { uint64_t base; uint64_t len; } iov[16];
            if (!is_user_address(mhdr.iov_ptr, mhdr.iovlen * 16)) return (uint64_t)-EFAULT;
            copy_from_user(iov, (void*)mhdr.iov_ptr, (size_t)mhdr.iovlen * 16);
            int total = 0;
            for (uint64_t i = 0; i < mhdr.iovlen; i++) {
                if (!iov[i].len) continue;
                if (num == 46) { /* SENDMSG */
                    if (!is_user_address(iov[i].base, iov[i].len)) break;
                    void *tmp = kmalloc((size_t)iov[i].len);
                    if (!tmp) break;
                    copy_from_user(tmp, (void*)iov[i].base, (size_t)iov[i].len);
                    int r = net_socket_send((net_socket_t*)e->socket, tmp, (int)iov[i].len);
                    kfree(tmp);
                    if (r < 0) { if (!total) total = r; break; }
                    total += r;
                } else { /* RECVMSG */
                    if (!is_user_address(iov[i].base, iov[i].len)) break;
                    int r = net_socket_recv((net_socket_t*)e->socket,
                                            (void*)iov[i].base, (int)iov[i].len, 0);
                    if (r <= 0) { if (!total) total = r; break; }
                    total += r;
                    break; /* Deliver to first iovec only per recv call */
                }
            }
            return (uint64_t)total;
        }

        case SYS_GETSOCKNAME:
        case SYS_GETPEERNAME: {
            fd_entry_t *e = fd_get(current_task->fd_table, (int)arg1);
            if (!e || e->type != FD_SOCKET) return (uint64_t)-EBADF;
            net_socket_t *s = (net_socket_t*)e->socket;
            if (arg2 && is_user_address(arg2, sizeof(sockaddr_in_t))) {
                uint32_t ip   = (num == SYS_GETSOCKNAME)
                              ? net_socket_get_local_ip(s) : net_socket_get_remote_ip(s);
                uint16_t port = (num == SYS_GETSOCKNAME)
                              ? net_socket_get_local_port(s) : net_socket_get_remote_port(s);
                sockaddr_in_t sa = {AF_INET, net_htons(port), net_htonl(ip), {0}};
                copy_to_user((void*)arg2, &sa, sizeof(sa));
            }
            if (arg3 && is_user_address(arg3, 4)) {
                uint32_t sz = sizeof(sockaddr_in_t);
                copy_to_user((void*)arg3, &sz, 4);
            }
            return 0;
        }

        case SYS_SETSOCKOPT: { // 54 — arg1=fd, arg2=level, arg3=optname, arg4=optval*, arg5=optlen
            fd_entry_t *soe = fd_get(current_task->fd_table, (int)arg1);
            if (!soe || soe->type != FD_SOCKET) return (uint64_t)-EBADF;
            /* Accept and ignore all common socket options (SO_REUSEADDR, TCP_NODELAY, etc.) */
            return 0;
        }

        case SYS_GETSOCKOPT: { // 55 — arg1=fd, arg2=level, arg3=optname, arg4=optval*, arg5=*optlen
            fd_entry_t *goe = fd_get(current_task->fd_table, (int)arg1);
            if (!goe || goe->type != FD_SOCKET) return (uint64_t)-EBADF;
            int optval = 0;
            switch ((int)arg3) {
                case 3: /* SO_TYPE */
                    optval = net_socket_get_type((net_socket_t *)goe->socket); break;
                case 4: /* SO_ERROR */ optval = 0; break;
                case 7: /* SO_SNDBUF */ optval = 65536; break;
                case 8: /* SO_RCVBUF */ optval = 65536; break;
                default: optval = 0; break;
            }
            if (arg4 && is_user_address(arg4, 4))
                copy_to_user((void *)arg4, &optval, 4);
            if (arg5 && is_user_address(arg5, 4)) {
                uint32_t sz = 4;
                copy_to_user((void *)arg5, &sz, 4);
            }
            return 0;
        }

        // ── CWD / Directory operations ────────────────────────────────────────

        case SYS_GETCWD: { // 79 — arg1=buf, arg2=size
            if (!arg1 || arg2 < 2) return (uint64_t)-EINVAL;
            const char *cwd = current_task->cwd;
            size_t cwd_len = strlen(cwd) + 1;
            if (cwd_len > arg2) return (uint64_t)-ERANGE;
            if (!is_user_address(arg1, cwd_len)) return (uint64_t)-EFAULT;
            copy_to_user((void *)arg1, cwd, cwd_len);
            return arg1; // Linux returns pointer to buf
        }

        case SYS_CHDIR: { // 80 — arg1=path
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            if (!node) return (uint64_t)-ENOENT;
            if (!(node->flags & VFS_DIRECTORY)) return (uint64_t)-ENOTDIR;
            strncpy(current_task->cwd, abs, sizeof(current_task->cwd) - 1);
            current_task->cwd[sizeof(current_task->cwd) - 1] = '\0';
            return 0;
        }

        case SYS_EXECVE: { // 59 — in-place exec (arg1=path, arg2=argv[], arg3=envp[])
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_node_t *node = NULL;

            /* If path contains a slash, resolve directly */
            int has_slash = 0;
            for (int pi = 0; path[pi]; pi++) if (path[pi] == '/') { has_slash = 1; break; }

            if (has_slash) {
                vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
                node = vfs_open(abs, 0);
            } else {
                /* Search PATH in envp, then fallback dirs */
                char path_env[1024] = "";
                if (arg3) {
                    const uint64_t *user_envp = (const uint64_t *)arg3;
                    for (int i = 0; i < 256; i++) {
                        uint64_t uptr = 0;
                        if (copy_from_user(&uptr, &user_envp[i], 8) < 0 || !uptr) break;
                        char evar[32];
                        copy_string_from_user(evar, (const char *)uptr, sizeof(evar));
                        if (evar[0]=='P' && evar[1]=='A' && evar[2]=='T' && evar[3]=='H' && evar[4]=='=') {
                            copy_string_from_user(path_env, (const char *)(uptr + 5), sizeof(path_env));
                            break;
                        }
                    }
                }
                /* Walk colon-separated PATH entries */
                const char *fallback[] = { "/bin", "/usr/bin", "/usr/local/bin", NULL };
                /* Try PATH dirs first */
                char *pp = path_env;
                while (!node) {
                    char dir[256] = "";
                    /* extract next dir from pp */
                    int di = 0;
                    while (*pp && *pp != ':' && di < 255) dir[di++] = *pp++;
                    dir[di] = '\0';
                    if (*pp == ':') pp++;
                    if (di > 0) {
                        char try_abs[512];
                        ksnprintf(try_abs, sizeof(try_abs), "%s/%s", dir, path);
                        node = vfs_open(try_abs, 0);
                        if (node) { strncpy(abs, try_abs, sizeof(abs)-1); abs[sizeof(abs)-1] = '\0'; }
                    }
                    if (!*pp && !node) break;
                }
                /* Fallback standard dirs */
                for (int fi = 0; !node && fallback[fi]; fi++) {
                    char try_abs[512];
                    ksnprintf(try_abs, sizeof(try_abs), "%s/%s", fallback[fi], path);
                    node = vfs_open(try_abs, 0);
                    if (node) { strncpy(abs, try_abs, sizeof(abs)-1); abs[sizeof(abs)-1] = '\0'; }
                }
            }
            if (!node) return (uint64_t)-ENOENT;

            /* Record executable path for /proc/self/exe */
            strncpy(current_task->exec_path, abs, sizeof(current_task->exec_path) - 1);
            current_task->exec_path[sizeof(current_task->exec_path) - 1] = '\0';

            // Copy argv/envp strings from user space (up to 64 each)
            #define EXEC_MAX_ARGS 64
            #define EXEC_STR_MAX  512
            // Flat kernel buffer for all strings; pointer arrays on stack (small)
            char *exec_strbuf = kmalloc((size_t)(EXEC_MAX_ARGS * 2 * EXEC_STR_MAX));
            const char *exec_argv[EXEC_MAX_ARGS + 1];
            const char *exec_envp[EXEC_MAX_ARGS + 1];
            int argc = 0, envc = 0;

            if (exec_strbuf) {
                if (arg2) {
                    const uint64_t *user_argv = (const uint64_t *)arg2;
                    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
                        uint64_t uptr = 0;
                        if (copy_from_user(&uptr, &user_argv[i], 8) < 0 || !uptr) break;
                        char *dst = exec_strbuf + argc * EXEC_STR_MAX;
                        copy_string_from_user(dst, (const char *)uptr, EXEC_STR_MAX);
                        exec_argv[argc++] = dst;
                    }
                }
                if (arg3) {
                    const uint64_t *user_envp = (const uint64_t *)arg3;
                    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
                        uint64_t uptr = 0;
                        if (copy_from_user(&uptr, &user_envp[i], 8) < 0 || !uptr) break;
                        char *dst = exec_strbuf + (EXEC_MAX_ARGS + envc) * EXEC_STR_MAX;
                        copy_string_from_user(dst, (const char *)uptr, EXEC_STR_MAX);
                        exec_envp[envc++] = dst;
                    }
                }
            }
            exec_argv[argc] = NULL;
            exec_envp[envc] = NULL;

            // Close O_CLOEXEC fds before replacing process image
            if (current_task->fd_table) {
                for (int i = 0; i < MAX_FDS; i++) {
                    fd_entry_t *ce2 = &current_task->fd_table->entries[i];
                    if (ce2->type != FD_NONE && (ce2->flags & O_CLOEXEC)) {
                        if (ce2->type == FD_PIPE && ce2->pipe)
                            pipe_close((pipe_t *)ce2->pipe, (ce2->flags & O_WRONLY) ? 1 : 0);
                        else if (ce2->type == FD_PTY_MASTER && ce2->pipe)
                            pty_close_master((pty_t *)ce2->pipe);
                        else if (ce2->type == FD_PTY_SLAVE && ce2->pipe)
                            pty_close_slave((pty_t *)ce2->pipe);
                        fd_free(current_task->fd_table, i);
                    }
                }
            }

            uint8_t *buf = kmalloc(node->length);
            if (!buf) { kfree(exec_strbuf); return (uint64_t)-ENOMEM; }
            vfs_read(node, 0, node->length, buf);

            /* Shebang support: if file starts with '#!' redirect to interpreter */
            if (node->length >= 2 && buf[0] == '#' && buf[1] == '!') {
                /* Parse interpreter and optional argument from first line */
                char interp[256] = {0};
                char iarg[256]   = {0};
                int ii = 0, ia = 0;
                size_t pos = 2;
                /* Skip spaces after #! */
                while (pos < node->length && buf[pos] == ' ') pos++;
                /* Read interpreter path */
                while (pos < node->length && buf[pos] != ' ' &&
                       buf[pos] != '\n' && buf[pos] != '\r' && ii < 255)
                    interp[ii++] = (char)buf[pos++];
                interp[ii] = '\0';
                /* Optional argument */
                while (pos < node->length && buf[pos] == ' ') pos++;
                while (pos < node->length && buf[pos] != '\n' &&
                       buf[pos] != '\r' && ia < 255)
                    iarg[ia++] = (char)buf[pos++];
                /* Trim trailing spaces from optional arg */
                while (ia > 0 && iarg[ia-1] == ' ') ia--;
                iarg[ia] = '\0';

                if (ii > 0) {
                    /* Build new argv: [interp, iarg?, abs, ...original argv[1]...] */
                    #define SB_MAX 66
                    const char *sb_argv[SB_MAX + 1];
                    int sb_argc = 0;
                    sb_argv[sb_argc++] = interp;
                    if (ia > 0) sb_argv[sb_argc++] = iarg;
                    sb_argv[sb_argc++] = abs; /* original script path */
                    /* Append original argv[1..] */
                    for (int i = 1; exec_argv[i] && sb_argc < SB_MAX; i++)
                        sb_argv[sb_argc++] = exec_argv[i];
                    sb_argv[sb_argc] = NULL;

                    /* Open interpreter */
                    char iabs[512];
                    vfs_resolve_path(current_task->cwd, interp, iabs, sizeof(iabs));
                    vfs_node_t *inode = vfs_open(iabs, 0);
                    if (inode) {
                        uint8_t *ibuf = kmalloc(inode->length);
                        if (ibuf) {
                            vfs_read(inode, 0, inode->length, ibuf);
                            kfree(buf);
                            int r2 = task_execve((registers_t *)regs, ibuf, inode->length,
                                                 sb_argv, exec_envp);
                            kfree(ibuf);
                            kfree(exec_strbuf);
                            if (r2 < 0) return (uint64_t)-ENOMEM;
                            return 0;
                        }
                        kfree(ibuf);
                    }
                    /* interpreter not found — fall through to ELF attempt */
                }
            }

            int r = task_execve((registers_t *)regs, buf, node->length, exec_argv, exec_envp);
            kfree(buf);
            kfree(exec_strbuf);
            if (r < 0) return (uint64_t)-ENOMEM;
            return 0; // iretq returns to new program
        }

        case SYS_FCHDIR: { // 81 — change cwd to the directory referenced by fd
            int dirfd = (int)arg1;
            fd_entry_t *dfe = fd_get(current_task->fd_table, dirfd);
            if (!dfe || dfe->type != FD_FILE) return (uint64_t)-EBADF;
            if (!dfe->node || !(dfe->node->flags & VFS_DIRECTORY)) return (uint64_t)-ENOTDIR;
            if (dfe->dir_path[0] == '\0') return (uint64_t)-ENOSYS; /* no path cached */
            strncpy(current_task->cwd, dfe->dir_path, sizeof(current_task->cwd) - 1);
            current_task->cwd[sizeof(current_task->cwd) - 1] = '\0';
            return 0;
        }

        case SYS_MKDIR: { // 83 — arg1=path, arg2=mode (ignored)
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            int r = vfs_mkdir(abs);
            if (r < 0) return (uint64_t)-ENOENT;
            return 0;
        }

        case SYS_UNLINK: { // 87
            char path[512], abs[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return (uint64_t)-EFAULT;
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            return vfs_unlink(abs) == 0 ? 0 : (uint64_t)-ENOENT;
        }

        case SYS_RENAME: { // 82 — arg1=old, arg2=new
            char old[512], old_abs[512], nw[512], nw_abs[512];
            if (copy_string_from_user(old, (const char *)arg1, sizeof(old)) < 0) return (uint64_t)-EFAULT;
            if (copy_string_from_user(nw,  (const char *)arg2, sizeof(nw))  < 0) return (uint64_t)-EFAULT;
            vfs_resolve_path(current_task->cwd, old, old_abs, sizeof(old_abs));
            vfs_resolve_path(current_task->cwd, nw,  nw_abs,  sizeof(nw_abs));
            return vfs_rename(old_abs, nw_abs) == 0 ? 0 : (uint64_t)-ENOENT;
        }

        case SYS_RMDIR: { // 84
            char path[512], abs[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            return vfs_rmdir(abs) == 0 ? 0 : (uint64_t)-ENOENT;
        }

        // ── Pipe & FD operations ──────────────────────────────────────────────

        case SYS_PIPE: { // 22 — arg1=int[2] user buffer
            if (!is_user_address(arg1, 8)) return (uint64_t)-EFAULT;
            int rfd, wfd;
            if (pipe_create(current_task->fd_table, &rfd, &wfd) < 0)
                return (uint64_t)-ENOMEM;
            int fds[2] = {rfd, wfd};
            if (copy_to_user((void *)arg1, fds, sizeof(fds)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }

        case SYS_DUP: { // 32
            int nfd = fd_dup(current_task->fd_table, (int)arg1);
            return (nfd < 0) ? (uint64_t)-EBADF : (uint64_t)nfd;
        }

        case SYS_DUP2: { // 33 — if old==new just validate old
            int old = (int)arg1, nw = (int)arg2;
            if (old == nw) {
                if (!fd_get(current_task->fd_table, old)) return (uint64_t)-EBADF;
                return (uint64_t)nw;
            }
            // Pipe-aware close of new_fd if open
            fd_entry_t *old_e = fd_get(current_task->fd_table, nw);
            if (old_e && old_e->type == FD_PIPE && old_e->pipe)
                pipe_close((pipe_t *)old_e->pipe, (old_e->flags & O_WRONLY) ? 1 : 0);
            int r = fd_dup2(current_task->fd_table, old, nw);
            return (r < 0) ? (uint64_t)-EBADF : (uint64_t)r;
        }

        case SYS_LSEEK: { // 8 — arg1=fd, arg2=offset, arg3=whence
            fd_entry_t *le = fd_get(current_task->fd_table, (int)arg1);
            if (!le) return (uint64_t)-EBADF;
            if (le->type == FD_PIPE) return (uint64_t)-ESPIPE;
            if (le->type != FD_FILE) return (uint64_t)-ESPIPE;
            int64_t off = (int64_t)arg2;
            int64_t new_off;
            switch ((int)arg3) {
                case SEEK_SET: new_off = off; break;
                case SEEK_CUR: new_off = (int64_t)le->offset + off; break;
                case SEEK_END: new_off = (int64_t)le->node->length + off; break;
                default: return (uint64_t)-EINVAL;
            }
            if (new_off < 0) return (uint64_t)-EINVAL;
            le->offset = (size_t)new_off;
            return (uint64_t)new_off;
        }

        case SYS_FLOCK: // 73 — advisory file lock; no-op (always succeed)
        {
            fd_entry_t *flke = fd_get(current_task->fd_table, (int)arg1);
            return flke ? 0 : (uint64_t)-EBADF;
        }

        case SYS_CREAT: { // 85 — creat(path, mode) = open(path, O_CREAT|O_WRONLY|O_TRUNC, mode)
            char creat_path[512];
            if (copy_string_from_user(creat_path, (const char *)arg1, sizeof(creat_path)) < 0)
                return (uint64_t)-EFAULT;
            char creat_abs[512];
            vfs_resolve_path(current_task->cwd, creat_path, creat_abs, sizeof(creat_abs));
            vfs_node_t *creat_node = vfs_open(creat_abs, O_CREAT | O_WRONLY | O_TRUNC);
            if (!creat_node) return (uint64_t)-ENOENT;
            int creat_fd = fd_alloc(current_task->fd_table);
            if (creat_fd < 0) return (uint64_t)-EMFILE;
            fd_entry_t *creat_fe = &current_task->fd_table->entries[creat_fd];
            creat_fe->type   = FD_FILE;
            creat_fe->node   = creat_node;
            creat_fe->offset = 0;
            creat_fe->flags  = O_WRONLY | O_CREAT | O_TRUNC;
            return (uint64_t)creat_fd;
        }

        case SYS_TIMES: { // 100 — struct tms: utime, stime, cutime, cstime (all clock_t)
            if (arg1 && is_user_address(arg1, 32)) {
                uint64_t zeros[4] = {0, 0, 0, 0};
                copy_to_user((void *)arg1, zeros, 32);
            }
            return timer_get_ticks(); // clock ticks since boot
        }

        case SYS_FCNTL: { // 72 — arg1=fd, arg2=cmd, arg3=arg
            int fd = (int)arg1, cmd = (int)arg2;
            fd_entry_t *fe = fd_get(current_task->fd_table, fd);
            if (!fe) return (uint64_t)-EBADF;
            switch (cmd) {
                case 0: // F_DUPFD
                case 1030: { // F_DUPFD_CLOEXEC
                    int nfd = fd_dup(current_task->fd_table, fd);
                    if (nfd < 0) return (uint64_t)-EMFILE;
                    if (cmd == 1030) current_task->fd_table->entries[nfd].flags |= O_CLOEXEC;
                    return (uint64_t)nfd;
                }
                case 1: // F_GETFD
                    return (fe->flags & O_CLOEXEC) ? 1 : 0;
                case 2: // F_SETFD  (FD_CLOEXEC = 1)
                    if (arg3 & 1) fe->flags |= O_CLOEXEC;
                    else          fe->flags &= ~(uint32_t)O_CLOEXEC;
                    return 0;
                case 3: // F_GETFL
                    return (uint64_t)(fe->flags & ~(uint32_t)O_CLOEXEC);
                case 4: // F_SETFL
                    fe->flags = (fe->flags & O_CLOEXEC) | ((uint32_t)arg3 & ~(uint32_t)O_CLOEXEC);
                    return 0;
                default:
                    return (uint64_t)-EINVAL;
            }
        }

        case SYS_GETPID:  // 39 — return tgid (process id for whole thread group)
            return current_task ? (uint64_t)current_task->tgid : (uint64_t)current_pid;

        case SYS_GETPPID: // 40 (Linux compat) / 110 (Linux GETPPID on some archs)
            return current_task ? (uint64_t)current_task->parent_pid : 0;

        case SYS_KILL: { // 62 — arg1=pid, arg2=sig
            if (arg2 == 0) return 0; // Signal 0 = existence check
            return (uint64_t)signal_send((uint32_t)arg1, (int)arg2);
        }

        case SYS_RT_SIGACTION: { // 13 — arg1=sig, arg2=*act, arg3=*oldact
            int sig = (int)arg1;
            if (sig < 1 || sig >= NSIG) return (uint64_t)-EINVAL;
            /* Return old handler (zeroed) */
            if (arg3 && is_user_address(arg3, 32)) {
                uint8_t zero[32];
                memset(zero, 0, 32);
                copy_to_user((void *)arg3, zero, 32);
            }
            if (arg2 && is_user_address(arg2, 32)) {
                uint64_t handler = 0, flags = 0, restorer = 0;
                copy_from_user(&handler,  (void *)arg2,       8);
                copy_from_user(&flags,    (void *)(arg2 + 8), 8);
                copy_from_user(&restorer, (void *)(arg2 + 16), 8);
                signal_set_userhandler(current_pid, sig, handler, restorer, (uint32_t)flags);
            }
            return 0;
        }

        case SYS_RT_SIGPROCMASK: { // 14 — arg1=how, arg2=*set, arg3=*oldset
            int how = (int)arg1;
            /* Return old mask */
            if (arg3 && is_user_address(arg3, 8))
                copy_to_user((void *)arg3, &current_task->sig_mask, 8);
            if (arg2 && is_user_address(arg2, 8)) {
                uint64_t new_mask = 0;
                copy_from_user(&new_mask, (void *)arg2, 8);
                if      (how == 0) current_task->sig_mask |=  new_mask;  /* SIG_BLOCK   */
                else if (how == 1) current_task->sig_mask &= ~new_mask;  /* SIG_UNBLOCK */
                else if (how == 2) current_task->sig_mask  =  new_mask;  /* SIG_SETMASK */
                /* SIGKILL and SIGSTOP are never blockable */
                current_task->sig_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
            }
            return 0;
        }

        case SYS_RT_SIGRETURN: { // 15 — restore context from rt_sigframe
            /* RSP = sigframe_base + 8 (after restorer popped pretcode via ret) */
            uint64_t frame_base = regs->rsp - 8;
            /* sigcontext starts at frame_base + 8(pretcode) + 40(uc header) = +48 */
            uint64_t sc = frame_base + 48;
            if (!is_user_address(sc, 192)) return (uint64_t)-EFAULT;
            uint64_t tmp;
            #define RREGS(off, dst) copy_from_user(&tmp, (void*)(sc+(off)), 8); dst = tmp
            RREGS(  0, regs->r8);   RREGS(  8, regs->r9);
            RREGS( 16, regs->r10);  RREGS( 24, regs->r11);
            RREGS( 32, regs->r12);  RREGS( 40, regs->r13);
            RREGS( 48, regs->r14);  RREGS( 56, regs->r15);
            RREGS( 64, regs->rdi);  RREGS( 72, regs->rsi);
            RREGS( 80, regs->rbp);  RREGS( 88, regs->rbx);
            RREGS( 96, regs->rdx);  RREGS(104, regs->rax);
            RREGS(112, regs->rcx);  RREGS(120, regs->rsp);
            RREGS(128, regs->rip);
            copy_from_user(&tmp, (void *)(sc + 136), 8);
            regs->rflags = tmp;
            #undef RREGS
            /* Restore sigmask from uc_sigmask at frame_base + 304 */
            uint64_t old_mask = 0;
            copy_from_user(&old_mask, (void *)(frame_base + 304), 8);
            current_task->sig_mask = old_mask &
                ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
            return regs->rax; /* return value comes from saved rax */
        }

        case SYS_MPROTECT: // 10 — stub (VMAs don't enforce perms yet)
            return 0;

        case SYS_NANOSLEEP: { // 35 — arg1=*timespec, arg2=*rem (ignored)
            // struct timespec { int64_t tv_sec; int64_t tv_nsec; }
            if (!is_user_address(arg1, 16)) return (uint64_t)-EFAULT;
            int64_t ts[2] = {0, 0};
            copy_from_user(ts, (void *)arg1, 16);
            uint32_t ms = (uint32_t)(ts[0] * 1000 + ts[1] / 1000000);
            if (ms > 0) timer_sleep(ms);
            return 0;
        }

        case SYS_ACCESS: { // 21 — arg1=path, arg2=mode (mode ignored)
            char path[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0) return (uint64_t)-EFAULT;
            char abs[512];
            vfs_resolve_path(current_task->cwd, path, abs, sizeof(abs));
            vfs_node_t *an = vfs_open(abs, 0);
            return an ? 0 : (uint64_t)-ENOENT;
        }

        case SYS_READV: { // 19 — arg1=fd, arg2=iov[], arg3=iovcnt
            fd_entry_t *rv_e = fd_get(current_task->fd_table, (int)arg1);
            if (!rv_e) return (uint64_t)-EBADF;
            if (arg3 > 1024) return (uint64_t)-EINVAL;
            if (!is_user_address(arg2, arg3 * 16)) return (uint64_t)-EFAULT;
            int64_t total = 0;
            for (uint64_t i = 0; i < arg3; i++) {
                uint64_t iov[2];
                copy_from_user(iov, (void *)(arg2 + i * 16), 16);
                if (!iov[1]) continue;
                regs->rdi = arg1; regs->rsi = iov[0]; regs->rdx = iov[1];
                int64_t r = (int64_t)syscall_handler(SYS_READ, regs);
                if (r < 0) { if (total == 0) total = r; break; }
                total += r;
                if ((uint64_t)r < iov[1]) break; // Short read
            }
            return (uint64_t)total;
        }

        case SYS_WRITEV: { // 20 — arg1=fd, arg2=iov[], arg3=iovcnt
            fd_entry_t *wv_e = fd_get(current_task->fd_table, (int)arg1);
            if (!wv_e) return (uint64_t)-EBADF;
            if (arg3 > 1024) return (uint64_t)-EINVAL;
            if (!is_user_address(arg2, arg3 * 16)) return (uint64_t)-EFAULT;
            int64_t total = 0;
            for (uint64_t i = 0; i < arg3; i++) {
                uint64_t iov[2];
                copy_from_user(iov, (void *)(arg2 + i * 16), 16);
                if (!iov[1]) continue;
                regs->rdi = arg1; regs->rsi = iov[0]; regs->rdx = iov[1];
                int64_t r = (int64_t)syscall_handler(SYS_WRITE, regs);
                if (r < 0) { if (total == 0) total = r; break; }
                total += r;
            }
            return (uint64_t)total;
        }

        case SYS_UNAME: { // 63 — arg1=*utsname (390-byte struct in Linux)
            // struct utsname: sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]
            if (!is_user_address(arg1, 390)) return (uint64_t)-EFAULT;
            char ubuf[390];
            memset(ubuf, 0, sizeof(ubuf));
            const char *fields[6] = { "Linux", "raeenos", "5.15.0-raeenos", "#1 SMP", "x86_64", "raeenos.local" };
            for (int f = 0; f < 6; f++) {
                const char *s = fields[f];
                char *dst = ubuf + f * 65;
                int j = 0;
                while (s[j] && j < 64) { dst[j] = s[j]; j++; }
            }
            copy_to_user((void *)arg1, ubuf, sizeof(ubuf));
            return 0;
        }

        case SYS_FSTAT: { // 5 — arg1=fd, arg2=*stat
            fd_entry_t *fs_e = fd_get(current_task->fd_table, (int)arg1);
            if (!fs_e) return (uint64_t)-EBADF;
            struct {
                uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
                uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t st_size;
                uint64_t st_blksize; uint64_t st_blocks; uint64_t st_atime; uint64_t st_mtime;
                uint64_t st_ctime;
            } kst;
            memset(&kst, 0, sizeof(kst));
            if (fs_e->type == FD_FILE && fs_e->node) {
                kst.st_ino = fs_e->node->inode;
                kst.st_size = fs_e->node->length;
                kst.st_mode = (fs_e->node->flags & VFS_DIRECTORY) ? 0040755 : 0100644;
            } else if (fs_e->type == FD_PIPE) {
                kst.st_mode = 0010666; /* S_IFIFO */
            } else if (fs_e->type == FD_PTY_MASTER || fs_e->type == FD_PTY_SLAVE) {
                kst.st_mode  = 0020666; /* S_IFCHR — isatty() checks this */
                kst.st_rdev  = 0x0500 | (fs_e->type == FD_PTY_SLAVE ?
                               ((pty_t *)fs_e->pipe)->index : 0);
            } else if (fs_e->type == FD_SOCKET) {
                kst.st_mode = 0140666; /* S_IFSOCK */
            } else {
                kst.st_mode = 0020666; /* S_IFCHR */
                kst.st_rdev = 0x101;
            }
            if (copy_to_user((void *)arg2, &kst, sizeof(kst)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }

        default:
            kprintf("[SYSCALL] Unknown #%lu from PID %u at RIP %lx\n", num, current_pid, regs->rip);
            return (uint64_t)-1;
    }
}

uint64_t syscall_handler(uint64_t num, struct interrupt_frame *regs) {
    uint32_t current_pid = task_current_id();
    task_t *current_task = task_get_by_id(current_pid);

    if (current_task && current_task->abi == ABI_LINUX) {
        return linux_syscall_handler(regs);
    }

    return native_syscall_handler(num, regs);
}
