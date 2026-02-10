#ifndef U_STDLIB_H
#define U_STDLIB_H

#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"

// Simple Heap Allocator
#define HEAP_BLOCK_SIZE (1024 * 1024) // 1 MB initial heap

typedef struct mem_block {
    size_t size;
    int free;
    struct mem_block *next;
} mem_block_t;

static void *heap_start = NULL;
static mem_block_t *free_list = NULL;

static void heap_init() {
    // Request shared memory for heap
    long shmem_id = syscall2(SYS_IPC_SHMEM_CREATE, HEAP_BLOCK_SIZE, 0);
    if (shmem_id <= 0) return;

    heap_start = (void*)syscall1(SYS_IPC_SHMEM_MAP, shmem_id);
    if (!heap_start) return;

    // Initialize first block
    free_list = (mem_block_t *)heap_start;
    free_list->size = HEAP_BLOCK_SIZE - sizeof(mem_block_t);
    free_list->free = 1;
    free_list->next = NULL;
}

static void *malloc(size_t size) {
    if (!heap_start) heap_init();
    if (!heap_start) return NULL;

    // Align size to 8 bytes
    size = (size + 7) & ~7;

    mem_block_t *curr = free_list;
    while (curr) {
        if (curr->free && curr->size >= size) {
            // Found a block
            // Split if large enough
            if (curr->size > size + sizeof(mem_block_t) + 16) {
                mem_block_t *new_block = (mem_block_t *)((uint8_t*)curr + sizeof(mem_block_t) + size);
                new_block->size = curr->size - size - sizeof(mem_block_t);
                new_block->free = 1;
                new_block->next = curr->next;

                curr->size = size;
                curr->next = new_block;
            }
            curr->free = 0;
            return (void*)((uint8_t*)curr + sizeof(mem_block_t));
        }
        curr = curr->next;
    }
    return NULL; // OOM
}

static void free(void *ptr) {
    if (!ptr) return;
    mem_block_t *block = (mem_block_t *)((uint8_t*)ptr - sizeof(mem_block_t));
    block->free = 1;

    // Merge with next if free
    if (block->next && block->next->free) {
        block->size += sizeof(mem_block_t) + block->next->size;
        block->next = block->next->next;
    }
    
    // Naive merge with prev is hard with singly linked list without traversal
    // We'll skip prev-merge for this simple implementation
}

static void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    
    mem_block_t *block = (mem_block_t *)((uint8_t*)ptr - sizeof(mem_block_t));
    if (block->size >= size) return ptr; // Already big enough

    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    
    // memcpy(new_ptr, ptr, block->size); // We need memcpy
    // Simple copy loop
    uint8_t *d = (uint8_t*)new_ptr;
    uint8_t *s = (uint8_t*)ptr;
    for(size_t i=0; i<block->size; i++) d[i] = s[i];
    
    free(ptr);
    return new_ptr;
}

// String Utils
static inline int snprintf(char *buf, size_t size, const char *fmt, ...) {
    // Stub
    if (size > 0) {
        buf[0] = 'M'; buf[1] = 's'; buf[2] = 'g'; buf[3] = 0;
    }
    return 3;
}
#endif // U_STDLIB_H
