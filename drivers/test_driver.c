#include <stdint.h>
#include <stddef.h>

// Kernel functions we will use (will be resolved by loader)
extern void kprintf(const char *fmt, ...);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

void driver_entry(void) {
    kprintf("[TEST_DRIVER] Hello from a loaded kernel module!\n");
    
    void *mem = kmalloc(128);
    if (mem) {
        kprintf("[TEST_DRIVER] Successfully allocated 128 bytes at %p\n", mem);
        kfree(mem);
    } else {
        kprintf("[TEST_DRIVER] Allocation failed!\n");
    }
}
