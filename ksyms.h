#ifndef KSYMS_H
#define KSYMS_H

#include <stdint.h>

// Kernel Symbol Structure
typedef struct {
    const char *name;
    void *address;
} kernel_symbol_t;

// Initialize symbol table
void ksyms_init(void);

// Register a symbol
void ksyms_register(const char *name, void *address);

// Lookup a symbol by name
void *ksyms_lookup(const char *name);

// I/O Wrappers
uint8_t port_inb(uint16_t port);
void port_outb(uint16_t port, uint8_t val);
uint16_t port_inw(uint16_t port);
void port_outw(uint16_t port, uint16_t val);
uint32_t port_inl(uint16_t port);
void port_outl(uint16_t port, uint32_t val);

// Macro to export symbols (manual registration for now)
// usage: ksyms_register("function_name", function_name);

#endif
