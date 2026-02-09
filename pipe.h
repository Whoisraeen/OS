#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"
#include "sched.h"

#define PIPE_BUF_SIZE 4096

typedef struct pipe {
    uint8_t buffer[PIPE_BUF_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t count;           // Bytes currently in buffer
    spinlock_t lock;
    int readers;            // Number of open read ends
    int writers;            // Number of open write ends
    task_t *blocked_reader; // Task blocked waiting for data
    task_t *blocked_writer; // Task blocked waiting for space
} pipe_t;

// Create a pipe. Returns 0 on success, -1 on failure.
// Stores read fd in *read_fd and write fd in *write_fd.
struct fd_table;
int pipe_create(struct fd_table *table, int *read_fd, int *write_fd);

// Read from pipe (called via fd layer)
size_t pipe_read(pipe_t *pipe, uint8_t *buf, size_t count);

// Write to pipe (called via fd layer)
size_t pipe_write(pipe_t *pipe, const uint8_t *buf, size_t count);

// Close a pipe end. type: 0 = read end, 1 = write end.
void pipe_close(pipe_t *pipe, int is_write_end);

#endif
