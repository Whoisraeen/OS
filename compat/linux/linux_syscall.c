#include "linux_syscalls.h"
#include "../../sched.h"
#include "../../syscall.h"
#include "../../idt.h"
#include "../../net/net.h"
#include "../../serial.h"
#include "../../vfs.h"
#include "../../fd.h"
#include "../../string.h"
#include "../../heap.h"
#include "../../vmm.h"
#include "../../pmm.h"
#include "../../vm_area.h"
#include "../../signal.h"
#include "../../pipe.h"
#include "../../pty.h"
#include "../../futex.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../elf.h"

/* Forward declaration */
extern uint64_t native_syscall_handler(uint64_t num, struct interrupt_frame *regs);

// ── Linux errno values ────────────────────────────────────────────────────────
#define LINUX_EPERM     1
#define LINUX_ENOENT    2
#define LINUX_ESRCH     3
#define LINUX_EINTR     4
#define LINUX_EBADF     9
#define LINUX_ECHILD    10
#define LINUX_EAGAIN    11
#define LINUX_ENOMEM    12
#define LINUX_EFAULT    14
#define LINUX_ENOTDIR   20
#define LINUX_EISDIR    21
#define LINUX_EINVAL    22
#define LINUX_EMFILE    24
#define LINUX_ENOSPC    28
#define LINUX_ERANGE    34
#define LINUX_ENOSYS    38
#define LINUX_ENOTSOCK  88
#define LINUX_ENOTSUP   95

#define LINUX_ENEG(e)  ((uint64_t)(-(int64_t)(e)))

// ── Linux ABI struct definitions ──────────────────────────────────────────────

// x86_64 struct stat (144 bytes) — must match glibc exactly
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;     // 512-byte units
    uint64_t st_atime;
    uint64_t st_atime_ns;
    uint64_t st_mtime;
    uint64_t st_mtime_ns;
    uint64_t st_ctime;
    uint64_t st_ctime_ns;
    int64_t  __unused[3];
} __attribute__((packed)) linux_stat_t;

// struct linux_dirent64 for getdents64
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} __attribute__((packed)) linux_dirent64_t;

// struct linux_dirent (old getdents, 32-bit d_ino but 64-bit syscall)
typedef struct {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    char     d_name[256];
    uint8_t  d_type;
} __attribute__((packed)) linux_dirent_t;

// struct timespec
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;

// struct timeval
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} linux_timeval_t;

// struct iovec
typedef struct {
    void    *iov_base;
    uint64_t iov_len;
} linux_iovec_t;

// struct rlimit
typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} linux_rlimit_t;

// struct utsname
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

// struct sigaction (rt_sigaction, 8-byte mask × 2 = 128-bit set)
typedef struct {
    uint64_t sa_handler;    // SIG_DFL=0, SIG_IGN=1, or function ptr
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask[2];
} __attribute__((packed)) linux_sigaction_t;

// ── IOCTL request codes ───────────────────────────────────────────────────────
#define TCGETS       0x5401
#define TCSETS       0x5402
#define TCSETSW      0x5403
#define TCSETSF      0x5404
#define TIOCGWINSZ   0x5413
#define TIOCSWINSZ   0x5414
#define TIOCGPGRP    0x540F
#define TIOCSPGRP    0x5410
#define FIONREAD     0x541B
#define TIOCGISATTY  0x5480

// ── FCNTL commands ────────────────────────────────────────────────────────────
#define F_DUPFD      0
#define F_GETFD      1
#define F_SETFD      2
#define F_GETFL      3
#define F_SETFL      4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC   1

// ── CLONE flags ───────────────────────────────────────────────────────────────
#define CLONE_VM         0x00000100
#define CLONE_FS         0x00000200
#define CLONE_FILES      0x00000400
#define CLONE_SIGHAND    0x00000800
#define CLONE_THREAD     0x00010000
#define CLONE_SETTLS     0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

// ── RLIMIT resources ──────────────────────────────────────────────────────────
#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9

// ── PRCTL options ─────────────────────────────────────────────────────────────
#define PR_SET_DUMPABLE  4
#define PR_GET_DUMPABLE  3
#define PR_SET_NAME      15
#define PR_GET_NAME      16
#define PR_SET_KEEPCAPS  8
#define PR_SET_SECCOMP   22

// ── Helper: fill Linux stat from vfs_node ────────────────────────────────────
static void fill_linux_stat(linux_stat_t *s, vfs_node_t *node)
{
    memset(s, 0, sizeof(*s));
    s->st_dev     = 1;
    s->st_ino     = node->inode ? node->inode : 1;
    s->st_nlink   = 1;
    s->st_mode    = (node->flags & VFS_DIRECTORY) ? (0040000 | 0755)
                                                   : (0100000 | 0644);
    s->st_size    = (int64_t)node->length;
    s->st_blksize = 4096;
    s->st_blocks  = (s->st_size + 511) / 512;
    uint64_t now  = rtc_get_timestamp();
    s->st_atime   = now;
    s->st_mtime   = now;
    s->st_ctime   = now;
}

// ── Pseudo-random state (for getrandom) ──────────────────────────────────────
static uint64_t prng_state = 0;

static uint8_t prng_byte(void)
{
    if (!prng_state) prng_state = rtc_get_timestamp() ^ 0xDEADCAFEBABE1234ULL;
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 7;
    prng_state ^= prng_state << 17;
    return (uint8_t)prng_state;
}

