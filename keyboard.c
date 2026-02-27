#include "keyboard.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "shell.h"
#include "console.h"
#include "spinlock.h"
#include "sched.h"

// ── Raw scancode event buffer (for SYS_GET_INPUT_EVENT) ──────────────────────
#define KBD_BUF_SIZE 32
static uint32_t kbd_buf[KBD_BUF_SIZE];
static int kbd_head = 0;
static int kbd_tail = 0;

int get_keyboard_event(uint32_t *type, uint32_t *code) {
    if (kbd_head == kbd_tail) return 0;
    *type = 1;
    *code = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return 1;
}

// ── ASCII text buffer (for stdin read) ───────────────────────────────────────
#define KBD_ASCII_BUF 512
static uint8_t  kbd_ascii[KBD_ASCII_BUF];
static int      kbd_ascii_head = 0;
static int      kbd_ascii_tail = 0;
static spinlock_t kbd_lock = {0};
static task_t  *kbd_waiting = NULL;

// PS/2 Set-1 scancode → ASCII (normal and shifted)
static const uint8_t sc_normal[128] = {
    0,   0x1b,'1','2','3','4','5','6','7','8','9','0','-','=','\b',   // 0x00-0x0e
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',        // 0x0f-0x1c
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',            // 0x1d-0x29
    0,   '\\','z','x','c','v','b','n','m',',','.','/',0,' ',0,        // 0x2a-0x39 (+padding)
};
static const uint8_t sc_shift[128] = {
    0,   0x1b,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',0,' ',0,
};

static int kbd_shift = 0;
static int kbd_ctrl  = 0;

void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t sc = inb(0x60);

    // Raw event buffer
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = sc;
        kbd_head = next;
    }

    // Track modifier keys
    if (sc == 0x2A || sc == 0x36) { kbd_shift = 1; pic_send_eoi(1); return; }
    if (sc == 0xAA || sc == 0xB6) { kbd_shift = 0; pic_send_eoi(1); return; }
    if (sc == 0x1D)                { kbd_ctrl  = 1; pic_send_eoi(1); return; }
    if (sc == 0x9D)                { kbd_ctrl  = 0; pic_send_eoi(1); return; }
    if (sc & 0x80) { pic_send_eoi(1); return; } // Key release — ignore others

    // Decode ASCII
    uint8_t ch = 0;
    if (sc < 128) ch = kbd_shift ? sc_shift[sc] : sc_normal[sc];
    if (!ch) { pic_send_eoi(1); return; }

    // Ctrl+C → 0x03, Ctrl+D → 0x04, Ctrl+Z → 0x1A
    if (kbd_ctrl) {
        if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 'a' + 1);
        else if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch - 'A' + 1);
    }

    // Echo to console
    if (ch == '\b') {
        console_putc('\b'); console_putc(' '); console_putc('\b');
    } else {
        console_putc((char)ch);
    }

    // Push to ASCII buffer and wake any blocked reader
    spinlock_acquire(&kbd_lock);

    if (ch == '\b') {
        // Erase last character from line buffer
        if (kbd_ascii_head != kbd_ascii_tail) {
            kbd_ascii_head = (kbd_ascii_head - 1 + KBD_ASCII_BUF) % KBD_ASCII_BUF;
        }
    } else {
        int anext = (kbd_ascii_head + 1) % KBD_ASCII_BUF;
        if (anext != kbd_ascii_tail) {
            kbd_ascii[kbd_ascii_head] = ch;
            kbd_ascii_head = anext;
        }
    }

    task_t *waiter = kbd_waiting;
    kbd_waiting = NULL;
    spinlock_release(&kbd_lock);

    if (waiter) task_unblock(waiter);

    pic_send_eoi(1);
}

// Blocking line-buffered ASCII read (returns after newline or count bytes)
size_t keyboard_read_ascii(uint8_t *buf, size_t count) {
    if (!buf || count == 0) return 0;
    size_t n = 0;

    while (n < count) {
        spinlock_acquire(&kbd_lock);

        while (kbd_ascii_head != kbd_ascii_tail && n < count) {
            uint8_t c = kbd_ascii[kbd_ascii_tail];
            kbd_ascii_tail = (kbd_ascii_tail + 1) % KBD_ASCII_BUF;
            buf[n++] = c;
            if (c == '\n') {
                spinlock_release(&kbd_lock);
                return n;
            }
        }

        if (n > 0) { spinlock_release(&kbd_lock); return n; }

        // Block until data arrives
        kbd_waiting = task_get_by_id(task_current_id());
        spinlock_release(&kbd_lock);
        task_block();
        // Woken by ISR — retry
    }
    return n;
}

int keyboard_ascii_available(void) {
    return kbd_ascii_head != kbd_ascii_tail;
}

void keyboard_init(void) {
    outb(0x21, inb(0x21) & 0xFD); // Unmask IRQ1
}
