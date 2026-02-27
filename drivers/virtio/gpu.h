#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>
#include "virtio.h"

// Virtio GPU Command IDs
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO 0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET 0x0109

// Virtio GPU Responses
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO 0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET 0x1103

// Common Header
typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr;

// Format constants
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68

// Command: Resource Create 2D
typedef struct {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d;

// Command: Set Scanout
typedef struct {
    virtio_gpu_ctrl_hdr hdr;
    struct {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
    } r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout;

// Command: Attach Backing
typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry;

typedef struct {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // Followed by nr_entries of virtio_gpu_mem_entry
} __attribute__((packed)) virtio_gpu_resource_attach_backing;

// Command: Transfer to Host 2D
typedef struct {
    virtio_gpu_ctrl_hdr hdr;
    struct {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
    } r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d;

// Command: Resource Flush
typedef struct {
    virtio_gpu_ctrl_hdr hdr;
    struct {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
    } r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush;

// Display Info Response
typedef struct {
    struct {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
    } r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one;

typedef struct {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_display_one pmodes[16];
} __attribute__((packed)) virtio_gpu_resp_display_info;

// Functions
void virtio_gpu_init(void);
void virtio_gpu_transfer(int x, int y, int w, int h);
void virtio_gpu_flush(int x, int y, int w, int h);

#endif
