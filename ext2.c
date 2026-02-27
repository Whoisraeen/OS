#include "ext2.h"
#include "bcache.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

// Global mounted ext2 filesystem
ext2_fs_t *ext2_root_fs = NULL;

// ============================================================
// Block I/O helpers
// ============================================================

// Read an ext2 block into a buffer
static int ext2_read_block(ext2_fs_t *fs, uint32_t block, void *buf) {
    if (block == 0) {
        memset(buf, 0, fs->block_size);
        return 0;
    }
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    for (uint32_t i = 0; i < fs->sectors_per_block; i++) {
        buf_t *b = bcache_get(fs->dev, lba + i);
        if (!b) return -1;
        memcpy((uint8_t *)buf + i * SECTOR_SIZE, b->data, SECTOR_SIZE);
        bcache_release(b);
    }
    return 0;
}

// Write an ext2 block from a buffer
static int ext2_write_block(ext2_fs_t *fs, uint32_t block, const void *buf) {
    if (block == 0) return -1;
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    for (uint32_t i = 0; i < fs->sectors_per_block; i++) {
        buf_t *b = bcache_get(fs->dev, lba + i);
        if (!b) return -1;
        memcpy(b->data, (const uint8_t *)buf + i * SECTOR_SIZE, SECTOR_SIZE);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    return 0;
}

// ============================================================
// Superblock and group descriptor I/O
// ============================================================

static int ext2_read_superblock(ext2_fs_t *fs) {
    // Superblock is at byte 1024, which is LBA 2 (in 512-byte sectors)
    uint8_t buf[1024];
    buf_t *s0 = bcache_get(fs->dev, 2);
    if (!s0) return -1;
    memcpy(buf, s0->data, 512);
    bcache_release(s0);

    buf_t *s1 = bcache_get(fs->dev, 3);
    if (!s1) return -1;
    memcpy(buf + 512, s1->data, 512);
    bcache_release(s1);

    memcpy(&fs->sb, buf, sizeof(ext2_superblock_t));
    return 0;
}

static int ext2_write_superblock(ext2_fs_t *fs) {
    uint8_t buf[1024];
    memcpy(buf, &fs->sb, sizeof(ext2_superblock_t));

    buf_t *s0 = bcache_get(fs->dev, 2);
    if (!s0) return -1;
    memcpy(s0->data, buf, 512);
    bcache_mark_dirty(s0);
    bcache_release(s0);

    buf_t *s1 = bcache_get(fs->dev, 3);
    if (!s1) return -1;
    memcpy(s1->data, buf + 512, 512);
    bcache_mark_dirty(s1);
    bcache_release(s1);
    return 0;
}

static int ext2_read_group_descs(ext2_fs_t *fs) {
    // Group descriptors start at block (first_data_block + 1)
    uint32_t gd_block = fs->sb.s_first_data_block + 1;
    uint32_t gd_size = fs->num_groups * sizeof(ext2_group_desc_t);
    uint32_t gd_blocks = (gd_size + fs->block_size - 1) / fs->block_size;

    fs->groups = (ext2_group_desc_t *)kmalloc(gd_blocks * fs->block_size);
    if (!fs->groups) return -1;

    for (uint32_t i = 0; i < gd_blocks; i++) {
        if (ext2_read_block(fs, gd_block + i,
                (uint8_t *)fs->groups + i * fs->block_size) != 0) {
            kfree(fs->groups);
            fs->groups = NULL;
            return -1;
        }
    }
    return 0;
}

static int ext2_write_group_descs(ext2_fs_t *fs) {
    uint32_t gd_block = fs->sb.s_first_data_block + 1;
    uint32_t gd_size = fs->num_groups * sizeof(ext2_group_desc_t);
    uint32_t gd_blocks = (gd_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t i = 0; i < gd_blocks; i++) {
        if (ext2_write_block(fs, gd_block + i,
                (uint8_t *)fs->groups + i * fs->block_size) != 0)
            return -1;
    }
    return 0;
}

// ============================================================
// Inode I/O
// ============================================================

int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    if (ino == 0 || ino > fs->sb.s_inodes_count) return -1;

    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    uint32_t inode_table_block = fs->groups[group].bg_inode_table;

    // Calculate which block in the inode table and the offset within it
    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block_in_table = index / inodes_per_block;
    uint32_t offset_in_block = (index % inodes_per_block) * fs->inode_size;

    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext2_read_block(fs, inode_table_block + block_in_table, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    memcpy(out, block_buf + offset_in_block, sizeof(ext2_inode_t));
    kfree(block_buf);
    return 0;
}

int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *inode) {
    if (ino == 0 || ino > fs->sb.s_inodes_count) return -1;

    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    uint32_t inode_table_block = fs->groups[group].bg_inode_table;

    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block_in_table = index / inodes_per_block;
    uint32_t offset_in_block = (index % inodes_per_block) * fs->inode_size;

    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext2_read_block(fs, inode_table_block + block_in_table, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    memcpy(block_buf + offset_in_block, inode, sizeof(ext2_inode_t));

    if (ext2_write_block(fs, inode_table_block + block_in_table, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    kfree(block_buf);
    return 0;
}

// ============================================================
// Block pointer resolution
// ============================================================

// Get the disk block number for a given logical block index in a file
static uint32_t ext2_get_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical_block) {
    uint32_t ppb = fs->ptrs_per_block; // Pointers per indirect block

    // Direct blocks (0-11)
    if (logical_block < EXT2_NDIR_BLOCKS) {
        return inode->i_block[logical_block];
    }
    logical_block -= EXT2_NDIR_BLOCKS;

    // Single indirect (block 12)
    if (logical_block < ppb) {
        if (inode->i_block[EXT2_IND_BLOCK] == 0) return 0;
        uint32_t *ind = (uint32_t *)kmalloc(fs->block_size);
        if (!ind) return 0;
        if (ext2_read_block(fs, inode->i_block[EXT2_IND_BLOCK], ind) != 0) {
            kfree(ind);
            return 0;
        }
        uint32_t result = ind[logical_block];
        kfree(ind);
        return result;
    }
    logical_block -= ppb;

    // Double indirect (block 13)
    if (logical_block < ppb * ppb) {
        if (inode->i_block[EXT2_DIND_BLOCK] == 0) return 0;
        uint32_t *dind = (uint32_t *)kmalloc(fs->block_size);
        if (!dind) return 0;
        if (ext2_read_block(fs, inode->i_block[EXT2_DIND_BLOCK], dind) != 0) {
            kfree(dind);
            return 0;
        }
        uint32_t ind_block = dind[logical_block / ppb];
        kfree(dind);
        if (ind_block == 0) return 0;

        uint32_t *ind = (uint32_t *)kmalloc(fs->block_size);
        if (!ind) return 0;
        if (ext2_read_block(fs, ind_block, ind) != 0) {
            kfree(ind);
            return 0;
        }
        uint32_t result = ind[logical_block % ppb];
        kfree(ind);
        return result;
    }
    logical_block -= ppb * ppb;

    // Triple indirect (block 14)
    if (logical_block < ppb * ppb * ppb) {
        if (inode->i_block[EXT2_TIND_BLOCK] == 0) return 0;
        uint32_t *tind = (uint32_t *)kmalloc(fs->block_size);
        if (!tind) return 0;
        if (ext2_read_block(fs, inode->i_block[EXT2_TIND_BLOCK], tind) != 0) {
            kfree(tind);
            return 0;
        }
        uint32_t dind_block = tind[logical_block / (ppb * ppb)];
        kfree(tind);
        if (dind_block == 0) return 0;

        uint32_t remainder = logical_block % (ppb * ppb);
        uint32_t *dind = (uint32_t *)kmalloc(fs->block_size);
        if (!dind) return 0;
        if (ext2_read_block(fs, dind_block, dind) != 0) {
            kfree(dind);
            return 0;
        }
        uint32_t ind_block = dind[remainder / ppb];
        kfree(dind);
        if (ind_block == 0) return 0;

        uint32_t *ind = (uint32_t *)kmalloc(fs->block_size);
        if (!ind) return 0;
        if (ext2_read_block(fs, ind_block, ind) != 0) {
            kfree(ind);
            return 0;
        }
        uint32_t result = ind[remainder % ppb];
        kfree(ind);
        return result;
    }

    return 0; // Beyond triple indirect — file too large
}

// ============================================================
// Block/Inode allocation and deallocation
// ============================================================

// Allocate a single block from a block group's bitmap
static uint32_t ext2_alloc_block(ext2_fs_t *fs) {
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->block_size);
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < fs->num_groups; g++) {
        if (fs->groups[g].bg_free_blocks_count == 0) continue;

        if (ext2_read_block(fs, fs->groups[g].bg_block_bitmap, bitmap) != 0)
            continue;

        for (uint32_t i = 0; i < fs->blocks_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                // Found a free block — mark it used
                bitmap[i / 8] |= (1 << (i % 8));
                ext2_write_block(fs, fs->groups[g].bg_block_bitmap, bitmap);

                fs->groups[g].bg_free_blocks_count--;
                fs->sb.s_free_blocks_count--;

                kfree(bitmap);
                return g * fs->blocks_per_group + i + fs->sb.s_first_data_block;
            }
        }
    }

    kfree(bitmap);
    return 0; // No free blocks
}

