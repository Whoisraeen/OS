#include "initrd.h"
#include "heap.h"
#include <stddef.h>

// USTAR tar header structure (512 bytes)
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];       // Octal string
    char mtime[12];
    char checksum[8];
    char typeflag;       // '0' = file, '5' = directory
    char linkname[100];
    char magic[6];       // "ustar\0"
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

// Maximum number of files in initrd
#define MAX_INITRD_FILES 64

// Initrd data
static uint8_t *initrd_base = NULL;
static size_t initrd_length = 0;
static vfs_node_t initrd_root;
static vfs_node_t initrd_files[MAX_INITRD_FILES];
static size_t initrd_file_count = 0;

// File content pointers (where the actual data is in the initrd)
static uint8_t *file_data[MAX_INITRD_FILES];
static size_t file_sizes[MAX_INITRD_FILES];

// Helper: parse octal string to integer
static size_t parse_octal(const char *str, size_t len) {
    size_t result = 0;
    for (size_t i = 0; i < len && str[i] != '\0' && str[i] != ' '; i++) {
        result = result * 8 + (str[i] - '0');
    }
    return result;
}

// Helper: string compare
static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

// Helper: string copy
static void str_copy(char *dest, const char *src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// Read callback for initrd files
static size_t initrd_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer) {
    size_t file_idx = (size_t)node->impl;
    
    if (offset >= file_sizes[file_idx]) {
        return 0;
    }
    
    size_t remaining = file_sizes[file_idx] - offset;
    if (size > remaining) {
        size = remaining;
    }
    
    uint8_t *src = file_data[file_idx] + offset;
    for (size_t i = 0; i < size; i++) {
        buffer[i] = src[i];
    }
    
    return size;
}

// Readdir callback for initrd root
static vfs_node_t *initrd_readdir(vfs_node_t *node, size_t index) {
    (void)node;
    if (index >= initrd_file_count) {
        return NULL;
    }
    return &initrd_files[index];
}

// Finddir callback for initrd root
static vfs_node_t *initrd_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    for (size_t i = 0; i < initrd_file_count; i++) {
        if (str_eq(initrd_files[i].name, name)) {
            return &initrd_files[i];
        }
    }
    return NULL;
}

vfs_node_t *initrd_init(void *initrd_start, size_t initrd_size) {
    initrd_base = (uint8_t *)initrd_start;
    initrd_length = initrd_size;
    initrd_file_count = 0;
    
    // Initialize root node
    str_copy(initrd_root.name, "/", 128);
    initrd_root.flags = VFS_DIRECTORY;
    initrd_root.length = 0;
    initrd_root.inode = 0;
    initrd_root.read = NULL;
    initrd_root.write = NULL;
    initrd_root.readdir = initrd_readdir;
    initrd_root.finddir = initrd_finddir;
    initrd_root.impl = NULL;
    
    // Parse tar archive
    uint8_t *ptr = initrd_base;
    uint8_t *end = initrd_base + initrd_length;
    
    while (ptr + 512 <= end && initrd_file_count < MAX_INITRD_FILES) {
        tar_header_t *header = (tar_header_t *)ptr;
        
        // Check for end of archive (empty header)
        if (header->name[0] == '\0') {
            break;
        }
        
        // Check magic (optional, some tars don't have it)
        // if (!str_eq(header->magic, "ustar")) break;
        
        // Parse file size
        size_t size = parse_octal(header->size, 12);
        
        // Skip to data (header is 512 bytes)
        uint8_t *data = ptr + 512;
        
        // Only handle regular files for now
        if (header->typeflag == '0' || header->typeflag == '\0') {
            vfs_node_t *file = &initrd_files[initrd_file_count];
            
            // Clean up name: remove "./" prefix if present
            const char *clean_name = header->name;
            if (clean_name[0] == '.' && clean_name[1] == '/') {
                clean_name += 2;
            }
            
            str_copy(file->name, clean_name, 128);
            file->flags = VFS_FILE;
            file->length = size;
            file->inode = initrd_file_count + 1;
            file->read = initrd_read;
            file->write = NULL;
            file->readdir = NULL;
            file->finddir = NULL;
            file->impl = (void *)initrd_file_count;
            
            file_data[initrd_file_count] = data;
            file_sizes[initrd_file_count] = size;
            
            initrd_file_count++;
        }
        
        // Move to next header (data is padded to 512-byte boundary)
        size_t data_blocks = (size + 511) / 512;
        ptr += 512 + data_blocks * 512;
    }
    
    return &initrd_root;
}

vfs_node_t *initrd_find(const char *path) {
    // Skip leading slash
    if (path[0] == '/') path++;
    
    return initrd_finddir(&initrd_root, path);
}
