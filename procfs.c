#include "procfs.h"
#include "vfs.h"
#include "sched.h"
#include "pmm.h"
#include "serial.h"
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "fd.h"
#include "vm_area.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Internal procfs node types ────────────────────────────────────────────────
#define PROC_ROOT       0   // /proc directory
#define PROC_VERSION    1   // /proc/version
#define PROC_UPTIME     2   // /proc/uptime
#define PROC_MEMINFO    3   // /proc/meminfo
#define PROC_CPUINFO    4   // /proc/cpuinfo
#define PROC_PID_DIR    5   // /proc/<pid>
#define PROC_PID_STATUS 6   // /proc/<pid>/status
#define PROC_PID_MAPS   7   // /proc/<pid>/maps
#define PROC_PID_FD     8   // /proc/<pid>/fd (directory)
#define PROC_SELF          9   // /proc/self (acts as /proc/<current_pid>)
#define PROC_CMDLINE       10  // /proc/cmdline (kernel)
#define PROC_STAT          11  // /proc/stat
#define PROC_PID_CMDLINE   12  // /proc/<pid>/cmdline (process argv)

typedef struct {
    int type;
    uint32_t pid;   // for PID-specific nodes
} procfs_node_t;

// ── Content generators ────────────────────────────────────────────────────────

static size_t proc_read_version(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    (void)node;
    char tmp[256];
    int len = ksnprintf(tmp, sizeof(tmp),
        "Linux version 5.15.0-raeenos (RaeenOS 2026) "
        "#1 SMP RaeenOS x86_64\n");
    if (len < 0) return 0;
    if (off >= (size_t)len) return 0;
    size_t avail = (size_t)len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    return copy;
}

static size_t proc_read_uptime(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    (void)node;
    char tmp[64];
    uint64_t secs = rtc_get_timestamp();
    int len = ksnprintf(tmp, sizeof(tmp), "%lu.00 %lu.00\n", secs, secs);
    if (len < 0) return 0;
    if (off >= (size_t)len) return 0;
    size_t avail = (size_t)len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    return copy;
}

static size_t proc_read_meminfo(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    (void)node;
    // Basic memory info — we report total and free from PMM
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t free_pages  = pmm_get_free_pages();
    uint64_t total_kb = total_pages * 4;
    uint64_t free_kb  = free_pages  * 4;

    char tmp[512];
    int len = ksnprintf(tmp, sizeof(tmp),
        "MemTotal:       %lu kB\n"
        "MemFree:        %lu kB\n"
        "MemAvailable:   %lu kB\n"
        "Buffers:        0 kB\n"
        "Cached:         0 kB\n"
        "SwapTotal:      0 kB\n"
        "SwapFree:       0 kB\n",
        total_kb, free_kb, free_kb);
    if (len < 0) return 0;
    if (off >= (size_t)len) return 0;
    size_t avail = (size_t)len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    return copy;
}

static size_t proc_read_cpuinfo(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    (void)node;
    char tmp[512];
    int len = ksnprintf(tmp, sizeof(tmp),
        "processor\t: 0\n"
        "vendor_id\t: GenuineIntel\n"
        "model name\t: RaeenOS Virtual CPU\n"
        "cpu MHz\t\t: 2000.000\n"
        "cache size\t: 4096 KB\n"
        "physical id\t: 0\n"
        "siblings\t: 1\n"
        "cpu cores\t: 1\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov "
                   "pat pse36 clflush mmx fxsr sse sse2 syscall nx lm\n"
        "bogomips\t: 4000.00\n"
        "\n");
    if (len < 0) return 0;
    if (off >= (size_t)len) return 0;
    size_t avail = (size_t)len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    return copy;
}

static size_t proc_read_cmdline(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    (void)node;
    const char *cmdline = "raeenos\0";
    size_t len = 9; // "raeenos" + 2 NULs (kernel + end)
    if (off >= len) return 0;
    size_t avail = len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, cmdline + off, copy);
    return copy;
}

// /proc/<pid>/status
static size_t proc_read_pid_status(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    procfs_node_t *pn = (procfs_node_t *)node->impl;
    uint32_t pid = pn->pid;
    task_t *task = task_get_by_id(pid);

    char tmp[512];
    int len;
    if (!task) {
        len = ksnprintf(tmp, sizeof(tmp), "Name:\t(gone)\nPid:\t%u\nState:\tZ (zombie)\n", pid);
    } else {
        const char *state_str =
            (task->state == TASK_RUNNING)     ? "R (running)"   :
            (task->state == TASK_READY)       ? "S (sleeping)"  :
            (task->state == TASK_BLOCKED)     ? "S (sleeping)"  :
            (task->state == TASK_SLEEPING)    ? "S (sleeping)"  :
            (task->state == TASK_TERMINATED)  ? "Z (zombie)"    : "? (unknown)";

        len = ksnprintf(tmp, sizeof(tmp),
            "Name:\t%s\n"
            "Umask:\t0022\n"
            "State:\t%s\n"
            "Tgid:\t%u\n"
            "Pid:\t%u\n"
            "PPid:\t%u\n"
            "Uid:\t0\t0\t0\t0\n"
            "Gid:\t0\t0\t0\t0\n"
            "VmRSS:\t0 kB\n",
            task->name, state_str, task->tgid, pid, task->parent_pid);
    }
    if (len < 0) return 0;
    if (off >= (size_t)len) return 0;
    size_t avail = (size_t)len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    return copy;
}

