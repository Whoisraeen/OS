#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

// Signal numbers
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define NSIG     32

// Signal actions
#define SIG_DFL  0  // Default action
#define SIG_IGN  1  // Ignore

// Default action types
#define SIG_ACTION_TERM     0   // Terminate process
#define SIG_ACTION_IGNORE   1   // Ignore
#define SIG_ACTION_CORE     2   // Terminate + core dump (treated as term for now)

// sa_flags bits (Linux-compatible)
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000  // Do not mask signal during handler
#define SA_RESETHAND  0x80000000  // Reset to SIG_DFL after first delivery

// Send a signal to a process. Returns 0 on success, -1 on error.
int signal_send(uint32_t pid, int sig);

// Set simple kernel-side disposition (SIG_DFL/SIG_IGN). Returns previous.
int signal_set_handler(uint32_t pid, int sig, int action);

// Set full userspace handler (called from rt_sigaction).
void signal_set_userhandler(uint32_t pid, int sig,
                            uint64_t handler, uint64_t restorer,
                            uint32_t flags);

// Check and deliver pending signals for a process.
// Called before returning to userspace. Returns 1 if process was killed.
int signal_deliver_pending(uint32_t pid);

// Build signal frame on user stack and redirect execution to the handler.
// regs: kernel-saved interrupt frame (rip/rsp will be modified in-place).
// Call this from the syscall return path. Returns 1 if frame was set up.
struct interrupt_frame;
int signal_try_deliver_frame(struct interrupt_frame *regs);

// Get default action for a signal
int signal_default_action(int sig);

#endif
