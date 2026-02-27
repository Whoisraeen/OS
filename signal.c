#include "signal.h"
#include "sched.h"
#include "idt.h"
#include "serial.h"
#include "console.h"
#include "vmm.h"
#include "string.h"

/* copy_to_user / copy_from_user forward declarations */
extern int copy_to_user(void *dst, const void *src, size_t n);
extern int copy_from_user(void *dst, const void *src, size_t n);

// Per-process signal dispositions (SIG_DFL or SIG_IGN)
// Index by [pid][signal]
static int sig_dispositions[MAX_TASKS][NSIG];

int signal_default_action(int sig) {
    switch (sig) {
        case SIGCHLD:
        case SIGCONT:
            return SIG_ACTION_IGNORE;
        case SIGSEGV:
        case SIGBUS:
        case SIGILL:
        case SIGFPE:
        case SIGABRT:
            return SIG_ACTION_CORE;
        case SIGKILL:
        case SIGTERM:
        case SIGINT:
        case SIGHUP:
        case SIGQUIT:
        case SIGPIPE:
        case SIGALRM:
        default:
            return SIG_ACTION_TERM;
    }
}

int signal_send(uint32_t pid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;

    task_t *task = task_get_by_id(pid);
    if (!task) return -1;

    // Set pending bit
    task->pending_signals |= (1ULL << sig);

    // For SIGKILL/SIGSTOP: cannot be caught or ignored
    if (sig == SIGKILL) {
        task->exit_code = 128 + sig;
        if (task->state == TASK_BLOCKED) {
            task_unblock(task);
        }
    }

    // If task is blocked, wake it so it can process the signal
    if (sig == SIGINT || sig == SIGTERM) {
        if (task->state == TASK_BLOCKED) {
            task_unblock(task);
        }
    }

    return 0;
}

int signal_set_handler(uint32_t pid, int sig, int action) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (pid >= MAX_TASKS) return -1;

    // SIGKILL and SIGSTOP cannot be caught or ignored
    if (sig == SIGKILL || sig == SIGSTOP) return -1;

    int prev = sig_dispositions[pid][sig];
    sig_dispositions[pid][sig] = action;
    return prev;
}

void signal_set_userhandler(uint32_t pid, int sig,
                            uint64_t handler, uint64_t restorer,
                            uint32_t flags)
{
    if (sig < 1 || sig >= NSIG) return;
    if (pid >= MAX_TASKS) return;
    if (sig == SIGKILL || sig == SIGSTOP) return;

    task_t *task = task_get_by_id(pid);
    if (!task) return;

    task->sig_handlers[sig] = handler;
    task->sig_restorer[sig] = restorer;
    task->sig_flags[sig]    = flags;

    /* Also mirror into the legacy disposition array */
    if (handler == 0)
        sig_dispositions[pid][sig] = SIG_DFL;
    else if (handler == 1)
        sig_dispositions[pid][sig] = SIG_IGN;
    else
        sig_dispositions[pid][sig] = 2; /* custom handler */
}

int signal_deliver_pending(uint32_t pid) {
    task_t *task = task_get_by_id(pid);
    if (!task || task->pending_signals == 0) return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(task->pending_signals & (1ULL << sig))) continue;

        // Clear pending bit
        task->pending_signals &= ~(1ULL << sig);

        // Determine action
        int disposition = sig_dispositions[pid][sig];

        if (disposition == SIG_IGN && sig != SIGKILL && sig != SIGSTOP) {
            continue; // Ignored
        }

        // Default action
        int action = signal_default_action(sig);

        if (action == SIG_ACTION_IGNORE) {
            continue;
        }

        if (action == SIG_ACTION_TERM || action == SIG_ACTION_CORE) {
            kprintf("[SIGNAL] Process %u killed by signal %d\n", pid, sig);
            task->exit_code   = 128 + sig;
            task->term_signal = (uint8_t)sig;
            task->state = TASK_TERMINATED;
            return 1; // Process was killed
        }
    }

    return 0;
}

