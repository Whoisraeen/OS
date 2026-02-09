#include "vm_area.h"
#include "heap.h"
#include "string.h"
#include "pmm.h"

// Allocate and initialize a VMA
vm_area_t *vma_create(uint64_t start, uint64_t end, uint32_t flags, vma_type_t type) {
    vm_area_t *vma = (vm_area_t *)kmalloc(sizeof(vm_area_t));
    if (!vma) return NULL;

    vma->start = start;
    vma->end = end;
    vma->flags = flags;
    vma->type = type;
    vma->next = NULL;
    return vma;
}

// Insert VMA into sorted list (sorted by start address)
void vma_insert(mm_struct_t *mm, vm_area_t *vma) {
    if (!mm || !vma) return;

    vm_area_t **prev = &mm->vma_list;
    while (*prev && (*prev)->start < vma->start) {
        prev = &(*prev)->next;
    }
    vma->next = *prev;
    *prev = vma;
}

// Find the VMA containing 'addr'
vm_area_t *vma_find(mm_struct_t *mm, uint64_t addr) {
    if (!mm) return NULL;

    for (vm_area_t *vma = mm->vma_list; vma; vma = vma->next) {
        if (addr >= vma->start && addr < vma->end)
            return vma;
        // Since list is sorted, if start > addr we can stop early
        if (vma->start > addr)
            break;
    }
    return NULL;
}

// Remove all VMAs overlapping [start, end)
void vma_remove(mm_struct_t *mm, uint64_t start, uint64_t end) {
    if (!mm) return;

    vm_area_t **prev = &mm->vma_list;
    while (*prev) {
        vm_area_t *cur = *prev;

        // No overlap — skip
        if (cur->end <= start || cur->start >= end) {
            prev = &cur->next;
            continue;
        }

        // Full overlap — remove entirely
        if (cur->start >= start && cur->end <= end) {
            *prev = cur->next;
            kfree(cur);
            continue;
        }

        // Partial overlap from the left: cur starts before 'start'
        if (cur->start < start && cur->end > start && cur->end <= end) {
            cur->end = start;
            prev = &cur->next;
            continue;
        }

        // Partial overlap from the right: cur ends after 'end'
        if (cur->start >= start && cur->start < end && cur->end > end) {
            cur->start = end;
            prev = &cur->next;
            continue;
        }

        // VMA spans both sides: split into two
        if (cur->start < start && cur->end > end) {
            vm_area_t *right = vma_create(end, cur->end, cur->flags, cur->type);
            if (right) {
                right->next = cur->next;
                cur->end = start;
                cur->next = right;
                prev = &right->next;
            }
            continue;
        }

        prev = &cur->next;
    }
}

// Find a free region of 'size' bytes, searching downward from mmap_base
uint64_t vma_find_free(mm_struct_t *mm, uint64_t size) {
    if (!mm || size == 0) return 0;

    // Align size to page boundary
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // Search downward from mmap_base
    uint64_t candidate = mm->mmap_base - size;

    // Walk VMAs to find a gap
    for (vm_area_t *vma = mm->vma_list; vma; vma = vma->next) {
        if (candidate >= vma->start && candidate < vma->end) {
            // Conflict — move below this VMA
            if (vma->start < size) return 0; // Not enough space
            candidate = (vma->start - size) & ~(PAGE_SIZE - 1);
        }
        if (candidate + size <= vma->start) {
            // Found a gap before this VMA
            break;
        }
    }

    if (candidate < PAGE_SIZE) return 0; // Can't map at 0

    // Update mmap_base for next allocation
    mm->mmap_base = candidate;
    return candidate;
}

// Create a new mm_struct
mm_struct_t *mm_create(void) {
    mm_struct_t *mm = (mm_struct_t *)kmalloc(sizeof(mm_struct_t));
    if (!mm) return NULL;

    mm->vma_list = NULL;
    mm->brk = 0;
    mm->start_brk = 0;
    mm->mmap_base = 0x7FF000000000ULL; // Below user stack region
    return mm;
}

// Destroy mm_struct and all its VMAs
void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;

    vm_area_t *vma = mm->vma_list;
    while (vma) {
        vm_area_t *next = vma->next;
        kfree(vma);
        vma = next;
    }
    kfree(mm);
}

// Clone mm_struct for fork (deep copy of VMA list)
mm_struct_t *mm_clone(mm_struct_t *src) {
    if (!src) return NULL;

    mm_struct_t *dst = (mm_struct_t *)kmalloc(sizeof(mm_struct_t));
    if (!dst) return NULL;

    dst->brk = src->brk;
    dst->start_brk = src->start_brk;
    dst->mmap_base = src->mmap_base;
    dst->vma_list = NULL;

    // Deep copy the VMA list
    vm_area_t **tail = &dst->vma_list;
    for (vm_area_t *sv = src->vma_list; sv; sv = sv->next) {
        vm_area_t *dv = vma_create(sv->start, sv->end, sv->flags, sv->type);
        if (!dv) {
            mm_destroy(dst);
            return NULL;
        }
        *tail = dv;
        tail = &dv->next;
    }

    return dst;
}
