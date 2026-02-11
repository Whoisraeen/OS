#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "sched.h"
#include "vm_area.h"
#include "limine/limine.h"
#include <stddef.h>

// Request kernel address from Limine
__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_addr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

// Use PMM's memory map request (declared in pmm.c)
extern volatile struct limine_memmap_request memmap_request;

// Our PML4 (top-level page table)
static uint64_t *pml4_virt = NULL;
static uint64_t pml4_phys = 0;
static uint64_t hhdm_offset = 0;

// Debugging globals
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Convert physical to virtual
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

// Convert virtual to physical (for addresses in HHDM range)
static inline uint64_t virt_to_phys(void *virt) {
    return (uint64_t)virt - hhdm_offset;
}

uint64_t vmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

// Get or create a page table entry, allocating if necessary
static uint64_t *vmm_get_next_level(uint64_t *table, size_t index, int allocate) {
    if (table[index] & PTE_PRESENT) {
        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        return (uint64_t *)phys_to_virt(next_phys);
    }

    if (!allocate) {
        return NULL;
    }

    uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
    if (new_table_phys == 0) {
        return NULL;
    }

    uint64_t *new_table_virt = (uint64_t *)phys_to_virt(new_table_phys);
    for (int i = 0; i < 512; i++) {
        new_table_virt[i] = 0;
    }

    table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE;
    return new_table_virt;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx   = PD_INDEX(virt);
    size_t pt_idx   = PT_INDEX(virt);

    uint64_t *pdpt = vmm_get_next_level(pml4_virt, pml4_idx, 1);
    if (!pdpt) return;

    uint64_t *pd = vmm_get_next_level(pdpt, pdpt_idx, 1);
    if (!pd) return;

    uint64_t *pt = vmm_get_next_level(pd, pd_idx, 1);
    if (!pt) return;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    vmm_invlpg(virt);
}

void vmm_init(void) {
    struct limine_kernel_address_response *kernel_addr = kernel_addr_request.response;

    // Get HHDM offset from PMM (which has the Limine request)
    hhdm_offset = pmm_get_hhdm_offset();

    if (hhdm_offset == 0) {
        if (fb_ptr) for (size_t i = 0; i < fb_width * fb_height; i++) fb_ptr[i] = 0xFFAA0000;
        for (;;) __asm__("hlt");
    }

    // Allocate our PML4
    pml4_phys = (uint64_t)pmm_alloc_page();
    pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);
    for (int i = 0; i < 512; i++) {
        pml4_virt[i] = 0;
    }

    // Determine highest physical address from memory map
    // Map ALL physical memory via HHDM, not just 512MB
    uint64_t highest_addr = 0;
    struct limine_memmap_response *memmap = memmap_request.response;
    if (memmap) {
        for (uint64_t i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = memmap->entries[i];
            uint64_t top = entry->base + entry->length;
            if (top > highest_addr) {
                highest_addr = top;
            }
        }
    }

    // Fallback: at least 512MB
    if (highest_addr < 512 * 1024 * 1024) {
        highest_addr = 512 * 1024 * 1024;
    }

    // Cap at 4GB to avoid excessive page table allocation during early boot
    if (highest_addr > 4ULL * 1024 * 1024 * 1024) {
        highest_addr = 4ULL * 1024 * 1024 * 1024;
    }

    kprintf("[VMM] Mapping HHDM: 0 - 0x%lx (%lu MB)\n",
            highest_addr, highest_addr / (1024 * 1024));

    for (uint64_t phys = 0; phys < highest_addr; phys += PAGE_SIZE) {
        uint64_t virt = hhdm_offset + phys;
        // KERNEL SECURITY: Only PTE_WRITABLE, NO PTE_USER
        vmm_map_page(virt, phys, PTE_WRITABLE);
    }

    // Map the kernel
    if (kernel_addr != NULL) {
        uint64_t kernel_virt = kernel_addr->virtual_base;
        uint64_t kernel_phys = kernel_addr->physical_base;
        uint64_t kernel_size = 16 * 1024 * 1024; // 16 MB (generous)

        for (uint64_t offset = 0; offset < kernel_size; offset += PAGE_SIZE) {
            vmm_map_page(kernel_virt + offset, kernel_phys + offset, PTE_WRITABLE);
        }
    }

    // Map the framebuffer
    if (fb_ptr != NULL) {
        uint64_t fb_virt = (uint64_t)fb_ptr;
        uint64_t fb_phys;
        
        // Determine physical address
        if (kernel_addr && fb_virt >= kernel_addr->virtual_base && fb_virt < kernel_addr->virtual_base + 64*1024*1024) {
            fb_phys = fb_virt - kernel_addr->virtual_base + kernel_addr->physical_base;
        } else {
            // Assume HHDM
            fb_phys = fb_virt - hhdm_offset;
        }

        uint64_t fb_size = fb_width * fb_height * 4;
        fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t offset = 0; offset < fb_size; offset += PAGE_SIZE) {
            vmm_map_page(fb_virt + offset, fb_phys + offset, PTE_WRITABLE);
        }
    }
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t current_pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
    current_pml4_phys &= PTE_ADDR_MASK;

    uint64_t *cur_pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);

    size_t idx4 = PML4_INDEX(virt);
    if (!(cur_pml4[idx4] & PTE_PRESENT)) return;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(cur_pml4[idx4] & PTE_ADDR_MASK);
    size_t idx3 = PDPT_INDEX(virt);
    if (!(pdpt[idx3] & PTE_PRESENT)) return;

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[idx3] & PTE_ADDR_MASK);
    size_t idx2 = PD_INDEX(virt);
    if (!(pd[idx2] & PTE_PRESENT)) return;

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[idx2] & PTE_ADDR_MASK);
    size_t idx1 = PT_INDEX(virt);

    pt[idx1] = 0;
    vmm_invlpg(virt);
}