// Free a single block
static void ext2_free_block(ext2_fs_t *fs, uint32_t block) {
    if (block == 0) return;
    uint32_t adjusted = block - fs->sb.s_first_data_block;
    uint32_t group = adjusted / fs->blocks_per_group;
    uint32_t index = adjusted % fs->blocks_per_group;

    if (group >= fs->num_groups) return;

    uint8_t *bitmap = (uint8_t *)kmalloc(fs->block_size);
    if (!bitmap) return;

    if (ext2_read_block(fs, fs->groups[group].bg_block_bitmap, bitmap) != 0) {
        kfree(bitmap);
        return;
    }

    bitmap[index / 8] &= ~(1 << (index % 8));
    ext2_write_block(fs, fs->groups[group].bg_block_bitmap, bitmap);

    fs->groups[group].bg_free_blocks_count++;
    fs->sb.s_free_blocks_count++;

    kfree(bitmap);
}

// Allocate an inode
static uint32_t ext2_alloc_inode(ext2_fs_t *fs, int is_dir) {
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->block_size);
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < fs->num_groups; g++) {
        if (fs->groups[g].bg_free_inodes_count == 0) continue;

        if (ext2_read_block(fs, fs->groups[g].bg_inode_bitmap, bitmap) != 0)
            continue;

        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext2_write_block(fs, fs->groups[g].bg_inode_bitmap, bitmap);

                fs->groups[g].bg_free_inodes_count--;
                fs->sb.s_free_inodes_count--;
                if (is_dir) fs->groups[g].bg_used_dirs_count++;

                kfree(bitmap);
                return g * fs->inodes_per_group + i + 1; // Inodes are 1-based
            }
        }
    }

    kfree(bitmap);
    return 0;
}