// ── Main Linux syscall dispatcher ─────────────────────────────────────────────
uint64_t linux_syscall_handler(struct interrupt_frame *regs)
{
    uint64_t num = regs->rax;
    uint32_t pid = task_current_id();
    task_t  *task = task_get_by_id(pid);

    // Linux ABI: args in RDI, RSI, RDX, R10, R8, R9
    uint64_t arg1 = regs->rdi;
    uint64_t arg2 = regs->rsi;
    uint64_t arg3 = regs->rdx;
    uint64_t arg4 = regs->r10;
    uint64_t arg5 = regs->r8;
    (void)arg5;
    // uint64_t arg6 = regs->r9;   (unused for now)

    if (!task) return LINUX_ENEG(LINUX_ESRCH);

    switch (num) {

        // ── Direct pass-through (same number & semantics as native) ────────────
        case LINUX_SYS_READ:        // 0
        case LINUX_SYS_WRITE:       // 1
        case LINUX_SYS_OPEN:        // 2
        case LINUX_SYS_CLOSE:       // 3
        case LINUX_SYS_POLL:        // 7
        case LINUX_SYS_LSEEK:       // 8
        case LINUX_SYS_MMAP:        // 9
        case LINUX_SYS_MUNMAP:      // 11
        case LINUX_SYS_BRK:         // 12
        case LINUX_SYS_SELECT:      // 23
        case LINUX_SYS_SCHED_YIELD: // 24
        case LINUX_SYS_GETPID:      // 39
        case LINUX_SYS_WAIT4:       // 61
        case LINUX_SYS_RENAME:      // 82
        case LINUX_SYS_RMDIR:       // 84
        case LINUX_SYS_UNLINK:      // 87
        case LINUX_SYS_GETDENTS64:  // 217
            return native_syscall_handler(num, regs);

        case LINUX_SYS_EXIT:        // 60
            return native_syscall_handler(SYS_EXIT, regs);

        case LINUX_SYS_EXIT_GROUP:  // 231 — kill all threads in the group
            return native_syscall_handler(231, regs);

        case LINUX_SYS_MOUNT:   // 165 — stub: we already have devfs/procfs mounted
        case LINUX_SYS_UMOUNT2: // 166 — stub
            return 0;

        case LINUX_SYS_REBOOT:  // 169 — delegate to native power-off
            return syscall_handler(169, regs);

        case LINUX_SYS_ARCH_PRCTL:  // 158
            return native_syscall_handler(SYS_ARCH_PRCTL, regs);

        case LINUX_SYS_CLOCK_GETTIME: // 228
            return native_syscall_handler(SYS_CLOCK_GETTIME, regs);

        case LINUX_SYS_FORK:   // 57
        case LINUX_SYS_VFORK:  // 58
            return (uint64_t)task_fork((registers_t *)regs);

        // ── stat family ───────────────────────────────────────────────────────

        case LINUX_SYS_STAT: { // 4
            char path[512], abs[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            vfs_resolve_path(task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            if (!node) return LINUX_ENEG(LINUX_ENOENT);
            linux_stat_t s;
            fill_linux_stat(&s, node);
            if (copy_to_user((void *)arg2, &s, sizeof(s)) < 0) return LINUX_ENEG(LINUX_EFAULT);
            return 0;
        }

        case LINUX_SYS_FSTAT: { // 5
            fd_entry_t *e = fd_get(task->fd_table, (int)arg1);
            if (!e) return LINUX_ENEG(LINUX_EBADF);
            linux_stat_t s;
            if (e->type == FD_FILE && e->node) {
                fill_linux_stat(&s, e->node);
            } else {
                // Pipe / device / socket: report as character device
                memset(&s, 0, sizeof(s));
                s.st_mode    = (e->type == FD_PIPE) ? (0010000 | 0666) : (0020000 | 0666);
                s.st_rdev    = 0x101;
                s.st_blksize = 4096;
            }
            if (copy_to_user((void *)arg2, &s, sizeof(s)) < 0) return LINUX_ENEG(LINUX_EFAULT);
            return 0;
        }

        case LINUX_SYS_LSTAT: { // 6 — same as stat (no symlinks yet)
            char path[512], abs[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            vfs_resolve_path(task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            if (!node) return LINUX_ENEG(LINUX_ENOENT);
            linux_stat_t s;
            fill_linux_stat(&s, node);
            if (copy_to_user((void *)arg2, &s, sizeof(s)) < 0) return LINUX_ENEG(LINUX_EFAULT);
            return 0;
        }

        case LINUX_SYS_FSTATAT: { // 262 — uses AT_FDCWD + relative path
            char path[512], abs[512];
            if (copy_string_from_user(path, (const char *)arg2, sizeof(path)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            vfs_resolve_path(task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            if (!node) return LINUX_ENEG(LINUX_ENOENT);
            linux_stat_t s;
            fill_linux_stat(&s, node);
            if (copy_to_user((void *)arg3, &s, sizeof(s)) < 0) return LINUX_ENEG(LINUX_EFAULT);
            return 0;
        }

        // ── Memory ────────────────────────────────────────────────────────────

        case LINUX_SYS_MPROTECT: // 10 — VMAs don't enforce strict perms yet
            return 0;

        case LINUX_SYS_MREMAP: // 25 — stub: return same address
            return arg1;

        case LINUX_SYS_MADVISE: // 28 — hint ignored
            return 0;

        // ── Signals ───────────────────────────────────────────────────────────

        case LINUX_SYS_RT_SIGACTION: { // 13
            int sig = (int)arg1;
            if (sig < 1 || sig >= NSIG) return LINUX_ENEG(LINUX_EINVAL);

            /* Return the previously-installed action if oldact pointer provided */
            if (arg3 && is_user_address(arg3, sizeof(linux_sigaction_t))) {
                linux_sigaction_t old;
                memset(&old, 0, sizeof(old));
                if (sig >= 1 && sig < NSIG) {
                    old.sa_handler  = task->sig_handlers[sig];
                    old.sa_restorer = task->sig_restorer[sig];
                    old.sa_flags    = task->sig_flags[sig];
                }
                if (copy_to_user((void *)arg3, &old, sizeof(old)) < 0)
                    return LINUX_ENEG(LINUX_EFAULT);
            }
            if (arg2 && is_user_address(arg2, sizeof(linux_sigaction_t))) {
                linux_sigaction_t act;
                if (copy_from_user(&act, (void *)arg2, sizeof(act)) < 0)
                    return LINUX_ENEG(LINUX_EFAULT);
                /* Store the full handler + restorer so signal_try_deliver_frame
                 * can build a proper rt_sigframe on the user stack. */
                signal_set_userhandler(pid, sig, act.sa_handler,
                                       act.sa_restorer, (uint32_t)act.sa_flags);
            }
            return 0;
        }

        case LINUX_SYS_RT_SIGPROCMASK: // 14 — delegate to native
            return native_syscall_handler(SYS_RT_SIGPROCMASK, regs);

        case LINUX_SYS_RT_SIGRETURN: // 15 — delegate to native (restores sigframe)
            return native_syscall_handler(SYS_RT_SIGRETURN, regs);
        case LINUX_SYS_SIGRETURN:    // 117 — old sigreturn (also delegate)
            return native_syscall_handler(SYS_RT_SIGRETURN, regs);

        case LINUX_SYS_SIGALTSTACK:  // 131 — stub
            return 0;

        case LINUX_SYS_KILL: { // 62
            int ret = signal_send((uint32_t)arg1, (int)arg2);
            return (ret == 0) ? 0 : LINUX_ENEG(LINUX_ESRCH);
        }

        case LINUX_SYS_TKILL:  // 200
        case LINUX_SYS_TGKILL: { // 234 — arg1=tgid, arg2=tid, arg3=sig (TGKILL)
            uint32_t target_tid = (num == LINUX_SYS_TGKILL) ? (uint32_t)arg2 : (uint32_t)arg1;
            int sig             = (num == LINUX_SYS_TGKILL) ? (int)arg3 : (int)arg2;
            int ret = signal_send(target_tid, sig);
            return (ret == 0) ? 0 : LINUX_ENEG(LINUX_ESRCH);
        }

        // ── I/O misc ─────────────────────────────────────────────────────────

        case LINUX_SYS_IOCTL: { // 16 — delegate to native SYS_IOCTL
            // The native handler now fully handles TCGETS/TCSETS, TIOCGWINSZ,
            // TIOCGPTN, TIOCSPTLCK, TIOCGPGRP, TIOCSPGRP, FIONREAD.
            return native_syscall_handler(SYS_IOCTL, regs);
        }

        case LINUX_SYS_READV: { // 19
            int fd     = (int)arg1;
            int iovcnt = (int)arg3;
            if (iovcnt <= 0 || iovcnt > 1024) return LINUX_ENEG(LINUX_EINVAL);
            if (!is_user_address(arg2, (uint64_t)iovcnt * sizeof(linux_iovec_t)))
                return LINUX_ENEG(LINUX_EFAULT);
            int64_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                linux_iovec_t iov;
                copy_from_user(&iov, (uint8_t *)arg2 + i * sizeof(linux_iovec_t), sizeof(iov));
                if (!iov.iov_len) continue;
                regs->rdi = (uint64_t)fd;
                regs->rsi = (uint64_t)iov.iov_base;
                regs->rdx = iov.iov_len;
                int64_t r = (int64_t)native_syscall_handler(SYS_READ, regs);
                if (r <= 0) { if (total == 0) total = r; break; }
                total += r;
            }
            return (uint64_t)total;
        }

        case LINUX_SYS_WRITEV: { // 20
            int fd     = (int)arg1;
            int iovcnt = (int)arg3;
            if (iovcnt <= 0 || iovcnt > 1024) return LINUX_ENEG(LINUX_EINVAL);
            if (!is_user_address(arg2, (uint64_t)iovcnt * sizeof(linux_iovec_t)))
                return LINUX_ENEG(LINUX_EFAULT);
            int64_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                linux_iovec_t iov;
                copy_from_user(&iov, (uint8_t *)arg2 + i * sizeof(linux_iovec_t), sizeof(iov));
                if (!iov.iov_len) continue;
                regs->rdi = (uint64_t)fd;
                regs->rsi = (uint64_t)iov.iov_base;
                regs->rdx = iov.iov_len;
                int64_t r = (int64_t)native_syscall_handler(SYS_WRITE, regs);
                if (r < 0) { if (total == 0) total = r; break; }
                total += r;
            }
            return (uint64_t)total;
        }

        case LINUX_SYS_PREAD64:  // 17 — delegate to native
        case LINUX_SYS_PWRITE64: // 18 — delegate to native
            return native_syscall_handler(num, regs);

        case LINUX_SYS_ACCESS: { // 21
            char path[512], abs[512];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            vfs_resolve_path(task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            return node ? 0 : LINUX_ENEG(LINUX_ENOENT);
        }

        case LINUX_SYS_PIPE: // 22 — delegate to native SYS_PIPE
            return native_syscall_handler(SYS_PIPE, regs);

        case LINUX_SYS_DUP:  // 32 — delegate
        case LINUX_SYS_DUP2: // 33 — delegate
            return native_syscall_handler(num, regs);

        case LINUX_SYS_DUP3: { // 292 — dup2 + optional O_CLOEXEC
            // arg1=oldfd, arg2=newfd, arg3=flags
            if ((int)arg1 == (int)arg2) return LINUX_ENEG(LINUX_EINVAL);
            // Reuse DUP2 logic via native
            regs->rdx = 0; // clear flags temporarily for dup2
            uint64_t r = native_syscall_handler(SYS_DUP2, regs);
            if ((int64_t)r < 0) return r;
            if (arg3 & O_CLOEXEC) {
                fd_entry_t *ne = fd_get(task->fd_table, (int)arg2);
                if (ne) ne->flags |= O_CLOEXEC;
            }
            return r;
        }

        case LINUX_SYS_PIPE2: { // 293 — pipe + flags (O_CLOEXEC, O_NONBLOCK)
            if (!is_user_address(arg1, 8)) return LINUX_ENEG(LINUX_EFAULT);
            int rfd, wfd;
            if (pipe_create(task->fd_table, &rfd, &wfd) < 0)
                return LINUX_ENEG(LINUX_ENOMEM);
            if (arg2 & O_CLOEXEC) {
                task->fd_table->entries[rfd].flags |= O_CLOEXEC;
                task->fd_table->entries[wfd].flags |= O_CLOEXEC;
            }
            if (arg2 & O_NONBLOCK) {
                task->fd_table->entries[rfd].flags |= O_NONBLOCK;
                task->fd_table->entries[wfd].flags |= O_NONBLOCK;
            }
            int fds[2] = {rfd, wfd};
            copy_to_user((void *)arg1, fds, sizeof(fds));
            return 0;
        }

        case LINUX_SYS_PAUSE: // 34
            task_block();
            return LINUX_ENEG(LINUX_EINTR);

        case LINUX_SYS_NANOSLEEP: { // 35
            if (!is_user_address(arg1, sizeof(linux_timespec_t)))
                return LINUX_ENEG(LINUX_EFAULT);
            linux_timespec_t ts;
            copy_from_user(&ts, (void *)arg1, sizeof(ts));
            uint32_t ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
            if (ms > 0) timer_sleep(ms);
            return 0;
        }

        case LINUX_SYS_CLOCK_NANOSLEEP: { // 230 — clockid ignored
            linux_timespec_t ts;
            const linux_timespec_t *tsptr = (arg3) ? (linux_timespec_t *)arg3
                                                    : (linux_timespec_t *)arg4;
            if (!tsptr || !is_user_address((uint64_t)tsptr, sizeof(*tsptr)))
                return LINUX_ENEG(LINUX_EFAULT);
            copy_from_user(&ts, tsptr, sizeof(ts));
            uint32_t ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
            if (ms > 0) timer_sleep(ms);
            return 0;
        }

        case LINUX_SYS_CLOCK_GETRES: { // 229
            if (arg2 && is_user_address(arg2, sizeof(linux_timespec_t))) {
                linux_timespec_t res = {0, 10000000}; // 10ms resolution
                copy_to_user((void *)arg2, &res, sizeof(res));
            }
            return 0;
        }

        // ── Process ───────────────────────────────────────────────────────────

        case LINUX_SYS_GETPPID:  return (uint64_t)task->parent_pid;
        case LINUX_SYS_GETTID:   return (uint64_t)pid;        // unique per-thread ID
        case LINUX_SYS_GETUID:   return 0;
        case LINUX_SYS_GETGID:   return 0;
        case LINUX_SYS_GETEUID:  return 0;
        case LINUX_SYS_GETEGID:  return 0;
        case LINUX_SYS_GETPGRP:  return (uint64_t)task->tgid; // process group = tgid
        case LINUX_SYS_SETUID:
        case LINUX_SYS_SETGID:
        case LINUX_SYS_SETPGID:
        case LINUX_SYS_SETSID:
            return 0;

        case LINUX_SYS_CLONE: { // 56
            uint64_t flags       = arg1;
            uint64_t child_stack = arg2;
            // arg3=parent_tidptr, arg4=tls, arg5=child_tidptr

            if (flags & CLONE_THREAD) {
                // Thread: share address space — use current RIP as entry
                // glibc's __clone stub jumps to the new stack, RIP is already there
                int tid = task_create_thread(regs->rip, 0, child_stack);
                if (tid < 0) return LINUX_ENEG(LINUX_ENOMEM);

                // Handle SETTLS: store TLS base (new thread will call arch_prctl itself)
                if ((flags & CLONE_SETTLS) && arg4 && is_user_address(arg4, 8)) {
                    task_t *child = task_get_by_id((uint32_t)tid);
                    if (child) {
                        child->tls_base = arg4;
                        // Will be applied when the thread runs and calls wrmsr
                    }
                }

                // Write tid to parent_tidptr / child_tidptr if requested
                if ((flags & CLONE_PARENT_SETTID) && arg3 && is_user_address(arg3, 4)) {
                    uint32_t tid32 = (uint32_t)tid;
                    copy_to_user((void *)arg3, &tid32, 4);
                }
                return (uint64_t)tid;
            }

            // Fork-like clone
            return (uint64_t)task_fork((registers_t *)regs);
        }

        case LINUX_SYS_EXECVE: { // 59 — delegate to native in-place exec
            // The native SYS_EXECVE handler uses CWD-relative path resolution
            // and calls task_execve() which patches regs in-place.
            // Keep ABI_LINUX for the replaced process.
            uint64_t r = native_syscall_handler(SYS_EXECVE, regs);
            if (r == 0) task->abi = ABI_LINUX; // Ensure Linux ABI persists
            return r;
        }

        // ── File system ───────────────────────────────────────────────────────

        case LINUX_SYS_UNAME: { // 63
            if (!is_user_address(arg1, sizeof(linux_utsname_t)))
                return LINUX_ENEG(LINUX_EFAULT);
            linux_utsname_t ku;
            memset(&ku, 0, sizeof(ku));
            strcpy(ku.sysname,    "Linux");
            strcpy(ku.nodename,   "raeenos");
            strcpy(ku.release,    "5.15.0-raeenos");
            strcpy(ku.version,    "#1 SMP RaeenOS 2026");
            strcpy(ku.machine,    "x86_64");
            strcpy(ku.domainname, "(none)");
            if (copy_to_user((void *)arg1, &ku, sizeof(ku)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            return 0;
        }

        case LINUX_SYS_FCNTL: // 72 — delegate to native (handles O_CLOEXEC properly)
            return native_syscall_handler(SYS_FCNTL, regs);

        case LINUX_SYS_FLOCK: // 73 — advisory lock; delegate to native no-op
            return native_syscall_handler(SYS_FLOCK, regs);

        case LINUX_SYS_TRUNCATE: { // 76
            char path[256];
            if (copy_string_from_user(path, (const char *)arg1, sizeof(path)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            // Stub: truncation not yet supported
            return LINUX_ENEG(LINUX_ENOSYS);
        }

        case LINUX_SYS_FTRUNCATE: // 77 — stub
            return 0;

        case LINUX_SYS_GETDENTS: { // 78 — old-style dirent, delegate to getdents64
            return native_syscall_handler(SYS_GETDENTS64, regs);
        }

        case LINUX_SYS_GETCWD: // 79 — delegate (same syscall number)
            return native_syscall_handler(SYS_GETCWD, regs);

        case LINUX_SYS_CHDIR:  // 80 — delegate
            return native_syscall_handler(SYS_CHDIR, regs);

        case LINUX_SYS_FCHDIR: // 81 — delegate (stub for now)
            return native_syscall_handler(SYS_FCHDIR, regs);

        case LINUX_SYS_MKDIR: // 83
            return native_syscall_handler(SYS_MKDIR, regs);

        case LINUX_SYS_CREAT: // 85 — delegate to native creat
            return native_syscall_handler(SYS_CREAT, regs);

        case LINUX_SYS_LINK: // 86 — hard links; stub (return error, no symlink support)
            return LINUX_ENEG(LINUX_EPERM);

        case LINUX_SYS_SYMLINK: // 88
            return LINUX_ENEG(LINUX_EPERM);   // No symlink creation

        case LINUX_SYS_READLINK: // 89 — delegate to native (handles /proc/self/exe etc.)
            return native_syscall_handler(SYS_READLINK, regs);

        case LINUX_SYS_CHMOD:
        case LINUX_SYS_FCHMOD:
        case LINUX_SYS_CHOWN:
        case LINUX_SYS_FCHOWN:
            return 0;  // Pretend success (no permission model yet)

        case LINUX_SYS_UMASK: // 95
            return 022;

        case LINUX_SYS_STATFS:
        case LINUX_SYS_FSTATFS: // 137/138 — return something reasonable
            if (arg2 && is_user_address(arg2, 80)) {
                // struct statfs: type, bsize, blocks, bfree, bavail, files, ffree, fsid, namelen, frsize, flags, spare
                uint64_t sf[10];
                memset(sf, 0, sizeof(sf));
                sf[0] = 0xEF53;         // EXT2_SUPER_MAGIC
                sf[1] = 4096;           // bsize
                sf[2] = 1024 * 1024;    // blocks (4GB)
                sf[3] = 512 * 1024;     // bfree
                sf[4] = 512 * 1024;     // bavail
                sf[7] = 255;            // namelen
                copy_to_user((void *)arg2, sf, sizeof(sf));
            }
            return 0;

        case LINUX_SYS_OPENAT: { // 257 — arg1=dirfd, arg2=path, arg3=flags
            // AT_FDCWD (-100) means use CWD — our SYS_OPEN already does that.
            // For fd-relative opens we only support AT_FDCWD for now.
            regs->rdi = arg2;  // path
            regs->rsi = arg3;  // flags
            regs->rdx = arg4;  // mode
            return native_syscall_handler(SYS_OPEN, regs);
        }

        case LINUX_SYS_MKDIRAT: { // 258 — arg1=dirfd, arg2=path, arg3=mode
            regs->rdi = arg2;  // path (AT_FDCWD → CWD, already supported)
            regs->rsi = arg3;  // mode
            return native_syscall_handler(SYS_MKDIR, regs);
        }

        case LINUX_SYS_UNLINKAT: { // 263 — arg1=dirfd, arg2=path, arg3=flags
            regs->rdi = arg2;
            return native_syscall_handler(SYS_UNLINK, regs);
        }

        case LINUX_SYS_RENAMEAT: { // 264 — arg1=olddirfd, arg2=old, arg3=newdirfd, arg4=new
            regs->rdi = arg2;  // old path
            regs->rsi = arg4;  // new path
            return native_syscall_handler(SYS_RENAME, regs);
        }

        case LINUX_SYS_FACCESSAT:  // 269 — arg1=dirfd, arg2=path, arg3=mode, arg4=flags
        case LINUX_SYS_FACCESSAT2: // 439 — same
        {
            /* Treat like access() — just verify path exists */
            char path[512], abs[512];
            if (!arg2 || copy_string_from_user(path, (const char *)arg2, sizeof(path)) < 0)
                return LINUX_ENEG(LINUX_EFAULT);
            vfs_resolve_path(task->cwd, path, abs, sizeof(abs));
            vfs_node_t *node = vfs_open(abs, 0);
            return node ? 0 : LINUX_ENEG(LINUX_ENOENT);
        }

        // ── Time ──────────────────────────────────────────────────────────────

        case LINUX_SYS_GETTIMEOFDAY: { // 96
            if (arg1 && is_user_address(arg1, sizeof(linux_timeval_t))) {
                linux_timeval_t tv = { (int64_t)rtc_get_timestamp(), 0 };
                copy_to_user((void *)arg1, &tv, sizeof(tv));
            }
            // arg2 = timezone (ignored)
            return 0;
        }

        // ── Limits / resources ────────────────────────────────────────────────

        case LINUX_SYS_GETRUSAGE: { // 98 — arg1=who (0=self), arg2=*rusage (144 bytes)
            // struct rusage: {timeval utime, timeval stime, 14 longs} = 144 bytes total
            if (!arg2 || !is_user_address(arg2, 144)) return LINUX_ENEG(LINUX_EFAULT);
            uint8_t zeros[144];
            memset(zeros, 0, sizeof(zeros));
            copy_to_user((void *)arg2, zeros, sizeof(zeros));
            return 0;
        }

        case LINUX_SYS_TIMES: { // 100 — struct tms: 4× clock_t (8 bytes each)
            if (arg1 && is_user_address(arg1, 32)) {
                uint64_t zeros[4] = {0, 0, 0, 0};
                copy_to_user((void *)arg1, zeros, 32);
            }
            return (uint64_t)timer_get_ticks(); // clock ticks since boot
        }

        case LINUX_SYS_GETRLIMIT: { // 97
            if (!is_user_address(arg2, sizeof(linux_rlimit_t))) return LINUX_ENEG(LINUX_EFAULT);
            linux_rlimit_t rl = {(uint64_t)-1, (uint64_t)-1};
            switch ((int)arg1) {
                case RLIMIT_STACK:
                    rl.rlim_cur = 8 * 1024 * 1024; rl.rlim_max = (uint64_t)-1; break;
                case RLIMIT_NOFILE:
                    rl.rlim_cur = MAX_FDS; rl.rlim_max = MAX_FDS; break;
                default: break;
            }
            copy_to_user((void *)arg2, &rl, sizeof(rl));
            return 0;
        }

        case LINUX_SYS_PRLIMIT64: { // 302
            // arg1=pid, arg2=resource, arg3=new_limit, arg4=old_limit
            if (arg4 && is_user_address(arg4, sizeof(linux_rlimit_t))) {
                linux_rlimit_t rl = {(uint64_t)-1, (uint64_t)-1};
                if ((int)arg2 == RLIMIT_NOFILE) { rl.rlim_cur = MAX_FDS; rl.rlim_max = MAX_FDS; }
                if ((int)arg2 == RLIMIT_STACK)  { rl.rlim_cur = 8*1024*1024; rl.rlim_max = (uint64_t)-1; }
                copy_to_user((void *)arg4, &rl, sizeof(rl));
            }
            return 0;
        }

        // ── Scheduler ─────────────────────────────────────────────────────────

        case LINUX_SYS_SCHED_SETPARAM:      // 142 — ignore policy changes
        case LINUX_SYS_SCHED_SETSCHEDULER: // 144
            return 0;

        case LINUX_SYS_SCHED_GETSCHEDULER: // 145 — SCHED_OTHER = 0
            return 0;

        case LINUX_SYS_SCHED_GETPARAM: { // 143 — sched_param { int sched_priority }
            if (arg2 && is_user_address(arg2, 4)) {
                uint32_t prio = 0;
                copy_to_user((void *)arg2, &prio, 4);
            }
            return 0;
        }

        case LINUX_SYS_SCHED_GET_PRIORITY_MAX: // 146 — max prio for policy arg1
        case LINUX_SYS_SCHED_GET_PRIORITY_MIN: // 147 — min prio for policy arg1
            return 0; // SCHED_OTHER has priority range [0, 0]

        // ── Process control ───────────────────────────────────────────────────

        case LINUX_SYS_PRCTL: { // 157
            switch ((int)arg1) {
                case PR_SET_NAME: {
                    char name[32];
                    if (copy_string_from_user(name, (const char *)arg2, sizeof(name)) == 0)
                        strncpy(task->name, name, sizeof(task->name) - 1);
                    return 0;
                }
                case PR_GET_NAME:
                    if (is_user_address(arg2, 16))
                        copy_to_user((void *)arg2, task->name, 16);
                    return 0;
                case PR_SET_DUMPABLE:
                case PR_GET_DUMPABLE:
                case PR_SET_KEEPCAPS:
                case PR_SET_SECCOMP:
                    return 0;
                default:
                    return 0;
            }
        }

        // ── Futex ─────────────────────────────────────────────────────────────

        case LINUX_SYS_FUTEX: { // 202
            // arg1=uaddr, arg2=op, arg3=val (arg4=timeout, arg5=uaddr2, arg6=val3)
            int op  = (int)arg2 & 0x7F;  // mask FUTEX_PRIVATE_FLAG (128) and FUTEX_CLOCK_REALTIME
            uint32_t val = (uint32_t)arg3;
            return (uint64_t)futex_op((uint64_t *)arg1, op, val);
        }

        case LINUX_SYS_SET_TID_ADDRESS: // 218 — save for CLONE_CHILD_CLEARTID on exit
            task->clear_tid_addr = arg1;
            return (uint64_t)pid;

        case LINUX_SYS_SET_ROBUST_LIST: // 273
        case LINUX_SYS_GET_ROBUST_LIST: // 274
        case LINUX_SYS_RSEQ:            // 334
            return 0;

        // ── Memory management stubs ───────────────────────────────────────────

        case LINUX_SYS_MSYNC:    // 26 — flush mmap'd pages; no-op (no page cache dirty tracking)
        case LINUX_SYS_MINCORE:  // 27 — page residency; succeed silently
            return 0;

        case LINUX_SYS_MLOCK:    // 149
        case LINUX_SYS_MUNLOCK:  // 150
        case LINUX_SYS_MLOCKALL: // 151
        case LINUX_SYS_MUNLOCKALL: // 152
            return 0; // All pages are always "wired" in our simple model

        // ── Credentials ───────────────────────────────────────────────────────

        case LINUX_SYS_GETGROUPS: { // 115 — we have no supplemental groups; return 0
            // arg1=size, arg2=list[]. If size>0, write nothing (0 groups). Return 0.
            return 0;
        }

        case LINUX_SYS_SETGROUPS: // 116 — ignored
            return 0;

        case LINUX_SYS_CAPGET: { // 125 — report full capabilities (root)
            if (arg2 && is_user_address(arg2, 8)) {
                // struct __user_cap_data_struct: effective/permitted/inheritable all 0xFFFFFFFF
                uint32_t caps[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
                copy_to_user((void *)arg2, caps, sizeof(caps));
            }
            return 0;
        }

        case LINUX_SYS_CAPSET: // 126 — ignore capability setting
            return 0;

        // ── System info ───────────────────────────────────────────────────────

        case LINUX_SYS_SYSINFO: { // 99
            // struct sysinfo { long uptime; ulong loads[3]; ulong totalram; ulong freeram;
            //   ulong sharedram; ulong bufferram; ulong totalswap; ulong freeswap;
            //   ushort procs; ulong totalhigh; ulong freehigh; uint mem_unit; }
            if (!is_user_address(arg1, 64)) return LINUX_ENEG(LINUX_EFAULT);
            uint64_t sysinfo[8];
            sysinfo[0] = rtc_get_timestamp(); // uptime (approximate)
            sysinfo[1] = 0; sysinfo[2] = 0; sysinfo[3] = 0; // loads
            uint64_t total_pages = pmm_get_total_pages();
            uint64_t free_pages  = pmm_get_free_pages();
            sysinfo[4] = total_pages * 4096; // totalram
            sysinfo[5] = free_pages  * 4096; // freeram
            sysinfo[6] = 0; // sharedram
            sysinfo[7] = 0; // bufferram
            copy_to_user((void *)arg1, sysinfo, 64);
            // Write mem_unit = 1 at offset 56 (after 7 uint64s + 2 uint64 swap + ushort procs)
            // For simplicity the above covers the essential fields
            return 0;
        }

        // ── Networking — delegate to native socket syscalls ───────────────────

        case LINUX_SYS_SOCKET:      // 41
        case LINUX_SYS_CONNECT:     // 42
        case LINUX_SYS_ACCEPT:      // 43
        case LINUX_SYS_SENDTO:      // 44
        case LINUX_SYS_RECVFROM:    // 45
        case LINUX_SYS_BIND:        // 49
        case LINUX_SYS_LISTEN:      // 50
        case LINUX_SYS_GETSOCKNAME: // 51
        case LINUX_SYS_GETPEERNAME: // 52
        case LINUX_SYS_SETSOCKOPT:  // 54
        case LINUX_SYS_GETSOCKOPT:  // 55
            // Linux socket syscall numbers match RaeenOS native ones exactly
            return native_syscall_handler(num, regs);

        case LINUX_SYS_SENDMSG:     // 46 — delegate to native (has full iovec impl)
        case LINUX_SYS_RECVMSG:     // 47
            return native_syscall_handler(num, regs);

        case LINUX_SYS_SOCKETPAIR: { // 53 — implement as a pipe pair
            if (!is_user_address(arg4, 8)) return LINUX_ENEG(LINUX_EFAULT);
            int rfd2, wfd2;
            if (pipe_create(task->fd_table, &rfd2, &wfd2) < 0) return LINUX_ENEG(LINUX_ENOMEM);
            int fds2[2] = {rfd2, wfd2};
            copy_to_user((void *)arg4, fds2, 8);
            return 0;
        }

        case LINUX_SYS_SHUTDOWN:    // 48 — delegate to native
            return syscall_handler(48, regs);

        case LINUX_SYS_ACCEPT4:     // 288 — delegate to native accept4
            return syscall_handler(288, regs);

        // ── Entropy ───────────────────────────────────────────────────────────

        case LINUX_SYS_GETRANDOM: { // 318
            if (!arg2) return 0;
            if (!is_user_address(arg1, arg2)) return LINUX_ENEG(LINUX_EFAULT);
            size_t len = (size_t)arg2;
            if (len > 4096) len = 4096; // cap per-call
            uint8_t *kbuf = kmalloc(len);
            if (!kbuf) return LINUX_ENEG(LINUX_ENOMEM);
            for (size_t i = 0; i < len; i++) kbuf[i] = prng_byte();
            copy_to_user((void *)arg1, kbuf, len);
            kfree(kbuf);
            return (int64_t)len;
        }

        // ── sendfile ──────────────────────────────────────────────────────────

        case LINUX_SYS_SENDFILE: { // 40 — in-kernel copy from src fd to dst fd
            int out_fd = (int)arg1, in_fd = (int)arg2;
            // arg3 = offset pointer (NULL = use fd offset), arg4 = count
            fd_entry_t *src = fd_get(task->fd_table, in_fd);
            fd_entry_t *dst = fd_get(task->fd_table, out_fd);
            if (!src || !dst) return LINUX_ENEG(LINUX_EBADF);
            if (src->type != FD_FILE || !src->node) return LINUX_ENEG(LINUX_EINVAL);
            size_t count = (size_t)arg4;
            if (count == 0) return 0;
            size_t off;
            if (arg3 && is_user_address(arg3, 8)) {
                uint64_t uoff;
                if (copy_from_user(&uoff, (void *)arg3, 8) < 0)
                    return LINUX_ENEG(LINUX_EFAULT);
                off = (size_t)uoff;
            } else {
                off = src->offset;
            }
            if (off >= src->node->length) return 0;
            if (off + count > src->node->length) count = src->node->length - off;
            uint8_t *kbuf = kmalloc(count);
            if (!kbuf) return LINUX_ENEG(LINUX_ENOMEM);
            size_t n = vfs_read(src->node, off, count, kbuf);
            size_t written = 0;
            if (dst->type == FD_FILE && dst->node) {
                written = vfs_write(dst->node, dst->offset, n, kbuf);
                dst->offset += written;
            } else if (dst->type == FD_PTY_MASTER || dst->type == FD_PTY_SLAVE) {
                written = (dst->type == FD_PTY_MASTER) ?
                    pty_master_write((pty_t *)dst->pipe, kbuf, n) :
                    pty_slave_write((pty_t *)dst->pipe, kbuf, n);
            }
            kfree(kbuf);
            if (!arg3) src->offset = off + n;
            else { uint64_t new_off = off + n; copy_to_user((void *)arg3, &new_off, 8); }
            return (int64_t)written;
        }

        // ── readlinkat ────────────────────────────────────────────────────────

        case LINUX_SYS_READLINKAT: { // 267 — arg1=dirfd, arg2=path, arg3=buf, arg4=bufsiz
            regs->rdi = arg2;   /* path */
            regs->rsi = arg3;   /* buf  */
            regs->rdx = arg4;   /* bufsiz */
            return native_syscall_handler(SYS_READLINK, regs);
        }

        // ── epoll ─────────────────────────────────────────────────────────────

        case LINUX_SYS_EPOLL_CREATE:  // 213
        case LINUX_SYS_EPOLL_CREATE1: // 291
        {
            /* Return a dummy fd (device type, no ops) so programs don't crash */
            int efd = fd_alloc(task->fd_table);
            if (efd < 0) return LINUX_ENEG(LINUX_EMFILE);
            fd_entry_t *ee = &task->fd_table->entries[efd];
            ee->type   = FD_DEVICE;
            ee->dev    = NULL;   /* no-op reads/writes */
            ee->flags  = (uint32_t)arg1; /* flags (O_CLOEXEC etc.) */
            ee->offset = 0;
            return (uint64_t)efd;
        }

        case LINUX_SYS_EPOLL_CTL:   // 233 — add/mod/del interest: stub
            return 0;

        case LINUX_SYS_EPOLL_WAIT:  // 232
        case LINUX_SYS_EPOLL_PWAIT: // 281
        case LINUX_SYS_EPOLL_PWAIT2:// 441
        {
            int timeout_ms = (int)arg4;
            if (timeout_ms > 0 && timeout_ms < 10000)
                timer_sleep((uint32_t)timeout_ms);
            else if (timeout_ms < 0)
                timer_sleep(10); /* blocking: yield briefly */
            return 0; /* no events */
        }

        // ── eventfd ───────────────────────────────────────────────────────────

        case LINUX_SYS_EVENTFD:  // 284
        case LINUX_SYS_EVENTFD2: // 290
        {
            /* Return a pipe as a fake eventfd */
            int rfd, wfd;
            if (pipe_create(task->fd_table, &rfd, &wfd) < 0) return LINUX_ENEG(LINUX_ENOMEM);
            (void)wfd; /* wfd stays open so reads don't immediately return EOF */
            return (uint64_t)rfd;
        }

        // ── timerfd ───────────────────────────────────────────────────────────

        case LINUX_SYS_TIMERFD_CREATE: { // 283
            int tfd = fd_alloc(task->fd_table);
            if (tfd < 0) return LINUX_ENEG(LINUX_EMFILE);
            task->fd_table->entries[tfd].type  = FD_DEVICE;
            task->fd_table->entries[tfd].dev   = NULL;
            task->fd_table->entries[tfd].flags = 0;
            return (uint64_t)tfd;
        }
        case LINUX_SYS_TIMERFD_SETTIME: // 286 — stub
        case LINUX_SYS_TIMERFD_GETTIME: // 287 — stub
            return 0;

        // ── inotify ───────────────────────────────────────────────────────────

        case LINUX_SYS_INOTIFY_INIT:  // 253
        case LINUX_SYS_INOTIFY_INIT1: // 294
        {
            int ifd = fd_alloc(task->fd_table);
            if (ifd < 0) return LINUX_ENEG(LINUX_EMFILE);
            task->fd_table->entries[ifd].type  = FD_DEVICE;
            task->fd_table->entries[ifd].dev   = NULL;
            task->fd_table->entries[ifd].flags = 0;
            return (uint64_t)ifd;
        }
        case LINUX_SYS_INOTIFY_ADD_WATCH: // 254 — stub
            return 1; /* fake watch descriptor */
        case LINUX_SYS_INOTIFY_RM_WATCH:  // 255 — stub
            return 0;

        // ── splice / tee / vmsplice ───────────────────────────────────────────

        case LINUX_SYS_SPLICE:     // 275 — stub
        case LINUX_SYS_TEE:        // 276 — stub
        case LINUX_SYS_VMSPLICE:   // 278 — stub
            return LINUX_ENEG(LINUX_ENOSYS);

        // ── fsync / fdatasync / fadvise ───────────────────────────────────────

        case LINUX_SYS_FSYNC:      // 74 — delegate to native
        case LINUX_SYS_FDATASYNC:  // 75 — delegate to native
            return native_syscall_handler(num, regs);

        case LINUX_SYS_FADVISE64:  // 221 — advisory; no-op
            return 0;

        case LINUX_SYS_WAITID:     // 247 — delegate to wait4 semantics
            return native_syscall_handler(SYS_WAIT4, regs);

        case LINUX_SYS_IOPRIO_SET: // 251 — I/O scheduling priority; stub
        case LINUX_SYS_IOPRIO_GET: // 252
            return 0;

        // ── Misc stubs that glibc/musl call at startup ─────────────────────────

        case LINUX_SYS_SYSLOG: // 103
            return 0;

        default:
            kprintf("[LINUX] Unimplemented syscall #%lu (arg1=%lx) from PID %u\n",
                    num, arg1, pid);
            return LINUX_ENEG(LINUX_ENOSYS);
    }
}
