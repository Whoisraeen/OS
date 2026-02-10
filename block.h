#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>

#define SECTOR_SIZE 512
#define MAX_BLOCK_DEVICES 16

// Forward declaration
struct block_device;

// Block device operations
typedef struct {
    int (*read_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
} block_ops_t;

// Block device
typedef struct block_device {
    char name[32];              // e.g. "sda", "sda1"
    uint64_t sector_count;      // Total sectors
    uint32_t sector_size;       // Bytes per sector (usually 512)
    block_ops_t *ops;
    void *private_data;         // Driver-specific data

    // For partitions: parent device + offset
    struct block_device *parent;
    uint64_t partition_start;   // Starting LBA on parent
    uint64_t partition_size;    // Size in sectors
    int partition_index;        // -1 = whole disk, 0+ = partition number
} block_device_t;

// Initialize block device subsystem
void block_init(void);

// Register a block device (returns index or -1)
int block_register(block_device_t *dev);

// Read/write through a block device
int block_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf);
int block_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf);

// Find block device by name
block_device_t *block_find(const char *name);

// Get device by index
block_device_t *block_get(int index);

// Get number of registered devices
int block_get_count(void);

// Create a partition block device (sub-device of a parent)
block_device_t *block_create_partition(block_device_t *parent, int part_num,
                                        uint64_t start_lba, uint64_t size_sectors);

#endif
