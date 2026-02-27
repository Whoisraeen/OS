#include "vfs.h"
#include <stddef.h>
#include "string.h"
#include "fd.h"
#include "serial.h"
#include <stdint.h>

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

void vfs_resolve_path(const char *cwd, const char *path, char *out, size_t out_len) {
    char buf[512];
    int len = 0;

    if (!path) path = "";
    if (!cwd || cwd[0] == '\0') cwd = "/";

    if (path[0] == '/') {
        // Absolute path: start from root
        buf[len++] = '/';
    } else {
        // Relative: start with cwd
        int cwd_len = (int)strlen(cwd);
        if (cwd_len > (int)sizeof(buf) - 2) cwd_len = (int)sizeof(buf) - 2;
        memcpy(buf, cwd, cwd_len);
        len = cwd_len;
        // Ensure trailing slash for component appending
        if (len == 0 || buf[len - 1] != '/') buf[len++] = '/';
    }

    // Process components from path
    const char *p = path;
    while (*p) {
        // Skip slashes
        while (*p == '/') p++;
        if (!*p) break;

        // Extract component
        const char *seg = p;
        while (*p && *p != '/') p++;
        int seg_len = (int)(p - seg);

        if (seg_len == 0) continue;
        if (seg_len == 1 && seg[0] == '.') continue; // "." → stay
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            // ".." → go up one level
            if (len > 1) {
                len--; // skip possible trailing slash
                while (len > 0 && buf[len - 1] != '/') len--;
                // len now points just after the parent '/'
                if (len == 0) { buf[0] = '/'; len = 1; }
            }
        } else {
            // Append separator if needed
            if (len > 0 && buf[len - 1] != '/') {
                if (len < (int)sizeof(buf) - 1) buf[len++] = '/';
            }
            // Append component
            if (len + seg_len < (int)sizeof(buf) - 1) {
                memcpy(buf + len, seg, seg_len);
                len += seg_len;
            }
        }
    }

    // Remove trailing slash unless root
    if (len > 1 && buf[len - 1] == '/') len--;
    buf[len] = '\0';

    if (out_len > 0) {
        int copy = len + 1;
        if (copy > (int)out_len) copy = (int)out_len;
        memcpy(out, buf, copy);
        out[out_len - 1] = '\0';
    }
}

int vfs_mkdir(const char *path) {
    if (!path || path[0] == '\0') return -1;

    char buf[512];
    int path_len = (int)strlen(path);
    if (path_len >= (int)sizeof(buf)) return -1;
    memcpy(buf, path, path_len + 1);

    // Find last '/' to split parent path from new dir name
    char *last_slash = NULL;
    for (char *cp = buf; *cp; cp++) {
        if (*cp == '/') last_slash = cp;
    }
    if (!last_slash) return -1; // no slash in path

    const char *name = last_slash + 1;
    if (*name == '\0') return -1; // path ends with '/'

    vfs_node_t *parent;
    if (last_slash == buf) {
        // Parent is root "/"
        parent = vfs_root;
    } else {
        *last_slash = '\0'; // terminate parent path
        parent = vfs_open(buf, 0);
    }

    if (!parent || !(parent->flags & VFS_DIRECTORY)) return -1;
    return vfs_mkdir_node(parent, name);
}

/* Split absolute path into parent node + leaf name. Writes leaf name into
   name_out (must be >= 128 bytes). Returns parent node or NULL. */
static vfs_node_t *path_split_parent(const char *path, char *name_out)
{
    if (!path || path[0] != '/') return NULL;

    char buf[512];
    size_t plen = strlen(path);
    if (plen >= sizeof(buf)) return NULL;
    memcpy(buf, path, plen + 1);

    char *last_slash = NULL;
    for (char *cp = buf; *cp; cp++) if (*cp == '/') last_slash = cp;
    if (!last_slash) return NULL;

    const char *name = last_slash + 1;
    if (*name == '\0') return NULL; /* trailing slash */

    /* Copy leaf name */
    size_t nlen = strlen(name);
    if (nlen >= 128) nlen = 127;
    memcpy(name_out, name, nlen);
    name_out[nlen] = '\0';

    /* Open parent */
    if (last_slash == buf) return vfs_root; /* parent is "/" */
    *last_slash = '\0';
    return vfs_open(buf, 0);
}

int vfs_unlink(const char *path)
{
    char name[128];
    vfs_node_t *parent = path_split_parent(path, name);
    if (!parent || !(parent->flags & VFS_DIRECTORY)) return -1;
    if (!parent->unlink) return -1;
    return parent->unlink(parent, name);
}

int vfs_rmdir(const char *path)
{
    char name[128];
    vfs_node_t *parent = path_split_parent(path, name);
    if (!parent || !(parent->flags & VFS_DIRECTORY)) return -1;
    if (!parent->rmdir) return -1;
    return parent->rmdir(parent, name);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char old_name[128], new_name[128];
    vfs_node_t *old_parent = path_split_parent(old_path, old_name);
    vfs_node_t *new_parent = path_split_parent(new_path, new_name);
    if (!old_parent || !new_parent) return -1;
    if (!old_parent->rename) return -1;
    return old_parent->rename(old_parent, old_name, new_parent, new_name);
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
