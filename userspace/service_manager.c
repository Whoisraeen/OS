#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "u_stdlib.h"

// Capability Constants
#define CAP_HW_VIDEO           (1ULL << 23)
#define CAP_HW_INPUT           (1ULL << 24)
#define CAP_IPC_CREATE         (1ULL << 30)
#define CAP_IPC_SHMEM          (1ULL << 33)
#define CAP_PROC_EXEC          (1ULL << 41)
#define CAP_SYS_REBOOT         (1ULL << 52)

// Utils moved to u_stdlib.h

void _start(void) {
    // Debug: Simple loop to verify we reached user mode
    // syscall3(SYS_WRITE, 1, (long)"[SM] ALIVE\n", 12);
    
    const char *msg = "[SM] Service Manager (PID 1) v2 Started.\n";
    syscall3(SYS_WRITE, 1, (long)msg, 41);

    // 1. Spawn Compositor (Critical GUI Service)
    const char *comp_path = "compositor.elf";
    long comp_pid = syscall1(SYS_PROC_EXEC, (long)comp_path);
    
    if (comp_pid > 0) {
        // Grant Capabilities: Video Hardware, IPC Creation, Shared Memory
        syscall2(SYS_SEC_GRANT, comp_pid, CAP_HW_VIDEO | CAP_IPC_CREATE | CAP_IPC_SHMEM);
        syscall3(SYS_WRITE, 1, (long)"[SM] Started Compositor.\n", 25);
    } else {
        syscall3(SYS_WRITE, 1, (long)"[SM] Failed to start Compositor!\n", 33);
    }
    
    // 1b. Spawn Panel (Desktop UI)
    const char *panel_path = "panel.elf";
    long panel_pid = syscall1(SYS_PROC_EXEC, (long)panel_path);
    if (panel_pid > 0) {
         // Grant Capabilities: IPC Creation, Shared Memory, Reboot/Shutdown, Process Execution
         syscall2(SYS_SEC_GRANT, panel_pid, CAP_IPC_CREATE | CAP_IPC_SHMEM | CAP_SYS_REBOOT | CAP_PROC_EXEC);
         syscall3(SYS_WRITE, 1, (long)"[SM] Started Panel.\n", 20);
    }

    // 2. Start Keyboard Driver
    const char *kbd_path = "keyboard_driver.elf";
    long kbd_pid = syscall1(SYS_PROC_EXEC, (long)kbd_path);
    if (kbd_pid > 0) {
         // CAP_HW_INPUT | CAP_IPC_CREATE
         syscall2(SYS_SEC_GRANT, kbd_pid, CAP_HW_INPUT | CAP_IPC_CREATE);
         syscall3(SYS_WRITE, 1, (long)"[SM] Started Keyboard Driver.\n", 30);
    }

    // 3. Start Mouse Driver
    const char *mouse_path = "mouse_driver.elf";
    long mouse_pid = syscall1(SYS_PROC_EXEC, (long)mouse_path);
    if (mouse_pid > 0) {
         // CAP_HW_INPUT | CAP_IPC_CREATE
         syscall2(SYS_SEC_GRANT, mouse_pid, CAP_HW_INPUT | CAP_IPC_CREATE);
         syscall3(SYS_WRITE, 1, (long)"[SM] Started Mouse Driver.\n", 27);
    }
    
    // 4. Start Terminal (Demo App)
    const char *term_path = "terminal.elf";
    long term_pid = syscall1(SYS_PROC_EXEC, (long)term_path);
    if (term_pid > 0) {
         // Grant Capabilities: IPC Creation, Shared Memory
         syscall2(SYS_SEC_GRANT, term_pid, CAP_IPC_CREATE | CAP_IPC_SHMEM);
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
