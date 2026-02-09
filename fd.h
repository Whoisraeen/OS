#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

#define MAX_FDS 64

// File descriptor types
typedef enum {
    FD_NONE = 0,
    FD_FILE,
    FD_PIPE,
    FD_SOCKET,
    FD_DEVICE
} fd_type_t;

// Open flags
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

// Seek whence
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

// Device operations (for FD_DEVICE type, e.g. console)
typedef struct fd_device_ops {
    size_t (*read)(struct fd_device_ops *dev, uint8_t *buf, size_t count);
    size_t (*write)(struct fd_device_ops *dev, const uint8_t *buf, size_t count);
} fd_device_ops_t;

// File descriptor entry
typedef struct {
    fd_type_t type;
    uint32_t flags;
    size_t offset;          // Current file position (FD_FILE)
    union {
        vfs_node_t *node;       // FD_FILE
        fd_device_ops_t *dev;   // FD_DEVICE
        void *pipe;             // FD_PIPE (future)
        void *socket;           // FD_SOCKET (future)
    };
} fd_entry_t;

// Per-process file descriptor table
struct fd_table {
    fd_entry_t entries[MAX_FDS];
};
typedef struct fd_table fd_table_t;

// Create/destroy fd tables
fd_table_t *fd_table_create(void);
void fd_table_destroy(fd_table_t *table);

// Allocate lowest available fd (returns fd number or -1)
int fd_alloc(fd_table_t *table);

// Free an fd
void fd_free(fd_table_t *table, int fd);

// Get fd entry (returns NULL if invalid or not open)
fd_entry_t *fd_get(fd_table_t *table, int fd);

// Duplicate fd to lowest available (returns new fd or -1)
int fd_dup(fd_table_t *table, int old_fd);

// Duplicate fd to specific number (closes new_fd if open, returns new_fd or -1)
int fd_dup2(fd_table_t *table, int old_fd, int new_fd);

// Initialize stdin(0)/stdout(1)/stderr(2) as console devices
void fd_init_stdio(fd_table_t *table);

#endif
