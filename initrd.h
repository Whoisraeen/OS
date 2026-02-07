#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

// Initialize initrd from a memory location
// Returns the root VFS node for the initrd
vfs_node_t *initrd_init(void *initrd_start, size_t initrd_size);

// Find a file in the initrd by path
vfs_node_t *initrd_find(const char *path);

#endif
