#include "signal.h"
#include "sched.h"
#include "serial.h"
#include "console.h"

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
            task->exit_code = 128 + sig;
            task->state = TASK_TERMINATED;
            return 1; // Process was killed
        }
    }

    return 0;
}
