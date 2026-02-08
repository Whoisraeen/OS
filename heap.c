#include "heap.h"
#include "pmm.h"
#include "serial.h"
#include "spinlock.h"
#include <stddef.h>
#include <stdbool.h>

// Linked List Allocator
// Supports kfree() to prevent memory leaks

typedef struct block_header {
    size_t size;            // Size of the data block (excluding header)
    struct block_header *next;
    bool is_free;
    uint32_t magic;         // To detect corruption
} block_header_t;

#define HEAP_MAGIC 0xC0FFEE
#define HEADER_SIZE sizeof(block_header_t)

static block_header_t *head = NULL;
static spinlock_t heap_lock;

// Statistics
static size_t total_heap_size = 0;
static size_t used_heap_size = 0;

// Get HHDM offset from PMM
extern uint64_t pmm_get_hhdm_offset(void);

static void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + pmm_get_hhdm_offset());
}

void heap_init(void) {
    spinlock_init(&heap_lock);
    kprintf("[HEAP] Initializing Linked List Heap...\n");
    head = NULL;
    total_heap_size = 0;
    used_heap_size = 0;
}

void *kmalloc(size_t size) {
    spinlock_acquire(&heap_lock);
    
    // Align size to 16 bytes
    size = (size + 15) & ~15;
    
    // 1. Search for free block
    block_header_t *curr = head;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            kprintf("[HEAP] CORRUPTION DETECTED at %p\n", curr);
            spinlock_release(&heap_lock);
            return NULL;
        }
        
        if (curr->is_free && curr->size >= size) {
            // Found a suitable block
            // Split if significantly larger
            if (curr->size >= size + HEADER_SIZE + 32) {
                block_header_t *new_block = (block_header_t *)((uint8_t*)curr + HEADER_SIZE + size);
                new_block->size = curr->size - size - HEADER_SIZE;
                new_block->is_free = true;
                new_block->next = curr->next;
                new_block->magic = HEAP_MAGIC;
                
                curr->size = size;
                curr->next = new_block;
            }
            
            curr->is_free = false;
            used_heap_size += curr->size + HEADER_SIZE;
            
            spinlock_release(&heap_lock);
            return (void*)(curr + 1);
        }
        curr = curr->next;
    }
    
    // 2. No block found, allocate new page(s)
    size_t total_req_size = size + HEADER_SIZE;
    size_t pages_needed = (total_req_size + 4095) / 4096;
    
    // If it's a small allocation, allocate at least 4 pages to reduce fragmentation
    if (pages_needed < 4) pages_needed = 4;
    
    void *new_mem = NULL;
    
    // Use pmm_alloc_pages for contiguous physical memory
    // Note: This relies on HHDM mapping contiguous physical pages to contiguous virtual addresses
    uint64_t phys = 0;
    if (pages_needed > 1) {
         phys = (uint64_t)pmm_alloc_pages(pages_needed);
    } else {
         phys = (uint64_t)pmm_alloc_page();
    }
    
    if (phys == 0) {
        spinlock_release(&heap_lock);
        kprintf("[HEAP] OOM: Failed to allocate %lu bytes (%lu pages)\n", size, pages_needed);
        return NULL;
    }
    
    new_mem = phys_to_virt(phys);
    
    // Create new block
    block_header_t *new_block = (block_header_t *)new_mem;
    new_block->size = (pages_needed * 4096) - HEADER_SIZE;
    new_block->is_free = true; // Temporarily free
    new_block->magic = HEAP_MAGIC;
    new_block->next = head;
    head = new_block;
    
    total_heap_size += pages_needed * 4096;
    
    // Use the new block (split logic)
    if (new_block->size >= size + HEADER_SIZE + 32) {
        block_header_t *split = (block_header_t *)((uint8_t*)new_block + HEADER_SIZE + size);
        split->size = new_block->size - size - HEADER_SIZE;
        split->is_free = true;
        split->next = new_block->next; // Point to old head
        split->magic = HEAP_MAGIC;
        
        new_block->size = size;
        new_block->next = split;
    }
    
    new_block->is_free = false;
    used_heap_size += new_block->size + HEADER_SIZE;
    
    spinlock_release(&heap_lock);
    return (void*)(new_block + 1);
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    spinlock_acquire(&heap_lock);
    
    block_header_t *header = (block_header_t *)((uint8_t*)ptr - HEADER_SIZE);
    
    if (header->magic != HEAP_MAGIC) {
        kprintf("[HEAP] Double free or corruption at %p\n", ptr);
        spinlock_release(&heap_lock);
        return;
    }
    
    header->is_free = true;
    used_heap_size -= (header->size + HEADER_SIZE);

    // Forward coalescing: merge with next block if adjacent and free
    if (header->next && header->next->is_free) {
        if ((uint8_t*)header + HEADER_SIZE + header->size == (uint8_t*)header->next) {
            block_header_t *next = header->next;
            header->size += HEADER_SIZE + next->size;
            header->next = next->next;
        }
    }

    // Backward coalescing: find a free block that ends right before 'header'
    block_header_t *prev = head;
    while (prev) {
        if (prev != header && prev->is_free && prev->magic == HEAP_MAGIC) {
            if ((uint8_t*)prev + HEADER_SIZE + prev->size == (uint8_t*)header) {
                // prev is adjacent before header — merge header into prev
                prev->size += HEADER_SIZE + header->size;
                prev->next = header->next;
                // header is now absorbed into prev
                break;
            }
        }
        prev = prev->next;
    }

    spinlock_release(&heap_lock);
}

// Get heap stats
size_t heap_used(void) {
    return used_heap_size;
}

size_t heap_free(void) {
    return total_heap_size - used_heap_size;
}
