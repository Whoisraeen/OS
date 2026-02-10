#include "ksyms.h"
#include "string.h"
#include "serial.h"
#include "heap.h"
#include "io.h"

// I/O Wrappers for Export
uint8_t port_inb(uint16_t port) { return inb(port); }
void port_outb(uint16_t port, uint8_t val) { outb(port, val); }
uint16_t port_inw(uint16_t port) { return inw(port); }
void port_outw(uint16_t port, uint16_t val) { outw(port, val); }
uint32_t port_inl(uint16_t port) { return inl(port); }
void port_outl(uint16_t port, uint32_t val) { outl(port, val); }

// Simple fixed-size symbol table
#define MAX_KERNEL_SYMBOLS 1024

static kernel_symbol_t symbols[MAX_KERNEL_SYMBOLS];
static int symbol_count = 0;

void ksyms_init(void) {
    symbol_count = 0;
    // Registration happens in kernel.c or individual files
    kprintf("[KSYMS] Kernel symbol table initialized\n");
}

void ksyms_register(const char *name, void *address) {
    if (symbol_count >= MAX_KERNEL_SYMBOLS) {
        kprintf("[KSYMS] Error: Symbol table full\n");
        return;
    }
    
    // Check for duplicates
    for (int i = 0; i < symbol_count; i++) {
        if (strcmp(symbols[i].name, name) == 0) {
            kprintf("[KSYMS] Warning: Duplicate symbol '%s'\n", name);
            return;
        }
    }
    
    symbols[symbol_count].name = name;
    symbols[symbol_count].address = address;
    symbol_count++;
    
    // Debug
    // kprintf("[KSYMS] Exported '%s' at %p\n", name, address);
}

void *ksyms_lookup(const char *name) {
    for (int i = 0; i < symbol_count; i++) {
        if (strcmp(symbols[i].name, name) == 0) {
            return symbols[i].address;
        }
    }
    return NULL;
}
