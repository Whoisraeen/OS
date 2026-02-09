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

// Send a signal to a process. Returns 0 on success, -1 on error.
int signal_send(uint32_t pid, int sig);

// Set signal disposition for current process. Returns previous disposition.
int signal_set_handler(uint32_t pid, int sig, int action);

// Check and deliver pending signals for a process.
// Called before returning to userspace. Returns 1 if process was killed.
int signal_deliver_pending(uint32_t pid);

// Get default action for a signal
int signal_default_action(int sig);

#endif
