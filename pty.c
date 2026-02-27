#include "pty.h"
#include "sched.h"
#include "signal.h"
#include "string.h"
#include "serial.h"

static pty_t pty_pool[MAX_PTYS];

/* Ring helpers */
#define RING_FULL(h,t)   (((h)+1)%PTY_BUF_SIZE == (t))
#define RING_EMPTY(h,t)  ((h)==(t))

static void ring_put(uint8_t *buf, uint32_t *head, uint8_t c)
{
    uint32_t next = (*head + 1) % PTY_BUF_SIZE;
    if (next != 0) { /* overflow: just drop */ }
    buf[*head] = c;
    *head = (*head + 1) % PTY_BUF_SIZE;
}

static int ring_get(uint8_t *buf, uint32_t head, uint32_t *tail, uint8_t *out)
{
    if (head == *tail) return 0;
    *out = buf[*tail];
    *tail = (*tail + 1) % PTY_BUF_SIZE;
    return 1;
}

/* Echo a character to the s2m (master reads it) — called with lock held */
static void echo_char(pty_t *p, uint8_t c)
{
    if (RING_FULL(p->s2m_head, p->s2m_tail)) return;
    ring_put(p->s2m, &p->s2m_head, c);
}

/* Flush the line buffer into m2s so slave can read it */
static void lbuf_flush(pty_t *p)
{
    for (int i = 0; i < p->lbuf_len; i++) {
        if (!RING_FULL(p->m2s_head, p->m2s_tail))
            ring_put(p->m2s, &p->m2s_head, p->lbuf[i]);
    }
    p->lbuf_len = 0;
}

void pty_subsystem_init(void)
{
    memset(pty_pool, 0, sizeof(pty_pool));
}

pty_t *pty_alloc(void)
{
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!pty_pool[i].in_use) {
            pty_t *p = &pty_pool[i];
            memset(p, 0, sizeof(*p));
            p->index       = i;
            p->in_use      = 1;
            p->master_open = 1;
            p->slave_open  = 0;
            p->locked      = 1;  /* slave locked until unlockpt() */
            p->rows        = 24;
            p->cols        = 80;

            /* Default termios: canonical + echo, 38400 baud, 8N1 */
            p->termios.c_iflag = 0x0500; /* ICRNL | IXON */
            p->termios.c_oflag = 0x0001; /* OPOST */
            p->termios.c_cflag = 0x04B2; /* B38400 | CS8 | CREAD */
            p->termios.c_lflag = PTY_ICANON | PTY_ECHO | PTY_ECHOE |
                                  PTY_ECHOK | PTY_ISIG | PTY_IEXTEN;
            p->termios.c_cc[VINTR]  = 3;   /* Ctrl+C */
            p->termios.c_cc[VERASE] = 127; /* DEL */
            p->termios.c_cc[VKILL]  = 21;  /* Ctrl+U */
            p->termios.c_cc[VEOF]   = 4;   /* Ctrl+D */
            p->termios.c_cc[VSUSP]  = 26;  /* Ctrl+Z */
            p->termios.c_cc[VMIN]   = 1;
            p->termios.c_cc[VTIME]  = 0;

            spinlock_init(&p->lock);
            kprintf("[PTY] Allocated /dev/pts/%d\n", i);
            return p;
        }
    }
    return NULL;
}

pty_t *pty_get(int index)
{
    if (index < 0 || index >= MAX_PTYS) return NULL;
    if (!pty_pool[index].in_use) return NULL;
    return &pty_pool[index];
}

void pty_close_master(pty_t *p)
{
    spinlock_acquire(&p->lock);
    p->master_open = 0;
    /* Wake any blocked slave reader so it can get EOF */
    task_t *sw = (task_t *)p->slave_waiter;
    p->slave_waiter = NULL;
    spinlock_release(&p->lock);
    if (sw) task_unblock(sw);

    if (!p->slave_open) {
        p->in_use = 0;
        kprintf("[PTY] /dev/pts/%d freed\n", p->index);
    }
}

