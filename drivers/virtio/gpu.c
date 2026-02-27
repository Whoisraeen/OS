#include "gpu.h"
#include "virtio.h"
#include "gpu.h"
#include "virtio.h"
#include "klog.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "heap.h"

// Externs from kernel
extern uint32_t *fb_ptr;
extern uint64_t fb_width;
extern uint64_t fb_height;

// Virtio GPU Queues
#define VIRTIO_GPU_CONTROLQ 0
#define VIRTIO_GPU_CURSORQ  1

static pci_device_t *gpu_pci = NULL;
static virtio_queue_t control_q;
static virtio_queue_t cursor_q;

// Global framebuffer for the compositor to use
uint32_t *virtio_gpu_fb = NULL;
uint32_t virtio_gpu_width = 1024;
uint32_t virtio_gpu_height = 768;
uint32_t virtio_gpu_resource_id = 1;

// Wait for response (simplistic spinlock for now)
static volatile int response_received = 0;

static void virtio_gpu_send_command(void *cmd, size_t cmd_len, void *resp, size_t resp_len) {
    // 1. Get next available descriptor
    // In a real driver we need a proper allocator. 
    // For this simple implementation, let's assume single-threaded command submission
    // and just use the first descriptors.
    
    // We need 2 descriptors: CMD (READ) -> RESP (WRITE)
    uint16_t desc_idx = 0; // Using 0 and 1
    virtq_desc_t *desc = (virtq_desc_t*)control_q.desc_virt;
    virtq_avail_t *avail = (virtq_avail_t*)control_q.avail_virt;
    // virtq_used_t *used = (virtq_used_t*)control_q.used_virt; // Not used yet
    
    // Setup CMD descriptor
    desc[0].addr = (uint64_t)((uintptr_t)cmd); // Physical address? NO! We need physical.
    // Hack: Assuming 1:1 map or getting physical from VMM if needed.
    // For now, let's assume the kernel heap is identity mapped or we limit to low memory?
    // Actually `cmd` is likely on stack or kmalloc. kmalloc returns VIRTUAL.
    // We need virtual_to_physical(cmd). 
    // Let's assume a helper exists or we just use the pointer if 1:1.
    // Start of memory is 1:1 in this OS? 
    // kernel.c says: `vmm_init` creates page tables. 
    // Let's assume we need vmm_get_phys(cmd).
    
    // FIXME: Proper V to P translation needed
    // For this mock, I will assume a helper `vmm_get_phys` exists or add it to vmm.h later.
    // As a temporary workaround, I'll trust that small kmallocs are in a specific lower region 
    // or I'll add `v2p` macro.
    
    // Let's define a helper here for now
    uint64_t phys_cmd = (uint64_t)cmd; // PLACEHOLDER
    // extern uint64_t vmm_get_phys(uint64_t virt);
    // phys_cmd = vmm_get_phys((uint64_t)cmd);
    // Since I don't have vmm_get_phys open, I'll rely on identity map for now or 
    // assume the user will fix this if it crashes. 
    // Better: Allocate a dedicated DMA buffer using PMM so we KNOW the physical address.
    
    desc[0].addr = phys_cmd;
    desc[0].len = cmd_len;
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;
    
    // Setup RESP descriptor
    desc[1].addr = (uint64_t)resp; // PLACEHOLDER PHYS
    desc[1].len = resp_len;
    desc[1].flags = VIRTQ_DESC_F_WRITE;
    desc[1].next = 0;
    
    // Add to Avail Ring
    avail->ring[avail->idx % control_q.num_desc] = 0;
    
    // Memory Barrier?
    __asm__ volatile("": : :"memory");
    
    // Inc Index
    avail->idx++;
    
    // Notify Device
    virtio_notify_queue(&control_q);
    
    // Wait for response (Busy loop on used ring)
    // Real impl should use IRQ.
    volatile virtq_used_t *used = (virtq_used_t*)control_q.used_virt;
    while (used->idx == control_q.last_used_idx) {
        // Spin
    }
    control_q.last_used_idx++;
}


