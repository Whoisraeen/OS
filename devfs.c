#include "devfs.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

// Device node entry
typedef struct {
    vfs_node_t node;
    driver_t *driver;
    int used;
} devfs_entry_t;

static devfs_entry_t dev_nodes[DEVFS_MAX_NODES];
static int dev_count = 0;
static vfs_node_t devfs_root_node;

// devfs readdir
static vfs_node_t *devfs_readdir(vfs_node_t *node, size_t index) {
    (void)node;
    if (index >= (size_t)dev_count) return NULL;

    // Find the nth used entry
    size_t found = 0;
    for (int i = 0; i < DEVFS_MAX_NODES; i++) {
        if (dev_nodes[i].used) {
            if (found == index) return &dev_nodes[i].node;
            found++;
        }
    }
    return NULL;
}

// devfs finddir
static vfs_node_t *devfs_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    for (int i = 0; i < DEVFS_MAX_NODES; i++) {
        if (!dev_nodes[i].used) continue;
        // strcmp
        const char *a = dev_nodes[i].node.name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return &dev_nodes[i].node;
    }
    return NULL;
}

// devfs node read (delegates to driver)
static size_t devfs_node_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    devfs_entry_t *entry = (devfs_entry_t *)node->impl;
    if (!entry || !entry->driver || !entry->driver->ops || !entry->driver->ops->read)
        return 0;
    int ret = entry->driver->ops->read(entry->driver, buffer, size, offset);
    return ret < 0 ? 0 : (size_t)ret;
}

// devfs node write (delegates to driver)
static size_t devfs_node_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    devfs_entry_t *entry = (devfs_entry_t *)node->impl;
    if (!entry || !entry->driver || !entry->driver->ops || !entry->driver->ops->write)
        return 0;
    int ret = entry->driver->ops->write(entry->driver, (const uint8_t *)buffer, size, offset);
    return ret < 0 ? 0 : (size_t)ret;
}

void devfs_init(void) {
    for (int i = 0; i < DEVFS_MAX_NODES; i++)
        dev_nodes[i].used = 0;
    dev_count = 0;

    // Setup root node
    for (int i = 0; i < 128; i++) devfs_root_node.name[i] = 0;
    devfs_root_node.name[0] = 'd';
    devfs_root_node.name[1] = 'e';
    devfs_root_node.name[2] = 'v';
    devfs_root_node.flags = VFS_DIRECTORY;
    devfs_root_node.length = 0;
    devfs_root_node.inode = 0;
    devfs_root_node.read = NULL;
    devfs_root_node.write = NULL;
    devfs_root_node.readdir = devfs_readdir;
    devfs_root_node.finddir = devfs_finddir;
    devfs_root_node.impl = NULL;

    kprintf("[DEVFS] Initialized\n");
}

int devfs_register(const char *name, driver_t *drv) {
    if (!name || !drv) return -1;
    if (dev_count >= DEVFS_MAX_NODES) return -1;

    // Find empty slot
    for (int i = 0; i < DEVFS_MAX_NODES; i++) {
        if (!dev_nodes[i].used) {
            devfs_entry_t *entry = &dev_nodes[i];
            entry->used = 1;
            entry->driver = drv;

            // Setup VFS node
            vfs_node_t *node = &entry->node;
            int j = 0;
            while (name[j] && j < 127) { node->name[j] = name[j]; j++; }
            node->name[j] = 0;
            node->flags = VFS_FILE;
            node->length = 0;
            node->inode = (uint64_t)(i + 1);
            node->read = devfs_node_read;
            node->write = devfs_node_write;
            node->readdir = NULL;
            node->finddir = NULL;
            node->impl = entry;

            dev_count++;
            kprintf("[DEVFS] Registered /dev/%s -> driver '%s'\n", name, drv->name);
            return 0;
        }
    }
    return -1;
}

vfs_node_t *devfs_get_root(void) {
    return &devfs_root_node;
}