void pty_close_slave(pty_t *p)
{
    spinlock_acquire(&p->lock);
    p->slave_open = 0;
    task_t *mw = (task_t *)p->master_waiter;
    p->master_waiter = NULL;
    spinlock_release(&p->lock);
    if (mw) task_unblock(mw);

    if (!p->master_open) {
        p->in_use = 0;
        kprintf("[PTY] /dev/pts/%d freed\n", p->index);
    }
}

/*
 * pty_master_write — terminal emulator sends characters (user keystrokes)
 * Applies line discipline before putting into m2s for slave to read.
 */
size_t pty_master_write(pty_t *p, const uint8_t *buf, size_t count)
{
    if (!p || !p->in_use) return 0;

    spinlock_acquire(&p->lock);

    int canon  = !!(p->termios.c_lflag & PTY_ICANON);
    int echo   = !!(p->termios.c_lflag & PTY_ECHO);
    int echoe  = !!(p->termios.c_lflag & PTY_ECHOE);
    int isig   = !!(p->termios.c_lflag & PTY_ISIG);
    uint8_t erase_ch = p->termios.c_cc[VERASE];
    uint8_t kill_ch  = p->termios.c_cc[VKILL];
    uint8_t intr_ch  = p->termios.c_cc[VINTR];
    uint8_t susp_ch  = p->termios.c_cc[VSUSP];
    uint8_t eof_ch   = p->termios.c_cc[VEOF];

    for (size_t i = 0; i < count; i++) {
        uint8_t c = buf[i];

        /* Signals */
        if (isig && c == intr_ch) {
            /* Send SIGINT to slave process — wake it to handle signal */
            if (echo) { echo_char(p, '^'); echo_char(p, 'C'); echo_char(p, '\n'); }
            /* We don't have a PID here — the slave reader will handle EINTR */
            /* Just flush to m2s as-is for now; shell checks for it */
            if (!RING_FULL(p->m2s_head, p->m2s_tail))
                ring_put(p->m2s, &p->m2s_head, c);
            goto wake_slave;
        }
        if (isig && c == susp_ch) {
            if (echo) { echo_char(p, '^'); echo_char(p, 'Z'); echo_char(p, '\n'); }
            if (!RING_FULL(p->m2s_head, p->m2s_tail))
                ring_put(p->m2s, &p->m2s_head, c);
            goto wake_slave;
        }

        if (canon) {
            /* Canonical mode — line-buffer until '\n' or EOF */
            if (c == erase_ch || c == '\b') {
                /* Backspace: erase last char from line buffer */
                if (p->lbuf_len > 0) {
                    p->lbuf_len--;
                    if (echoe) {
                        echo_char(p, '\b');
                        echo_char(p, ' ');
                        echo_char(p, '\b');
                    }
                }
                continue;
            }
            if (c == kill_ch) {
                /* Kill line */
                if (echo) {
                    for (int k = 0; k < p->lbuf_len; k++) {
                        echo_char(p, '\b'); echo_char(p, ' '); echo_char(p, '\b');
                    }
                }
                p->lbuf_len = 0;
                continue;
            }
            if (c == eof_ch) {
                /* Ctrl+D: flush line (empty = EOF) */
                lbuf_flush(p);
                goto wake_slave;
            }

            /* Convert CR → LF if ICRNL set */
            if (c == '\r' && (p->termios.c_iflag & 0x0100)) c = '\n';

            if (p->lbuf_len < PTY_LINE_MAX - 1)
                p->lbuf[p->lbuf_len++] = c;

            if (echo) echo_char(p, c);

            if (c == '\n') {
                lbuf_flush(p);
                goto wake_slave;
            }
            continue;
        } else {
            /* Raw mode — bytes go directly to m2s */
            if (!RING_FULL(p->m2s_head, p->m2s_tail))
                ring_put(p->m2s, &p->m2s_head, c);
            if (echo) echo_char(p, c);
        }
        continue;

wake_slave:
        {
            task_t *sw = (task_t *)p->slave_waiter;
            p->slave_waiter = NULL;
            spinlock_release(&p->lock);
            if (sw) task_unblock(sw);
            spinlock_acquire(&p->lock);
        }
    }

    /* Wake slave waiter if there is data (raw mode or last '\n' flush) */
    if (!RING_EMPTY(p->m2s_head, p->m2s_tail)) {
        task_t *sw = (task_t *)p->slave_waiter;
        p->slave_waiter = NULL;
        spinlock_release(&p->lock);
        if (sw) task_unblock(sw);
        return count;
    }

    /* Wake master waiter if there is echo data (s2m) */
    if (!RING_EMPTY(p->s2m_head, p->s2m_tail)) {
        task_t *mw = (task_t *)p->master_waiter;
        p->master_waiter = NULL;
        spinlock_release(&p->lock);
        if (mw) task_unblock(mw);
        return count;
    }

    spinlock_release(&p->lock);
    return count;
}

