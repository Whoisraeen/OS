#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

// IOAPIC register offsets (indirect access via IOREGSEL/IOWIN)
#define IOAPIC_REGSEL   0x00
#define IOAPIC_IOWIN    0x10

// IOAPIC registers
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDTBL   0x10  // Redirection table base (entries at 0x10 + 2*n)

// Redirection entry flags
#define IOAPIC_FIXED        0x00000000
#define IOAPIC_LOWEST       0x00000100
#define IOAPIC_PHYSICAL     0x00000000
#define IOAPIC_LOGICAL      0x00000800
#define IOAPIC_IDLE         0x00000000
#define IOAPIC_PENDING      0x00001000
#define IOAPIC_ACTIVE_HIGH  0x00000000
#define IOAPIC_ACTIVE_LOW   0x00002000
#define IOAPIC_EDGE         0x00000000
#define IOAPIC_LEVEL        0x00008000
#define IOAPIC_MASKED       0x00010000

// Initialize IOAPIC (call after acpi_init)
void ioapic_init(void);

// Route a GSI (Global System Interrupt) to a specific vector on a target LAPIC
void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t dest_lapic, uint32_t flags);

// Mask/unmask a GSI
void ioapic_mask(uint8_t gsi);
void ioapic_unmask(uint8_t gsi);

// Get the GSI for a legacy ISA IRQ (applies IRQ source overrides from MADT)
uint32_t ioapic_get_gsi_for_irq(uint8_t irq);

// Get the flags (polarity/trigger) for a legacy ISA IRQ override
uint16_t ioapic_get_flags_for_irq(uint8_t irq);

#endif