// Free an inode
static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir) {
    if (ino == 0) return;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    if (group >= fs->num_groups) return;

    uint8_t *bitmap = (uint8_t *)kmalloc(fs->block_size);
    if (!bitmap) return;

    if (ext2_read_block(fs, fs->groups[group].bg_inode_bitmap, bitmap) != 0) {
        kfree(bitmap);
        return;
    }

    bitmap[index / 8] &= ~(1 << (index % 8));
    ext2_write_block(fs, fs->groups[group].bg_inode_bitmap, bitmap);

    fs->groups[group].bg_free_inodes_count++;
    fs->sb.s_free_inodes_count++;
    if (is_dir && fs->groups[group].bg_used_dirs_count > 0)
        fs->groups[group].bg_used_dirs_count--;

    kfree(bitmap);
}

// ============================================================
// Set a block pointer in an inode (allocating indirect blocks as needed)
// ============================================================

static int ext2_set_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical_block,
                          uint32_t disk_block) {
    uint32_t ppb = fs->ptrs_per_block;

    // Direct blocks
    if (logical_block < EXT2_NDIR_BLOCKS) {
        inode->i_block[logical_block] = disk_block;
        return 0;
    }
    logical_block -= EXT2_NDIR_BLOCKS;

    // Single indirect
    if (logical_block < ppb) {
        if (inode->i_block[EXT2_IND_BLOCK] == 0) {
            uint32_t new_ind = ext2_alloc_block(fs);
            if (!new_ind) return -1;
            inode->i_block[EXT2_IND_BLOCK] = new_ind;
            // Zero out new indirect block
            uint8_t *zero = (uint8_t *)kmalloc(fs->block_size);
            if (zero) { memset(zero, 0, fs->block_size); ext2_write_block(fs, new_ind, zero); kfree(zero); }
        }
        uint32_t *ind = (uint32_t *)kmalloc(fs->block_size);
        if (!ind) return -1;
        ext2_read_block(fs, inode->i_block[EXT2_IND_BLOCK], ind);
        ind[logical_block] = disk_block;
        ext2_write_block(fs, inode->i_block[EXT2_IND_BLOCK], ind);
        kfree(ind);
        return 0;
    }
    logical_block -= ppb;

    // Double indirect
    if (logical_block < ppb * ppb) {
        if (inode->i_block[EXT2_DIND_BLOCK] == 0) {
            uint32_t new_dind = ext2_alloc_block(fs);
            if (!new_dind) return -1;
            inode->i_block[EXT2_DIND_BLOCK] = new_dind;
            uint8_t *zero = (uint8_t *)kmalloc(fs->block_size);
            if (zero) { memset(zero, 0, fs->block_size); ext2_write_block(fs, new_dind, zero); kfree(zero); }
        }
        uint32_t *dind = (uint32_t *)kmalloc(fs->block_size);
        if (!dind) return -1;
        ext2_read_block(fs, inode->i_block[EXT2_DIND_BLOCK], dind);

        uint32_t ind_idx = logical_block / ppb;
        if (dind[ind_idx] == 0) {
            uint32_t new_ind = ext2_alloc_block(fs);
            if (!new_ind) { kfree(dind); return -1; }
            dind[ind_idx] = new_ind;
            ext2_write_block(fs, inode->i_block[EXT2_DIND_BLOCK], dind);
            uint8_t *zero = (uint8_t *)kmalloc(fs->block_size);
            if (zero) { memset(zero, 0, fs->block_size); ext2_write_block(fs, new_ind, zero); kfree(zero); }
        }
        uint32_t ind_blk = dind[ind_idx];
        kfree(dind);

        uint32_t *ind = (uint32_t *)kmalloc(fs->block_size);
        if (!ind) return -1;
        ext2_read_block(fs, ind_blk, ind);
        ind[logical_block % ppb] = disk_block;
        ext2_write_block(fs, ind_blk, ind);
        kfree(ind);
        return 0;
    }

    // Triple indirect not implemented for writes — files would need to be > 64GB
    return -1;
}

// ============================================================
// File data read/write
// ============================================================

size_t ext2_read_data(ext2_fs_t *fs, ext2_inode_t *inode, size_t offset, size_t size, uint8_t *buf) {
    uint32_t file_size = inode->i_size;
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return 0;

    size_t bytes_read = 0;
    while (bytes_read < size) {
        uint32_t logical_block = (offset + bytes_read) / fs->block_size;
        uint32_t offset_in_block = (offset + bytes_read) % fs->block_size;
        uint32_t to_copy = fs->block_size - offset_in_block;
        if (to_copy > size - bytes_read) to_copy = size - bytes_read;

        uint32_t disk_block = ext2_get_block(fs, inode, logical_block);
        if (disk_block == 0) {
            // Sparse file — block is a hole, fill with zeros
            memset(buf + bytes_read, 0, to_copy);
        } else {
            if (ext2_read_block(fs, disk_block, block_buf) != 0) break;
            memcpy(buf + bytes_read, block_buf + offset_in_block, to_copy);
        }
        bytes_read += to_copy;
    }

    kfree(block_buf);
    return bytes_read;
}

size_t ext2_write_data(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode,
                       size_t offset, size_t size, const uint8_t *buf) {
    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return 0;

    size_t bytes_written = 0;
    while (bytes_written < size) {
        uint32_t logical_block = (offset + bytes_written) / fs->block_size;
        uint32_t offset_in_block = (offset + bytes_written) % fs->block_size;
        uint32_t to_copy = fs->block_size - offset_in_block;
        if (to_copy > size - bytes_written) to_copy = size - bytes_written;

        uint32_t disk_block = ext2_get_block(fs, inode, logical_block);
        if (disk_block == 0) {
            // Need to allocate a new block
            disk_block = ext2_alloc_block(fs);
            if (disk_block == 0) break;
            if (ext2_set_block(fs, inode, logical_block, disk_block) != 0) {
                ext2_free_block(fs, disk_block);
                break;
            }
            inode->i_blocks += fs->sectors_per_block;
            memset(block_buf, 0, fs->block_size);
        } else {
            // Read existing block if partial write
            if (offset_in_block != 0 || to_copy != fs->block_size) {
                ext2_read_block(fs, disk_block, block_buf);
            }
        }

        memcpy(block_buf + offset_in_block, buf + bytes_written, to_copy);
        ext2_write_block(fs, disk_block, block_buf);
        bytes_written += to_copy;
    }

    // Update file size if we wrote beyond current end
    if (offset + bytes_written > inode->i_size) {
        inode->i_size = offset + bytes_written;
    }

    // Write inode back
    ext2_write_inode(fs, ino, inode);

    kfree(block_buf);
    return bytes_written;
}

