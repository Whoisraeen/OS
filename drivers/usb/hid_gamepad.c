// HID Gamepad Driver
// Supports:
//   - DualShock 4  (VID 054C, PID 09CC / 05C4)
//   - DualSense    (VID 054C, PID 0CE6)
//   - Generic HID gamepad (standard Joystick report, 8-byte boot-like layout)
//
// Reports are fed in by the xHCI driver via hid_gamepad_handle_report().
// Userspace reads via SYS_GET_INPUT_EVENT (type=INPUT_TYPE_GAMEPAD).

#include <stdint.h>
#include <stdbool.h>
#include "string.h"
#include "console.h"
#include "serial.h"
#include "drivers/usb/hid_gamepad.h"

// ── Known gamepad VID/PID ─────────────────────────────────────────────────────
#define VID_SONY         0x054C
#define PID_DS4_V1       0x05C4
#define PID_DS4_V2       0x09CC
#define PID_DUALSENSE    0x0CE6

typedef enum {
    GP_PROTO_GENERIC,    // Generic 8-byte HID joystick
    GP_PROTO_DS4,        // DualShock 4
    GP_PROTO_DUALSENSE,  // DualSense
} gp_proto_t;

// ── Per-gamepad slot ──────────────────────────────────────────────────────────
typedef struct {
    bool       active;
    uint8_t    xhci_slot;
    gp_proto_t proto;
    gamepad_state_t state;
} gp_slot_t;

static gp_slot_t gp_slots[MAX_GAMEPADS];

// ── Event queue (ring buffer) ─────────────────────────────────────────────────
// Each entry encodes a full gamepad state snapshot.
#define GP_QUEUE_SIZE 16

typedef struct {
    int             gp_index;
    gamepad_state_t state;
} gp_event_t;

static gp_event_t gp_queue[GP_QUEUE_SIZE];
static volatile int gp_q_head = 0;
static volatile int gp_q_tail = 0;

static void gp_queue_push(int idx, const gamepad_state_t *s) {
    int next = (gp_q_tail + 1) % GP_QUEUE_SIZE;
    if (next == gp_q_head) return; // drop if full
    gp_queue[gp_q_tail].gp_index = idx;
    gp_queue[gp_q_tail].state    = *s;
    gp_q_tail = next;
}

// Returns true and fills *evt if an event is available.
static bool gp_queue_pop(gp_event_t *evt) {
    if (gp_q_head == gp_q_tail) return false;
    *evt     = gp_queue[gp_q_head];
    gp_q_head = (gp_q_head + 1) % GP_QUEUE_SIZE;
    return true;
}

// ── Axis scaling: byte (0–255) → int16_t centered ────────────────────────────
static inline int16_t scale_axis_byte(uint8_t v) {
    return (int16_t)((int32_t)v * 257 - 32768);
}

