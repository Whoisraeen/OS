#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

// Forward declaration
struct vfs_node;

// Function pointer types for VFS operations
typedef size_t (*read_fn)(struct vfs_node *node, size_t offset, size_t size, uint8_t *buffer);
typedef size_t (*write_fn)(struct vfs_node *node, size_t offset, size_t size, uint8_t *buffer);
typedef struct vfs_node *(*readdir_fn)(struct vfs_node *node, size_t index);
typedef struct vfs_node *(*finddir_fn)(struct vfs_node *node, const char *name);

// File types
#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02

// VFS Node structure
typedef struct vfs_node {
    char name[128];          // Filename
    uint32_t flags;          // File type flags
    size_t length;           // File size
    uint64_t inode;          // Inode number (unique identifier)
    
    // Function pointers for operations
    read_fn read;
    write_fn write;
    readdir_fn readdir;
    finddir_fn finddir;
    
    // Pointer to implementation-specific data
    void *impl;
} vfs_node_t;

// VFS functions
size_t vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
size_t vfs_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
vfs_node_t *vfs_readdir(vfs_node_t *node, size_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);

// Root filesystem
extern vfs_node_t *vfs_root;

#endif
