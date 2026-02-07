#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// PIT (Programmable Interval Timer) frequency
#define PIT_FREQUENCY 1193182
#define TIMER_HZ 100  // 100 Hz = 10ms per tick

// Initialize PIT timer
void timer_init(void);

// Get tick count since boot
uint64_t timer_get_ticks(void);

// Sleep for milliseconds (busy wait)
void timer_sleep(uint32_t ms);

// Timer tick handler (called from IRQ0)
void timer_tick(void);

#endif
