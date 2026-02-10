#ifndef BCACHE_H
#define BCACHE_H

#include "block.h"

// Number of cached blocks
#define BCACHE_SIZE 128

// Buffer flags
#define BUF_VALID   (1 << 0)  // Data is valid (read from disk)
#define BUF_DIRTY   (1 << 1)  // Data has been modified (needs writeback)

// Cached block buffer
typedef struct buf {
    uint32_t flags;
    uint64_t lba;             // Sector number
    block_device_t *dev;      // Which device this belongs to
    uint8_t data[512];        // Sector data
    struct buf *prev;         // LRU list previous
    struct buf *next;         // LRU list next
    uint32_t refcount;        // Reference count
} buf_t;

// Initialize the buffer cache
void bcache_init(void);

// Get a cached block (reads from disk if not cached)
// Returns a locked buffer with valid data. Caller must call bcache_release() when done.
buf_t *bcache_get(block_device_t *dev, uint64_t lba);

// Release a buffer (decrements refcount, allows reuse)
void bcache_release(buf_t *b);

// Mark a buffer as dirty (will be written back on sync or eviction)
void bcache_mark_dirty(buf_t *b);

// Write a dirty buffer to disk immediately
void bcache_write(buf_t *b);

// Flush all dirty buffers to disk
void bcache_sync(void);

// Get cache statistics
void bcache_stats(uint32_t *hits, uint32_t *misses, uint32_t *dirty_count);

#endif
