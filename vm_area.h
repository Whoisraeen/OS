#ifndef VM_AREA_H
#define VM_AREA_H

#include <stdint.h>
#include <stddef.h>

// VMA permission flags
#define VMA_READ    (1 << 0)
#define VMA_WRITE   (1 << 1)
#define VMA_EXEC    (1 << 2)
#define VMA_USER    (1 << 3)
#define VMA_SHARED  (1 << 4)

// VMA types
typedef enum {
    VMA_TYPE_ANONYMOUS = 0,  // Stack, heap, mmap anonymous
    VMA_TYPE_FILE,           // File-backed (ELF segments, mmap file)
} vma_type_t;

// Virtual Memory Area descriptor
typedef struct vm_area {
    uint64_t start;          // Start virtual address (page-aligned, inclusive)
    uint64_t end;            // End virtual address (page-aligned, exclusive)
    uint32_t flags;          // VMA_READ | VMA_WRITE | VMA_EXEC | VMA_USER
    vma_type_t type;
    struct vm_area *next;    // Sorted linked list (ascending by start)
} vm_area_t;

// Per-process memory descriptor
typedef struct mm_struct {
    vm_area_t *vma_list;     // Sorted linked list of VMAs
    uint64_t brk;            // Current program break
    uint64_t start_brk;      // Start of heap region
    uint64_t mmap_base;      // Base for mmap allocations (grows downward)
} mm_struct_t;

// mm_struct lifecycle
mm_struct_t *mm_create(void);
void mm_destroy(mm_struct_t *mm);
mm_struct_t *mm_clone(mm_struct_t *src);  // Deep copy for fork

// VMA operations
vm_area_t *vma_create(uint64_t start, uint64_t end, uint32_t flags, vma_type_t type);
void vma_insert(mm_struct_t *mm, vm_area_t *vma);
vm_area_t *vma_find(mm_struct_t *mm, uint64_t addr);
void vma_remove(mm_struct_t *mm, uint64_t start, uint64_t end);

// Find a free region of 'size' bytes in the address space
uint64_t vma_find_free(mm_struct_t *mm, uint64_t size);

#endif
