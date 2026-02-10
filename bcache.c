#include "bcache.h"
#include "serial.h"
#include "string.h"
#include "heap.h"

// All buffers
static buf_t *bufs = NULL;

// LRU doubly-linked list (head.next = most recently used, head.prev = least recently used)
static buf_t lru_head;

// Statistics
static uint32_t stat_hits = 0;
static uint32_t stat_misses = 0;

void bcache_init(void) {
    // Allocate buffers
    bufs = (buf_t *)kmalloc(BCACHE_SIZE * sizeof(buf_t));
    if (!bufs) {
        kprintf("[BCACHE] PANIC: Failed to allocate buffer cache!\n");
        for (;;);
    }

    // Initialize LRU list as circular
    lru_head.prev = &lru_head;
    lru_head.next = &lru_head;

    // Initialize all buffers and add to LRU list
    for (int i = 0; i < BCACHE_SIZE; i++) {
        bufs[i].flags = 0;
        bufs[i].lba = 0;
        bufs[i].dev = NULL;
        bufs[i].refcount = 0;

        // Insert at head (all empty, order doesn't matter yet)
        bufs[i].next = lru_head.next;
        bufs[i].prev = &lru_head;
        lru_head.next->prev = &bufs[i];
        lru_head.next = &bufs[i];
    }

    kprintf("[BCACHE] Initialized (%d blocks, %d KB)\n",
            BCACHE_SIZE, BCACHE_SIZE * 512 / 1024);
}

// Move buffer to front of LRU list (most recently used)
static void lru_move_to_front(buf_t *b) {
    // Remove from current position
    b->prev->next = b->next;
    b->next->prev = b->prev;

    // Insert after head
    b->next = lru_head.next;
    b->prev = &lru_head;
    lru_head.next->prev = b;
    lru_head.next = b;
}

buf_t *bcache_get(block_device_t *dev, uint64_t lba) {
    // Search for existing cached block
    for (buf_t *b = lru_head.next; b != &lru_head; b = b->next) {
        if (b->dev == dev && b->lba == lba && (b->flags & BUF_VALID)) {
            b->refcount++;
            lru_move_to_front(b);
            stat_hits++;
            return b;
        }
    }

    // Cache miss — find a free buffer (LRU eviction)
    // Walk from tail (least recently used) to find unreferenced buffer
    stat_misses++;

    for (buf_t *b = lru_head.prev; b != &lru_head; b = b->prev) {
        if (b->refcount == 0) {
            // Evict this buffer
            if (b->flags & BUF_DIRTY) {
                // Write back dirty data before evicting
                bcache_write(b);
            }

            // Repurpose this buffer
            b->dev = dev;
            b->lba = lba;
            b->flags = 0;
            b->refcount = 1;

            // Read from disk
            if (block_read(dev, lba, 1, b->data) == 0) {
                b->flags = BUF_VALID;
            } else {
                kprintf("[BCACHE] Read error: dev=%s lba=%lu\n", dev->name, (uint64_t)lba);
                // Return buffer anyway with invalid flag — caller should check
                b->refcount = 0;
                return NULL;
            }

            lru_move_to_front(b);
            return b;
        }
    }

    // All buffers are in use — this is bad
    kprintf("[BCACHE] PANIC: no free buffers!\n");
    return NULL;
}

void bcache_release(buf_t *b) {
    if (!b) return;
    if (b->refcount > 0) b->refcount--;
}

void bcache_mark_dirty(buf_t *b) {
    if (b) b->flags |= BUF_DIRTY;
}

void bcache_write(buf_t *b) {
    if (!b || !b->dev || !(b->flags & BUF_DIRTY)) return;

    if (block_write(b->dev, b->lba, 1, b->data) == 0) {
        b->flags &= ~BUF_DIRTY;
    } else {
        kprintf("[BCACHE] Write error: dev=%s lba=%lu\n", b->dev->name, (uint64_t)b->lba);
    }
}

void bcache_sync(void) {
    int flushed = 0;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (bufs[i].flags & BUF_DIRTY) {
            bcache_write(&bufs[i]);
            flushed++;
        }
    }
    if (flushed > 0) {
        kprintf("[BCACHE] Synced %d dirty block(s)\n", flushed);
    }
}

void bcache_stats(uint32_t *hits, uint32_t *misses, uint32_t *dirty_count) {
    if (hits) *hits = stat_hits;
    if (misses) *misses = stat_misses;
    if (dirty_count) {
        uint32_t d = 0;
        for (int i = 0; i < BCACHE_SIZE; i++) {
            if (bufs[i].flags & BUF_DIRTY) d++;
        }
        *dirty_count = d;
    }
}
