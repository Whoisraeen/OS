#include "speaker.h"
#include "io.h"
#include "timer.h"

// PIT frequency for speaker
#define PIT_FREQUENCY 1193180

// Enable PC speaker with given frequency
static void speaker_on(uint32_t frequency) {
    if (frequency == 0) return;
    
    uint32_t divisor = PIT_FREQUENCY / frequency;
    
    // Set PIT channel 2 to square wave mode
    outb(0x43, 0xB6);
    
    // Set frequency divisor
    outb(0x42, divisor & 0xFF);
    outb(0x42, (divisor >> 8) & 0xFF);
    
    // Enable speaker (bits 0 and 1 of port 0x61)
    uint8_t temp = inb(0x61);
    if ((temp & 3) != 3) {
        outb(0x61, temp | 3);
    }
}

// Disable PC speaker
static void speaker_off(void) {
    uint8_t temp = inb(0x61) & 0xFC;
    outb(0x61, temp);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_on(frequency);
    timer_sleep(duration_ms);
    speaker_off();
}

void speaker_click(void) {
    speaker_beep(1000, 10);
}

void speaker_error(void) {
    speaker_beep(200, 100);
    timer_sleep(50);
    speaker_beep(150, 150);
}

void speaker_success(void) {
    speaker_beep(800, 50);
    timer_sleep(25);
    speaker_beep(1000, 50);
    timer_sleep(25);
    speaker_beep(1200, 100);
}
