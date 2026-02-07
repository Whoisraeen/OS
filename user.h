#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stddef.h>

// Jump to user mode
// entry = user code entry point
// stack = user stack pointer
extern void jump_to_usermode(uint64_t entry, uint64_t stack);

// Simple embedded user program for testing
// This is the user code that will run in Ring 3
void user_program_entry(void);

// Start a user process
void start_user_process(void);

#endif
