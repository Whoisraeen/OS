#include "pmm.h"
#include <stddef.h>

// Request Memory Map from Limine
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// Request HHDM (Higher Half Direct Map) offset from Limine
// This tells us how to convert physical addresses to virtual addresses.
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static uint8_t *bitmap = NULL;          // Virtual pointer to the bitmap
static uint64_t bitmap_phys = 0;        // Physical address of the bitmap
static uint64_t highest_page = 0;
static uint64_t bitmap_size = 0;
static uint64_t hhdm_offset = 0;        // Limine's HHDM offset

// Helper to convert physical address to virtual address
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

static void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

// Export HHDM offset for other modules (e.g., VMM)
uint64_t pmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

static void bitmap_unset(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int bitmap_test(uint64_t bit) {
    return bitmap[bit / 8] & (1 << (bit % 8));
}

// Debugging globals
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

void pmm_init(void) {
    // DEBUG: Entered pmm_init -> MAGENTA
    if (fb_ptr) for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFFF00FF;

    struct limine_memmap_response *memmap = memmap_request.response;
    struct limine_hhdm_response *hhdm = hhdm_request.response;

    // CRITICAL: Check if Limine fulfilled our requests
    if (memmap == NULL || hhdm == NULL) {
        // Limine didn't respond. Panic.
        if (fb_ptr) for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFAA00FF; // Purple
        for (;;) __asm__("hlt");
    }

    hhdm_offset = hhdm->offset;

    // 1. Find the highest writable page index
    uint64_t highest_addr = 0;
    
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        
        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) {
            highest_addr = top;
        }
    }

    highest_page = highest_addr / PAGE_SIZE;
    bitmap_size = highest_page / 8 + 1;

    // 2. Find a place to put the bitmap (in USABLE memory)
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            if (entry->length >= bitmap_size) {
                // DEBUG: Found candidate -> YELLOW
                if (fb_ptr) for (size_t j = 0; j < fb_width * fb_height / 4; j++) fb_ptr[j] = 0xFFFFFF00;

                // Store both physical and virtual addresses
                bitmap_phys = entry->base;
                bitmap = (uint8_t *)phys_to_virt(bitmap_phys);
                
                // Initialize bitmap to all 1s (Used)
                for (uint64_t b = 0; b < bitmap_size; b++) {
                    bitmap[b] = 0xFF; 
                }
                
                break;
            }
        }
    }

    if (bitmap == NULL) {
        // Panic: No memory for bitmap -> RED
        if (fb_ptr) for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFFF0000;
        for (;;) __asm__("hlt");
    }

    // 3. Populate Bitmap based on Memory Map
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_page = entry->base / PAGE_SIZE;
            uint64_t num_pages = entry->length / PAGE_SIZE;

            for (uint64_t p = 0; p < num_pages; p++) {
                bitmap_unset(start_page + p);
            }
        }
    }

    // 4. Mark the bitmap's own memory as used
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    uint64_t bitmap_num_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = 0; p < bitmap_num_pages; p++) {
        bitmap_set(bitmap_start_page + p);
    }
    
    // 5. Mark Page 0 as used (null pointer protection)
    bitmap_set(0); 
}

void *pmm_alloc_page(void) {
    // Find first free bit
    for (uint64_t i = 1; i < highest_page; i++) { // Skip page 0
        if (!bitmap_test(i)) {
            bitmap_set(i);
            // Return physical address (caller uses phys_to_virt if needed)
            return (void *)(i * PAGE_SIZE);
        }
    }
    return NULL; // Out of memory
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    
    // Find contiguous range of free pages
    uint64_t start = 1; // Skip page 0
    uint64_t found = 0;
    
    for (uint64_t i = 1; i < highest_page; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) start = i;
            found++;
            if (found == count) {
                // Success! Mark them as used and return
                for (uint64_t j = 0; j < count; j++) {
                    bitmap_set(start + j);
                }
                return (void *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
    }
    return NULL; // Out of memory
}

void pmm_free_page(void *ptr) {
    uint64_t addr = (uint64_t)ptr;
    uint64_t page = addr / PAGE_SIZE;
    bitmap_unset(page);
}
