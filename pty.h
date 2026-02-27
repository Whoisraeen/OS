#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

#define PTY_BUF_SIZE  4096
#define MAX_PTYS      16
#define PTY_LINE_MAX  512

/*
 * Termios subset — matches Linux struct termios (x86-64):
 *   c_iflag, c_oflag, c_cflag, c_lflag (4 bytes each)
 *   c_line (1 byte), c_cc[19] (19 bytes)  → 36 bytes total
 * Matches TCGETS (0x5401) / TCSETS (0x5402) ioctl layout.
 */
typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
} __attribute__((packed)) pty_termios_t;

/* c_lflag bits (Linux values) */
#define PTY_ISIG    0x0001
#define PTY_ICANON  0x0002
#define PTY_ECHO    0x0008
#define PTY_ECHOE   0x0010
#define PTY_ECHOK   0x0020
#define PTY_IEXTEN  0x8000

/* c_cc indices */
#define VEOF    4
#define VERASE  2
#define VKILL   3
#define VINTR   0
#define VSUSP   10
#define VMIN    6
#define VTIME   7

/* Linux ioctl codes for PTY */
#define TIOCGPTN    0x80045430U   /* get PTY slave number */
#define TIOCSPTLCK  0x40045431U   /* lock/unlock PTY */
#define TIOCGPTLCK  0x80045439U   /* get lock state */

typedef struct pty {
    int      index;        /* 0..MAX_PTYS-1 */
    int      in_use;
    int      master_open;
    int      slave_open;
    int      locked;       /* 1 = slave locked (not yet opened) */

    uint16_t rows, cols;   /* window size */

    pty_termios_t termios;

    /* master→slave: user input destined for program's stdin */
    uint8_t  m2s[PTY_BUF_SIZE];
    uint32_t m2s_head, m2s_tail;

    /* slave→master: program output destined for terminal display */
    uint8_t  s2m[PTY_BUF_SIZE];
    uint32_t s2m_head, s2m_tail;

    /* canonical line accumulation buffer */
    uint8_t  lbuf[PTY_LINE_MAX];
    int      lbuf_len;

    spinlock_t lock;
    void *master_waiter;  /* task_t* blocked waiting for slave output */
    void *slave_waiter;   /* task_t* blocked waiting for master input */
} pty_t;

void   pty_subsystem_init(void);
pty_t *pty_alloc(void);
void   pty_close_master(pty_t *p);
void   pty_close_slave(pty_t *p);
pty_t *pty_get(int index);

/* Master side (terminal emulator writes user input, reads program output) */
size_t pty_master_write(pty_t *p, const uint8_t *buf, size_t count);
size_t pty_master_read(pty_t *p, uint8_t *buf, size_t count);

/* Slave side (program reads stdin, writes stdout) */
size_t pty_slave_read(pty_t *p, uint8_t *buf, size_t count);
size_t pty_slave_write(pty_t *p, const uint8_t *buf, size_t count);

/* Non-blocking readiness checks for poll/select */
int pty_slave_avail(pty_t *p);   /* 1 if slave can read without blocking */
int pty_master_avail(pty_t *p);  /* 1 if master can read without blocking */

#endif