/*
 * pty_master_read — terminal emulator reads program output (s2m ring)
 * Blocks if no data available.
 */
size_t pty_master_read(pty_t *p, uint8_t *buf, size_t count)
{
    if (!p || !p->in_use || count == 0) return 0;

    while (1) {
        spinlock_acquire(&p->lock);
        if (!RING_EMPTY(p->s2m_head, p->s2m_tail)) {
            size_t n = 0;
            uint8_t c;
            while (n < count && ring_get(p->s2m, p->s2m_head, &p->s2m_tail, &c))
                buf[n++] = c;
            spinlock_release(&p->lock);
            return n;
        }
        /* Nothing available — check if slave is gone (return EOF) */
        if (!p->slave_open) {
            spinlock_release(&p->lock);
            return 0;
        }
        /* Block until slave writes */
        p->master_waiter = task_get_by_id(task_current_id());
        spinlock_release(&p->lock);
        task_block();
    }
}

/*
 * pty_slave_read — program reads stdin (m2s ring)
 * Blocks until a complete line is available (canonical) or any data (raw).
 */
size_t pty_slave_read(pty_t *p, uint8_t *buf, size_t count)
{
    if (!p || !p->in_use || count == 0) return 0;

    while (1) {
        spinlock_acquire(&p->lock);
        if (!RING_EMPTY(p->m2s_head, p->m2s_tail)) {
            size_t n = 0;
            uint8_t c;
            while (n < count && ring_get(p->m2s, p->m2s_head, &p->m2s_tail, &c)) {
                buf[n++] = c;
                /* In canonical mode, return after '\n' (complete line) */
                if ((p->termios.c_lflag & PTY_ICANON) && c == '\n')
                    break;
            }
            spinlock_release(&p->lock);
            return n;
        }
        /* EOF if master closed */
        if (!p->master_open) {
            spinlock_release(&p->lock);
            return 0;
        }
        /* Block */
        p->slave_waiter = task_get_by_id(task_current_id());
        spinlock_release(&p->lock);
        task_block();
    }
}

/*
 * pty_slave_write — program writes output (stdout/stderr) → s2m ring
 * Terminal emulator reads from master side.
 */
int pty_slave_avail(pty_t *p)
{
    if (!p || !p->in_use) return 0;
    spinlock_acquire(&p->lock);
    int avail = !RING_EMPTY(p->m2s_head, p->m2s_tail) || !p->master_open;
    spinlock_release(&p->lock);
    return avail;
}

int pty_master_avail(pty_t *p)
{
    if (!p || !p->in_use) return 0;
    spinlock_acquire(&p->lock);
    int avail = !RING_EMPTY(p->s2m_head, p->s2m_tail) || !p->slave_open;
    spinlock_release(&p->lock);
    return avail;
}

size_t pty_slave_write(pty_t *p, const uint8_t *buf, size_t count)
{
    if (!p || !p->in_use) return 0;

    spinlock_acquire(&p->lock);
    size_t n = 0;
    for (; n < count; n++) {
        if (RING_FULL(p->s2m_head, p->s2m_tail)) break;
        uint8_t c = buf[n];
        /* OPOST: translate '\n' to '\r\n' if set */
        if ((p->termios.c_oflag & 0x0001) && c == '\n') {
            if (!RING_FULL(p->s2m_head, p->s2m_tail))
                ring_put(p->s2m, &p->s2m_head, '\r');
        }
        ring_put(p->s2m, &p->s2m_head, c);
    }
    task_t *mw = (task_t *)p->master_waiter;
    p->master_waiter = NULL;
    spinlock_release(&p->lock);
    if (mw) task_unblock(mw);
    return n;
}