// /proc/<pid>/cmdline — process executable path (argv[0])
static size_t proc_read_pid_cmdline(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    procfs_node_t *pn = (procfs_node_t *)node->impl;
    task_t *task = task_get_by_id(pn->pid);
    if (!task) return 0;
    const char *path = task->exec_path[0] ? task->exec_path : task->name;
    size_t len = strlen(path) + 1; // include NUL terminator
    if (off >= len) return 0;
    size_t avail = len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, path + off, copy);
    return copy;
}

// /proc/<pid>/maps — iterate actual VMAs
static size_t proc_read_pid_maps(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    procfs_node_t *pn = (procfs_node_t *)node->impl;
    task_t *task = task_get_by_id(pn->pid);
    if (!task || !task->mm) return 0;

    char *tmp = kmalloc(8192);
    if (!tmp) return 0;

    int pos = 0;
    for (vm_area_t *vma = task->mm->vma_list; vma && pos < 8192 - 128; vma = vma->next) {
        char perms[5];
        perms[0] = (vma->flags & VMA_READ)   ? 'r' : '-';
        perms[1] = (vma->flags & VMA_WRITE)  ? 'w' : '-';
        perms[2] = (vma->flags & VMA_EXEC)   ? 'x' : '-';
        perms[3] = (vma->flags & VMA_SHARED) ? 's' : 'p';
        perms[4] = '\0';

        const char *label = "";
        if (vma->end >= USER_STACK_TOP - 4096 && vma->end <= USER_STACK_TOP)
            label = "[stack]";
        else if (task->mm->start_brk && vma->start == task->mm->start_brk)
            label = "[heap]";

        pos += ksnprintf(tmp + pos, 8192 - pos,
            "%012lx-%012lx %s 00000000 00:00 0          %s\n",
            vma->start, vma->end, perms, label);
    }

    size_t len = (size_t)pos;
    if (off >= len) { kfree(tmp); return 0; }
    size_t avail = len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    kfree(tmp);
    return copy;
}

// /proc/stat — minimal CPU stats (required by many runtime libraries)
static size_t proc_read_stat(vfs_node_t *node, size_t off, size_t size, uint8_t *buf) {
    (void)node;
    char tmp[256];
    // Format: cpu <user> <nice> <system> <idle> <iowait> <irq> <softirq>
    int len = ksnprintf(tmp, sizeof(tmp),
        "cpu  0 0 0 1000000 0 0 0 0 0 0\n"
        "cpu0 0 0 0 1000000 0 0 0 0 0 0\n"
        "processes 1\n"
        "procs_running 1\n"
        "procs_blocked 0\n");
    if (len < 0) return 0;
    if (off >= (size_t)len) return 0;
    size_t avail = (size_t)len - off;
    size_t copy  = avail < size ? avail : size;
    memcpy(buf, tmp + off, copy);
    return copy;
}

// ── Node factory helpers ──────────────────────────────────────────────────────

static vfs_node_t *make_proc_file(const char *name, size_t (*read_fn)(vfs_node_t*, size_t, size_t, uint8_t*),
                                  int type, uint32_t pid) {
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(vfs_node_t));
    strncpy(n->name, name, 127);
    n->flags  = VFS_FILE;
    n->length = 4096; // approximate; real length determined by read
    n->read   = read_fn;

    procfs_node_t *pn = kmalloc(sizeof(procfs_node_t));
    if (!pn) { kfree(n); return NULL; }
    pn->type = type;
    pn->pid  = pid;
    n->impl  = pn;
    return n;
}

static vfs_node_t *make_proc_dir(const char *name, int type, uint32_t pid);

// /proc/<pid>/ finddir
static vfs_node_t *pid_dir_finddir(vfs_node_t *node, const char *name) {
    procfs_node_t *pn = (procfs_node_t *)node->impl;
    uint32_t pid = pn->pid;

    if (strcmp(name, "status") == 0)
        return make_proc_file("status", proc_read_pid_status, PROC_PID_STATUS, pid);
    if (strcmp(name, "maps") == 0)
        return make_proc_file("maps", proc_read_pid_maps, PROC_PID_MAPS, pid);
    if (strcmp(name, "fd") == 0)
        return make_proc_dir("fd", PROC_PID_FD, pid);
    if (strcmp(name, "cmdline") == 0)
        return make_proc_file("cmdline", proc_read_pid_cmdline, PROC_PID_CMDLINE, pid);
    if (strcmp(name, "exe") == 0)
        return make_proc_file("exe", proc_read_pid_cmdline, PROC_PID_CMDLINE, pid);
    return NULL;
}

