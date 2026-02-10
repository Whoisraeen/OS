import struct
import time
import os

DISK_SIZE = 64 * 1024 * 1024
BLOCK_SIZE = 1024
SECTORS_PER_BLOCK = BLOCK_SIZE // 512
INODE_SIZE = 128
BLOCKS_PER_GROUP = 8192
INODES_PER_GROUP = 2048

# Calculate derived values
BLOCK_COUNT = DISK_SIZE // BLOCK_SIZE
GROUP_COUNT = (BLOCK_COUNT + BLOCKS_PER_GROUP - 1) // BLOCKS_PER_GROUP
INODE_COUNT = INODES_PER_GROUP * GROUP_COUNT

# Superblock (1024 bytes, but only first ~100 used usually)
# struct ext2_superblock {
#   uint32_t s_inodes_count;
#   uint32_t s_blocks_count;
#   uint32_t s_r_blocks_count;
#   uint32_t s_free_blocks_count;
#   uint32_t s_free_inodes_count;
#   uint32_t s_first_data_block;
#   uint32_t s_log_block_size;
#   uint32_t s_log_frag_size;
#   uint32_t s_blocks_per_group;
#   uint32_t s_frags_per_group;
#   uint32_t s_inodes_per_group;
#   uint32_t s_mtime;
#   uint32_t s_wtime;
#   uint16_t s_mnt_count;
#   uint16_t s_max_mnt_count;
#   uint16_t s_magic;
#   uint16_t s_state;
#   uint16_t s_errors;
#   uint16_t s_minor_rev_level;
#   uint32_t s_lastcheck;
#   uint32_t s_checkinterval;
#   uint32_t s_creator_os;
#   uint32_t s_rev_level;
#   uint16_t s_def_resuid;
#   uint16_t s_def_resgid;
#   // ...
# }

def create_superblock():
    sb = bytearray(1024)
    struct.pack_into("<IIIIIIIIIIIIIHH", sb, 0,
        INODE_COUNT,        # s_inodes_count
        BLOCK_COUNT,        # s_blocks_count
        0,                  # s_r_blocks_count
        BLOCK_COUNT - 1 - GROUP_COUNT * 1 - 1, # s_free_blocks_count (approx, fixed later)
        INODE_COUNT - 11,   # s_free_inodes_count (first 10 reserved + root used)
        1,                  # s_first_data_block (1 for 1KB blocks)
        0,                  # s_log_block_size (0 = 1024)
        0,                  # s_log_frag_size
        BLOCKS_PER_GROUP,   # s_blocks_per_group
        BLOCKS_PER_GROUP,   # s_frags_per_group
        INODES_PER_GROUP,   # s_inodes_per_group
        int(time.time()),   # s_mtime
        int(time.time()),   # s_wtime
        0,                  # s_mnt_count
        20,                 # s_max_mnt_count
    )
    struct.pack_into("<H", sb, 56, 0xEF53) # s_magic
    struct.pack_into("<H", sb, 58, 1)      # s_state (clean)
    struct.pack_into("<H", sb, 60, 1)      # s_errors (continue)
    
    # Revision 0 (simplest)
    struct.pack_into("<I", sb, 76, 0)      # s_rev_level (0)
    
    return sb

# Group Descriptor
# struct ext2_group_desc {
#   uint32_t bg_block_bitmap;
#   uint32_t bg_inode_bitmap;
#   uint32_t bg_inode_table;
#   uint16_t bg_free_blocks_count;
#   uint16_t bg_free_inodes_count;
#   uint16_t bg_used_dirs_count;
#   uint16_t bg_pad;
#   uint32_t bg_reserved[3];
# }

class BlockGroup:
    def __init__(self, id):
        self.id = id
        # Layout:
        # Block 0: Boot (if group 0) / Superblock (if backup) / Data
        # But for 1KB blocks:
        # Group 0:
        #   Block 1: Superblock
        #   Block 2 to 2+N: Group Descriptors
        #   Block X: Block Bitmap
        #   Block Y: Inode Bitmap
        #   Block Z: Inode Table
        
        # Calculate offsets
        # For simplicity, we put metadata at the start of every group?
        # Standard Ext2 puts SB and GDT in specific groups (0, 1, 3, 5, 7...).
        # But simpler to put copies in group 0 and that's it for now?
        # Actually, let's follow standard layout.
        
        # We need to allocate blocks for metadata.
        self.block_bitmap_blk = 0
        self.inode_bitmap_blk = 0
        self.inode_table_blk = 0
        
        self.free_blocks_count = BLOCKS_PER_GROUP
        self.free_inodes_count = INODES_PER_GROUP
        self.used_dirs_count = 0
        
        self.block_bitmap = bytearray(BLOCK_SIZE)
        self.inode_bitmap = bytearray(BLOCK_SIZE)

