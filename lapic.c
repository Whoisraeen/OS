#include "lapic.h"
#include "io.h"
#include "serial.h"
#include "vmm.h"
#include "timer.h"

// MSR for APIC Base
#define MSR_APIC_BASE 0x1B

// Offsets
#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

static uint64_t lapic_base_addr = 0;
static uint32_t lapic_ticks_per_10ms = 0;  // Calibrated value
static int ioapic_mode = 0;

// Read a value from LAPIC register
static uint32_t lapic_read(uint32_t reg) {
    if (lapic_base_addr == 0) return 0;
    return *(volatile uint32_t *)(lapic_base_addr + reg);
}

// Write a value to LAPIC register
static void lapic_write(uint32_t reg, uint32_t value) {
    if (lapic_base_addr == 0) return;
    *(volatile uint32_t *)(lapic_base_addr + reg) = value;
}

void lapic_init(void) {
    // 1. Get LAPIC Base from MSR
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);

    // Physical address is in bits 12-51
    uint64_t phys_base = apic_msr & 0xFFFFF000;

    if (lapic_base_addr == 0) {
        lapic_base_addr = phys_base + vmm_get_hhdm_offset();

        // Map it explicitly to ensure it's accessible and uncached
        // 0x13 = Present | Writable | NoCache (Bit 4)
        vmm_map_page(lapic_base_addr, phys_base, 0x13);

        kprintf("[LAPIC] Mapped 0x%lx -> 0x%lx\n", phys_base, lapic_base_addr);
    }

    // 2. Enable LAPIC
    // Set Spurious Interrupt Vector to 0xFF (255) and bit 8 (Enable)
    lapic_write(LAPIC_SPURIOUS, 0x1FF);

    // 3. Setup Timer — start masked until calibrated
    lapic_write(LAPIC_TIMER_DIV, 0x3); // Divide by 16
    lapic_write(LAPIC_LVT_TIMER, 0x10020); // Masked, periodic, vector 32

    kprintf("[LAPIC] Initialized on CPU. Base: 0x%lx\n", lapic_base_addr);
}

void lapic_timer_calibrate(void) {
    // Use PIT to measure LAPIC timer frequency
    // Strategy: set LAPIC counter to max, wait ~10ms using PIT, read remaining count

    kprintf("[LAPIC] Calibrating timer...\n");

    // Set divider to 16
    lapic_write(LAPIC_TIMER_DIV, 0x3);

    // Set initial count to max
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // LVT: one-shot, masked, vector 32
    lapic_write(LAPIC_LVT_TIMER, 0x10020);

    // Wait for one PIT tick (10ms at 100Hz)
    uint64_t start_tick = timer_get_ticks();
    while (timer_get_ticks() == start_tick);
    // Now we're at the beginning of a fresh tick
    start_tick = timer_get_ticks();
    while (timer_get_ticks() == start_tick);
    // One full PIT period (10ms) has passed

    // Read remaining LAPIC count
    uint32_t remaining = lapic_read(LAPIC_TIMER_CUR);
    uint32_t elapsed = 0xFFFFFFFF - remaining;

    lapic_ticks_per_10ms = elapsed;

    // Stop the timer
    lapic_write(LAPIC_TIMER_INIT, 0);

    kprintf("[LAPIC] Calibration: %d ticks per 10ms (%d kHz bus)\n",
            elapsed, elapsed / 10);
}

void lapic_timer_start(void) {
    if (lapic_ticks_per_10ms == 0) {
        kprintf("[LAPIC] Timer not calibrated!\n");
        return;
    }

    // Set divider to 16
    lapic_write(LAPIC_TIMER_DIV, 0x3);

    // Set initial count for ~100Hz (10ms period)
    lapic_write(LAPIC_TIMER_INIT, lapic_ticks_per_10ms);

    // LVT: periodic, unmasked, vector 32
    lapic_write(LAPIC_LVT_TIMER, 0x20020);

    kprintf("[LAPIC] Timer started: periodic at ~100Hz (init=%d)\n",
            lapic_ticks_per_10ms);
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void) {
    return (lapic_read(LAPIC_ID) >> 24);
}

int lapic_is_ioapic_mode(void) {
    return ioapic_mode;
}

void lapic_set_ioapic_mode(int enabled) {
    ioapic_mode = enabled;
}
