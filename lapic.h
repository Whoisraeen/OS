#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

// Initialize Local APIC for the current CPU
void lapic_init(void);

// Acknowledge interrupt (EOI)
void lapic_eoi(void);

// Get LAPIC ID
uint32_t lapic_id(void);

#endif
