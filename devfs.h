#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"
#include "driver.h"

// Maximum device nodes in /dev
#define DEVFS_MAX_NODES 32

// Initialize devfs (creates /dev directory node)
void devfs_init(void);

// Register a device node in /dev
// Returns 0 on success, -1 on failure
int devfs_register(const char *name, driver_t *drv);

// Get the /dev root node (for mounting)
vfs_node_t *devfs_get_root(void);

#endif