// ============================================================
// Directory operations
// ============================================================

uint32_t ext2_dir_lookup(ext2_fs_t *fs, uint32_t dir_ino, const char *name) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) != 0) return 0;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return 0;

    size_t name_len = strlen(name);
    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return 0;

    uint32_t dir_size = inode.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t logical_block = pos / fs->block_size;
        uint32_t disk_block = ext2_get_block(fs, &inode, logical_block);
        if (disk_block == 0) { pos += fs->block_size; continue; }

        if (ext2_read_block(fs, disk_block, block_buf) != 0) break;

        uint32_t offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == name_len) {
                if (memcmp(de->name, name, name_len) == 0) {
                    uint32_t result = de->inode;
                    kfree(block_buf);
                    return result;
                }
            }
            offset += de->rec_len;
        }
        pos += fs->block_size;
    }

    kfree(block_buf);
    return 0;
}

// Add a directory entry to a directory
static int ext2_dir_add_entry(ext2_fs_t *fs, uint32_t dir_ino, uint32_t new_ino,
                              const char *name, uint8_t file_type) {
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) != 0) return -1;

    size_t name_len = strlen(name);
    uint16_t needed = ((8 + name_len + 3) / 4) * 4; // 4-byte aligned entry size

    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return -1;

    uint32_t dir_size = dir_inode.i_size;
    uint32_t num_blocks = (dir_size + fs->block_size - 1) / fs->block_size;

    // Search existing blocks for space
    for (uint32_t b = 0; b < num_blocks; b++) {
        uint32_t disk_block = ext2_get_block(fs, &dir_inode, b);
        if (disk_block == 0) continue;

        if (ext2_read_block(fs, disk_block, block_buf) != 0) continue;

        uint32_t offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (de->rec_len == 0) break;

            uint16_t actual_size = ((8 + de->name_len + 3) / 4) * 4;
            uint16_t available = de->rec_len - actual_size;

            if (de->inode == 0 && de->rec_len >= needed) {
                // Reuse deleted entry
                de->inode = new_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                ext2_write_block(fs, disk_block, block_buf);
                kfree(block_buf);
                return 0;
            }

            if (available >= needed && de->inode != 0) {
                // Split this entry
                uint16_t old_rec_len = de->rec_len;
                de->rec_len = actual_size;

                ext2_dir_entry_t *new_de = (ext2_dir_entry_t *)(block_buf + offset + actual_size);
                new_de->inode = new_ino;
                new_de->rec_len = old_rec_len - actual_size;
                new_de->name_len = (uint8_t)name_len;
                new_de->file_type = file_type;
                memcpy(new_de->name, name, name_len);

                ext2_write_block(fs, disk_block, block_buf);
                kfree(block_buf);
                return 0;
            }

            offset += de->rec_len;
        }
    }

    // No space in existing blocks — allocate a new directory block
    uint32_t new_block = ext2_alloc_block(fs);
    if (new_block == 0) { kfree(block_buf); return -1; }

    uint32_t new_logical = dir_size / fs->block_size;
    if (ext2_set_block(fs, &dir_inode, new_logical, new_block) != 0) {
        ext2_free_block(fs, new_block);
        kfree(block_buf);
        return -1;
    }

    // Initialize the new block with our entry spanning the entire block
    memset(block_buf, 0, fs->block_size);
    ext2_dir_entry_t *de = (ext2_dir_entry_t *)block_buf;
    de->inode = new_ino;
    de->rec_len = fs->block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);
    ext2_write_block(fs, new_block, block_buf);

    dir_inode.i_size += fs->block_size;
    dir_inode.i_blocks += fs->sectors_per_block;
    ext2_write_inode(fs, dir_ino, &dir_inode);

    kfree(block_buf);
    return 0;
}

// Remove a directory entry by name
static int ext2_dir_remove_entry(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                                 uint32_t *removed_ino) {
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) != 0) return -1;

    size_t name_len = strlen(name);
    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return -1;

    uint32_t dir_size = dir_inode.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t logical_block = pos / fs->block_size;
        uint32_t disk_block = ext2_get_block(fs, &dir_inode, logical_block);
        if (disk_block == 0) { pos += fs->block_size; continue; }

        if (ext2_read_block(fs, disk_block, block_buf) != 0) break;

        uint32_t offset = 0;
        ext2_dir_entry_t *prev = NULL;

        while (offset < fs->block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                if (removed_ino) *removed_ino = de->inode;

                if (prev) {
                    // Merge with previous entry
                    prev->rec_len += de->rec_len;
                } else {
                    // First entry in block — just zero the inode
                    de->inode = 0;
                }
                ext2_write_block(fs, disk_block, block_buf);
                kfree(block_buf);
                return 0;
            }

            prev = de;
            offset += de->rec_len;
        }
        pos += fs->block_size;
    }

    kfree(block_buf);
    return -1; // Not found
}

// ============================================================
// Free all data blocks of an inode (for truncate/delete)
// ============================================================