void vmm_switch(void) {
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(pml4_phys)
        : "memory"
    );
}

// Map a page with USER permissions using CURRENT CR3
void vmm_map_user_page(uint64_t virt, uint64_t phys) {
    uint64_t current_pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
    current_pml4_phys &= PTE_ADDR_MASK;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);

    size_t pml4_idx = PML4_INDEX(virt);
    size_t pdpt_idx = PDPT_INDEX(virt);
    size_t pd_idx   = PD_INDEX(virt);
    size_t pt_idx   = PT_INDEX(virt);

    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        if (!new_phys) {
             kprintf("[VMM] OOM allocating PML4 entry for %lx\n", virt);
             return;
        }
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pml4[pml4_idx] = new_phys | intermediate_flags;
    } else {
        pml4[pml4_idx] |= PTE_USER;
    }

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        if (!new_phys) {
             kprintf("[VMM] OOM allocating PDPT entry for %lx\n", virt);
             return;
        }
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pdpt[pdpt_idx] = new_phys | intermediate_flags;
    } else {
        pdpt[pdpt_idx] |= PTE_USER;
    }

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_phys = (uint64_t)pmm_alloc_page();
        if (!new_phys) {
             kprintf("[VMM] OOM allocating PD entry for %lx\n", virt);
             return;
        }
        uint64_t *new_virt = (uint64_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 512; i++) new_virt[i] = 0;
        pd[pd_idx] = new_phys | intermediate_flags;
    } else {
        pd[pd_idx] |= PTE_USER;
    }

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
    pt[pt_idx] = (phys & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    vmm_invlpg(virt);
}

void vmm_destroy_user_space(uint64_t pml4_phys_addr) {
    if (!pml4_phys_addr) return;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys_addr);

    // Walk user-space entries only (indices 0-255)
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) continue;

        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;

            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                if (pd[i2] & PTE_HUGE) {
                    // 2MB page — skip (we don't allocate these yet)
                    continue;
                }

                uint64_t *pt = (uint64_t *)phys_to_virt(pd[i2] & PTE_ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PTE_PRESENT) {
                        uint64_t page_phys = pt[i1] & PTE_ADDR_MASK;
                        pmm_free_page((void *)page_phys);
                    }
                }
                // Free the PT itself
                pmm_free_page((void *)(pd[i2] & PTE_ADDR_MASK));
            }
            // Free the PD
            pmm_free_page((void *)(pdpt[i3] & PTE_ADDR_MASK));
        }
        // Free the PDPT
        pmm_free_page((void *)(pml4[i4] & PTE_ADDR_MASK));
    }

    // Free the PML4 itself
    pmm_free_page((void *)pml4_phys_addr);
}

