#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TASKS 128
#define TASK_STACK_SIZE (16 * 1024)

// User-space memory layout constants
#define USER_STACK_TOP   0x7FFFFFFFF000ULL
#define USER_STACK_SIZE  (1024 * 1024) // 1MB Stack

// CPU Registration State (what is pushed by ISR)
// Note: This must match the stack layout in interrupts.S
typedef struct {
    uint64_t fs, es, ds;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) registers_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,       // Blocked on mutex/semaphore/IPC
    TASK_TERMINATED,
    TASK_ALLOCATING
} task_state_t;

// Forward declarations
struct fd_table;
struct mm_struct;

typedef enum {
    ABI_NATIVE = 0,
    ABI_LINUX
} abi_type_t;

typedef struct task_t {
    uint32_t id;
    char name[32];
    task_state_t state;
    abi_type_t abi;      // Process personality (Native vs Linux)

    uint64_t rsp;        // Kernel stack pointer (saved context)
    void *stack_base;    // Base of allocated kernel stack
    uint64_t cr3;        // Page table (0 = use kernel CR3)

    // Per-process file descriptor table
    struct fd_table *fd_table;

    // Per-process memory descriptor (VMAs, brk, mmap)
    struct mm_struct *mm;

    // Process relationships
    uint32_t parent_pid;    // Parent process ID (0 = kernel)
    int32_t  exit_code;     // Exit code (valid when TASK_TERMINATED)
    uint8_t  term_signal;   // Signal that killed this task (0 = normal exit)

    // Thread support
    uint32_t tgid;          // Thread group ID (= id for group leader)
    uint64_t tls_base;      // Thread-local storage FS base address

    // Pending signals (bitmask)
    uint64_t pending_signals;

    // Per-signal userspace handlers (set via rt_sigaction)
    // 0 = SIG_DFL, 1 = SIG_IGN, else = handler function pointer
    uint64_t sig_handlers[32];
    uint64_t sig_restorer[32];  // sa_restorer per signal
    uint32_t sig_flags[32];     // sa_flags per signal
    uint64_t sig_mask;          // currently blocked signals (sigprocmask)

    // Wakeup time for sleeping tasks
    uint64_t wakeup_ticks;

    // Current working directory
    char cwd[256];

    // Executable path (set by execve, used by /proc/self/exe)
    char exec_path[512];

    // For CLONE_CHILD_CLEARTID: write 0 here + futex_wake on thread exit
    uint64_t clear_tid_addr;

    // Linked List for Run Queue
    struct task_t *next;

    // CPU Affinity (which CPU this task belongs to)
    uint32_t cpu_id;
} task_t;

// API
void scheduler_init(void);
int task_create(const char *name, void (*entry)(void));
void task_exit(void);
void task_yield(void);
uint32_t task_current_id(void);

// Called by Timer ISR (returns new RSP)
uint64_t scheduler_switch(registers_t *regs);

// Block current task (must be called with interrupts disabled or from ISR context)
// The task is set to TASK_BLOCKED and yields. Caller must arrange for task_unblock() later.
void task_block(void);

// Unblock a specific task (moves it from BLOCKED to READY on its assigned CPU's run queue)
void task_unblock(task_t *task);

// Get a task by its ID (returns NULL if invalid or unused)
task_t *task_get_by_id(uint32_t id);

// Create a new thread in the current process.
// Returns thread ID (task slot) or -1 on failure.
int task_create_thread(uint64_t entry, uint64_t arg, uint64_t user_stack);

// Join a thread (block until it exits). Returns exit code or -1.
int task_thread_join(uint32_t tid);

// wait() option flags
#define WNOHANG   1   // Don't block; return 0 if no child has exited
#define WUNTRACED 2   // Also report stopped children (not implemented)

// Linux wait status encoding helpers
#define WSTATUS_EXIT(code)   (((code) & 0xff) << 8)        // normal exit
#define WSTATUS_SIGNAL(sig)  ((sig) & 0x7f)                 // killed by signal

// Wait for any child to exit. Returns child PID, status in *status (Linux encoding).
// Returns 0 if WNOHANG and no child ready. Returns -1 if no children.
int task_wait(int *status, int options);

// Wait for specific child (pid>0) or any child (pid==-1).
// Returns child PID, status in *status (Linux encoding).
int task_waitpid(int pid, int *status, int options);

// Exit with a specific exit code
void task_exit_code(int code);

// Kill all threads in the same thread group (SYS_EXIT_GROUP)
void task_exit_group(int code);

// Fork the current process (COW). Returns child PID to parent, 0 to child, -1 on error.
int task_fork(registers_t *parent_regs);

// In-place exec: replace current process image with new ELF.
// argv and envp are NULL-terminated arrays of user-space string pointers (may be NULL).
// Modifies regs so that on syscall return, execution starts in the new program.
// Returns 0 on success (execution continues in new program via iretq), -1 on failure.
int task_execve(registers_t *regs, const void *elf_data, size_t size,
                const char *const *argv, const char *const *envp);

int task_create_user(const char *name, const void *elf_data, size_t size, uint32_t parent_pid, abi_type_t abi);

// Debug
void scheduler_debug_print_tasks(void);

#endif
