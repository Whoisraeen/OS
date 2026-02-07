#include "vfs.h"
#include <stddef.h>

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
    if (node == NULL || !(node->flags & VFS_DIRECTORY) || node->readdir == NULL) {
        return NULL;
    }
    return node->readdir(node, index);
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    if (node == NULL || !(node->flags & VFS_DIRECTORY) || node->finddir == NULL) {
        return NULL;
    }
    return node->finddir(node, name);
}
