#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stddef.h>

// Maximum command line length
#define SHELL_BUFFER_SIZE 256

// Initialize shell
void shell_init(void);

// Process a character from keyboard (called from keyboard handler)
void shell_input(char c);

// Run the shell (main loop - for blocking mode)
void shell_run(void);

// Check if shell has a complete command ready
int shell_has_command(void);

// Execute pending command
void shell_execute(void);

#endif
