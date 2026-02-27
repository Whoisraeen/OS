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
typedef struct vfs_node *(*create_fn)(struct vfs_node *parent, const char *name, int flags);
typedef int (*mkdir_fn)(struct vfs_node *parent, const char *name);
typedef int (*unlink_fn)(struct vfs_node *parent, const char *name);
typedef int (*rmdir_fn)(struct vfs_node *parent, const char *name);
typedef int (*rename_fn)(struct vfs_node *old_parent, const char *old_name,
                         struct vfs_node *new_parent, const char *new_name);

// File types
#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02
#define VFS_MOUNTPOINT 0x04

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
    create_fn create;
    mkdir_fn  mkdir;
    unlink_fn unlink;
    rmdir_fn  rmdir;
    rename_fn rename;

    // Pointer to implementation-specific data
    void *impl;
    
    // Mount point target (if VFS_MOUNTPOINT is set)
    struct vfs_node *ptr;
} vfs_node_t;

// VFS functions
size_t vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
size_t vfs_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
vfs_node_t *vfs_readdir(vfs_node_t *node, size_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name);
vfs_node_t *vfs_create(vfs_node_t *parent, const char *name, int flags);
int vfs_mkdir_node(vfs_node_t *parent, const char *name);

// Path resolution and mounting
vfs_node_t *vfs_open(const char *path, int flags);
int vfs_mount(const char *path, vfs_node_t *fs_root);
int vfs_mkdir(const char *path);           // Create directory at absolute path
int vfs_unlink(const char *path);          // Remove file
int vfs_rmdir(const char *path);           // Remove empty directory
int vfs_rename(const char *old_path, const char *new_path); // Rename/move

// Resolve a possibly-relative path against cwd into an absolute path in out.
// Handles ".", "..", multiple slashes. out must be at least out_len bytes.
void vfs_resolve_path(const char *cwd, const char *path, char *out, size_t out_len);

// Root filesystem
extern vfs_node_t *vfs_root;

#endif