// ── Generic HID joystick report parser ───────────────────────────────────────
// Assumes the common 8-byte layout used by many cheap USB gamepads:
//   Byte 0: LX  (0-255)
//   Byte 1: LY  (0-255, up = 0)
//   Byte 2: RX  (0-255)
//   Byte 3: RY  (0-255)
//   Byte 4-5: Buttons (16 bits, various layouts)
//   Byte 6: D-pad hat switch (0=up, 2=right, 4=down, 6=left, 0xF=none)
//   Byte 7: Misc buttons
static void parse_generic(gp_slot_t *gp, const uint8_t *d, int len) {
    if (len < 6) return;
    gamepad_state_t *s = &gp->state;

    s->lx = scale_axis_byte(d[0]);
    s->ly = scale_axis_byte(d[1]);
    s->rx = scale_axis_byte(d[2]);
    s->ry = scale_axis_byte(d[3]);
    s->lt = 0;
    s->rt = 0;

    uint32_t raw_btns = (uint32_t)d[4] | ((uint32_t)d[5] << 8);
    s->buttons = 0;
    if (raw_btns & (1u << 0))  s->buttons |= GP_BTN_CROSS;
    if (raw_btns & (1u << 1))  s->buttons |= GP_BTN_CIRCLE;
    if (raw_btns & (1u << 2))  s->buttons |= GP_BTN_SQUARE;
    if (raw_btns & (1u << 3))  s->buttons |= GP_BTN_TRIANGLE;
    if (raw_btns & (1u << 4))  s->buttons |= GP_BTN_L1;
    if (raw_btns & (1u << 5))  s->buttons |= GP_BTN_R1;
    if (raw_btns & (1u << 6))  s->buttons |= GP_BTN_SELECT;
    if (raw_btns & (1u << 7))  s->buttons |= GP_BTN_START;
    if (raw_btns & (1u << 8))  s->buttons |= GP_BTN_L3;
    if (raw_btns & (1u << 9))  s->buttons |= GP_BTN_R3;

    // D-pad hat (if present)
    if (len > 6) {
        uint8_t hat = d[6] & 0xF;
        if (hat == 0 || hat == 1 || hat == 7) s->buttons |= GP_BTN_DPAD_UP;
        if (hat == 1 || hat == 2 || hat == 3) s->buttons |= GP_BTN_DPAD_RIGHT;
        if (hat == 3 || hat == 4 || hat == 5) s->buttons |= GP_BTN_DPAD_DOWN;
        if (hat == 5 || hat == 6 || hat == 7) s->buttons |= GP_BTN_DPAD_LEFT;
    }
}

// ── DualShock 4 report parser ─────────────────────────────────────────────────
// USB HID report (no Report ID byte):
//   0: LX  1: LY  2: RX  3: RY
//   4: D-pad(3:0) + buttons(7:4)
//   5: buttons  6: buttons
//   7: L2 analog  8: R2 analog
static void parse_ds4(gp_slot_t *gp, const uint8_t *d, int len) {
    if (len < 9) return;
    gamepad_state_t *s = &gp->state;

    s->lx = scale_axis_byte(d[0]);
    s->ly = scale_axis_byte(d[1]);
    s->rx = scale_axis_byte(d[2]);
    s->ry = scale_axis_byte(d[3]);
    s->lt = (int16_t)((int32_t)d[7] * 128);
    s->rt = (int16_t)((int32_t)d[8] * 128);

    s->buttons = 0;
    uint8_t dpad = d[4] & 0x0F;
    if (dpad == 0 || dpad == 1 || dpad == 7) s->buttons |= GP_BTN_DPAD_UP;
    if (dpad == 1 || dpad == 2 || dpad == 3) s->buttons |= GP_BTN_DPAD_RIGHT;
    if (dpad == 3 || dpad == 4 || dpad == 5) s->buttons |= GP_BTN_DPAD_DOWN;
    if (dpad == 5 || dpad == 6 || dpad == 7) s->buttons |= GP_BTN_DPAD_LEFT;

    if (d[4] & 0x10) s->buttons |= GP_BTN_SQUARE;
    if (d[4] & 0x20) s->buttons |= GP_BTN_CROSS;
    if (d[4] & 0x40) s->buttons |= GP_BTN_CIRCLE;
    if (d[4] & 0x80) s->buttons |= GP_BTN_TRIANGLE;

    if (d[5] & 0x01) s->buttons |= GP_BTN_L1;
    if (d[5] & 0x02) s->buttons |= GP_BTN_R1;
    if (d[5] & 0x04) s->buttons |= GP_BTN_L2;
    if (d[5] & 0x08) s->buttons |= GP_BTN_R2;
    if (d[5] & 0x10) s->buttons |= GP_BTN_SELECT; // Share
    if (d[5] & 0x20) s->buttons |= GP_BTN_START;  // Options
    if (d[5] & 0x40) s->buttons |= GP_BTN_L3;
    if (d[5] & 0x80) s->buttons |= GP_BTN_R3;

    if (d[6] & 0x01) s->buttons |= GP_BTN_HOME;   // PS button
}