/*
 * x86-64 Linux rt_sigframe layout
 * (matches arch/x86/include/asm/sigframe.h)
 *
 * struct rt_sigframe {
 *   char    *pretcode;       // +0  (8 bytes) → sa_restorer address
 *   struct ucontext uc;      // +8
 *     uc_flags  uint64  +8
 *     uc_link   uint64  +16
 *     uc_stack  (ss_sp:8, ss_flags:4, _pad:4, ss_size:8) +24  (24 bytes)
 *     uc_mcontext (sigcontext) +48  (256 bytes)
 *       r8..r15  : +48  ..+104  (8×8)
 *       rdi      : +112
 *       rsi      : +120
 *       rbp      : +128
 *       rbx      : +136
 *       rdx      : +144
 *       rax      : +152
 *       rcx      : +160
 *       rsp      : +168
 *       rip      : +176
 *       eflags   : +184
 *       cs+gs+fs+ss: +192 (4×uint16 = 8 bytes)
 *       err      : +200
 *       trapno   : +208
 *       oldmask  : +216
 *       cr2      : +224
 *       fpstate  : +232 (pointer, NULL for us)
 *       reserved1[8]: +240
 *     uc_sigmask uint64 +304
 *   struct siginfo info;     // +312  (128 bytes)
 * };
 * Total: 440 bytes — we'll allocate 512 for alignment
 */

#define SIGFRAME_PRETCODE_OFF   0
#define SIGFRAME_UC_FLAGS_OFF   8
#define SIGFRAME_UC_LINK_OFF    16
#define SIGFRAME_SS_SP_OFF      24
#define SIGFRAME_SS_FLAGS_OFF   32
#define SIGFRAME_SS_SIZE_OFF    40
#define SIGFRAME_SC_OFF         48   /* sigcontext start */
#define SIGFRAME_SC_R8_OFF      (SIGFRAME_SC_OFF + 0)
#define SIGFRAME_SC_RDI_OFF     (SIGFRAME_SC_OFF + 64)
#define SIGFRAME_SC_RSP_OFF     (SIGFRAME_SC_OFF + 120)
#define SIGFRAME_SC_RIP_OFF     (SIGFRAME_SC_OFF + 128)
#define SIGFRAME_SC_EFLAGS_OFF  (SIGFRAME_SC_OFF + 136)
#define SIGFRAME_SC_CS_OFF      (SIGFRAME_SC_OFF + 144)
#define SIGFRAME_SIGMASK_OFF    304
#define SIGFRAME_TOTAL_SIZE     512  /* rounded up for alignment */

