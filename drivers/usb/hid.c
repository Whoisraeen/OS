#include "drivers/usb/usb.h"
#include "console.h"

// HID Class Requests
#define HID_GET_REPORT      0x01
#define HID_GET_IDLE        0x02
#define HID_GET_PROTOCOL    0x03
#define HID_SET_REPORT      0x09
#define HID_SET_IDLE        0x0A
#define HID_SET_PROTOCOL    0x0B

// Protocol Codes
#define HID_PROTOCOL_BOOT   0x00
#define HID_PROTOCOL_REPORT 0x01

// Boot Interface Subclass
#define HID_SUBCLASS_BOOT   0x01

// Protocol codes for Boot Subclass
#define HID_PROTOCOL_KBD    0x01
#define HID_PROTOCOL_MOUSE  0x02

void hid_init(void) {
    // Register with USB subsystem
    // usb_register_driver(USB_CLASS_HID, ...);
    // console_printf("[HID] Driver Initialized\n");
}

void hid_handle_keyboard_boot_report(uint8_t *report) {
    // Byte 0: Modifiers
    // Byte 1: Reserved
    // Byte 2-7: Keycodes
    
    uint8_t modifiers = report[0];
    for (int i = 2; i < 8; i++) {
        uint8_t key = report[i];
        if (key == 0) continue;
        
        // details: convert usage ID to scancode or ASCII
        // console_printf("[HID] Key: %d\n", key);
    }
}

void hid_handle_mouse_boot_report(uint8_t *report) {
    // Byte 0: Buttons
    // Byte 1: X displacement
    // Byte 2: Y displacement
    
    uint8_t buttons = report[0];
    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];
    
    // console_printf("[HID] Mouse: B=%x X=%d Y=%d\n", buttons, dx, dy);
}
