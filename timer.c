#include "timer.h"
#include "io.h"
#include "serial.h"

// PIT ports
#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43

// Tick counter
static volatile uint64_t ticks = 0;

void timer_init(void) {
    // Calculate divisor for desired frequency
    uint16_t divisor = PIT_FREQUENCY / TIMER_HZ;
    
    // Set PIT to mode 3 (square wave), channel 0, lo/hi byte access
    // 0x36 = 00 11 011 0
    //        Channel 0, Access lo/hi, Mode 3, Binary
    outb(PIT_CMD, 0x36);
    
    // Send divisor (low byte first, then high byte)
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
    
    // Enable IRQ0 on PIC (unmask)
    // NOTE: This will cause interrupts to fire immediately if IF=1
    outb(0x21, inb(0x21) & 0xFE);
    
    kprintf("[TIMER] PIT initialized at %d Hz\n", TIMER_HZ);
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

void timer_sleep(uint32_t ms) {
    uint64_t target = ticks + (ms * TIMER_HZ / 1000);
    while (ticks < target) {
        __asm__ volatile ("hlt");
    }
}

void timer_tick(void) {
    ticks++;
}
