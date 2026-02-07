#include <stdint.h>

// Syscall numbers
#define SYS_EXIT 0
#define SYS_WRITE 1
#define SYS_YIELD 2

static inline long syscall3(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#define SYS_PROC_EXEC 41 // Not implemented yet, we need a way to spawn processes

// Mock spawn for now, we will rely on Kernel to have loaded compositor separately
// OR, we can implement SYS_EXEC properly.
// But for now, let's just loop and print.

void _start(void) {
    const char *msg = "Hello from Userspace Init Process! Waiting for Compositor...\n";
    
    // Calculate length
    long len = 0;
    while (msg[len]) len++;
    
    // Write to stdout (1)
    syscall3(SYS_WRITE, 1, (long)msg, len);
    
    // Yield loop
    for (;;) {
        syscall3(SYS_YIELD, 0, 0, 0);
    }
    
    // Exit (should not reach)
    syscall3(SYS_EXIT, 0, 0, 0);
}