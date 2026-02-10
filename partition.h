#ifndef PARTITION_H
#define PARTITION_H

#include "block.h"

// MBR partition entry (16 bytes)
typedef struct {
    uint8_t  status;        // 0x80 = bootable
    uint8_t  chs_first[3];  // CHS of first sector
    uint8_t  type;          // Partition type
    uint8_t  chs_last[3];   // CHS of last sector
    uint32_t lba_start;     // Starting LBA
    uint32_t sector_count;  // Number of sectors
} __attribute__((packed)) mbr_partition_t;

// MBR structure (first 512 bytes of disk)
typedef struct {
    uint8_t  bootstrap[446];
    mbr_partition_t partitions[4];
    uint16_t signature;     // 0xAA55
} __attribute__((packed)) mbr_t;

// GPT header
typedef struct {
    char     signature[8];   // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t partition_table_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t partition_crc32;
} __attribute__((packed)) gpt_header_t;

// GPT partition entry (128 bytes)
typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];      // UTF-16LE name
} __attribute__((packed)) gpt_entry_t;

// Probe a block device for partitions and register them
// Returns number of partitions found
int partition_probe(block_device_t *dev);

#endif
