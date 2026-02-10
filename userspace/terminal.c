#include <stdint.h>
#include <stddef.h>
#include "syscalls.h"
#include "font.h"
#include "u_stdlib.h"

// Directory entry structure
typedef struct {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} dirent_t;

// ============================================================================
// Utils
// ============================================================================

static inline void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static inline void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

static inline size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

// ============================================================================
// GUI Definitions
// ============================================================================

typedef struct {
    uint32_t msg_id;
    uint32_t sender_pid;
    uint32_t reply_port;
    uint32_t size;
    uint64_t timestamp;
    uint8_t data[128];
} ipc_message_t;

typedef struct {
    char title[64];
    int x, y;
    int w, h;
    uint32_t shmem_id;
    uint32_t reply_port;
} msg_create_window_t;

typedef struct {
    uint32_t type; // 1=Key, 2=Mouse
    uint32_t code;
    int32_t x;
    int32_t y;
} msg_input_event_t;

#define EVENT_KEY_DOWN 1

// Colors
#define BG_COLOR 0xFF000000 // Black
#define FG_COLOR 0xFF00FF00 // Green

// Terminal Config
#define TERM_WIDTH  600
#define TERM_HEIGHT 400
#define ROWS (TERM_HEIGHT / FONT_HEIGHT)
#define COLS (TERM_WIDTH / FONT_WIDTH)

// State
static char text_buffer[ROWS][COLS];
static uint32_t color_buffer[ROWS][COLS];
static int cursor_row = 0;
static int cursor_col = 0;
static uint32_t *win_buffer = NULL;
static long comp_port = 0;
static char cmd_buffer[256];
static int cmd_len = 0;

// ANSI State
#define STATE_NORMAL 0
#define STATE_ESC    1
#define STATE_CSI    2

static int term_state = STATE_NORMAL;
static int term_params[4];
static int term_param_count = 0;
static uint32_t current_fg = FG_COLOR;
static uint32_t current_bg = BG_COLOR;

// ============================================================================
// Drawing
// ============================================================================

static void put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= TERM_WIDTH || y < 0 || y >= TERM_HEIGHT) return;
    win_buffer[y * TERM_WIDTH + x] = color;
}

static void draw_char_pixel(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font_data[idx][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

static void render_console() {
    // Clear
    for (int i = 0; i < TERM_WIDTH * TERM_HEIGHT; i++) {
        win_buffer[i] = BG_COLOR;
    }
    
    // Draw Text
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            char ch = text_buffer[r][c];
            if (ch != 0) {
                uint32_t color = color_buffer[r][c];
                if (color == 0) color = FG_COLOR; // Default
                draw_char_pixel(c * FONT_WIDTH, r * FONT_HEIGHT, ch, color);
            }
        }
    }
    
    // Draw Cursor
    int cx = cursor_col * FONT_WIDTH;
    int cy = cursor_row * FONT_HEIGHT;
    for (int r = 0; r < FONT_HEIGHT; r++) {
        for (int c = 0; c < FONT_WIDTH; c++) {
            put_pixel(cx + c, cy + r, current_fg);
        }
    }
}

static void scroll_up() {
    for (int r = 0; r < ROWS - 1; r++) {
        for (int c = 0; c < COLS; c++) {
            text_buffer[r][c] = text_buffer[r+1][c];
            color_buffer[r][c] = color_buffer[r+1][c];
        }
    }
    for (int c = 0; c < COLS; c++) {
        text_buffer[ROWS-1][c] = 0;
        color_buffer[ROWS-1][c] = current_fg;
    }
    cursor_row = ROWS - 1;
}

static void handle_csi(char c) {
    if (c >= '0' && c <= '9') {
        term_params[term_param_count] = term_params[term_param_count] * 10 + (c - '0');
    } else if (c == ';') {
        if (term_param_count < 3) term_param_count++;
    } else if (c == 'm') {
        // SGR - Select Graphic Rendition
        for (int i = 0; i <= term_param_count; i++) {
            int code = term_params[i];
            if (code == 0) {
                current_fg = FG_COLOR;
                current_bg = BG_COLOR;
            } else if (code >= 30 && code <= 37) {
                // Basic Colors
                uint32_t colors[] = {
                    0xFF000000, // Black
                    0xFFFF0000, // Red
                    0xFF00FF00, // Green
                    0xFFFFFF00, // Yellow
                    0xFF0000FF, // Blue
                    0xFFFF00FF, // Magenta
                    0xFF00FFFF, // Cyan
                    0xFFFFFFFF  // White
                };
                current_fg = colors[code - 30];
            } else if (code == 1) {
                // Bold (Bright) - ignore for now or implement
            }
        }
        term_state = STATE_NORMAL;
    } else if (c == 'J') {
        // Clear screen
        if (term_params[0] == 2) {
             for(int r=0; r<ROWS; r++) 
                for(int col=0; col<COLS; col++) {
                    text_buffer[r][col] = 0;
                    color_buffer[r][col] = current_fg;
                }
             cursor_row = 0;
             cursor_col = 0;
        }
        term_state = STATE_NORMAL;
    } else {
        // Unknown or unsupported
        term_state = STATE_NORMAL;
    }
}

