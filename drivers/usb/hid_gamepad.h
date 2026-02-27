#ifndef HID_GAMEPAD_H
#define HID_GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>

// ── Gamepad button bitmask ────────────────────────────────────────────────────
#define GP_BTN_CROSS     (1u << 0)   // A / Cross
#define GP_BTN_CIRCLE    (1u << 1)   // B / Circle
#define GP_BTN_SQUARE    (1u << 2)   // X / Square
#define GP_BTN_TRIANGLE  (1u << 3)   // Y / Triangle
#define GP_BTN_L1        (1u << 4)
#define GP_BTN_R1        (1u << 5)
#define GP_BTN_L2        (1u << 6)
#define GP_BTN_R2        (1u << 7)
#define GP_BTN_SELECT    (1u << 8)
#define GP_BTN_START     (1u << 9)
#define GP_BTN_L3        (1u << 10)  // Left stick click
#define GP_BTN_R3        (1u << 11)  // Right stick click
#define GP_BTN_DPAD_UP   (1u << 12)
#define GP_BTN_DPAD_DOWN (1u << 13)
#define GP_BTN_DPAD_LEFT (1u << 14)
#define GP_BTN_DPAD_RIGHT (1u << 15)
#define GP_BTN_HOME      (1u << 16)  // Guide / PS button

// ── Gamepad state ─────────────────────────────────────────────────────────────
typedef struct {
    int16_t  lx, ly;    // Left stick  (-32768 .. 32767)
    int16_t  rx, ry;    // Right stick (-32768 .. 32767)
    int16_t  lt, rt;    // Triggers    (0 .. 32767)
    uint32_t buttons;   // GP_BTN_* bitmask
    bool     connected;
} gamepad_state_t;

#define MAX_GAMEPADS 4

// Called from kernel / syscall layer
void hid_gamepad_init(void);
int  gamepad_get_state(int index, gamepad_state_t *out);

// Called from xHCI driver
void hid_gamepad_connected(uint8_t slot_id, uint16_t vid, uint16_t pid);
void hid_gamepad_handle_report(const uint8_t *data, int len);

// Called from syscall layer
int gamepad_get_event(int *gp_idx, gamepad_state_t *state);

// Input event type for SYS_GET_INPUT_EVENT
#define INPUT_TYPE_GAMEPAD  3

#endif // HID_GAMEPAD_H
