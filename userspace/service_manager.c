#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "u_stdlib.h"

void _start(void) {
    const char *msg = "[SM] Service Manager (PID 1) Started.\n";
    syscall3(SYS_WRITE, 1, (long)msg, 38);

    // 1. Spawn Compositor (Critical GUI Service)
    const char *comp_path = "compositor.elf";
    long comp_pid = syscall1(SYS_PROC_EXEC, (long)comp_path);
    
    if (comp_pid > 0) {
        syscall3(SYS_WRITE, 1, (long)"[SM] Started Compositor.\n", 25);
    } else {
        syscall3(SYS_WRITE, 1, (long)"[SM] Failed to start Compositor!\n", 33);
    }

    // 2. Start Keyboard Driver
    const char *kbd_path = "keyboard_driver.elf";
    long kbd_pid = syscall1(SYS_PROC_EXEC, (long)kbd_path);
    if (kbd_pid > 0) {
         syscall3(SYS_WRITE, 1, (long)"[SM] Started Keyboard Driver.\n", 30);
    }

    // 3. Start Mouse Driver
    const char *mouse_path = "mouse_driver.elf";
    long mouse_pid = syscall1(SYS_PROC_EXEC, (long)mouse_path);
    if (mouse_pid > 0) {
         syscall3(SYS_WRITE, 1, (long)"[SM] Started Mouse Driver.\n", 27);
    }
    
    // 4. Start Terminal (Demo App)
    const char *term_path = "terminal.elf";
    long term_pid = syscall1(SYS_PROC_EXEC, (long)term_path);
    if (term_pid > 0) {
         syscall3(SYS_WRITE, 1, (long)"[SM] Started Terminal.\n", 23);
    }

    // 5. Wait loop (Prevent exit)
    while (1) {
        int status;
        long pid = syscall1(SYS_WAIT, (long)&status);
        if (pid > 0) {
            // Restart critical services
            if (pid == comp_pid) {
                syscall3(SYS_WRITE, 1, (long)"[SM] Compositor died! Restarting...\n", 36);
                comp_pid = syscall1(SYS_PROC_EXEC, (long)comp_path);
            }
        }
    }
}
