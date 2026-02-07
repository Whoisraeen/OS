#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

// Initialize the heap
void heap_init(void);

// Allocate memory
void *kmalloc(size_t size);

// Free memory
void kfree(void *ptr);

// Get heap stats
size_t heap_used(void);
size_t heap_free(void);

#endif
