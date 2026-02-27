#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "block.h"
#include "spinlock.h"

/*
 * ext3-style metadata journal for RaeenOS ext2 filesystem.
 *
 * Design:
 *   - The journal occupies a contiguous region of blocks on the block device.
 *   - It is a circular log of transactions, each consisting of:
 *       1. A descriptor block listing which FS blocks are being modified
 *       2. Copies of the metadata blocks (the "before" or "after" images)
 *       3. A commit block with a checksum
 *   - On clean unmount, the journal is marked empty.
 *   - On mount, if the journal is dirty, we replay committed transactions.
 *
 * On-disk layout (within journal region):
 *   [Superblock] [Descriptor|Data|...|Commit] [Descriptor|Data|...|Commit] ...
 */

// Journal block magic numbers
#define JNL_MAGIC            0x4A524E4C  // "JRNL"
#define JNL_BLOCK_SUPER      1
#define JNL_BLOCK_DESCRIPTOR 2
#define JNL_BLOCK_COMMIT     3
#define JNL_BLOCK_REVOKE     4           // Future: block revocation

// Journal superblock (stored in first journal block)
typedef struct {
    uint32_t js_magic;          // JNL_MAGIC
    uint32_t js_blocksize;      // Must match FS block size
    uint32_t js_maxlen;         // Total journal blocks (including this one)
    uint32_t js_first;          // First usable journal block (after super)
    uint32_t js_sequence;       // Next expected transaction sequence number
    uint32_t js_start;          // Journal block of first live transaction (0 = empty)
    uint32_t js_errno;          // Error code from last recovery
    uint32_t js_flags;          // JNL_FLAG_*
    uint8_t  js_pad[480];       // Pad to 512 bytes
} __attribute__((packed)) jnl_superblock_t;

#define JNL_FLAG_CLEAN  0x01    // Journal is clean (no recovery needed)

// Block tag in a descriptor block
// Each tag describes one metadata block being journaled
typedef struct {
    uint32_t bt_blocknr;        // FS block number being journaled
    uint32_t bt_flags;          // BT_FLAG_*
} __attribute__((packed)) jnl_block_tag_t;

#define BT_FLAG_LAST     0x01   // Last tag in this descriptor
#define BT_FLAG_ESCAPE   0x02   // Block data had magic number, was escaped

// Descriptor block header
typedef struct {
    uint32_t jh_magic;          // JNL_MAGIC
    uint32_t jh_blocktype;      // JNL_BLOCK_DESCRIPTOR
    uint32_t jh_sequence;       // Transaction sequence number
    uint32_t jh_count;          // Number of block tags following this header
    // Followed by jnl_block_tag_t[] array, then padding to block boundary
} __attribute__((packed)) jnl_descriptor_t;

// Commit block header
typedef struct {
    uint32_t jh_magic;          // JNL_MAGIC
    uint32_t jh_blocktype;      // JNL_BLOCK_COMMIT
    uint32_t jh_sequence;       // Must match descriptor's sequence
    uint32_t jh_checksum;       // XOR checksum of all data blocks in transaction
} __attribute__((packed)) jnl_commit_t;

// ── In-memory transaction state ───────────────────────────────────────────────

#define JNL_MAX_BLOCKS_PER_TXN  32  // Max metadata blocks per transaction

typedef struct {
    uint32_t t_sequence;                        // Sequence counter
    uint32_t t_count;                           // Number of blocks logged
    uint32_t t_blocknrs[JNL_MAX_BLOCKS_PER_TXN]; // FS block numbers
    uint8_t *t_data[JNL_MAX_BLOCKS_PER_TXN];    // Copies of block data
    bool     t_active;                           // Transaction in progress?
} jnl_transaction_t;

typedef struct {
    block_device_t *dev;        // Underlying block device
    uint32_t        start_lba;  // First LBA of journal region (in sectors)
    uint32_t        block_size; // FS block size (bytes)
    uint32_t        sectors_per_block;
    uint32_t        maxlen;     // Total journal blocks
    uint32_t        first;      // First usable block index
    uint32_t        head;       // Next free journal block index (circular)
    uint32_t        sequence;   // Next transaction sequence number

    jnl_transaction_t txn;      // Current in-flight transaction
    spinlock_t       lock;      // Protects journal state
} journal_t;

// ── API ───────────────────────────────────────────────────────────────────────

// Create and initialize a journal on the given block device.
// journal_start_block = first FS block of the journal region
// journal_blocks      = number of FS blocks reserved for journal
journal_t *jnl_init(block_device_t *dev, uint32_t journal_start_block,
                    uint32_t journal_blocks, uint32_t fs_block_size);

// Mount: load journal superblock and replay if dirty
int jnl_load(journal_t *jnl);

// Begin a new transaction
int jnl_begin(journal_t *jnl);

// Log a metadata block (copies current block data into transaction)
int jnl_log_block(journal_t *jnl, uint32_t fs_block);

// Commit the current transaction (writes descriptor + data + commit to journal)
int jnl_commit(journal_t *jnl);

// Abort the current transaction (discard without writing)
void jnl_abort(journal_t *jnl);

// Flush: write journal superblock to mark recovery point
// Call after the actual metadata has been written to its final FS location
int jnl_checkpoint(journal_t *jnl);

// Recovery: replay all committed transactions from journal to FS
int jnl_recover(journal_t *jnl);

// Mark journal clean and sync
void jnl_shutdown(journal_t *jnl);

#endif