static void ext2_free_indirect(ext2_fs_t *fs, uint32_t block, int depth) {
    if (block == 0) return;

    if (depth > 0) {
        uint32_t *ptrs = (uint32_t *)kmalloc(fs->block_size);
        if (!ptrs) return;
        if (ext2_read_block(fs, block, ptrs) != 0) { kfree(ptrs); return; }

        for (uint32_t i = 0; i < fs->ptrs_per_block; i++) {
            if (ptrs[i] != 0) {
                ext2_free_indirect(fs, ptrs[i], depth - 1);
            }
        }
        kfree(ptrs);
    }

    ext2_free_block(fs, block);
}

int ext2_truncate(ext2_fs_t *fs, uint32_t ino) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) return -1;

    // Free direct blocks
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (inode.i_block[i]) {
            ext2_free_block(fs, inode.i_block[i]);
            inode.i_block[i] = 0;
        }
    }

    // Free indirect blocks (recursively)
    ext2_free_indirect(fs, inode.i_block[EXT2_IND_BLOCK], 1);
    inode.i_block[EXT2_IND_BLOCK] = 0;

    ext2_free_indirect(fs, inode.i_block[EXT2_DIND_BLOCK], 2);
    inode.i_block[EXT2_DIND_BLOCK] = 0;

    ext2_free_indirect(fs, inode.i_block[EXT2_TIND_BLOCK], 3);
    inode.i_block[EXT2_TIND_BLOCK] = 0;

    inode.i_size = 0;
    inode.i_blocks = 0;
    ext2_write_inode(fs, ino, &inode);
    return 0;
}

// ============================================================
// High-level operations: create, unlink, mkdir, rmdir, rename
// ============================================================

uint32_t ext2_create(ext2_fs_t *fs, uint32_t parent_ino, const char *name, uint16_t mode) {
    // Check if name already exists
    if (ext2_dir_lookup(fs, parent_ino, name) != 0) return 0;

    int is_dir = (mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    uint32_t new_ino = ext2_alloc_inode(fs, is_dir);
    if (new_ino == 0) return 0;

    // Initialize the new inode
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = mode;
    inode.i_links_count = is_dir ? 2 : 1; // Dirs get "." link + parent link
    inode.i_uid = 0;
    inode.i_gid = 0;
    ext2_write_inode(fs, new_ino, &inode);

    // Add entry in parent directory
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (ext2_dir_add_entry(fs, parent_ino, new_ino, name, ft) != 0) {
        ext2_free_inode(fs, new_ino, is_dir);
        return 0;
    }

    // If directory, create "." and ".." entries
    if (is_dir) {
        // Allocate a block for the new directory
        uint32_t dir_block = ext2_alloc_block(fs);
        if (dir_block == 0) {
            ext2_free_inode(fs, new_ino, is_dir);
            return 0;
        }

        inode.i_block[0] = dir_block;
        inode.i_size = fs->block_size;
        inode.i_blocks = fs->sectors_per_block;

        uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
        if (block_buf) {
            memset(block_buf, 0, fs->block_size);

            // "." entry
            ext2_dir_entry_t *dot = (ext2_dir_entry_t *)block_buf;
            dot->inode = new_ino;
            dot->rec_len = 12;
            dot->name_len = 1;
            dot->file_type = EXT2_FT_DIR;
            dot->name[0] = '.';

            // ".." entry
            ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(block_buf + 12);
            dotdot->inode = parent_ino;
            dotdot->rec_len = fs->block_size - 12;
            dotdot->name_len = 2;
            dotdot->file_type = EXT2_FT_DIR;
            dotdot->name[0] = '.';
            dotdot->name[1] = '.';

            ext2_write_block(fs, dir_block, block_buf);
            kfree(block_buf);
        }

        ext2_write_inode(fs, new_ino, &inode);

        // Increment parent's link count (for ".." reference)
        ext2_inode_t parent_inode;
        if (ext2_read_inode(fs, parent_ino, &parent_inode) == 0) {
            parent_inode.i_links_count++;
            ext2_write_inode(fs, parent_ino, &parent_inode);
        }
    }

    return new_ino;
}

int ext2_unlink(ext2_fs_t *fs, uint32_t parent_ino, const char *name) {
    // Don't allow unlinking "." or ".."
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -1;

    uint32_t target_ino = 0;
    if (ext2_dir_remove_entry(fs, parent_ino, name, &target_ino) != 0) return -1;
    if (target_ino == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, target_ino, &inode) != 0) return -1;

    // Don't unlink directories (use rmdir instead)
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        // Re-add the entry (undo the removal) — or just fail. Simpler to fail.
        return -1;
    }

    inode.i_links_count--;
    if (inode.i_links_count == 0) {
        // Free all data blocks
        ext2_truncate(fs, target_ino);
        ext2_free_inode(fs, target_ino, 0);
        inode.i_dtime = 1; // Mark as deleted
    }
    ext2_write_inode(fs, target_ino, &inode);
    return 0;
}

