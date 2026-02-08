#ifndef GUI_H
#define GUI_H

#include <stdint.h>

// Window Management Protocol
#define MSG_CREATE_WINDOW 0x1001
#define MSG_WINDOW_CREATED 0x1002
#define MSG_INPUT_EVENT   0x1003

// Input Event Types
#define EVENT_KEY_DOWN    1
#define EVENT_KEY_UP      2
#define EVENT_MOUSE_MOVE  3
#define EVENT_MOUSE_DOWN  4
#define EVENT_MOUSE_UP    5

// Message: Create Window
typedef struct {
    int x, y, w, h;
    uint32_t shmem_id;
    uint32_t reply_port; // Port to receive events
    char title[32];
} msg_create_window_t;

// Message: Input Event (Sent from Compositor to App)
typedef struct {
    uint32_t type;
    uint32_t code; // Keycode or Button
    int32_t x;     // Mouse X (relative to window)
    int32_t y;     // Mouse Y (relative to window)
} msg_input_event_t;

#endif