uint64_t vmm_get_pml4(void) {
    return pml4_phys;
}

uint64_t vmm_create_user_pml4(void) {
    uint64_t phys = (uint64_t)pmm_alloc_page();
    if (!phys) return 0;

    uint64_t *virt = (uint64_t *)phys_to_virt(phys);

    // Zero lower half (User space)
    for (int i = 0; i < 256; i++) virt[i] = 0;

    // Copy upper half (Kernel space) from kernel PML4
    for (int i = 256; i < 512; i++) virt[i] = pml4_virt[i];

    return phys;
}

// ---- Page fault handling (demand paging + COW) ----

// Get the PTE value for a virtual address using current CR3
uint64_t vmm_get_pte(uint64_t virt) {
    uint64_t current_pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
    current_pml4_phys &= PTE_ADDR_MASK;

    uint64_t *cur_pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);

    size_t idx4 = PML4_INDEX(virt);
    if (!(cur_pml4[idx4] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(cur_pml4[idx4] & PTE_ADDR_MASK);
    size_t idx3 = PDPT_INDEX(virt);
    if (!(pdpt[idx3] & PTE_PRESENT)) return 0;

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[idx3] & PTE_ADDR_MASK);
    size_t idx2 = PD_INDEX(virt);
    if (!(pd[idx2] & PTE_PRESENT)) return 0;

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[idx2] & PTE_ADDR_MASK);
    size_t idx1 = PT_INDEX(virt);
    return pt[idx1];
}

static int vmm_handle_page_fault_internal(uint64_t fault_addr, uint64_t error_code);

// Handle a page fault. Returns 1 if handled, 0 if unrecoverable.
int vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    // Only attempt to handle faults for addresses in the user-space range
    if (is_user_address(fault_addr, 1)) {
        return vmm_handle_page_fault_internal(fault_addr, error_code);
    }
    return 0;
}