int ext2_rmdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -1;

    uint32_t dir_ino = ext2_dir_lookup(fs, parent_ino, name);
    if (dir_ino == 0) return -1;

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) != 0) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    // Check if directory is empty (only "." and ".." entries)
    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return -1;

    int entry_count = 0;
    uint32_t pos = 0;
    while (pos < dir_inode.i_size) {
        uint32_t lb = pos / fs->block_size;
        uint32_t db = ext2_get_block(fs, &dir_inode, lb);
        if (db == 0) { pos += fs->block_size; continue; }

        if (ext2_read_block(fs, db, block_buf) != 0) break;
        uint32_t offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (de->rec_len == 0) break;
            if (de->inode != 0) {
                // Skip "." and ".."
                int is_dot = (de->name_len == 1 && de->name[0] == '.');
                int is_dotdot = (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
                if (!is_dot && !is_dotdot) entry_count++;
            }
            offset += de->rec_len;
        }
        pos += fs->block_size;
    }
    kfree(block_buf);

    if (entry_count > 0) return -1; // Not empty

    // Remove entry from parent
    uint32_t removed;
    if (ext2_dir_remove_entry(fs, parent_ino, name, &removed) != 0) return -1;

    // Free directory blocks and inode
    ext2_truncate(fs, dir_ino);
    ext2_free_inode(fs, dir_ino, 1);
    dir_inode.i_links_count = 0;
    dir_inode.i_dtime = 1;
    ext2_write_inode(fs, dir_ino, &dir_inode);

    // Decrement parent link count (no more ".." pointing to it)
    ext2_inode_t parent_inode;
    if (ext2_read_inode(fs, parent_ino, &parent_inode) == 0) {
        if (parent_inode.i_links_count > 0) parent_inode.i_links_count--;
        ext2_write_inode(fs, parent_ino, &parent_inode);
    }

    return 0;
}

int ext2_rename(ext2_fs_t *fs, uint32_t old_parent_ino, const char *old_name,
                uint32_t new_parent_ino, const char *new_name) {
    // Look up old entry
    uint32_t ino = ext2_dir_lookup(fs, old_parent_ino, old_name);
    if (ino == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) return -1;
    uint8_t ft = ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

    // Remove existing entry at destination if present
    uint32_t existing = ext2_dir_lookup(fs, new_parent_ino, new_name);
    if (existing != 0) {
        ext2_unlink(fs, new_parent_ino, new_name);
    }

    // Add entry in new parent
    if (ext2_dir_add_entry(fs, new_parent_ino, ino, new_name, ft) != 0) return -1;

    // Remove old entry
    uint32_t dummy;
    ext2_dir_remove_entry(fs, old_parent_ino, old_name, &dummy);

    // If directory, update ".." to point to new parent
    if (ft == EXT2_FT_DIR && old_parent_ino != new_parent_ino) {
        uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
        if (block_buf) {
            uint32_t db = ext2_get_block(fs, &inode, 0);
            if (db && ext2_read_block(fs, db, block_buf) == 0) {
                // Walk to find ".." entry
                uint32_t off = 0;
                while (off < fs->block_size) {
                    ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + off);
                    if (de->rec_len == 0) break;
                    if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
                        de->inode = new_parent_ino;
                        ext2_write_block(fs, db, block_buf);
                        break;
                    }
                    off += de->rec_len;
                }
            }
            kfree(block_buf);
        }

        // Update link counts
        ext2_inode_t old_parent;
        if (ext2_read_inode(fs, old_parent_ino, &old_parent) == 0) {
            if (old_parent.i_links_count > 0) old_parent.i_links_count--;
            ext2_write_inode(fs, old_parent_ino, &old_parent);
        }
        ext2_inode_t new_parent;
        if (ext2_read_inode(fs, new_parent_ino, &new_parent) == 0) {
            new_parent.i_links_count++;
            ext2_write_inode(fs, new_parent_ino, &new_parent);
        }
    }

    return 0;
}

// ============================================================
// Stat and getdents
// ============================================================

int ext2_stat(ext2_fs_t *fs, uint32_t ino, ext2_stat_t *st) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) return -1;

    st->st_ino = ino;
    st->st_mode = inode.i_mode;
    st->st_nlink = inode.i_links_count;
    st->st_uid = inode.i_uid;
    st->st_gid = inode.i_gid;
    st->st_size = inode.i_size;
    st->st_atime = inode.i_atime;
    st->st_mtime = inode.i_mtime;
    st->st_ctime = inode.i_ctime;
    st->st_blocks = inode.i_blocks;
    return 0;
}

int ext2_getdents(ext2_fs_t *fs, uint32_t dir_ino, dirent_t *entries, int max_entries) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) != 0) return -1;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return -1;

    uint8_t *block_buf = (uint8_t *)kmalloc(fs->block_size);
    if (!block_buf) return -1;

    int count = 0;
    uint32_t pos = 0;

    while (pos < inode.i_size && count < max_entries) {
        uint32_t lb = pos / fs->block_size;
        uint32_t db = ext2_get_block(fs, &inode, lb);
        if (db == 0) { pos += fs->block_size; continue; }

        if (ext2_read_block(fs, db, block_buf) != 0) break;

        uint32_t offset = 0;
        while (offset < fs->block_size && count < max_entries) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                entries[count].d_ino = de->inode;
                entries[count].d_reclen = sizeof(dirent_t);
                entries[count].d_type = de->file_type;
                size_t copy_len = de->name_len;
                if (copy_len > 255) copy_len = 255;
                memcpy(entries[count].d_name, de->name, copy_len);
                entries[count].d_name[copy_len] = '\0';
                count++;
            }
            offset += de->rec_len;
        }
        pos += fs->block_size;
    }

    kfree(block_buf);
    return count;
}

// ============================================================
// VFS integration
// ============================================================

// Per-node ext2 context (stored in vfs_node->impl)
typedef struct {
    ext2_fs_t *fs;
    uint32_t ino;
} ext2_vfs_ctx_t;

// Forward declarations
static size_t ext2_vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
static size_t ext2_vfs_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
static vfs_node_t *ext2_vfs_readdir(vfs_node_t *node, size_t index);
static vfs_node_t *ext2_vfs_finddir(vfs_node_t *node, const char *name);
static vfs_node_t *ext2_vfs_create(vfs_node_t *parent, const char *name, int flags);
static int ext2_vfs_mkdir(vfs_node_t *parent, const char *name);
static int ext2_vfs_unlink(vfs_node_t *parent, const char *name);
static int ext2_vfs_rmdir(vfs_node_t *parent, const char *name);
static int ext2_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                            vfs_node_t *new_parent, const char *new_name);

