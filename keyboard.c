#include "keyboard.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "shell.h"
#include "console.h"

// Forward declarations
// void pic_eoi(uint8_t irq); // Wrong name
// void shell_handle_key(char c); // Wrong name



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

    // Push raw scancode to buffer (Userspace will handle translation)
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = scancode;
        kbd_head = next;
    }
    
    pic_send_eoi(1);
}

void keyboard_init(void) {
    // Unmask IRQ1 (Keyboard) on PIC
    outb(0x21, inb(0x21) & 0xFD);
}