static int vmm_handle_page_fault_internal(uint64_t fault_addr, uint64_t error_code) {
    // Get current task
    task_t *task = task_get_by_id(task_current_id());
    if (!task || !task->mm) return 0;

    // Find the VMA covering this address
    vm_area_t *vma = vma_find(task->mm, fault_addr);
    if (!vma) return 0; // No VMA = invalid access

    // Check permission: write fault to non-writable VMA?
    if ((error_code & 2) && !(vma->flags & VMA_WRITE)) return 0;

    uint64_t page_addr = fault_addr & ~0xFFFULL;
    uint64_t pte = vmm_get_pte(page_addr);

    // Case 1: Page not present — demand paging
    if (!(pte & PTE_PRESENT)) {
        uint64_t phys = (uint64_t)pmm_alloc_page();
        if (!phys) return 0;

        // Zero the page
        void *kptr = phys_to_virt(phys);
        memset(kptr, 0, PAGE_SIZE);

        // Map with appropriate permissions
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (vma->flags & VMA_WRITE) flags |= PTE_WRITABLE;
        vmm_map_user_page(page_addr, phys);
        // Adjust flags: vmm_map_user_page always sets writable, fix if read-only
        if (!(vma->flags & VMA_WRITE)) {
            // Re-map as read-only
            uint64_t current_pml4_phys;
            __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
            current_pml4_phys &= PTE_ADDR_MASK;
            uint64_t *cur_pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);
            uint64_t *pdpt = (uint64_t *)phys_to_virt(cur_pml4[PML4_INDEX(page_addr)] & PTE_ADDR_MASK);
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(page_addr)] & PTE_ADDR_MASK);
            uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(page_addr)] & PTE_ADDR_MASK);
            pt[PT_INDEX(page_addr)] = phys | PTE_PRESENT | PTE_USER;
            vmm_invlpg(page_addr);
        }
        return 1;
    }

    // Case 2: COW — page is present but write fault and page is read-only
    if ((error_code & 2) && (pte & PTE_PRESENT) && !(pte & PTE_WRITABLE)) {
        uint64_t old_phys = pte & PTE_ADDR_MASK;
        uint32_t refcount = pmm_get_refcount((void *)old_phys);

        if (refcount > 1) {
            // Shared page: copy-on-write
            uint64_t new_phys = (uint64_t)pmm_alloc_page();
            if (!new_phys) return 0;

            // Copy the page contents
            void *src = phys_to_virt(old_phys);
            void *dst = phys_to_virt(new_phys);
            memcpy(dst, src, PAGE_SIZE);

            // Decrement old page refcount
            pmm_page_unref((void *)old_phys);

            // Map new page as writable
            uint64_t current_pml4_phys;
            __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
            current_pml4_phys &= PTE_ADDR_MASK;
            uint64_t *cur_pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);
            uint64_t *pdpt = (uint64_t *)phys_to_virt(cur_pml4[PML4_INDEX(page_addr)] & PTE_ADDR_MASK);
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(page_addr)] & PTE_ADDR_MASK);
            uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(page_addr)] & PTE_ADDR_MASK);
            pt[PT_INDEX(page_addr)] = new_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            vmm_invlpg(page_addr);
        } else {
            // Sole owner: just make it writable again
            uint64_t current_pml4_phys;
            __asm__ volatile("mov %%cr3, %0" : "=r"(current_pml4_phys));
            current_pml4_phys &= PTE_ADDR_MASK;
            uint64_t *cur_pml4 = (uint64_t *)phys_to_virt(current_pml4_phys);
            uint64_t *pdpt = (uint64_t *)phys_to_virt(cur_pml4[PML4_INDEX(page_addr)] & PTE_ADDR_MASK);
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[PDPT_INDEX(page_addr)] & PTE_ADDR_MASK);
            uint64_t *pt = (uint64_t *)phys_to_virt(pd[PD_INDEX(page_addr)] & PTE_ADDR_MASK);
            pt[PT_INDEX(page_addr)] |= PTE_WRITABLE;
            vmm_invlpg(page_addr);
        }
        return 1;
    }

    return 0; // Not handled
}

