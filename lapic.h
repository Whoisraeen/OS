#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

// Initialize Local APIC for the current CPU
void lapic_init(void);

// Acknowledge interrupt (EOI)
void lapic_eoi(void);

// Get LAPIC ID
uint32_t lapic_id(void);

// Calibrate and start LAPIC timer on this CPU
// Call after IOAPIC init (PIT IRQ must still work for calibration)
void lapic_timer_calibrate(void);

// Start LAPIC timer in periodic mode at ~100Hz
void lapic_timer_start(void);

// Check if IOAPIC is active (use LAPIC EOI instead of PIC EOI)
int lapic_is_ioapic_mode(void);

// Set IOAPIC mode flag
void lapic_set_ioapic_mode(int enabled);

#endif