static vfs_node_t *ext2_create_vfs_node(ext2_fs_t *fs, uint32_t ino) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) return NULL;

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vfs_node_t));

    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)kmalloc(sizeof(ext2_vfs_ctx_t));
    if (!ctx) { kfree(node); return NULL; }
    ctx->fs = fs;
    ctx->ino = ino;

    node->inode = ino;
    node->length = inode.i_size;
    node->impl = ctx;
    node->read = ext2_vfs_read;
    node->write = ext2_vfs_write;

    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        node->flags = VFS_DIRECTORY;
        node->readdir = ext2_vfs_readdir;
        node->finddir = ext2_vfs_finddir;
        node->create = ext2_vfs_create;
        node->mkdir  = ext2_vfs_mkdir;
        node->unlink = ext2_vfs_unlink;
        node->rmdir  = ext2_vfs_rmdir;
        node->rename = ext2_vfs_rename;
    } else {
        node->flags = VFS_FILE;
    }

    node->name[0] = '\0'; // Caller should set name

    return node;
}

static size_t ext2_vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)node->impl;
    if (!ctx) return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(ctx->fs, ctx->ino, &inode) != 0) return 0;

    return ext2_read_data(ctx->fs, &inode, offset, size, buffer);
}

static size_t ext2_vfs_write(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)node->impl;
    if (!ctx) return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(ctx->fs, ctx->ino, &inode) != 0) return 0;

    size_t written = ext2_write_data(ctx->fs, ctx->ino, &inode, offset, size, (const uint8_t *)buffer);
    node->length = inode.i_size; // Update cached size
    return written;
}

static vfs_node_t *ext2_vfs_readdir(vfs_node_t *node, size_t index) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)node->impl;
    if (!ctx) return NULL;

    ext2_inode_t inode;
    if (ext2_read_inode(ctx->fs, ctx->ino, &inode) != 0) return NULL;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return NULL;

    uint8_t *block_buf = (uint8_t *)kmalloc(ctx->fs->block_size);
    if (!block_buf) return NULL;

    size_t current = 0;
    uint32_t pos = 0;

    while (pos < inode.i_size) {
        uint32_t lb = pos / ctx->fs->block_size;
        uint32_t db = ext2_get_block(ctx->fs, &inode, lb);
        if (db == 0) { pos += ctx->fs->block_size; continue; }

        if (ext2_read_block(ctx->fs, db, block_buf) != 0) break;

        uint32_t offset = 0;
        while (offset < ctx->fs->block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                if (current == index) {
                    // Create VFS node for this entry
                    vfs_node_t *child = ext2_create_vfs_node(ctx->fs, de->inode);
                    if (child) {
                        size_t copy_len = de->name_len;
                        if (copy_len > 127) copy_len = 127;
                        memcpy(child->name, de->name, copy_len);
                        child->name[copy_len] = '\0';
                    }
                    kfree(block_buf);
                    return child;
                }
                current++;
            }
            offset += de->rec_len;
        }
        pos += ctx->fs->block_size;
    }

    kfree(block_buf);
    return NULL;
}

static vfs_node_t *ext2_vfs_finddir(vfs_node_t *node, const char *name) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)node->impl;
    if (!ctx) return NULL;

    uint32_t ino = ext2_dir_lookup(ctx->fs, ctx->ino, name);
    if (ino == 0) return NULL;

    vfs_node_t *child = ext2_create_vfs_node(ctx->fs, ino);
    if (child) {
        strncpy(child->name, name, 127);
        child->name[127] = '\0';
    }
    return child;
}

static vfs_node_t *ext2_vfs_create(vfs_node_t *parent, const char *name, int flags) {
    (void)flags;
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)parent->impl;
    if (!ctx) return NULL;
    
    // Default to file (0644)
    uint32_t ino = ext2_create(ctx->fs, ctx->ino, name, EXT2_S_IFREG | 0644);
    if (ino == 0) return NULL;
    
    vfs_node_t *node = ext2_create_vfs_node(ctx->fs, ino);
    if (node) {
        size_t len = strlen(name);
        if (len > 127) len = 127;
        memcpy(node->name, name, len);
        node->name[len] = 0;
    }
    return node;
}

static int ext2_vfs_mkdir(vfs_node_t *parent, const char *name) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)parent->impl;
    if (!ctx) return -1;
    
    uint32_t ino = ext2_create(ctx->fs, ctx->ino, name, EXT2_S_IFDIR | 0755);
    return (ino != 0) ? 0 : -1;
}

static int ext2_vfs_unlink(vfs_node_t *parent, const char *name) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)parent->impl;
    if (!ctx) return -1;
    return ext2_unlink(ctx->fs, ctx->ino, name);
}

static int ext2_vfs_rmdir(vfs_node_t *parent, const char *name) {
    ext2_vfs_ctx_t *ctx = (ext2_vfs_ctx_t *)parent->impl;
    if (!ctx) return -1;
    return ext2_rmdir(ctx->fs, ctx->ino, name);
}

static int ext2_vfs_rename(vfs_node_t *old_parent, const char *old_name,
                            vfs_node_t *new_parent, const char *new_name) {
    ext2_vfs_ctx_t *old_ctx = (ext2_vfs_ctx_t *)old_parent->impl;
    ext2_vfs_ctx_t *new_ctx = (ext2_vfs_ctx_t *)new_parent->impl;
    if (!old_ctx || !new_ctx) return -1;
    return ext2_rename(old_ctx->fs, old_ctx->ino, old_name,
                       new_ctx->ino, new_name);
}