int signal_try_deliver_frame(struct interrupt_frame *regs)
{
    uint32_t pid  = task_current_id();
    task_t  *task = task_get_by_id(pid);
    if (!task || task->pending_signals == 0) return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(task->pending_signals & (1ULL << sig))) continue;
        if (task->sig_mask & (1ULL << sig)) continue; /* blocked */

        uint64_t handler = task->sig_handlers[sig];

        /* SIG_IGN */
        if (handler == 1 && sig != SIGKILL && sig != SIGSTOP) {
            task->pending_signals &= ~(1ULL << sig);
            continue;
        }

        /* SIG_DFL or no handler registered yet */
        if (handler == 0) {
            int act = signal_default_action(sig);
            if (act == SIG_ACTION_IGNORE) {
                task->pending_signals &= ~(1ULL << sig);
                continue;
            }
            /* Terminate */
            task->pending_signals &= ~(1ULL << sig);
            kprintf("[SIGNAL] PID %u killed by sig %d (default)\n", pid, sig);
            task->exit_code   = 128 + sig;
            task->term_signal = (uint8_t)sig;
            task->state = TASK_TERMINATED;
            return 1;
        }

        /* Custom userspace handler — build rt_sigframe on user stack */
        task->pending_signals &= ~(1ULL << sig);

        /* Block the signal during handler (unless SA_NODEFER) */
        uint64_t old_mask = task->sig_mask;
        if (!(task->sig_flags[sig] & SA_NODEFER))
            task->sig_mask |= (1ULL << sig);

        /* SA_RESETHAND: reset to SIG_DFL before invoking handler */
        if (task->sig_flags[sig] & SA_RESETHAND) {
            task->sig_handlers[sig] = 0; /* SIG_DFL */
            task->sig_flags[sig]    = 0;
        }

        /* Align RSP to 16 bytes, then subtract frame size */
        uint64_t user_rsp = regs->rsp & ~0xFULL;
        user_rsp -= SIGFRAME_TOTAL_SIZE;
        user_rsp &= ~0xFULL;

        if (!is_user_address(user_rsp, SIGFRAME_TOTAL_SIZE)) {
            /* Can't build frame — kill process */
            kprintf("[SIGNAL] SIGSEGV: bad user RSP %lx\n", user_rsp);
            task->exit_code = 128 + 11; /* SIGSEGV */
            task->state = TASK_TERMINATED;
            return 1;
        }

        /* Zero out the frame */
        uint8_t zero[SIGFRAME_TOTAL_SIZE];
        memset(zero, 0, SIGFRAME_TOTAL_SIZE);
        copy_to_user((void *)user_rsp, zero, SIGFRAME_TOTAL_SIZE);

        /* pretcode = sa_restorer (NULL → kernel will handle) */
        uint64_t restorer = task->sig_restorer[sig];
        copy_to_user((void *)(user_rsp + SIGFRAME_PRETCODE_OFF), &restorer, 8);

        /* uc_sigmask — save pre-handler mask so sigreturn restores it */
        copy_to_user((void *)(user_rsp + SIGFRAME_SIGMASK_OFF), &old_mask, 8);

        /* sigcontext — save current registers */
        uint64_t tmp;
        tmp = regs->r8;  copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +   0), &tmp, 8);
        tmp = regs->r9;  copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +   8), &tmp, 8);
        tmp = regs->r10; copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +  16), &tmp, 8);
        tmp = regs->r11; copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +  24), &tmp, 8);
        tmp = regs->r12; copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +  32), &tmp, 8);
        tmp = regs->r13; copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +  40), &tmp, 8);
        tmp = regs->r14; copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +  48), &tmp, 8);
        tmp = regs->r15; copy_to_user((void *)(user_rsp + SIGFRAME_SC_R8_OFF +  56), &tmp, 8);
        tmp = regs->rdi; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF),      &tmp, 8);
        tmp = regs->rsi; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF +  8), &tmp, 8);
        tmp = regs->rbp; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF + 16), &tmp, 8);
        tmp = regs->rbx; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF + 24), &tmp, 8);
        tmp = regs->rdx; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF + 32), &tmp, 8);
        tmp = regs->rax; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF + 40), &tmp, 8);
        tmp = regs->rcx; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RDI_OFF + 48), &tmp, 8);
        tmp = regs->rsp; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RSP_OFF),     &tmp, 8);
        tmp = regs->rip; copy_to_user((void *)(user_rsp + SIGFRAME_SC_RIP_OFF),     &tmp, 8);
        tmp = regs->rflags; copy_to_user((void *)(user_rsp + SIGFRAME_SC_EFLAGS_OFF), &tmp, 8);
        /* cs = 0x23 (user code) */
        uint16_t cs = 0x23;
        copy_to_user((void *)(user_rsp + SIGFRAME_SC_CS_OFF), &cs, 2);

        /* Redirect execution to the signal handler */
        regs->rsp = user_rsp;    /* new user stack = signal frame */
        regs->rip = handler;     /* jump to handler */
        regs->rdi = (uint64_t)sig;  /* arg1 = signal number */
        regs->rsi = 0;           /* arg2 = siginfo_t* (NULL for SA_HANDLER) */
        regs->rdx = user_rsp + 8;   /* arg3 = ucontext* */
        /* Don't modify rax — the syscall return value isn't used in the handler */

        return 1; /* frame set up, deliver one signal at a time */
    }

    return 0;
}
