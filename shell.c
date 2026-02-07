#include "shell.h"
#include "console.h"
#include "serial.h"
#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "timer.h"
#include "speaker.h"
#include "elf.h"

// Command buffer
static char cmd_buffer[SHELL_BUFFER_SIZE];
static size_t cmd_pos = 0;
static int cmd_ready = 0;

// String comparison
static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

// String starts with
static int starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

// Get string length
static size_t slen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

// Command handlers
static void cmd_help(void) {
    console_printf("\n Available commands:\n");
    console_printf("   help   - Show this help\n");
    console_printf("   clear  - Clear the screen\n");
    console_printf("   echo   - Echo text (echo <text>)\n");
    console_printf("   ls     - List files in initrd\n");
    console_printf("   cat    - Show file contents (cat <file>)\n");
    console_printf("   exec   - Execute ELF binary (exec <file>)\n");
    console_printf("   info   - System information\n");
    console_printf("   mem    - Memory usage\n");
    console_printf("   uptime - System uptime\n");
    console_printf("   beep   - Play a sound\n");
    console_printf("   reboot - Reboot the system\n");
}

static void cmd_clear(void) {
    console_clear();
}

static void cmd_info(void) {
    console_printf("\n RaeenOS v0.1\n");
    console_printf(" Architecture: x86_64\n");
    console_printf(" Features: PMM, VMM, Heap, VFS, Ring 3, Syscalls\n");
}

static void cmd_ls(void) {
    console_printf("\n");
    extern vfs_node_t *vfs_root;
    if (vfs_root == NULL) {
        console_printf(" No filesystem mounted.\n");
        return;
    }
    
    for (size_t i = 0; ; i++) {
        vfs_node_t *node = vfs_readdir(vfs_root, i);
        if (node == NULL) break;
        console_printf("   %s (%lu bytes)\n", node->name, node->length);
    }
}

static void cmd_cat(const char *filename) {
    console_printf("\n");
    
    // Skip "cat " prefix
    while (*filename == ' ') filename++;
    
    if (*filename == '\0') {
        console_printf(" Usage: cat <filename>\n");
        return;
    }
    
    vfs_node_t *file = initrd_find(filename);
    if (file == NULL) {
        console_printf(" File not found: %s\n", filename);
        return;
    }
    
    uint8_t buffer[512];
    size_t read = vfs_read(file, 0, 511, buffer);
    buffer[read] = '\0';
    console_printf(" %s\n", (char *)buffer);
}

static void cmd_echo(const char *text) {
    console_printf("\n %s\n", text);
}

static void cmd_reboot(void) {
    console_printf("\n Rebooting...\n");
    // Triple fault to reboot
    __asm__ volatile (
        "lidt (0)\n"
        "int $0"
    );
}

static void cmd_mem(void) {
    size_t used = heap_used();
    size_t available = heap_free();
    size_t total = used + available;
    console_printf("\n Memory Usage:\n");
    console_printf("   Used:      %lu KB\n", used / 1024);
    console_printf("   Free:      %lu KB\n", available / 1024);
    console_printf("   Total:     %lu KB\n", total / 1024);
}

static void cmd_uptime(void) {
    uint64_t ticks = timer_get_ticks();
    uint64_t seconds = ticks / 100;  // 100 Hz timer
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    console_printf("\n Uptime: %lu:%02lu:%02lu (%lu ticks)\n", 
                   hours, minutes % 60, seconds % 60, ticks);
}

static void cmd_beep(void) {
    console_printf("\n Beep!\n");
    speaker_success();
}

static void cmd_exec(const char *filename) {
    console_printf("\n Loading %s...\n", filename);
    
    // Find file
    vfs_node_t *file = initrd_find(filename);
    if (!file) {
        console_printf(" File not found: %s\n", filename);
        return;
    }
    
    // Read file (max 64KB for now)
    size_t size = file->length;
    if (size > 65536) {
        console_printf(" File too large (max 64KB)\n");
        return;
    }
    
    void *buffer = kmalloc(size);
    if (!buffer) {
        console_printf(" Out of memory\n");
        return;
    }
    
    vfs_read(file, 0, size, buffer);
    
    // Load and execute
    elf_load_result_t result = elf_load(buffer, size);
    if (result.success) {
        console_printf(" Executing at 0x%lx...\n", result.entry_point);
        elf_execute(result.entry_point);
    } else {
        console_printf(" Failed to load ELF\n");
    }
    
    kfree(buffer);
}

// Execute a command
static void execute_command(const char *cmd) {
    // Skip leading spaces
    while (*cmd == ' ') cmd++;
    
    if (*cmd == '\0') {
        return;
    }
    
    if (streq(cmd, "help") || streq(cmd, "?")) {
        cmd_help();
    } else if (streq(cmd, "clear") || streq(cmd, "cls")) {
        cmd_clear();
    } else if (streq(cmd, "info")) {
        cmd_info();
    } else if (streq(cmd, "ls") || streq(cmd, "dir")) {
        cmd_ls();
    } else if (starts_with(cmd, "cat ")) {
        cmd_cat(cmd + 4);
    } else if (starts_with(cmd, "echo ")) {
        cmd_echo(cmd + 5);
    } else if (starts_with(cmd, "exec ")) {
        cmd_exec(cmd + 5);
    } else if (streq(cmd, "mem")) {
        cmd_mem();
    } else if (streq(cmd, "uptime")) {
        cmd_uptime();
    } else if (streq(cmd, "beep")) {
        cmd_beep();
    } else if (streq(cmd, "reboot")) {
        cmd_reboot();
    } else {
        speaker_error();
        console_printf("\n Unknown command: %s\n", cmd);
        console_printf(" Type 'help' for available commands.\n");
    }
}

void shell_init(void) {
    cmd_pos = 0;
    cmd_ready = 0;
    cmd_buffer[0] = '\0';
    
    console_printf("\n");
    console_printf(" RaeenOS Shell v1.0\n");
    console_printf(" Type 'help' for commands.\n\n");
    console_printf("$ ");
}

void shell_input(char c) {
    if (c == '\n') {
        // Command complete
        console_putc('\n');
        cmd_buffer[cmd_pos] = '\0';
        cmd_ready = 1;
    } else if (c == '\b') {
        // Backspace
        if (cmd_pos > 0) {
            cmd_pos--;
            console_printf("\b \b");  // Erase character
        }
    } else if (c >= 32 && c < 127) {
        // Printable character
        if (cmd_pos < SHELL_BUFFER_SIZE - 1) {
            cmd_buffer[cmd_pos++] = c;
            console_putc(c);
        }
    }
}

int shell_has_command(void) {
    return cmd_ready;
}

void shell_execute(void) {
    if (cmd_ready) {
        execute_command(cmd_buffer);
        cmd_pos = 0;
        cmd_ready = 0;
        cmd_buffer[0] = '\0';
        console_printf("$ ");
    }
}

void shell_run(void) {
    // Interactive loop (uses HLT, keyboard interrupts drive input)
    for (;;) {
        if (shell_has_command()) {
            shell_execute();
        }
        __asm__ volatile ("hlt");
    }
}
