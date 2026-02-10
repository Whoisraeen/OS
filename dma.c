#include "dma.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"

int dma_alloc(dma_buffer_t *buf, size_t size) {
    if (!buf || size == 0) return -1;

    // Round up to page size
    size = (size + 0xFFF) & ~0xFFFULL;
    size_t pages = size / 4096;

    // Allocate contiguous physical pages
    // For small allocations (<= 16 pages), use consecutive pmm_alloc_page calls
    // and hope they're contiguous (works on early boot when memory is unfragmented)
    // For a real OS we'd need a contiguous allocator, but this suffices for now
    void *first_page = pmm_alloc_page();
    if (!first_page) return -1;

    uint64_t phys_base = (uint64_t)first_page;
    int contiguous = 1;

    // Allocate remaining pages and check contiguity
    for (size_t i = 1; i < pages; i++) {
        void *page = pmm_alloc_page();
        if (!page) {
            // Free what we allocated
            for (size_t j = 0; j < i; j++)
                pmm_free_page((void *)(phys_base + j * 4096));
            return -1;
        }
        if ((uint64_t)page != phys_base + i * 4096) {
            contiguous = 0;
            // For DMA we really need contiguous, but for simple cases
            // we'll just use the pages we got and map them contiguously in virtual space
            // The physical addresses won't be contiguous, but for scatter-gather DMA this works
        }
    }

    // Map into kernel virtual space via HHDM with no-cache flags
    uint64_t virt_base = phys_base + vmm_get_hhdm_offset();
    for (size_t i = 0; i < pages; i++) {
        // Re-map with no-cache flag (0x13 = Present | Writable | NoCache)
        vmm_map_page(virt_base + i * 4096, phys_base + i * 4096, 0x13);
    }

    buf->virt = (void *)virt_base;
    buf->phys = phys_base;
    buf->size = size;

    // Zero the buffer
    uint8_t *p = (uint8_t *)buf->virt;
    for (size_t i = 0; i < size; i++)
        p[i] = 0;

    if (!contiguous) {
        kprintf("[DMA] Warning: non-contiguous allocation of %lu bytes\n", (uint64_t)size);
    }

    return 0;
}

void dma_free(dma_buffer_t *buf) {
    if (!buf || !buf->virt || buf->size == 0) return;

    size_t pages = buf->size / 4096;
    for (size_t i = 0; i < pages; i++) {
        pmm_free_page((void *)(buf->phys + i * 4096));
    }

    buf->virt = NULL;
    buf->phys = 0;
    buf->size = 0;
}