static vfs_node_t *pid_dir_readdir(vfs_node_t *node, size_t index) {
    procfs_node_t *pn = (procfs_node_t *)node->impl;
    uint32_t pid = pn->pid;
    static const char *pid_entries[] = { "status", "maps", "fd", "cmdline", NULL };
    if (!pid_entries[index]) return NULL;
    return make_proc_file(pid_entries[index], proc_read_pid_status, PROC_PID_STATUS, pid);
}

// /proc/ root finddir — handles numeric PIDs and special names
static vfs_node_t *proc_root_finddir(vfs_node_t *node, const char *name);
static vfs_node_t *proc_root_readdir(vfs_node_t *node, size_t index);

static vfs_node_t *make_proc_dir(const char *name, int type, uint32_t pid) {
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(vfs_node_t));
    strncpy(n->name, name, 127);
    n->flags   = VFS_DIRECTORY;
    n->length  = 0;

    if (type == PROC_ROOT) {
        n->finddir = proc_root_finddir;
        n->readdir = proc_root_readdir;
    } else {
        n->finddir = pid_dir_finddir;
        n->readdir = pid_dir_readdir;
    }

    procfs_node_t *pn = kmalloc(sizeof(procfs_node_t));
    if (!pn) { kfree(n); return NULL; }
    pn->type = type;
    pn->pid  = pid;
    n->impl  = pn;
    return n;
}

// ── /proc root ────────────────────────────────────────────────────────────────

static vfs_node_t *proc_root_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "version")  == 0) return make_proc_file("version",  proc_read_version,  PROC_VERSION,  0);
    if (strcmp(name, "uptime")   == 0) return make_proc_file("uptime",   proc_read_uptime,   PROC_UPTIME,   0);
    if (strcmp(name, "meminfo")  == 0) return make_proc_file("meminfo",  proc_read_meminfo,  PROC_MEMINFO,  0);
    if (strcmp(name, "cpuinfo")  == 0) return make_proc_file("cpuinfo",  proc_read_cpuinfo,  PROC_CPUINFO,  0);
    if (strcmp(name, "cmdline")  == 0) return make_proc_file("cmdline",  proc_read_cmdline,  PROC_CMDLINE,  0);
    if (strcmp(name, "stat")     == 0) return make_proc_file("stat",     proc_read_stat,     PROC_STAT,     0);
    if (strcmp(name, "self")     == 0) {
        // Return a PID dir for the current process
        uint32_t cur = task_current_id();
        char pid_str[16];
        ksnprintf(pid_str, sizeof(pid_str), "%u", cur);
        return make_proc_dir(pid_str, PROC_PID_DIR, cur);
    }

    // Try numeric PID
    uint32_t pid = 0;
    bool all_digits = true;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') { all_digits = false; break; }
        pid = pid * 10 + (uint32_t)(*p - '0');
    }
    if (all_digits && pid > 0) {
        task_t *t = task_get_by_id(pid);
        if (t && t->state != TASK_UNUSED) {
            char pid_str[16];
            ksnprintf(pid_str, sizeof(pid_str), "%u", pid);
            return make_proc_dir(pid_str, PROC_PID_DIR, pid);
        }
    }

    return NULL;
}

// Fixed entries always present in /proc listing
static const char *proc_static_entries[] = {
    "version", "uptime", "meminfo", "cpuinfo", "cmdline", "stat", "self", NULL
};
#define PROC_STATIC_COUNT 7

static vfs_node_t *proc_root_readdir(vfs_node_t *node, size_t index) {
    (void)node;
    if (index < PROC_STATIC_COUNT)
        return proc_root_finddir(node, proc_static_entries[index]);
    // Return running PIDs after the static entries (skip kernel task 0)
    size_t pid_index = index - PROC_STATIC_COUNT;
    size_t count = 0;
    for (uint32_t pid = 1; pid < MAX_TASKS; pid++) {
        task_t *t = task_get_by_id(pid);
        if (!t || t->state == TASK_UNUSED || t->state == TASK_TERMINATED) continue;
        if (count == pid_index) {
            char pid_str[16];
            ksnprintf(pid_str, sizeof(pid_str), "%u", pid);
            return make_proc_dir(pid_str, PROC_PID_DIR, pid);
        }
        count++;
    }
    return NULL;
}

// ── Public API ────────────────────────────────────────────────────────────────

void procfs_init(void) {
    // Create the /proc mount point directory in the VFS root
    if (!vfs_root) return;

    // Make a /proc directory entry in initrd root
    vfs_mkdir_node(vfs_root, "proc");

    // Build the proc root node
    vfs_node_t *proc_root_node = make_proc_dir("proc", PROC_ROOT, 0);
    if (!proc_root_node) return;

    // Mount it at /proc
    if (vfs_mount("/proc", proc_root_node) == 0) {
        kprintf("[PROCFS] Mounted at /proc\n");
    }
}