// Fork user-space: clone page tables with COW
// Returns new PML4 physical address (0 on failure)
uint64_t vmm_fork_user_space(uint64_t parent_pml4_phys) {
    if (!parent_pml4_phys) return 0;

    // Create new PML4
    uint64_t child_pml4_phys = (uint64_t)pmm_alloc_page();
    if (!child_pml4_phys) return 0;

    uint64_t *parent_pml4 = (uint64_t *)phys_to_virt(parent_pml4_phys);
    uint64_t *child_pml4 = (uint64_t *)phys_to_virt(child_pml4_phys);

    // Copy kernel half (indices 256-511)
    for (int i = 256; i < 512; i++) child_pml4[i] = parent_pml4[i];

    // Zero user half initially
    for (int i = 0; i < 256; i++) child_pml4[i] = 0;

    // Walk user-space entries and create COW copies
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(parent_pml4[i4] & PTE_PRESENT)) continue;

        // Allocate new PDPT for child
        uint64_t child_pdpt_phys = (uint64_t)pmm_alloc_page();
        if (!child_pdpt_phys) goto fail;
        uint64_t *child_pdpt = (uint64_t *)phys_to_virt(child_pdpt_phys);
        for (int z = 0; z < 512; z++) child_pdpt[z] = 0;
        child_pml4[i4] = child_pdpt_phys | (parent_pml4[i4] & ~PTE_ADDR_MASK);

        uint64_t *parent_pdpt = (uint64_t *)phys_to_virt(parent_pml4[i4] & PTE_ADDR_MASK);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PTE_PRESENT)) continue;

            uint64_t child_pd_phys = (uint64_t)pmm_alloc_page();
            if (!child_pd_phys) goto fail;
            uint64_t *child_pd = (uint64_t *)phys_to_virt(child_pd_phys);
            for (int z = 0; z < 512; z++) child_pd[z] = 0;
            child_pdpt[i3] = child_pd_phys | (parent_pdpt[i3] & ~PTE_ADDR_MASK);

            uint64_t *parent_pd = (uint64_t *)phys_to_virt(parent_pdpt[i3] & PTE_ADDR_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PTE_PRESENT)) continue;
                if (parent_pd[i2] & PTE_HUGE) continue; // Skip 2MB pages

                uint64_t child_pt_phys = (uint64_t)pmm_alloc_page();
                if (!child_pt_phys) goto fail;
                uint64_t *child_pt = (uint64_t *)phys_to_virt(child_pt_phys);
                child_pd[i2] = child_pt_phys | (parent_pd[i2] & ~PTE_ADDR_MASK);

                uint64_t *parent_pt = (uint64_t *)phys_to_virt(parent_pd[i2] & PTE_ADDR_MASK);

                for (int i1 = 0; i1 < 512; i1++) {
                    if (!(parent_pt[i1] & PTE_PRESENT)) {
                        child_pt[i1] = 0;
                        continue;
                    }

                    uint64_t page_phys = parent_pt[i1] & PTE_ADDR_MASK;
                    uint64_t flags = parent_pt[i1] & ~PTE_ADDR_MASK;

                    // Mark as read-only in both parent and child (COW)
                    flags &= ~PTE_WRITABLE;
                    parent_pt[i1] = page_phys | flags;
                    child_pt[i1] = page_phys | flags;

                    // Increment refcount for this physical page
                    pmm_page_ref((void *)page_phys);

                    // Flush TLB for this page in parent
                    uint64_t vaddr = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                     ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    vmm_invlpg(vaddr);
                }
            }
        }
    }

    return child_pml4_phys;

fail:
    // Cleanup: destroy the partially-built child page tables
    // (don't free the shared physical pages — parent still owns them)
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(child_pml4[i4] & PTE_PRESENT)) continue;
        uint64_t *cpdpt = (uint64_t *)phys_to_virt(child_pml4[i4] & PTE_ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(cpdpt[i3] & PTE_PRESENT)) continue;
            uint64_t *cpd = (uint64_t *)phys_to_virt(cpdpt[i3] & PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(cpd[i2] & PTE_PRESENT)) continue;
                pmm_free_page((void *)(cpd[i2] & PTE_ADDR_MASK));
            }
            pmm_free_page((void *)(cpdpt[i3] & PTE_ADDR_MASK));
        }
        pmm_free_page((void *)(child_pml4[i4] & PTE_ADDR_MASK));
    }
    pmm_free_page((void *)child_pml4_phys);
    return 0;
}

// ---- User-space pointer validation ----

bool is_user_address(uint64_t addr, size_t size) {
    if (size == 0) return true;
    // Check for overflow
    if (addr + size < addr) return false;
    // Must be entirely within the user canonical address range
    return (addr + size - 1) <= USER_ADDR_MAX;
}

int copy_from_user(void *kernel_dst, const void *user_src, size_t size) {
    if (!is_user_address((uint64_t)user_src, size)) return -1;
    memcpy(kernel_dst, user_src, size);
    return 0;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t size) {
    if (!is_user_address((uint64_t)user_dst, size)) return -1;
    memcpy(user_dst, kernel_src, size);
    return 0;
}

int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    if (!is_user_address((uint64_t)user_src, 1)) return -1;

    for (size_t i = 0; i < max_len - 1; i++) {
        // Validate each byte as we go (string might cross into kernel space)
        if (!is_user_address((uint64_t)&user_src[i], 1)) return -1;
        kernel_dst[i] = user_src[i];
        if (user_src[i] == '\0') return (int)i;
    }
    kernel_dst[max_len - 1] = '\0';
    return (int)(max_len - 1);
}
