#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"
#include "driver.h"

// Maximum device nodes in /dev
#define DEVFS_MAX_NODES 32

// Initialize devfs (creates /dev directory node)
void devfs_init(void);

// Register a device node in /dev backed by a driver
// Returns 0 on success, -1 on failure
int devfs_register(const char *name, driver_t *drv);

// Register a simple device node with bare read/write function pointers
// (no driver_t needed — for null, zero, urandom, tty, etc.)
int devfs_register_simple(const char *name,
                          size_t (*read_fn)(uint8_t *buf, size_t count),
                          size_t (*write_fn)(const uint8_t *buf, size_t count));

// Get the /dev root node (for mounting)
vfs_node_t *devfs_get_root(void);

#endif