groups = []
for i in range(GROUP_COUNT):
    groups.append(BlockGroup(i))

# Allocation logic
current_block = 1 + 1 # Superblock (1) + GDT (assuming 1 block for 8 groups)
# GDT size = 8 * 32 bytes = 256 bytes -> 1 block

# Assign metadata blocks for each group
for g in groups:
    # Reserve Superblock and GDT space in Group 0
    start_block = g.id * BLOCKS_PER_GROUP + 1 # +1 for first_data_block offset
    if g.id == 0:
        # Group 0 has SB (blk 1) and GDT (blk 2)
        # So metadata starts at 3
        # Mark SB and GDT as used in bitmap
        g.block_bitmap[0] |= (1<<0) # Block 0 relative to group (Block 1 absolute)
        g.block_bitmap[0] |= (1<<1) # Block 1 relative to group (Block 2 absolute)
        
        g.free_blocks_count -= 2
        current_alloc = 3
    else:
        # Other groups don't strictly need SB copies for a minimal FS
        # But we'll keep it simple. Standard mkfs puts backups.
        # We won't put backups to keep it simple.
        current_alloc = start_block

    # Allocate Block Bitmap
    g.block_bitmap_blk = current_alloc
    # Mark in bitmap (relative to group start)
    rel = current_alloc - (g.id * BLOCKS_PER_GROUP + 1)
    g.block_bitmap[rel // 8] |= (1 << (rel % 8))
    g.free_blocks_count -= 1
    current_alloc += 1
    
    # Allocate Inode Bitmap
    g.inode_bitmap_blk = current_alloc
    rel = current_alloc - (g.id * BLOCKS_PER_GROUP + 1)
    g.block_bitmap[rel // 8] |= (1 << (rel % 8))
    g.free_blocks_count -= 1
    current_alloc += 1
    
    # Allocate Inode Table
    inode_table_blocks = (INODES_PER_GROUP * INODE_SIZE) // BLOCK_SIZE
    g.inode_table_blk = current_alloc
    
    for k in range(inode_table_blocks):
        rel = (current_alloc + k) - (g.id * BLOCKS_PER_GROUP + 1)
        g.block_bitmap[rel // 8] |= (1 << (rel % 8))
        g.free_blocks_count -= 1
        
    current_alloc += inode_table_blocks

# Create Root Directory (Inode 2)
root_inode_index = 2
root_group_idx = (root_inode_index - 1) // INODES_PER_GROUP
root_inode_offset = (root_inode_index - 1) % INODES_PER_GROUP
groups[root_group_idx].inode_bitmap[root_inode_offset // 8] |= (1 << (root_inode_offset % 8))
groups[root_group_idx].free_inodes_count -= 1
groups[root_group_idx].used_dirs_count += 1

# Reserve inodes 1-10
for i in range(1, 11):
    if i == 2: continue
    grp = (i - 1) // INODES_PER_GROUP
    off = (i - 1) % INODES_PER_GROUP
    groups[grp].inode_bitmap[off // 8] |= (1 << (off % 8))
    groups[grp].free_inodes_count -= 1

# Allocate data block for Root Directory
root_data_block = 0
# Find free block in group 0
g = groups[0]
for i in range(BLOCKS_PER_GROUP):
    if not (g.block_bitmap[i // 8] & (1 << (i % 8))):
        g.block_bitmap[i // 8] |= (1 << (i % 8))
        g.free_blocks_count -= 1
        root_data_block = g.id * BLOCKS_PER_GROUP + 1 + i
        break

# Create lost+found (Inode 11)
lf_inode_index = 11
lf_group_idx = (lf_inode_index - 1) // INODES_PER_GROUP
lf_inode_offset = (lf_inode_index - 1) % INODES_PER_GROUP
groups[lf_group_idx].inode_bitmap[lf_inode_offset // 8] |= (1 << (lf_inode_offset % 8))
groups[lf_group_idx].free_inodes_count -= 1
groups[lf_group_idx].used_dirs_count += 1

lf_data_block = 0
g = groups[lf_group_idx]
for i in range(BLOCKS_PER_GROUP):
    if not (g.block_bitmap[i // 8] & (1 << (i % 8))):
        g.block_bitmap[i // 8] |= (1 << (i % 8))
        g.free_blocks_count -= 1
        lf_data_block = g.id * BLOCKS_PER_GROUP + 1 + i
        break

# Update Superblock free counts
total_free_blocks = sum(g.free_blocks_count for g in groups)
total_free_inodes = sum(g.free_inodes_count for g in groups)

sb_data = create_superblock()
struct.pack_into("<I", sb_data, 12, total_free_blocks)
struct.pack_into("<I", sb_data, 16, total_free_inodes)

# Write to disk
with open("disk.img", "wb") as f:
    f.truncate(DISK_SIZE)
    
    # Write Superblock (Block 1)
    f.seek(1024)
    f.write(sb_data)
    
    # Write Group Descriptors (Block 2)
    # 8 groups * 32 bytes = 256 bytes
    gdt = bytearray(BLOCK_SIZE)
    for i, g in enumerate(groups):
        struct.pack_into("<IIIHHHH", gdt, i * 32,
            g.block_bitmap_blk,
            g.inode_bitmap_blk,
            g.inode_table_blk,
            g.free_blocks_count,
            g.free_inodes_count,
            g.used_dirs_count,
            0 # pad
        )
    f.seek(2 * BLOCK_SIZE)
    f.write(gdt)
    
    # Write Bitmaps and Inode Tables
    for g in groups:
        # Block Bitmap
        f.seek(g.block_bitmap_blk * BLOCK_SIZE)
        f.write(g.block_bitmap)
        
        # Inode Bitmap
        f.seek(g.inode_bitmap_blk * BLOCK_SIZE)
        f.write(g.inode_bitmap)
        
        # Inode Table (Initialize to 0)
        # But we need to write Root Inode and Lost+Found
        if g.id == root_group_idx:
            # Root Inode (Inode 2)
            # Offset in table = (2-1) * 128 = 128
            inode_offset = (root_inode_index - 1) % INODES_PER_GROUP
            
            root_inode = bytearray(INODE_SIZE)
            # Mode: Directory (0x4000) | 0755 (0x1ED) = 0x41ED
            struct.pack_into("<H", root_inode, 0, 0x41ED)
            struct.pack_into("<I", root_inode, 4, BLOCK_SIZE) # Size
            struct.pack_into("<I", root_inode, 28, 2) # Sectors (1KB block = 2 sectors)
            struct.pack_into("<H", root_inode, 26, 3) # Links (., .., lost+found)
            
            # Block 0
            struct.pack_into("<I", root_inode, 40, root_data_block)
            
            f.seek(g.inode_table_blk * BLOCK_SIZE + inode_offset * INODE_SIZE)
            f.write(root_inode)
            
        if g.id == lf_group_idx:
             # Lost+Found Inode (Inode 11)
            inode_offset = (lf_inode_index - 1) % INODES_PER_GROUP
            
            lf_inode = bytearray(INODE_SIZE)
            struct.pack_into("<H", lf_inode, 0, 0x41ED)
            struct.pack_into("<I", lf_inode, 4, BLOCK_SIZE)
            struct.pack_into("<I", lf_inode, 28, 2)
            struct.pack_into("<H", lf_inode, 26, 2) # Links (., ..)
            struct.pack_into("<I", lf_inode, 40, lf_data_block)
            
            f.seek(g.inode_table_blk * BLOCK_SIZE + inode_offset * INODE_SIZE)
            f.write(lf_inode)

    # Write Directory Entries
    
    # Root Directory Entries
    # 1. "." (Inode 2)
    # 2. ".." (Inode 2)
    # 3. "lost+found" (Inode 11)
    
    root_block = bytearray(BLOCK_SIZE)
    offset = 0
    
    # Entry 1: "."
    # inode=2, rec_len=12, name_len=1, type=2(DIR), name='.'
    struct.pack_into("<IHBB1s", root_block, offset, 2, 12, 1, 2, b'.')
    offset += 12
    
    # Entry 2: ".."
    # inode=2, rec_len=12, name_len=2, type=2, name='..'
    struct.pack_into("<IHBB2s", root_block, offset, 2, 12, 2, 2, b'..')
    offset += 12
    
    # Entry 3: "lost+found"
    # inode=11, rec_len=remaining, name_len=10, type=2, name='lost+found'
    name_len = 10
    rec_len = BLOCK_SIZE - offset
    struct.pack_into("<IHBB10s", root_block, offset, 11, rec_len, name_len, 2, b'lost+found')
    
    f.seek(root_data_block * BLOCK_SIZE)
    f.write(root_block)
    
    # Lost+Found Directory Entries
    # 1. "." (Inode 11)
    # 2. ".." (Inode 2)
    
    lf_block = bytearray(BLOCK_SIZE)
    offset = 0
    
    struct.pack_into("<IHBB1s", lf_block, offset, 11, 12, 1, 2, b'.')
    offset += 12
    
    rec_len = BLOCK_SIZE - offset
    struct.pack_into("<IHBB2s", lf_block, offset, 2, rec_len, 2, 2, b'..')
    
    f.seek(lf_data_block * BLOCK_SIZE)
    f.write(lf_block)

print("Formatted disk.img with Ext2")
