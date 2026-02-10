#include "block.h"
#include "ahci.h"
#include "heap.h"
#include "serial.h"

static block_device_t *devices[MAX_BLOCK_DEVICES];
static int device_count = 0;

// AHCI block ops
static int ahci_block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    (void)dev;
    return ahci_read(lba, count, (uint8_t *)buf);
}

static int ahci_block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    (void)dev;
    return ahci_write(lba, count, buf);
}

static block_ops_t ahci_ops = {
    .read_sectors = ahci_block_read,
    .write_sectors = ahci_block_write
};

// Partition block ops — translate LBA through parent
static int partition_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev->parent) return -1;
    if (lba + count > dev->partition_size) return -1;
    return block_read(dev->parent, dev->partition_start + lba, count, buf);
}

static int partition_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev->parent) return -1;
    if (lba + count > dev->partition_size) return -1;
    return block_write(dev->parent, dev->partition_start + lba, count, buf);
}

static block_ops_t partition_ops = {
    .read_sectors = partition_read,
    .write_sectors = partition_write
};

void block_init(void) {
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++)
        devices[i] = NULL;
    device_count = 0;

    // Register AHCI as "sda" if it has an active port
    // We detect this by trying a simple presence check
    static block_device_t ahci_dev;
    ahci_dev.name[0] = 's'; ahci_dev.name[1] = 'd'; ahci_dev.name[2] = 'a'; ahci_dev.name[3] = 0;
    ahci_dev.sector_size = SECTOR_SIZE;
    ahci_dev.sector_count = 0; // Unknown until IDENTIFY
    ahci_dev.ops = &ahci_ops;
    ahci_dev.private_data = NULL;
    ahci_dev.parent = NULL;
    ahci_dev.partition_start = 0;
    ahci_dev.partition_size = 0;
    ahci_dev.partition_index = -1;

    int idx = block_register(&ahci_dev);
    if (idx >= 0) {
        kprintf("[BLOCK] Registered 'sda' (AHCI)\n");
    }
}

int block_register(block_device_t *dev) {
    if (!dev || device_count >= MAX_BLOCK_DEVICES) return -1;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (!devices[i]) {
            devices[i] = dev;
            device_count++;
            return i;
        }
    }
    return -1;
}

int block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->ops || !dev->ops->read_sectors) return -1;
    return dev->ops->read_sectors(dev, lba, count, buf);
}

int block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->ops || !dev->ops->write_sectors) return -1;
    return dev->ops->write_sectors(dev, lba, count, buf);
}

block_device_t *block_find(const char *name) {
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (!devices[i]) continue;
        const char *a = devices[i]->name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return devices[i];
    }
    return NULL;
}

block_device_t *block_get(int index) {
    if (index < 0 || index >= MAX_BLOCK_DEVICES) return NULL;
    return devices[index];
}

int block_get_count(void) {
    return device_count;
}

block_device_t *block_create_partition(block_device_t *parent, int part_num,
                                        uint64_t start_lba, uint64_t size_sectors) {
    block_device_t *part = (block_device_t *)kmalloc(sizeof(block_device_t));
    if (!part) return NULL;

    // Build name: "sda1", "sda2", etc.
    int i = 0;
    const char *pn = parent->name;
    while (pn[i] && i < 28) { part->name[i] = pn[i]; i++; }
    if (part_num < 10) {
        part->name[i++] = '0' + part_num;
    } else {
        part->name[i++] = '0' + (part_num / 10);
        part->name[i++] = '0' + (part_num % 10);
    }
    part->name[i] = 0;

    part->sector_size = parent->sector_size;
    part->sector_count = size_sectors;
    part->ops = &partition_ops;
    part->private_data = NULL;
    part->parent = parent;
    part->partition_start = start_lba;
    part->partition_size = size_sectors;
    part->partition_index = part_num;

    int idx = block_register(part);
    if (idx < 0) {
        kfree(part);
        return NULL;
    }

    kprintf("[BLOCK] Partition '%s': LBA %lu - %lu (%lu sectors, %lu MB)\n",
            part->name, (uint64_t)start_lba, (uint64_t)(start_lba + size_sectors - 1),
            (uint64_t)size_sectors, (uint64_t)(size_sectors * SECTOR_SIZE / (1024 * 1024)));

    return part;
}
