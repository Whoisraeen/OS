#include "keyboard.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "shell.h"
#include "console.h"

// Shift state
static int shift_pressed = 0;

// Scancode Table (US QWERTY, Set 1) - lowercase
static unsigned char kbdus_lower[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
  '9', '0', '-', '=', '\b',
  '\t',
  'q', 'w', 'e', 'r',
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
 '\'', '`',   0,
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',
  'm', ',', '.', '/',   0,
  '*',
    0,
  ' ',
    0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,
    0,
    0,
    0,
    0,
    0,
  '-',
    0,
    0,
    0,
  '+',
    0,
    0,
    0,
    0,
    0,   0,   0,
    0,
    0,
    0,
};

// Scancode Table (US QWERTY, Set 1) - uppercase/shifted
static unsigned char kbdus_upper[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',
  '(', ')', '_', '+', '\b',
  '\t',
  'Q', 'W', 'E', 'R',
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
 '"', '~',   0,
 '|', 'Z', 'X', 'C', 'V', 'B', 'N',
  'M', '<', '>', '?',   0,
  '*',
    0,
  ' ',
    0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,
    0,
    0,
    0,
    0,
    0,
  '-',
    0,
    0,
    0,
  '+',
    0,
    0,
    0,
    0,
    0,   0,   0,
    0,
    0,
    0,
};

void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    // Read from keyboard port
    uint8_t scancode = inb(0x60);

    // Handle shift keys
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return;
    }
    
    // If the top bit is set, it's a "key release" event
    if (scancode & 0x80) {
        return;
    }
    
    // Key press - get character
    char c = shift_pressed ? kbdus_upper[scancode] : kbdus_lower[scancode];
    
    if (c != 0) {
        // Send to shell
        shell_input(c);
    }
}

void keyboard_init(void) {
    // Unmask IRQ1 (Keyboard) on PIC
    outb(0x21, inb(0x21) & 0xFD);
}
