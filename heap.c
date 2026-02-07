#include "heap.h"
#include "pmm.h"
#include "serial.h"
#include <stddef.h>

// Simple bump allocator for kernel heap
// We allocate pages from PMM and bump-allocate within them

static uint8_t *heap_start = NULL;
static uint8_t *heap_current = NULL;
static uint8_t *heap_end = NULL;

// Initial heap size (1024 pages = 4MB - enough for back buffer)
#define INITIAL_HEAP_PAGES 1024

// Get HHDM offset from PMM
extern uint64_t pmm_get_hhdm_offset(void);

static void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + pmm_get_hhdm_offset());
}

void heap_init(void) {
    // Allocate initial heap pages
    uint64_t first_page_phys = (uint64_t)pmm_alloc_page();
    if (first_page_phys == 0) {
        kprintf("[HEAP] Failed to allocate first page!\n");
        return;
    }
    
    heap_start = (uint8_t *)phys_to_virt(first_page_phys);
    heap_current = heap_start;
    heap_end = heap_start + 4096; // Start with one page
    
    // Allocate more pages for initial heap (we need ~4MB for back buffer)
    int pages_allocated = 1;
    for (int i = 1; i < INITIAL_HEAP_PAGES; i++) {
        uint64_t page_phys = (uint64_t)pmm_alloc_page();
        if (page_phys == 0) break;
        
        // These pages should be contiguous in virtual space (HHDM)
        // In practice, this works because HHDM maps all physical memory linearly
        heap_end += 4096;
        pages_allocated++;
    }
    
    size_t heap_size = heap_end - heap_start;
    kprintf("[HEAP] Initialized: %lu KB (%d pages)\n", heap_size / 1024, pages_allocated);
}

void *kmalloc(size_t size) {
    if (heap_start == NULL) {
        heap_init();
    }
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    // Check if we have space
    while (heap_current + size > heap_end) {
        // Need more pages - try to extend
        uint64_t new_page_phys = (uint64_t)pmm_alloc_page();
        if (new_page_phys == 0) {
            kprintf("[HEAP] kmalloc(%lu) failed - out of memory!\n", size);
            return NULL; // Out of memory
        }
        
        // Extend the heap
        heap_end += 4096;
    }
    
    void *result = heap_current;
    heap_current += size;
    
    return result;
}

void kfree(void *ptr) {
    // Simple bump allocator doesn't support freeing individual allocations
    // In a real OS, we'd use a more sophisticated allocator (slab, buddy, etc.)
    // For now, this is a no-op
    (void)ptr;
}

// Get heap stats
size_t heap_used(void) {
    if (heap_start == NULL) return 0;
    return heap_current - heap_start;
}

size_t heap_free(void) {
    if (heap_start == NULL) return 0;
    return heap_end - heap_current;
}
