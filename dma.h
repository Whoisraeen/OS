#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include <stddef.h>

// DMA buffer — contiguous physical memory mapped uncached
typedef struct {
    void *virt;          // Virtual address (usable by kernel)
    uint64_t phys;       // Physical address (for hardware)
    size_t size;         // Size in bytes (page-aligned)
} dma_buffer_t;

// Allocate a DMA buffer (physically contiguous, page-aligned, uncached)
// Returns 0 on success, -1 on failure
int dma_alloc(dma_buffer_t *buf, size_t size);

// Free a DMA buffer
void dma_free(dma_buffer_t *buf);

#endif