// ── DualSense report parser ───────────────────────────────────────────────────
// USB report (no Report ID, same layout as DS4 for the first 9 bytes):
//   0: LX  1: LY  2: RX  3: RY  4-6: buttons (same as DS4)  7: L2  8: R2
static void parse_dualsense(gp_slot_t *gp, const uint8_t *d, int len) {
    // DualSense USB HID report matches DS4 layout for the basic axes/buttons
    parse_ds4(gp, d, len);
}

// ── Public API ────────────────────────────────────────────────────────────────
void hid_gamepad_init(void) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        gp_slots[i].active = false;
    }
    gp_q_head = gp_q_tail = 0;
    kprintf("[GAMEPAD] Driver initialized.\n");
}

void hid_gamepad_connected(uint8_t slot_id, uint16_t vid, uint16_t pid) {
    // Find a free gamepad slot
    int idx = -1;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (!gp_slots[i].active) { idx = i; break; }
    }
    if (idx < 0) { kprintf("[GAMEPAD] No free slots.\n"); return; }

    gp_slot_t *gp = &gp_slots[idx];
    gp->active    = true;
    gp->xhci_slot = slot_id;
    gp->state     = (gamepad_state_t){ .connected = true };

    if (vid == VID_SONY && pid == PID_DUALSENSE) {
        gp->proto = GP_PROTO_DUALSENSE;
        kprintf("[GAMEPAD] DualSense connected (slot %d, index %d)\n", slot_id, idx);
    } else if (vid == VID_SONY && (pid == PID_DS4_V1 || pid == PID_DS4_V2)) {
        gp->proto = GP_PROTO_DS4;
        kprintf("[GAMEPAD] DualShock 4 connected (slot %d, index %d)\n", slot_id, idx);
    } else {
        gp->proto = GP_PROTO_GENERIC;
        kprintf("[GAMEPAD] Generic HID gamepad %04x:%04x (slot %d, index %d)\n",
                vid, pid, slot_id, idx);
    }
}

// Called from xHCI ISR context when a HID report arrives.
void hid_gamepad_handle_report(const uint8_t *data, int len) {
    if (len < 4) return;

    // Find which gamepad slot owns this report (by checking active slots).
    // For simplicity, we dispatch to the first active gamepad.
    // A more complete implementation would match by xhci_slot passed through.
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (!gp_slots[i].active) continue;
        gp_slot_t *gp = &gp_slots[i];

        // Some controllers send a Report ID byte first; skip if it's ≤ 3
        const uint8_t *d = data;
        int            l = len;
        if (d[0] <= 3 && l > 1) { d++; l--; } // skip report ID

        switch (gp->proto) {
            case GP_PROTO_DS4:       parse_ds4(gp, d, l);       break;
            case GP_PROTO_DUALSENSE: parse_dualsense(gp, d, l); break;
            default:                 parse_generic(gp, d, l);   break;
        }

        gp->state.connected = true;
        gp_queue_push(i, &gp->state);
        break; // dispatch to first active gamepad
    }
}

// Read current gamepad state (polling interface for userspace).
int gamepad_get_state(int index, gamepad_state_t *out) {
    if (index < 0 || index >= MAX_GAMEPADS) return -1;
    *out = gp_slots[index].state;
    return gp_slots[index].active ? 0 : -1;
}

// ── Event queue interface for SYS_GET_INPUT_EVENT ─────────────────────────────
// Returns 1 and fills *gp_idx and *state if an event is pending.
int gamepad_get_event(int *gp_idx, gamepad_state_t *state) {
    gp_event_t evt;
    if (!gp_queue_pop(&evt)) return 0;
    *gp_idx = evt.gp_index;
    *state  = evt.state;
    return 1;
}
