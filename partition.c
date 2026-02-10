#include "partition.h"
#include "serial.h"
#include "heap.h"
#include "string.h"

// Check if a GUID is all zeros (empty partition entry)
static int guid_is_zero(const uint8_t *guid) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) return 0;
    }
    return 1;
}

// Try to parse GPT
static int try_gpt(block_device_t *dev, uint8_t *sector_buf) {
    // Read LBA 1 (GPT header)
    if (block_read(dev, 1, 1, sector_buf) != 0) return -1;

    gpt_header_t *hdr = (gpt_header_t *)sector_buf;

    // Check signature "EFI PART"
    const char *sig = "EFI PART";
    for (int i = 0; i < 8; i++) {
        if (hdr->signature[i] != sig[i]) return -1;
    }

    kprintf("[GPT] Found GPT: revision=0x%x, entries=%d, entry_size=%d\n",
            hdr->revision, hdr->num_entries, hdr->entry_size);
    kprintf("[GPT] First usable LBA=%lu, Last usable LBA=%lu\n",
            (uint64_t)hdr->first_usable, (uint64_t)hdr->last_usable);

    uint32_t num_entries = hdr->num_entries;
    uint32_t entry_size = hdr->entry_size;
    uint64_t table_lba = hdr->partition_table_lba;

    if (entry_size < 128 || num_entries == 0) return -1;
    if (num_entries > 128) num_entries = 128; // Sanity limit

    // Read partition entries
    // Each sector can hold (512 / entry_size) entries
    uint32_t entries_per_sector = 512 / entry_size;
    int found = 0;

    for (uint32_t i = 0; i < num_entries; i++) {
        // Which sector and offset?
        uint64_t lba = table_lba + (i / entries_per_sector);
        uint32_t offset = (i % entries_per_sector) * entry_size;

        if (offset == 0) {
            if (block_read(dev, lba, 1, sector_buf) != 0) break;
        }

        gpt_entry_t *entry = (gpt_entry_t *)(sector_buf + offset);

        // Skip empty entries
        if (guid_is_zero(entry->type_guid)) continue;

        uint64_t first = entry->first_lba;
        uint64_t last = entry->last_lba;
        uint64_t size = last - first + 1;

        // Extract ASCII name from UTF-16LE
        char name[37];
        for (int j = 0; j < 36; j++) {
            uint16_t c = entry->name[j];
            name[j] = (c < 128) ? (char)c : '?';
        }
        name[36] = 0;
        // Trim trailing zeros
        for (int j = 35; j >= 0; j--) {
            if (name[j] == 0 || name[j] == ' ') name[j] = 0;
            else break;
        }

        kprintf("[GPT] Partition %d: LBA %lu - %lu (%lu MB) '%s'\n",
                found + 1, (uint64_t)first, (uint64_t)last,
                (uint64_t)(size * 512 / (1024 * 1024)), name);

        block_create_partition(dev, found + 1, first, size);
        found++;
    }

    return found;
}

// Try to parse MBR
static int try_mbr(block_device_t *dev, uint8_t *sector_buf) {
    // sector_buf already has LBA 0
    mbr_t *mbr = (mbr_t *)sector_buf;

    if (mbr->signature != 0xAA55) {
        kprintf("[MBR] No valid MBR signature\n");
        return -1;
    }

    int found = 0;
    for (int i = 0; i < 4; i++) {
        mbr_partition_t *p = &mbr->partitions[i];
        if (p->type == 0 || p->sector_count == 0) continue;

        kprintf("[MBR] Partition %d: type=0x%02x LBA=%u count=%u (%u MB)\n",
                i + 1, p->type, p->lba_start, p->sector_count,
                (uint32_t)(p->sector_count * 512 / (1024 * 1024)));

        block_create_partition(dev, i + 1, p->lba_start, p->sector_count);
        found++;
    }

    return found;
}

int partition_probe(block_device_t *dev) {
    if (!dev) return 0;

    // Allocate a sector buffer
    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) return 0;

    // Read LBA 0 (MBR / protective MBR)
    if (block_read(dev, 0, 1, buf) != 0) {
        kprintf("[PARTITION] Failed to read LBA 0\n");
        kfree(buf);
        return 0;
    }

    // Try GPT first (LBA 1 has GPT header; LBA 0 has protective MBR)
    int found = try_gpt(dev, buf);
    if (found > 0) {
        kprintf("[PARTITION] Found %d GPT partition(s)\n", found);
        kfree(buf);
        return found;
    }

    // Re-read LBA 0 (try_gpt may have overwritten buf)
    if (block_read(dev, 0, 1, buf) != 0) {
        kfree(buf);
        return 0;
    }

    // Fall back to MBR
    found = try_mbr(dev, buf);
    if (found > 0) {
        kprintf("[PARTITION] Found %d MBR partition(s)\n", found);
    } else {
        kprintf("[PARTITION] No partitions found\n");
    }

    kfree(buf);
    return found > 0 ? found : 0;
}
