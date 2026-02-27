#include "devfs.h"
#include "heap.h"
#include "serial.h"
#include "string.h"
#include "console.h"
#include "rtc.h"

// Device node entry
typedef struct {
    vfs_node_t node;
    driver_t *driver;
    // Simple device ops (used when driver == NULL)
    size_t (*simple_read)(uint8_t *buf, size_t count);
    size_t (*simple_write)(const uint8_t *buf, size_t count);
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

// devfs node read
static size_t devfs_node_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    devfs_entry_t *entry = (devfs_entry_t *)node->impl;
    if (!entry) return 0;
    if (entry->driver) {
        if (!entry->driver->ops || !entry->driver->ops->read) return 0;
        int ret = entry->driver->ops->read(entry->driver, buffer, size, offset);
        return ret < 0 ? 0 : (size_t)ret;
    }
    if (entry->simple_read) return entry->simple_read(buffer, size);
    return 0;
}

// devfs node write
static size_t devfs_node_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    devfs_entry_t *entry = (devfs_entry_t *)node->impl;
    if (!entry) return 0;
    if (entry->driver) {
        if (!entry->driver->ops || !entry->driver->ops->write) return 0;
        int ret = entry->driver->ops->write(entry->driver, (const uint8_t *)buffer, size, offset);
        return ret < 0 ? 0 : (size_t)ret;
    }
    if (entry->simple_write) return entry->simple_write(buffer, size);
    return 0;
}

// ── Built-in simple character devices ─────────────────────────────────────────

static size_t dev_null_read(uint8_t *buf, size_t count) { (void)buf; (void)count; return 0; }
static size_t dev_null_write(const uint8_t *buf, size_t count) { (void)buf; return count; }

static size_t dev_zero_read(uint8_t *buf, size_t count) {
    for (size_t i = 0; i < count; i++) buf[i] = 0;
    return count;
}
static size_t dev_zero_write(const uint8_t *buf, size_t count) { (void)buf; return count; }

static uint64_t urnd_state = 0;
static size_t dev_urandom_read(uint8_t *buf, size_t count) {
    if (!urnd_state) urnd_state = rtc_get_timestamp() ^ 0xDEADCAFE12345678ULL;
    for (size_t i = 0; i < count; i++) {
        urnd_state ^= urnd_state << 13;
        urnd_state ^= urnd_state >> 7;
        urnd_state ^= urnd_state << 17;
        buf[i] = (uint8_t)urnd_state;
    }
    return count;
}
static size_t dev_urandom_write(const uint8_t *buf, size_t count) { (void)buf; return count; }

static size_t dev_tty_read(uint8_t *buf, size_t count) { (void)buf; (void)count; return 0; }
static size_t dev_tty_write(const uint8_t *buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (buf[i] == '\n') console_putc('\r');
        console_putc((char)buf[i]);
    }
    return count;
}

// ── devfs_register_simple ─────────────────────────────────────────────────────

int devfs_register_simple(const char *name,
                          size_t (*read_fn)(uint8_t *buf, size_t count),
                          size_t (*write_fn)(const uint8_t *buf, size_t count)) {
    if (!name) return -1;
    if (dev_count >= DEVFS_MAX_NODES) return -1;

    for (int i = 0; i < DEVFS_MAX_NODES; i++) {
        if (!dev_nodes[i].used) {
            devfs_entry_t *entry = &dev_nodes[i];
            entry->used = 1;
            entry->driver = NULL;
            entry->simple_read  = read_fn;
            entry->simple_write = write_fn;

            vfs_node_t *node = &entry->node;
            int j = 0;
            while (name[j] && j < 127) { node->name[j] = name[j]; j++; }
            node->name[j] = 0;
            node->flags   = VFS_FILE;
            node->length  = 0;
            node->inode   = (uint64_t)(i + 1);
            node->read    = devfs_node_read;
            node->write   = devfs_node_write;
            node->readdir = NULL;
            node->finddir = NULL;
            node->impl    = entry;

            dev_count++;
            return 0;
        }
    }
    return -1;
}

// ── devfs_init ────────────────────────────────────────────────────────────────

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

    // Register built-in character devices
    devfs_register_simple("null",    dev_null_read,    dev_null_write);
    devfs_register_simple("zero",    dev_zero_read,    dev_zero_write);
    devfs_register_simple("random",  dev_urandom_read, dev_urandom_write);
    devfs_register_simple("urandom", dev_urandom_read, dev_urandom_write);
    devfs_register_simple("tty",     dev_tty_read,     dev_tty_write);
    devfs_register_simple("console", dev_tty_read,     dev_tty_write);

    kprintf("[DEVFS] Initialized (/dev/null, /dev/zero, /dev/urandom, /dev/tty)\n");
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
            entry->simple_read  = NULL;
            entry->simple_write = NULL;

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
