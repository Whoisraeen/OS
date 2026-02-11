#include "vfs.h"
#include <stddef.h>
#include "string.h"
#include "fd.h"
#include "serial.h"

// Root filesystem node
vfs_node_t *vfs_root = NULL;

size_t vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    if (node == NULL || node->read == NULL) {
        return 0;
    }
    return node->read(node, offset, size, buffer);
}

size_t vfs_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    if (node == NULL || node->write == NULL) {
        return 0;
    }
    return node->write(node, offset, size, buffer);
}

vfs_node_t *vfs_readdir(vfs_node_t *node, size_t index) {
    if (node == NULL) return NULL;
    
    // Traverse mount point
    if ((node->flags & VFS_MOUNTPOINT) && node->ptr) {
        node = node->ptr;
    }

    if (!(node->flags & VFS_DIRECTORY) || node->readdir == NULL) {
        return NULL;
    }
    return node->readdir(node, index);
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    if (node == NULL) return NULL;
    
    // Traverse mount point
    if ((node->flags & VFS_MOUNTPOINT) && node->ptr) {
        node = node->ptr;
    }

    if (!(node->flags & VFS_DIRECTORY) || node->finddir == NULL) {
        return NULL;
    }
    
    vfs_node_t *child = node->finddir(node, name);
    
    // Check if child is a mount point
    if (child && (child->flags & VFS_MOUNTPOINT) && child->ptr) {
        return child->ptr;
    }
    
    return child;
}

vfs_node_t *vfs_create(vfs_node_t *parent, const char *name, int flags) {
    // Traverse mount points for parent
    if (parent && (parent->flags & VFS_MOUNTPOINT) && parent->ptr) {
        parent = parent->ptr;
    }
    
    if (parent == NULL || !(parent->flags & VFS_DIRECTORY)) return NULL;
    
    // Check if file already exists
    if (parent->finddir) {
        vfs_node_t *existing = parent->finddir(parent, name);
        if (existing) return existing; // Or fail with EEXIST if O_EXCL?
    }
    
    if (parent->create == NULL) return NULL;
    
    return parent->create(parent, name, flags);
}

int vfs_mkdir_node(vfs_node_t *parent, const char *name) {
    // Traverse mount points
    if (parent && (parent->flags & VFS_MOUNTPOINT) && parent->ptr) {
        parent = parent->ptr;
    }

    if (parent == NULL || !(parent->flags & VFS_DIRECTORY)) return -1;
    if (parent->mkdir == NULL) return -1;
    
    return parent->mkdir(parent, name);
}

vfs_node_t *vfs_open(const char *path, int flags) {
    if (!path) return NULL;
    
    vfs_node_t *current = vfs_root;
    if (!current) return NULL;
    
    // Handle root path
    if (strcmp(path, "/") == 0) return current;
    
    // Skip leading slash
    if (path[0] == '/') path++;
    
    char name[128];
    const char *p = path;
    
    while (*p) {
        // Extract component
        int i = 0;
        while (*p && *p != '/' && i < 127) {
            name[i++] = *p++;
        }
        name[i] = '\0';
        
        // Skip consecutive slashes
        while (*p == '/') p++;
        
        // Check if this is the last component
        int is_last = (*p == '\0');

        // Find child
        vfs_node_t *child = vfs_finddir(current, name);
        
        if (!child) {
            // If missing and last component and O_CREAT
            if (is_last && (flags & O_CREAT)) {
                return vfs_create(current, name, flags);
            }
            return NULL;
        }
        
        current = child;
    }
    
    return current;
}

int vfs_mount(const char *path, vfs_node_t *fs_root) {
    if (!path || !fs_root) return -1;
    
    // Resolve the mount point directory
    // We can't use vfs_open because it follows mount points, and we might want to mount OVER an existing one?
    // Actually, usually we mount ON a directory.
    
    // For now, simple implementation: find the node and set the flag.
    // We use vfs_open to find the node.
    vfs_node_t *node = vfs_open(path, 0);
    if (!node) return -1;
    
    if (!(node->flags & VFS_DIRECTORY)) return -1;
    
    node->flags |= VFS_MOUNTPOINT;
    node->ptr = fs_root;
    
    return 0;
}
