/*
 * journal.c — ext3-style metadata journal for RaeenOS
 *
 * Implements write-ahead logging for filesystem metadata operations.
 * Before metadata blocks are modified in-place on disk, their new contents
 * are first written to the journal. On crash recovery, committed transactions
 * are replayed from the journal to restore consistency.
 */

#include "journal.h"
#include "bcache.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

// ── Journal block I/O (bypasses bcache for journal region) ────────────────────

static int jnl_read_block(journal_t *jnl, uint32_t jblock, void *buf) {
    uint64_t lba = jnl->start_lba + (uint64_t)jblock * jnl->sectors_per_block;
    for (uint32_t i = 0; i < jnl->sectors_per_block; i++) {
        buf_t *b = bcache_get(jnl->dev, lba + i);
        if (!b) return -1;
        memcpy((uint8_t *)buf + i * 512, b->data, 512);
        bcache_release(b);
    }
    return 0;
}

static int jnl_write_block(journal_t *jnl, uint32_t jblock, const void *buf) {
    uint64_t lba = jnl->start_lba + (uint64_t)jblock * jnl->sectors_per_block;
    for (uint32_t i = 0; i < jnl->sectors_per_block; i++) {
        buf_t *b = bcache_get(jnl->dev, lba + i);
        if (!b) return -1;
        memcpy(b->data, (const uint8_t *)buf + i * 512, 512);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    return 0;
}

// Read an FS block (not journal block) using bcache
static int jnl_read_fs_block(journal_t *jnl, uint32_t fs_block, void *buf) {
    uint64_t lba = (uint64_t)fs_block * jnl->sectors_per_block;
    for (uint32_t i = 0; i < jnl->sectors_per_block; i++) {
        buf_t *b = bcache_get(jnl->dev, lba + i);
        if (!b) return -1;
        memcpy((uint8_t *)buf + i * 512, b->data, 512);
        bcache_release(b);
    }
    return 0;
}

// Write an FS block (not journal block) using bcache
static int jnl_write_fs_block(journal_t *jnl, uint32_t fs_block, const void *buf) {
    uint64_t lba = (uint64_t)fs_block * jnl->sectors_per_block;
    for (uint32_t i = 0; i < jnl->sectors_per_block; i++) {
        buf_t *b = bcache_get(jnl->dev, lba + i);
        if (!b) return -1;
        memcpy(b->data, (const uint8_t *)buf + i * 512, 512);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    return 0;
}

// ── Checksum ──────────────────────────────────────────────────────────────────

static uint32_t jnl_checksum(const void *data, size_t len) {
    const uint32_t *p = (const uint32_t *)data;
    uint32_t csum = 0;
    for (size_t i = 0; i < len / 4; i++)
        csum ^= p[i];
    return csum;
}

// ── Journal block index arithmetic (circular) ────────────────────────────────

static uint32_t jnl_wrap(journal_t *jnl, uint32_t idx) {
    uint32_t usable = jnl->maxlen - jnl->first;
    return jnl->first + ((idx - jnl->first) % usable);
}

// ── Journal superblock I/O ───────────────────────────────────────────────────

static int jnl_read_super(journal_t *jnl, jnl_superblock_t *js) {
    uint8_t *buf = kmalloc(jnl->block_size);
    if (!buf) return -1;
    if (jnl_read_block(jnl, 0, buf) != 0) { kfree(buf); return -1; }
    memcpy(js, buf, sizeof(*js));
    kfree(buf);
    return 0;
}

static int jnl_write_super(journal_t *jnl, const jnl_superblock_t *js) {
    uint8_t *buf = kmalloc(jnl->block_size);
    if (!buf) return -1;
    memset(buf, 0, jnl->block_size);
    memcpy(buf, js, sizeof(*js));
    int r = jnl_write_block(jnl, 0, buf);
    kfree(buf);
    return r;
}

// ── Init: create fresh journal ────────────────────────────────────────────────

journal_t *jnl_init(block_device_t *dev, uint32_t journal_start_block,
                    uint32_t journal_blocks, uint32_t fs_block_size) {
    if (journal_blocks < 8) {
        kprintf("[JNL] Journal too small (need >= 8 blocks, got %u)\n", journal_blocks);
        return NULL;
    }

    journal_t *jnl = kmalloc(sizeof(journal_t));
    if (!jnl) return NULL;
    memset(jnl, 0, sizeof(*jnl));

    jnl->dev             = dev;
    jnl->block_size      = fs_block_size;
    jnl->sectors_per_block = fs_block_size / 512;
    jnl->start_lba       = (uint64_t)journal_start_block * jnl->sectors_per_block;
    jnl->maxlen          = journal_blocks;
    jnl->first           = 1;  // Block 0 is journal superblock
    jnl->head            = 1;
    jnl->sequence        = 1;

    spinlock_init(&jnl->lock);
    memset(&jnl->txn, 0, sizeof(jnl->txn));

    // Write a fresh journal superblock
    jnl_superblock_t js;
    memset(&js, 0, sizeof(js));
    js.js_magic     = JNL_MAGIC;
    js.js_blocksize = fs_block_size;
    js.js_maxlen    = journal_blocks;
    js.js_first     = 1;
    js.js_sequence  = 1;
    js.js_start     = 0;  // Empty journal
    js.js_flags     = JNL_FLAG_CLEAN;

    if (jnl_write_super(jnl, &js) != 0) {
        kfree(jnl);
        return NULL;
    }

    bcache_sync();
    kprintf("[JNL] Journal initialized: %u blocks at FS block %u\n",
            journal_blocks, journal_start_block);
    return jnl;
}

// ── Load: read existing journal superblock ────────────────────────────────────

int jnl_load(journal_t *jnl) {
    jnl_superblock_t js;
    if (jnl_read_super(jnl, &js) != 0) return -1;

    if (js.js_magic != JNL_MAGIC) {
        kprintf("[JNL] Bad journal magic: 0x%08X\n", js.js_magic);
        return -1;
    }

    jnl->maxlen   = js.js_maxlen;
    jnl->first    = js.js_first;
    jnl->sequence = js.js_sequence;
    jnl->head     = js.js_first; // Will be updated by recovery

    if (!(js.js_flags & JNL_FLAG_CLEAN) && js.js_start != 0) {
        kprintf("[JNL] Dirty journal detected — replaying...\n");
        return jnl_recover(jnl);
    }

    kprintf("[JNL] Journal is clean\n");
    return 0;
}

// ── Begin transaction ─────────────────────────────────────────────────────────

int jnl_begin(journal_t *jnl) {
    spinlock_acquire(&jnl->lock);
    if (jnl->txn.t_active) {
        spinlock_release(&jnl->lock);
        return -1; // Already in a transaction
    }

    jnl->txn.t_active   = true;
    jnl->txn.t_sequence = jnl->sequence;
    jnl->txn.t_count    = 0;
    spinlock_release(&jnl->lock);
    return 0;
}

// ── Log a metadata block ──────────────────────────────────────────────────────

int jnl_log_block(journal_t *jnl, uint32_t fs_block) {
    spinlock_acquire(&jnl->lock);
    if (!jnl->txn.t_active) {
        spinlock_release(&jnl->lock);
        return -1;
    }

    // Check if already logged in this transaction
    for (uint32_t i = 0; i < jnl->txn.t_count; i++) {
        if (jnl->txn.t_blocknrs[i] == fs_block) {
            // Re-read the block to get latest data
            jnl_read_fs_block(jnl, fs_block, jnl->txn.t_data[i]);
            spinlock_release(&jnl->lock);
            return 0;
        }
    }

    if (jnl->txn.t_count >= JNL_MAX_BLOCKS_PER_TXN) {
        spinlock_release(&jnl->lock);
        kprintf("[JNL] Transaction full (%u blocks)\n", JNL_MAX_BLOCKS_PER_TXN);
        return -1;
    }

    uint32_t idx = jnl->txn.t_count;
    jnl->txn.t_blocknrs[idx] = fs_block;
    jnl->txn.t_data[idx] = kmalloc(jnl->block_size);
    if (!jnl->txn.t_data[idx]) {
        spinlock_release(&jnl->lock);
        return -1;
    }

    // Read the current block data (this is the "after" image for redo journaling)
    jnl_read_fs_block(jnl, fs_block, jnl->txn.t_data[idx]);
    jnl->txn.t_count++;
    spinlock_release(&jnl->lock);
    return 0;
}

// ── Commit transaction ────────────────────────────────────────────────────────

int jnl_commit(journal_t *jnl) {
    spinlock_acquire(&jnl->lock);
    if (!jnl->txn.t_active || jnl->txn.t_count == 0) {
        // Nothing to commit — just end the transaction
        jnl->txn.t_active = false;
        spinlock_release(&jnl->lock);
        return 0;
    }

    uint32_t count = jnl->txn.t_count;
    uint32_t seq   = jnl->txn.t_sequence;

    // Check we have enough journal space: 1 descriptor + count data + 1 commit
    uint32_t needed = 1 + count + 1;
    uint32_t usable = jnl->maxlen - jnl->first;
    if (needed > usable) {
        kprintf("[JNL] Transaction too large for journal\n");
        spinlock_release(&jnl->lock);
        return -1;
    }

    uint8_t *block_buf = kmalloc(jnl->block_size);
    if (!block_buf) {
        spinlock_release(&jnl->lock);
        return -1;
    }

    // 1. Write descriptor block at jnl->head
    memset(block_buf, 0, jnl->block_size);
    jnl_descriptor_t *desc = (jnl_descriptor_t *)block_buf;
    desc->jh_magic     = JNL_MAGIC;
    desc->jh_blocktype = JNL_BLOCK_DESCRIPTOR;
    desc->jh_sequence  = seq;
    desc->jh_count     = count;

    jnl_block_tag_t *tags = (jnl_block_tag_t *)(block_buf + sizeof(jnl_descriptor_t));
    for (uint32_t i = 0; i < count; i++) {
        tags[i].bt_blocknr = jnl->txn.t_blocknrs[i];
        tags[i].bt_flags   = (i == count - 1) ? BT_FLAG_LAST : 0;
    }

    uint32_t jpos = jnl->head;
    jnl_write_block(jnl, jpos, block_buf);
    jpos = jnl_wrap(jnl, jpos + 1);

    // 2. Write data blocks
    uint32_t data_checksum = 0;
    for (uint32_t i = 0; i < count; i++) {
        jnl_write_block(jnl, jpos, jnl->txn.t_data[i]);
        data_checksum ^= jnl_checksum(jnl->txn.t_data[i], jnl->block_size);
        jpos = jnl_wrap(jnl, jpos + 1);
    }

    // 3. Write commit block
    memset(block_buf, 0, jnl->block_size);
    jnl_commit_t *commit = (jnl_commit_t *)block_buf;
    commit->jh_magic     = JNL_MAGIC;
    commit->jh_blocktype = JNL_BLOCK_COMMIT;
    commit->jh_sequence  = seq;
    commit->jh_checksum  = data_checksum;

    jnl_write_block(jnl, jpos, block_buf);
    jpos = jnl_wrap(jnl, jpos + 1);

    // 4. Force all journal writes to disk
    bcache_sync();

    // 5. Update journal superblock to mark start of live data
    jnl_superblock_t js;
    jnl_read_super(jnl, &js);
    if (js.js_start == 0) {
        js.js_start = jnl->head; // First live transaction
    }
    js.js_sequence = seq + 1;
    js.js_flags &= ~JNL_FLAG_CLEAN; // Mark dirty
    jnl_write_super(jnl, &js);
    bcache_sync();

    // 6. Update in-memory state
    jnl->head     = jpos;
    jnl->sequence = seq + 1;

    // 7. Free transaction data
    for (uint32_t i = 0; i < count; i++) {
        kfree(jnl->txn.t_data[i]);
        jnl->txn.t_data[i] = NULL;
    }
    jnl->txn.t_active = false;
    jnl->txn.t_count  = 0;

    kfree(block_buf);
    spinlock_release(&jnl->lock);
    return 0;
}

// ── Abort transaction ─────────────────────────────────────────────────────────

void jnl_abort(journal_t *jnl) {
    spinlock_acquire(&jnl->lock);
    for (uint32_t i = 0; i < jnl->txn.t_count; i++) {
        kfree(jnl->txn.t_data[i]);
        jnl->txn.t_data[i] = NULL;
    }
    jnl->txn.t_active = false;
    jnl->txn.t_count  = 0;
    spinlock_release(&jnl->lock);
}

// ── Recovery: replay committed transactions ───────────────────────────────────

int jnl_recover(journal_t *jnl) {
    jnl_superblock_t js;
    if (jnl_read_super(jnl, &js) != 0) return -1;

    if (js.js_start == 0) {
        kprintf("[JNL] Nothing to recover\n");
        return 0;
    }

    uint8_t *block_buf = kmalloc(jnl->block_size);
    uint8_t *data_buf  = kmalloc(jnl->block_size);
    if (!block_buf || !data_buf) {
        kfree(block_buf);
        kfree(data_buf);
        return -1;
    }

    uint32_t jpos = js.js_start;
    uint32_t expected_seq = js.js_sequence > 0 ? js.js_sequence - 1 : 0;
    // Scan from js_start. We don't know how many txns there are; scan until
    // we find a block that isn't a valid descriptor.
    int replayed = 0;

    for (int pass = 0; pass < 256; pass++) { // safety limit
        // Try to read a descriptor block
        if (jnl_read_block(jnl, jpos, block_buf) != 0) break;

        jnl_descriptor_t *desc = (jnl_descriptor_t *)block_buf;
        if (desc->jh_magic != JNL_MAGIC || desc->jh_blocktype != JNL_BLOCK_DESCRIPTOR)
            break; // Not a descriptor — end of journal

        uint32_t seq   = desc->jh_sequence;
        uint32_t count = desc->jh_count;

        if (count == 0 || count > JNL_MAX_BLOCKS_PER_TXN) break;

        // Read block tags
        jnl_block_tag_t *tags = (jnl_block_tag_t *)
            (block_buf + sizeof(jnl_descriptor_t));

        // Move past descriptor
        uint32_t data_pos = jnl_wrap(jnl, jpos + 1);

        // Verify commit block exists
        uint32_t commit_pos = jnl_wrap(jnl, jpos + 1 + count);
        if (jnl_read_block(jnl, commit_pos, data_buf) != 0) break;

        jnl_commit_t *commit = (jnl_commit_t *)data_buf;
        if (commit->jh_magic != JNL_MAGIC ||
            commit->jh_blocktype != JNL_BLOCK_COMMIT ||
            commit->jh_sequence != seq) {
            kprintf("[JNL] Incomplete transaction seq=%u — stopping recovery\n", seq);
            break; // Incomplete transaction — don't replay
        }

        // Verify data checksum
        uint32_t data_checksum = 0;
        bool valid = true;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t dp = jnl_wrap(jnl, data_pos + i);
            if (jnl_read_block(jnl, dp, data_buf) != 0) { valid = false; break; }
            data_checksum ^= jnl_checksum(data_buf, jnl->block_size);
        }

        if (!valid || data_checksum != commit->jh_checksum) {
            kprintf("[JNL] Checksum mismatch for seq=%u — stopping\n", seq);
            break;
        }

        // Replay: write each data block to its FS location
        kprintf("[JNL] Replaying transaction seq=%u (%u blocks)\n", seq, count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t dp = jnl_wrap(jnl, data_pos + i);
            if (jnl_read_block(jnl, dp, data_buf) != 0) continue;
            jnl_write_fs_block(jnl, tags[i].bt_blocknr, data_buf);
        }
        replayed++;

        // Advance past this transaction
        jpos = jnl_wrap(jnl, commit_pos + 1);
        expected_seq = seq + 1;
    }

    // Force all replayed data to disk
    bcache_sync();

    // Mark journal as clean
    js.js_start    = 0;
    js.js_sequence = expected_seq + 1;
    js.js_flags    = JNL_FLAG_CLEAN;
    jnl_write_super(jnl, &js);
    bcache_sync();

    // Update in-memory state
    jnl->head     = jnl->first;
    jnl->sequence = js.js_sequence;

    kprintf("[JNL] Recovery complete: %d transaction(s) replayed\n", replayed);

    kfree(block_buf);
    kfree(data_buf);
    return 0;
}

// ── Checkpoint: advance journal start past completed transactions ─────────────

int jnl_checkpoint(journal_t *jnl) {
    spinlock_acquire(&jnl->lock);

    // All committed data is now also written to its final FS location.
    // We can reset the journal start to the current head.
    jnl_superblock_t js;
    if (jnl_read_super(jnl, &js) != 0) {
        spinlock_release(&jnl->lock);
        return -1;
    }

    js.js_start = 0; // All transactions checkpointed
    jnl_write_super(jnl, &js);
    bcache_sync();

    spinlock_release(&jnl->lock);
    return 0;
}

// ── Shutdown: mark journal clean ──────────────────────────────────────────────

void jnl_shutdown(journal_t *jnl) {
    // Ensure no in-flight transaction
    if (jnl->txn.t_active) {
        jnl_abort(jnl);
    }

    jnl_superblock_t js;
    if (jnl_read_super(jnl, &js) == 0) {
        js.js_start = 0;
        js.js_flags = JNL_FLAG_CLEAN;
        jnl_write_super(jnl, &js);
    }

    bcache_sync();
    kprintf("[JNL] Journal shut down cleanly\n");
}
