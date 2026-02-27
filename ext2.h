#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>
#include "block.h"
#include "vfs.h"

// Forward declare journal type
struct journal_t_tag;
typedef struct journal_t_tag journal_t;

// ext2 magic number
#define EXT2_MAGIC 0xEF53

// Superblock is always at byte offset 1024
#define EXT2_SUPERBLOCK_OFFSET 1024

// Special inode numbers
#define EXT2_ROOT_INO        2
#define EXT2_FIRST_INO_REV0  11   // First non-reserved inode (rev 0)

// File type in inode i_mode (upper 4 bits)
#define EXT2_S_IFREG  0x8000  // Regular file
#define EXT2_S_IFDIR  0x4000  // Directory
#define EXT2_S_IFLNK  0xA000  // Symbolic link
#define EXT2_S_IFBLK  0x6000  // Block device
#define EXT2_S_IFCHR  0x2000  // Character device
#define EXT2_S_IFIFO  0x1000  // FIFO
#define EXT2_S_IFSOCK 0xC000  // Socket
#define EXT2_S_IFMT   0xF000  // Format mask

// Permission bits
#define EXT2_S_IRUSR 0x0100
#define EXT2_S_IWUSR 0x0080
#define EXT2_S_IXUSR 0x0040
#define EXT2_S_IRGRP 0x0020
#define EXT2_S_IWGRP 0x0010
#define EXT2_S_IXGRP 0x0008
#define EXT2_S_IROTH 0x0004
#define EXT2_S_IWOTH 0x0002
#define EXT2_S_IXOTH 0x0001

// Directory entry file type
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

// Block pointer indices
#define EXT2_NDIR_BLOCKS  12
#define EXT2_IND_BLOCK    12
#define EXT2_DIND_BLOCK   13
#define EXT2_TIND_BLOCK   14
#define EXT2_N_BLOCKS     15

// ============================================================
// On-disk structures
// ============================================================

// Superblock (at byte offset 1024, always 1024 bytes)
typedef struct {
    uint32_t s_inodes_count;        // Total inodes
    uint32_t s_blocks_count;        // Total blocks
    uint32_t s_r_blocks_count;      // Reserved blocks
    uint32_t s_free_blocks_count;   // Free blocks
    uint32_t s_free_inodes_count;   // Free inodes
    uint32_t s_first_data_block;    // First data block (0 for 4K blocks, 1 for 1K)
    uint32_t s_log_block_size;      // block_size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;       // Fragment size (unused)
    uint32_t s_blocks_per_group;    // Blocks per group
    uint32_t s_frags_per_group;     // Fragments per group
    uint32_t s_inodes_per_group;    // Inodes per group
    uint32_t s_mtime;               // Last mount time
    uint32_t s_wtime;               // Last write time
    uint16_t s_mnt_count;           // Mount count since last check
    uint16_t s_max_mnt_count;       // Max mounts before check
    uint16_t s_magic;               // 0xEF53
    uint16_t s_state;               // FS state
    uint16_t s_errors;              // Error behavior
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;           // 0=rev0, 1=rev1 (dynamic)
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // Rev 1+ fields
    uint32_t s_first_ino;           // First non-reserved inode
    uint16_t s_inode_size;          // Inode size (128 for rev 0)
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // Padding to 1024 bytes
    uint8_t  s_padding[820];
} __attribute__((packed)) ext2_superblock_t;

