#include "pmm.h"
#include <stddef.h>

// Request Memory Map from Limine (exported for use by VMM)
__attribute__((used, section(".requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// Request HHDM (Higher Half Direct Map) offset from Limine
// This tells us how to convert physical addresses to virtual addresses.
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static uint8_t *bitmap = NULL;          // Virtual pointer to the bitmap
static uint64_t bitmap_phys = 0;        // Physical address of the bitmap
static uint64_t highest_page = 0;
static uint64_t bitmap_size = 0;
static uint64_t hhdm_offset = 0;        // Limine's HHDM offset
static uint64_t first_free_hint = 1;    // Hint: first possibly-free page (skip page 0)

// Reference counting array for COW support
// refcount[page_index] = number of references to this physical page
// 0 = free, 1 = single owner, >1 = shared (COW)
static uint16_t *refcounts = NULL;
static uint64_t refcounts_phys = 0;

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
    struct limine_memmap_response *memmap = memmap_request.response;
    struct limine_hhdm_response *hhdm = hhdm_request.response;

    if (memmap == NULL || hhdm == NULL) {
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
        for (;;) __asm__("hlt");
    }

    // 2b. Allocate refcount array (2 bytes per page) in USABLE memory after bitmap
    uint64_t refcounts_size = highest_page * sizeof(uint16_t);
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            // Skip the bitmap's region
            uint64_t avail_base = entry->base;
            uint64_t avail_end = entry->base + entry->length;
            if (avail_base <= bitmap_phys && bitmap_phys < avail_end) {
                avail_base = bitmap_phys + bitmap_size;
                avail_base = (avail_base + 7) & ~7ULL; // Align to 8
            }
            if (avail_end - avail_base >= refcounts_size) {
                refcounts_phys = avail_base;
                refcounts = (uint16_t *)phys_to_virt(refcounts_phys);
                for (uint64_t r = 0; r < highest_page; r++) {
                    refcounts[r] = 0;
                }
                break;
            }
        }
    }

    if (refcounts == NULL) {
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
    
    // 5. Mark refcount array as used
    uint64_t ref_start_page = refcounts_phys / PAGE_SIZE;
    uint64_t ref_num_pages = (refcounts_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = 0; p < ref_num_pages; p++) {
        bitmap_set(ref_start_page + p);
    }

    // 6. Mark Page 0 as used (null pointer protection)
    bitmap_set(0);
}

void *pmm_alloc_page(void) {
    // Start from hint instead of always scanning from 1
    for (uint64_t i = first_free_hint; i < highest_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            refcounts[i] = 1;
            first_free_hint = i + 1; // Next search starts after this page
            return (void *)(i * PAGE_SIZE);
        }
    }
    // Wrap around: search from 1 to hint (in case pages were freed before hint)
    for (uint64_t i = 1; i < first_free_hint && i < highest_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            refcounts[i] = 1;
            first_free_hint = i + 1;
            return (void *)(i * PAGE_SIZE);
        }
    }
    return NULL; // Out of memory
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;

    // Find contiguous range of free pages, starting from hint
    uint64_t start = first_free_hint;
    uint64_t found = 0;

    for (uint64_t i = first_free_hint; i < highest_page; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) start = i;
            found++;
            if (found == count) {
                for (uint64_t j = 0; j < count; j++) {
                    bitmap_set(start + j);
                }
                first_free_hint = start + count;
                return (void *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
    }

    // Wrap around: search from 1 to hint
    found = 0;
    for (uint64_t i = 1; i < first_free_hint && i < highest_page; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) start = i;
            found++;
            if (found == count) {
                for (uint64_t j = 0; j < count; j++) {
                    bitmap_set(start + j);
                }
                first_free_hint = start + count;
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
    if (page >= highest_page) return;
    // Use refcount-aware free: decrement and only free if refcount reaches 0
    if (refcounts && refcounts[page] > 0) {
        refcounts[page]--;
        if (refcounts[page] > 0) return; // Still shared
    }
    bitmap_unset(page);
    if (page < first_free_hint) {
        first_free_hint = page;
    }
}

void pmm_free_pages(void *ptr, size_t count) {
    uint64_t addr = (uint64_t)ptr;
    uint64_t page = addr / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        pmm_free_page((void *)((page + i) * PAGE_SIZE));
    }
}

// Increment reference count for a physical page
void pmm_page_ref(void *phys) {
    uint64_t page = (uint64_t)phys / PAGE_SIZE;
    if (page < highest_page && refcounts) {
        refcounts[page]++;
    }
}

// Decrement reference count, free if reaches 0
void pmm_page_unref(void *phys) {
    pmm_free_page(phys); // pmm_free_page already handles refcount
}

// Get current reference count
uint32_t pmm_get_refcount(void *phys) {
    uint64_t page = (uint64_t)phys / PAGE_SIZE;
    if (page < highest_page && refcounts) {
        return refcounts[page];
    }
    return 0;
}
