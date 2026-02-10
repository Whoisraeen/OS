#ifndef U_STDLIB_H
#define U_STDLIB_H

#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "include/stdarg.h"

static inline void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static inline void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

static inline char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

static inline int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    size_t i = 0;
    while (*fmt && i < size - 1) {
        if (*fmt == '%') {
            fmt++;
            
            // Parse width and padding
            char pad = ' ';
            int width = 0;
            if (*fmt == '0') {
                pad = '0';
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
            
            if (*fmt == 's') {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && i < size - 1) buf[i++] = *s++;
            } else if (*fmt == 'd' || *fmt == 'u') {
                long n;
                if (*fmt == 'd') {
                    n = va_arg(args, int);
                } else {
                    n = va_arg(args, unsigned int);
                }
                
                if (n < 0 && *fmt == 'd') {
                    if (i < size - 1) buf[i++] = '-';
                    n = -n;
                }
                char tmp[20];
                int j = 0;
                if (n == 0) tmp[j++] = '0';
                while (n > 0) {
                    tmp[j++] = '0' + (n % 10);
                    n /= 10;
                }
                
                // Padding
                while (j < width && i < size - 1) {
                    // Check if we can prepend padding... complicated in single pass buffer fill
                    // Actually, we should fill tmp with padding?
                    // Or just output padding char before tmp content?
                    // We printed '-' already. Padding usually goes between sign and number for '0', or before sign for ' '.
                    // For simplicity, just pad 0s here (assuming %02d style)
                    if (pad == '0') {
                        tmp[j++] = '0';
                    } else {
                         // Space padding usually prepended. 
                         // But we are in "print tmp" phase.
                    }
                }
                
                while (j > 0 && i < size - 1) buf[i++] = tmp[--j];
            } else if (*fmt == 'x' || *fmt == 'X' || *fmt == 'p') {
                unsigned long n = (*fmt == 'p') ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                char tmp[20];
                int j = 0;
                if (n == 0) tmp[j++] = '0';
                while (n > 0) {
                    unsigned int digit = n % 16;
                    tmp[j++] = (digit < 10) ? ('0' + digit) : ((*fmt == 'x' || *fmt == 'p' ? 'a' : 'A') + digit - 10);
                    n /= 16;
                }
                while (j > 0 && i < size - 1) buf[i++] = tmp[--j];
            } else {
                buf[i++] = *fmt;
            }
        } else {
            buf[i++] = *fmt;
        }
        fmt++;
    }
    buf[i] = '\0';
    return (int)i;
}

static inline int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

#define stderr 2
#define stdout 1

static inline int fprintf(int fd, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    int ret = vsnprintf(buf, 512, fmt, args);
    va_end(args);
    syscall3(SYS_WRITE, fd, (long)buf, strlen(buf));
    return ret;
}

static inline int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    int ret = vsnprintf(buf, 512, fmt, args);
    va_end(args);
    syscall3(SYS_WRITE, stdout, (long)buf, strlen(buf));
    return ret;
}

// Simple Heap Allocator
#define HEAP_BLOCK_SIZE (1024 * 1024)

typedef struct mem_block {
    size_t size;
    int free;
    struct mem_block *next;
} mem_block_t;

static void *heap_start = NULL;
static mem_block_t *free_list = NULL;

static void heap_init() {
    long shmem_id = syscall2(SYS_IPC_SHMEM_CREATE, HEAP_BLOCK_SIZE, 0);
    if (shmem_id <= 0) return;
    heap_start = (void*)syscall1(SYS_IPC_SHMEM_MAP, shmem_id);
    if (!heap_start) return;
    free_list = (mem_block_t *)heap_start;
    free_list->size = HEAP_BLOCK_SIZE - sizeof(mem_block_t);
    free_list->free = 1;
    free_list->next = NULL;
}

static void *malloc(size_t size) {
    if (!heap_start) heap_init();
    if (!heap_start) return NULL;
    size = (size + 7) & ~7;
    mem_block_t *curr = free_list;
    while (curr) {
        if (curr->free && curr->size >= size) {
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
    return NULL;
}

static void free(void *ptr) {
    if (!ptr) return;
    mem_block_t *block = (mem_block_t *)((uint8_t*)ptr - sizeof(mem_block_t));
    block->free = 1;
    if (block->next && block->next->free) {
        block->size += sizeof(mem_block_t) + block->next->size;
        block->next = block->next->next;
    }
}

static inline void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    mem_block_t *block = (mem_block_t *)((uint8_t*)ptr - sizeof(mem_block_t));
    if (block->size >= size) return ptr;
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}

#endif // U_STDLIB_H