// ============================================================
// Mount/Unmount
// ============================================================

ext2_fs_t *ext2_mount(block_device_t *dev) {
    if (!dev) return NULL;

    ext2_fs_t *fs = (ext2_fs_t *)kmalloc(sizeof(ext2_fs_t));
    if (!fs) return NULL;
    memset(fs, 0, sizeof(ext2_fs_t));
    fs->dev = dev;

    // Read superblock
    if (ext2_read_superblock(fs) != 0) {
        kprintf("[EXT2] Failed to read superblock\n");
        kfree(fs);
        return NULL;
    }

    // Verify magic
    if (fs->sb.s_magic != EXT2_MAGIC) {
        kprintf("[EXT2] Invalid magic: 0x%x (expected 0xEF53)\n", fs->sb.s_magic);
        kfree(fs);
        return NULL;
    }

    // Calculate derived values
    fs->block_size = 1024 << fs->sb.s_log_block_size;
    fs->sectors_per_block = fs->block_size / SECTOR_SIZE;
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    fs->blocks_per_group = fs->sb.s_blocks_per_group;
    fs->num_groups = (fs->sb.s_blocks_count + fs->blocks_per_group - 1) / fs->blocks_per_group;
    fs->inode_size = (fs->sb.s_rev_level >= 1) ? fs->sb.s_inode_size : 128;
    fs->ptrs_per_block = fs->block_size / sizeof(uint32_t);

    kprintf("[EXT2] Mounted: %u blocks, %u inodes, block_size=%u, groups=%u\n",
            fs->sb.s_blocks_count, fs->sb.s_inodes_count,
            fs->block_size, fs->num_groups);
    kprintf("[EXT2] Free: %u blocks, %u inodes\n",
            fs->sb.s_free_blocks_count, fs->sb.s_free_inodes_count);

    if (fs->sb.s_volume_name[0]) {
        kprintf("[EXT2] Volume: '%.16s'\n", fs->sb.s_volume_name);
    }

    // Read block group descriptors
    if (ext2_read_group_descs(fs) != 0) {
        kprintf("[EXT2] Failed to read group descriptors\n");
        kfree(fs);
        return NULL;
    }

    // Create root VFS node (inode 2)
    fs->root_node = ext2_create_vfs_node(fs, EXT2_ROOT_INO);
    if (!fs->root_node) {
        kprintf("[EXT2] Failed to create root node\n");
        kfree(fs->groups);
        kfree(fs);
        return NULL;
    }
    strncpy(fs->root_node->name, "/", 2);

    kprintf("[EXT2] Root inode loaded, FS ready\n");
    return fs;
}

void ext2_unmount(ext2_fs_t *fs) {
    if (!fs) return;
    ext2_sync(fs);
    if (fs->groups) kfree(fs->groups);
    // Note: VFS nodes are not freed here (they may still be referenced)
    kfree(fs);
}

vfs_node_t *ext2_get_root(ext2_fs_t *fs) {
    return fs ? fs->root_node : NULL;
}

void ext2_sync(ext2_fs_t *fs) {
    if (!fs) return;
    ext2_write_superblock(fs);
    ext2_write_group_descs(fs);
    bcache_sync();
    kprintf("[EXT2] Synced to disk\n");
}

// ============================================================
// Path resolution helper (for syscalls)
// ============================================================

// Resolve a path like "/foo/bar/baz" to (parent_ino, basename)
// Returns the inode of the final component, or 0 if not found
// If parent_out is non-NULL, stores the parent directory inode
uint32_t ext2_resolve_path(ext2_fs_t *fs, const char *path, uint32_t *parent_out) {
    if (!fs || !path) return 0;

    uint32_t current_ino = EXT2_ROOT_INO;
    uint32_t parent_ino = EXT2_ROOT_INO;

    // Skip leading /
    while (*path == '/') path++;
    if (*path == '\0') {
        if (parent_out) *parent_out = EXT2_ROOT_INO;
        return EXT2_ROOT_INO;
    }

    while (*path) {
        // Extract next component
        char component[256];
        int i = 0;
        while (*path && *path != '/' && i < 255) {
            component[i++] = *path++;
        }
        component[i] = '\0';

        // Skip trailing slashes
        while (*path == '/') path++;

        parent_ino = current_ino;
        current_ino = ext2_dir_lookup(fs, current_ino, component);
        if (current_ino == 0) {
            // Not found
            if (parent_out) *parent_out = parent_ino;
            return 0;
        }
    }

    if (parent_out) *parent_out = parent_ino;
    return current_ino;
}

// Get the basename and parent directory inode from a path
// Returns parent_ino, stores basename in name_out
uint32_t ext2_resolve_parent(ext2_fs_t *fs, const char *path, char *name_out, size_t name_size) {
    if (!fs || !path || !name_out) return 0;

    // Find the last '/' in path
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash || last_slash == path) {
        // File is in root directory
        const char *name_start = path;
        while (*name_start == '/') name_start++;
        strncpy(name_out, name_start, name_size - 1);
        name_out[name_size - 1] = '\0';
        return EXT2_ROOT_INO;
    }

    // Resolve parent path
    char parent_path[256];
    size_t parent_len = last_slash - path;
    if (parent_len >= sizeof(parent_path)) parent_len = sizeof(parent_path) - 1;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    uint32_t parent_ino = ext2_resolve_path(fs, parent_path, NULL);
    if (parent_ino == 0) return 0;

    // Copy basename
    const char *basename = last_slash + 1;
    strncpy(name_out, basename, name_size - 1);
    name_out[name_size - 1] = '\0';

    return parent_ino;
}