static void terminal_putc(char c) {
    if (term_state == STATE_NORMAL) {
        if (c == '\033') {
            term_state = STATE_ESC;
        } else if (c == '\n') {
            cursor_col = 0;
            cursor_row++;
        } else if (c == '\b') {
            if (cursor_col > 0) {
                cursor_col--;
                text_buffer[cursor_row][cursor_col] = 0;
            } else if (cursor_row > 0) {
                cursor_row--;
                cursor_col = COLS - 1;
                text_buffer[cursor_row][cursor_col] = 0;
            }
        } else {
            text_buffer[cursor_row][cursor_col] = c;
            color_buffer[cursor_row][cursor_col] = current_fg;
            cursor_col++;
            if (cursor_col >= COLS) {
                cursor_col = 0;
                cursor_row++;
            }
        }
    } else if (term_state == STATE_ESC) {
        if (c == '[') {
            term_state = STATE_CSI;
            term_param_count = 0;
            term_params[0] = 0;
            term_params[1] = 0;
            term_params[2] = 0;
            term_params[3] = 0;
        } else {
            term_state = STATE_NORMAL;
        }
    } else if (term_state == STATE_CSI) {
        handle_csi(c);
    }
    
    if (cursor_row >= ROWS) {
        scroll_up();
    }
}

static void terminal_puts(const char *s) {
    while (*s) terminal_putc(*s++);
}

static void terminal_put_hex(uint32_t val) {
    char buf[16];
    char *p = buf + 15;
    *p = 0;
    if (val == 0) {
        terminal_puts("0");
        return;
    }
    while (val) {
        int d = val % 16;
        *--p = (d < 10) ? ('0' + d) : ('A' + d - 10);
        val /= 16;
    }
    terminal_puts(p);
}

// ============================================================================
// Command Handling
// ============================================================================

static void handle_command(char *cmd) {
    terminal_puts("\n");
    if (cmd[0] == 0) return;

    if (strncmp(cmd, "ls", 2) == 0) {
        char *path = cmd + 2;
        while (*path == ' ') path++;
        if (*path == 0) path = "/"; // Default to root

        terminal_puts("Listing: ");
        terminal_puts(path);
        terminal_puts("\n");

        long fd = syscall2(SYS_OPEN, (long)path, 0); // O_RDONLY
        if (fd < 0) {
            terminal_puts("Failed to open directory.\n");
        } else {
            dirent_t entries[16];
            while (1) {
                long n = syscall3(SYS_GETDENTS, fd, (long)entries, 16);
                if (n <= 0) break;
                
                for (int i = 0; i < n; i++) {
                    terminal_puts("  ");
                    terminal_puts(entries[i].d_name);
                    terminal_puts("\n");
                }
            }
            syscall1(SYS_CLOSE, fd);
        }
    } else if (strcmp(cmd, "help") == 0) {
        terminal_puts("Available commands:\n");
        terminal_puts("  ls [path] - List directory\n");
        terminal_puts("  help      - Show this help\n");
    } else {
        terminal_puts("Unknown command: ");
        terminal_puts(cmd);
        terminal_puts("\n");
    }
}

// ============================================================================
// Main
// ============================================================================