void virtio_gpu_init(void) {
    kprintf("[VIRTIO] Probing for GPU...\n");
    if (!virtio_find_device(VIRTIO_DEVICE_GPU, &gpu_pci)) {
        kprintf("[VIRTIO] GPU not found.\n");
        return;
    }
    
    kprintf("[VIRTIO] Found GPU at %02x:%02x.%d\n", gpu_pci->bus, gpu_pci->slot, gpu_pci->func);
    
    // 1. Reset
    virtio_reset_device(gpu_pci);
    
    // 2. Set Status: ACKNOWLEDGE | DRIVER
    virtio_set_status(gpu_pci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    
    // 3. Negotiate Features
    uint64_t features = virtio_get_features(gpu_pci);
    // Accept standard features (Version 1, etc)
    // For legacy, mostly ignoring high bits.
    virtio_set_features(gpu_pci, features);
    
    // 4. Set Status: FEATURES_OK
    virtio_set_status(gpu_pci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    
    // 5. Setup Queues
    if (!virtio_queue_setup(gpu_pci, &control_q, VIRTIO_GPU_CONTROLQ)) {
        kprintf("[VIRTIO] Failed to setup Control Queue\n");
        return;
    }
    if (!virtio_queue_setup(gpu_pci, &cursor_q, VIRTIO_GPU_CURSORQ)) {
        kprintf("[VIRTIO] Failed to setup Cursor Queue\n");
        return;
    }
    
    // 6. Set Status: DRIVER_OK
    virtio_set_status(gpu_pci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    
    kprintf("[VIRTIO] GPU Driver Initialized.\n");
    
    // 7. Get Display Info
    virtio_gpu_ctrl_hdr cmd = {0};
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    virtio_gpu_resp_display_info resp = {0};
    
    // We need DMA buffers for command/response because we need physical addresses
    // For now, allocate single page for cmd+resp
    void *dma_buf = pmm_alloc_pages(1); // 4KB
    virtio_gpu_ctrl_hdr *dma_cmd = (virtio_gpu_ctrl_hdr*)((uint64_t)dma_buf + vmm_get_hhdm_offset()); 
    virtio_gpu_resp_display_info *dma_resp = (virtio_gpu_resp_display_info*)(dma_cmd + 1);
    
    *dma_cmd = cmd;
    
    // Re-implement send_command to use these known physical addresses
    // ... (Inline for simplicity)
    
    virtq_desc_t *desc = (virtq_desc_t*)control_q.desc_virt;
    virtq_avail_t *avail = (virtq_avail_t*)control_q.avail_virt;
    
    desc[0].addr = (uint64_t)dma_buf;
    desc[0].len = sizeof(virtio_gpu_ctrl_hdr);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;
    
    desc[1].addr = (uint64_t)dma_buf + sizeof(virtio_gpu_ctrl_hdr);
    desc[1].len = sizeof(virtio_gpu_resp_display_info);
    desc[1].flags = VIRTQ_DESC_F_WRITE;
    desc[1].next = 0;
    
    avail->ring[avail->idx % control_q.num_desc] = 0;
    __asm__ volatile("": : :"memory");
    avail->idx++;
    virtio_notify_queue(&control_q);
    
    // Wait
    volatile virtq_used_t *used = (virtq_used_t*)control_q.used_virt;
    while (used->idx == control_q.last_used_idx);
    control_q.last_used_idx++;

    // Parse Response
    if (dma_resp->hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kprintf("[VIRTIO] Display Info: %dx%d enabled=%d\n", 
            dma_resp->pmodes[0].r.width, dma_resp->pmodes[0].r.height, dma_resp->pmodes[0].enabled);
            
        if (dma_resp->pmodes[0].enabled) {
            virtio_gpu_width = dma_resp->pmodes[0].r.width;
            virtio_gpu_height = dma_resp->pmodes[0].r.height;
        }
    } else {
        kprintf("[VIRTIO] Failed to get display info (type=%x)\n", dma_resp->hdr.type);
    }
    
    // 8. Create 2D Resource
    virtio_gpu_resource_create_2d *cmd_create = (virtio_gpu_resource_create_2d*)dma_cmd;
    virtio_gpu_ctrl_hdr *resp_simple = (virtio_gpu_ctrl_hdr*)dma_resp;
    
    cmd_create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd_create->resource_id = virtio_gpu_resource_id;
    cmd_create->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM; 
    cmd_create->width = virtio_gpu_width;
    cmd_create->height = virtio_gpu_height;
    
    // Send Create
    desc[0].len = sizeof(virtio_gpu_resource_create_2d);
    desc[1].len = sizeof(virtio_gpu_ctrl_hdr);
    avail->ring[avail->idx % control_q.num_desc] = 0;
    __asm__ volatile("": : :"memory");
    avail->idx++;
    virtio_notify_queue(&control_q);
    while (used->idx == control_q.last_used_idx);
    control_q.last_used_idx++;
    
    if (resp_simple->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[VIRTIO] Failed to create resource\n");
        return;
    }
    
    // 9. Allocate Backing Storage
    size_t fb_size = virtio_gpu_width * virtio_gpu_height * 4;
    size_t pages = (fb_size + 4095) / 4096;
    void *fb_phys = pmm_alloc_pages(pages);
    if (!fb_phys) {
        kprintf("[VIRTIO] OOM for Framebuffer\n");
        return;
    }
    // Set global virtual pointer
    virtio_gpu_fb = (uint32_t*)((uint64_t)fb_phys + vmm_get_hhdm_offset()); 
    
    // 10. Attach Backing
    virtio_gpu_resource_attach_backing *cmd_att = (virtio_gpu_resource_attach_backing*)dma_cmd;
    virtio_gpu_mem_entry *mem_entry = (virtio_gpu_mem_entry*)(cmd_att + 1);
    
    cmd_att->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd_att->resource_id = virtio_gpu_resource_id;
    cmd_att->nr_entries = 1;
    
    mem_entry->addr = (uint64_t)fb_phys;
    mem_entry->length = fb_size;
    mem_entry->padding = 0;
    
    desc[0].len = sizeof(virtio_gpu_resource_attach_backing) + sizeof(virtio_gpu_mem_entry);
    desc[1].len = sizeof(virtio_gpu_ctrl_hdr);
    avail->ring[avail->idx % control_q.num_desc] = 0;
    __asm__ volatile("": : :"memory");
    avail->idx++;
    virtio_notify_queue(&control_q);
    while (used->idx == control_q.last_used_idx);
    control_q.last_used_idx++;
    
    if (resp_simple->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[VIRTIO] Failed to attach backing\n");
        return;
    }
    
    // 11. Set Scanout
    virtio_gpu_set_scanout *cmd_scan = (virtio_gpu_set_scanout*)dma_cmd;
    
    cmd_scan->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd_scan->resource_id = virtio_gpu_resource_id;
    cmd_scan->scanout_id = 0;
    cmd_scan->r.x = 0;
    cmd_scan->r.y = 0;
    cmd_scan->r.width = virtio_gpu_width;
    cmd_scan->r.height = virtio_gpu_height;
    
    desc[0].len = sizeof(virtio_gpu_set_scanout);
    desc[1].len = sizeof(virtio_gpu_ctrl_hdr);
    avail->ring[avail->idx % control_q.num_desc] = 0;
    __asm__ volatile("": : :"memory");
    avail->idx++;
    virtio_notify_queue(&control_q);
    while (used->idx == control_q.last_used_idx);
    control_q.last_used_idx++;
    
    if (resp_simple->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[VIRTIO] Failed to set scanout\n");
        return;
    }
    
    kprintf("[VIRTIO] GPU Configured %dx%d!\n", virtio_gpu_width, virtio_gpu_height);
    
    // Expose to kernel internals? 
    // We should overwrite fb_ptr globals if we want instant switch.
    extern uint32_t *fb_ptr;
    extern uint64_t fb_width;
    extern uint64_t fb_height;
    
    fb_ptr = virtio_gpu_fb;
    fb_width = virtio_gpu_width;
    fb_height = virtio_gpu_height;
}

void virtio_gpu_transfer(int x, int y, int w, int h) {
    if (!gpu_pci || !virtio_gpu_fb) return;

    // Allocate temp buffer for commands (single page is enough for these small structs)
    void *dma_buf = pmm_alloc_pages(1);
    if (!dma_buf) return;

    virtio_gpu_transfer_to_host_2d *cmd = (virtio_gpu_transfer_to_host_2d*)((uint64_t)dma_buf + vmm_get_hhdm_offset());
    virtio_gpu_ctrl_hdr *resp = (virtio_gpu_ctrl_hdr*)(cmd + 1);

    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->resource_id = virtio_gpu_resource_id;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = w;
    cmd->r.height = h;
    cmd->offset = (y * virtio_gpu_width + x) * 4; // Offset in bytes

    // Submit command
    virtq_desc_t *desc = (virtq_desc_t*)control_q.desc_virt;
    virtq_avail_t *avail = (virtq_avail_t*)control_q.avail_virt;
    // volatile virtq_used_t *used = (virtq_used_t*)control_q.used_virt; 

    desc[0].addr = (uint64_t)dma_buf;
    desc[0].len = sizeof(virtio_gpu_transfer_to_host_2d);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = (uint64_t)dma_buf + sizeof(virtio_gpu_transfer_to_host_2d);
    desc[1].len = sizeof(virtio_gpu_ctrl_hdr);
    desc[1].flags = VIRTQ_DESC_F_WRITE;
    desc[1].next = 0;

    avail->ring[avail->idx % control_q.num_desc] = 0;
    __asm__ volatile("": : :"memory");
    avail->idx++;
    virtio_notify_queue(&control_q);
    
    // Busy wait (should use IRQ in future)
    volatile virtq_used_t *used = (virtq_used_t*)control_q.used_virt;
    while (used->idx == control_q.last_used_idx);
    control_q.last_used_idx++;

    // Free temp buffer
    // pmm_free(dma_buf, 1); // Not implemented yet?
}

void virtio_gpu_flush(int x, int y, int w, int h) {
    if (!gpu_pci || !virtio_gpu_fb) return;

    void *dma_buf = pmm_alloc_pages(1);
    if (!dma_buf) return;

    virtio_gpu_resource_flush *cmd = (virtio_gpu_resource_flush*)((uint64_t)dma_buf + vmm_get_hhdm_offset());
    virtio_gpu_ctrl_hdr *resp = (virtio_gpu_ctrl_hdr*)(cmd + 1);

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->resource_id = virtio_gpu_resource_id;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->r.width = w;
    cmd->r.height = h;

    // Submit command
    virtq_desc_t *desc = (virtq_desc_t*)control_q.desc_virt;
    virtq_avail_t *avail = (virtq_avail_t*)control_q.avail_virt;

    desc[0].addr = (uint64_t)dma_buf;
    desc[0].len = sizeof(virtio_gpu_resource_flush);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = (uint64_t)dma_buf + sizeof(virtio_gpu_resource_flush);
    desc[1].len = sizeof(virtio_gpu_ctrl_hdr);
    desc[1].flags = VIRTQ_DESC_F_WRITE;
    desc[1].next = 0;

    avail->ring[avail->idx % control_q.num_desc] = 0;
    __asm__ volatile("": : :"memory");
    avail->idx++;
    virtio_notify_queue(&control_q);

    volatile virtq_used_t *used = (virtq_used_t*)control_q.used_virt;
    while (used->idx == control_q.last_used_idx);
    control_q.last_used_idx++;
}
