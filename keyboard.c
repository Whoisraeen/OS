#include "keyboard.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "shell.h"
#include "console.h"

// Forward declarations
// void pic_eoi(uint8_t irq); // Wrong name
// void shell_handle_key(char c); // Wrong name

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

// Simple keyboard buffer
#define KBD_BUF_SIZE 32
static uint32_t kbd_buf[KBD_BUF_SIZE];
static int kbd_head = 0;
static int kbd_tail = 0;

int get_keyboard_event(uint32_t *type, uint32_t *code) {
    if (kbd_head == kbd_tail) return 0;
    
    *type = 1; // Keyboard
    *code = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return 1;
}

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

    if (scancode & 0x80) {
        // Key release
    } else {
        // Key press
        char c = shift_pressed ? kbdus_upper[scancode] : kbdus_lower[scancode];
        if (c) {
            console_putc(c); // Echo to debug console
            shell_input(c); // Send to shell
            
            // Push to buffer
            int next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {
                kbd_buf[kbd_head] = c;
                kbd_head = next;
            }
        }
    }
    
    pic_send_eoi(1);
}

void keyboard_init(void) {
    // Unmask IRQ1 (Keyboard) on PIC
    outb(0x21, inb(0x21) & 0xFD);
}