void _start(void) {
    // 1. Setup IPC Port
    long my_port = syscall1(SYS_IPC_CREATE, IPC_PORT_FLAG_RECEIVE);
    if (my_port <= 0) {
        syscall1(SYS_EXIT, 1);
    }
    
    // 2. Create Shared Memory for Window
    size_t buf_size = TERM_WIDTH * TERM_HEIGHT * 4;
    long shmem_id = syscall2(SYS_IPC_SHMEM_CREATE, buf_size, 0);
    if (shmem_id <= 0) {
        syscall1(SYS_EXIT, 1);
    }
    
    win_buffer = (uint32_t *)syscall1(SYS_IPC_SHMEM_MAP, shmem_id);
    if (!win_buffer) {
        syscall1(SYS_EXIT, 1);
    }
    
    // Initial Render
    memset(text_buffer, 0, sizeof(text_buffer));
    terminal_puts("RaeenOS Terminal v0.2\n");
    terminal_puts("Filesystem mounted at /disk\n");
    terminal_puts("Try 'ls /disk' to see installed files.\n");
    terminal_puts("> ");
    render_console();
    
    // 3. Connect to Compositor
    comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
    while (comp_port <= 0) {
        syscall3(SYS_YIELD, 0, 0, 0); // Wait for compositor
        comp_port = syscall1(SYS_IPC_LOOKUP, (long)"compositor");
    }
    
    // 4. Create Window
    ipc_message_t msg;
    msg.msg_id = 0; // Kernel overwrites?
    msg.size = sizeof(msg_create_window_t);
    
    msg_create_window_t *req = (msg_create_window_t *)msg.data;
    memcpy(req->title, "Terminal", 9);
    req->x = 50;
    req->y = 50;
    req->w = TERM_WIDTH;
    req->h = TERM_HEIGHT;
    req->shmem_id = shmem_id;
    req->reply_port = my_port;
    
    syscall3(SYS_IPC_SEND, comp_port, (long)&msg, 0);
    
    // 5. Event Loop
    cmd_len = 0;
    
    for (;;) {
        long res = syscall3(SYS_IPC_RECV, my_port, (long)&msg, 0); // Blocking
        if (res == 0) {
            // Check for Input Event
            if (msg.size == sizeof(msg_input_event_t)) {
                msg_input_event_t *evt = (msg_input_event_t *)msg.data;
                if (evt->type == EVENT_KEY_DOWN) {
                    // Simple ASCII mapping (Very basic)
                    char c = 0;
                    uint32_t sc = evt->code;
                    
                    // QWERTY map (partial)
                    if (sc == 0x1C) c = '\n'; // Enter
                    else if (sc == 0x0E) c = '\b'; // Backspace
                    else if (sc == 0x39) c = ' '; // Space
                    else if (sc == 0x35) c = '/'; // Slash
                    else if (sc == 0x34) c = '.'; // Dot
                    else if (sc >= 0x02 && sc <= 0x0B) {
                        const char *nums = "1234567890";
                        c = nums[sc - 0x02];
                    }
                    else if (sc >= 0x10 && sc <= 0x19) {
                        const char *row1 = "qwertyuiop";
                        c = row1[sc - 0x10];
                    }
                    else if (sc >= 0x1E && sc <= 0x26) {
                        const char *row2 = "asdfghjkl";
                        c = row2[sc - 0x1E];
                    }
                    else if (sc >= 0x2C && sc <= 0x32) {
                        const char *row3 = "zxcvbnm";
                        c = row3[sc - 0x2C];
                    }
                    
                    if (c != 0) {
                        terminal_putc(c);
                        
                        if (c == '\n') {
                            cmd_buffer[cmd_len] = 0;
                            handle_command(cmd_buffer);
                            cmd_len = 0;
                            terminal_puts("> ");
                        } else if (c == '\b') {
                            if (cmd_len > 0) cmd_len--;
                        } else {
                            if (cmd_len < 255) cmd_buffer[cmd_len++] = c;
                        }
                        
                        render_console();
                        
                        // Notify Compositor to Redraw
                        ipc_message_t inv;
                        inv.size = 0; // Empty message = Invalidate
                        syscall3(SYS_IPC_SEND, comp_port, (long)&inv, 0);
                    }
                } else if (evt->type == 4) { // EVENT_MOUSE_DOWN
                    // Click to move cursor
                    int col = evt->x / FONT_WIDTH;
                    int row = evt->y / FONT_HEIGHT;
                    
                    if (col >= 0 && col < COLS && row >= 0 && row < ROWS) {
                        cursor_col = col;
                        cursor_row = row;
                        render_console();
                        
                        ipc_message_t inv;
                        inv.size = 0;
                        syscall3(SYS_IPC_SEND, comp_port, (long)&inv, 0);
                    }
                }
            }
        }
    }
    
    syscall1(SYS_EXIT, 0);
}