// Block Group Descriptor (32 bytes)
typedef struct {
    uint32_t bg_block_bitmap;       // Block bitmap block number
    uint32_t bg_inode_bitmap;       // Inode bitmap block number
    uint32_t bg_inode_table;        // Inode table start block
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

// Inode (128 bytes for rev 0, variable for rev 1)
typedef struct {
    uint16_t i_mode;            // File mode (type + permissions)
    uint16_t i_uid;             // Owner UID
    uint32_t i_size;            // File size (lower 32 bits)
    uint32_t i_atime;           // Access time
    uint32_t i_ctime;           // Creation time
    uint32_t i_mtime;           // Modification time
    uint32_t i_dtime;           // Deletion time
    uint16_t i_gid;             // Group ID
    uint16_t i_links_count;     // Hard links count
    uint32_t i_blocks;          // 512-byte block count
    uint32_t i_flags;           // Inode flags
    uint32_t i_osd1;            // OS-dependent
    uint32_t i_block[EXT2_N_BLOCKS]; // Block pointers
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;        // i_size_high for regular files (rev 1)
    uint32_t i_faddr;
    uint8_t  i_osd2[12];       // OS-dependent
} __attribute__((packed)) ext2_inode_t;

// Directory Entry (variable length)
typedef struct {
    uint32_t inode;             // Inode number (0 = deleted)
    uint16_t rec_len;           // Total entry size (padded to 4-byte boundary)
    uint8_t  name_len;          // Name length
    uint8_t  file_type;         // File type (EXT2_FT_*)
    char     name[];            // Name (NOT null-terminated on disk)
} __attribute__((packed)) ext2_dir_entry_t;

// ============================================================
// In-memory filesystem state
// ============================================================

typedef struct {
    block_device_t *dev;            // Underlying block device
    ext2_superblock_t sb;           // Cached superblock
    ext2_group_desc_t *groups;      // Block group descriptor array
    uint32_t block_size;            // Bytes per block (1024/2048/4096)
    uint32_t sectors_per_block;     // block_size / 512
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t num_groups;
    uint32_t inode_size;            // Bytes per inode on disk
    uint32_t ptrs_per_block;        // block_size / 4 (pointers in indirect block)
    vfs_node_t *root_node;          // VFS root node for this mount
    journal_t  *jnl;                // Metadata journal (NULL if not journaled)
} ext2_fs_t;

// Stat structure for SYS_STAT
typedef struct {
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint32_t st_blocks;
} ext2_stat_t;

// Directory entry for SYS_GETDENTS
typedef struct {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} dirent_t;

// ============================================================
// API
// ============================================================

// Mount an ext2 filesystem from a block device
// Returns the ext2 fs handle, or NULL on failure
ext2_fs_t *ext2_mount(block_device_t *dev);

// Unmount (flush dirty data, free resources)
void ext2_unmount(ext2_fs_t *fs);

// Get the VFS root node for the mounted filesystem
vfs_node_t *ext2_get_root(ext2_fs_t *fs);

// Read inode from disk
int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out);

// Write inode to disk
int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *inode);

// Read file data (follows block pointers, handles indirect blocks)
size_t ext2_read_data(ext2_fs_t *fs, ext2_inode_t *inode, size_t offset, size_t size, uint8_t *buf);

// Write file data (allocates blocks as needed)
size_t ext2_write_data(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode,
                       size_t offset, size_t size, const uint8_t *buf);

// Look up a name in a directory. Returns inode number, or 0 on failure.
uint32_t ext2_dir_lookup(ext2_fs_t *fs, uint32_t dir_ino, const char *name);

// Create a file or directory in a parent directory
// Returns new inode number, or 0 on failure
uint32_t ext2_create(ext2_fs_t *fs, uint32_t parent_ino, const char *name, uint16_t mode);

// Remove a file (unlink)
int ext2_unlink(ext2_fs_t *fs, uint32_t parent_ino, const char *name);

// Remove an empty directory
int ext2_rmdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name);

// Rename (move) a file/directory
int ext2_rename(ext2_fs_t *fs, uint32_t old_parent_ino, const char *old_name,
                uint32_t new_parent_ino, const char *new_name);

// Stat an inode
int ext2_stat(ext2_fs_t *fs, uint32_t ino, ext2_stat_t *st);

// Read directory entries into user buffer
// Returns number of entries written, or -1 on error
int ext2_getdents(ext2_fs_t *fs, uint32_t dir_ino, dirent_t *entries, int max_entries);

// Truncate a file to zero length (free all data blocks)
int ext2_truncate(ext2_fs_t *fs, uint32_t ino);

// Sync superblock and group descriptors to disk
void ext2_sync(ext2_fs_t *fs);

// Path resolution: resolve "/foo/bar/baz" to inode number
// If parent_out is non-NULL, stores the parent directory inode
uint32_t ext2_resolve_path(ext2_fs_t *fs, const char *path, uint32_t *parent_out);

// Resolve parent directory and extract basename from a path
// Returns parent inode, stores basename in name_out
uint32_t ext2_resolve_parent(ext2_fs_t *fs, const char *path, char *name_out, size_t name_size);

// Global mounted ext2 filesystem (for syscall access)
extern ext2_fs_t *ext2_root_fs;

#endif
