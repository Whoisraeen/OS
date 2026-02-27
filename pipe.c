#include "pipe.h"
#include "fd.h"
#include "heap.h"
#include "string.h"

// --- fd_device_ops adapters for pipe read/write ---

static size_t pipe_dev_read(fd_device_ops_t *dev, uint8_t *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return 0;
}

static size_t pipe_dev_write(fd_device_ops_t *dev, const uint8_t *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return 0;
}

// Unused but required for the device ops struct
static fd_device_ops_t pipe_read_ops = { .read = pipe_dev_read, .write = NULL };
static fd_device_ops_t pipe_write_ops = { .read = NULL, .write = pipe_dev_write };

// Suppress unused warnings
__attribute__((used)) static void *_pipe_ops_refs[] = { &pipe_read_ops, &pipe_write_ops };

// --- Core pipe operations ---

size_t pipe_read(pipe_t *pipe, uint8_t *buf, size_t count) {
    if (!pipe || !buf || count == 0) return 0;

    while (1) {
        spinlock_acquire(&pipe->lock);

        if (pipe->count > 0) {
            // Data available — read up to count bytes
            size_t to_read = count;
            if (to_read > pipe->count) to_read = pipe->count;

            for (size_t i = 0; i < to_read; i++) {
                buf[i] = pipe->buffer[pipe->read_pos];
                pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
            }
            pipe->count -= to_read;

            // Wake blocked writer if any
            task_t *writer = pipe->blocked_writer;
            pipe->blocked_writer = NULL;
            spinlock_release(&pipe->lock);

            if (writer) task_unblock(writer);
            return to_read;
        }

        // No data
        if (pipe->writers == 0) {
            // EOF — all write ends closed
            spinlock_release(&pipe->lock);
            return 0;
        }

        // Block until data arrives
        pipe->blocked_reader = task_get_by_id(task_current_id());
        spinlock_release(&pipe->lock);
        task_block();
        // Woken up — loop and try again
    }
}

size_t pipe_write(pipe_t *pipe, const uint8_t *buf, size_t count) {
    if (!pipe || !buf || count == 0) return 0;

    size_t written = 0;

    while (written < count) {
        spinlock_acquire(&pipe->lock);

        if (pipe->readers == 0) {
            // Broken pipe — no readers
            spinlock_release(&pipe->lock);
            return (size_t)-1;
        }

        if (pipe->count < PIPE_BUF_SIZE) {
            // Space available — write as much as we can
            size_t space = PIPE_BUF_SIZE - pipe->count;
            size_t to_write = count - written;
            if (to_write > space) to_write = space;

            for (size_t i = 0; i < to_write; i++) {
                pipe->buffer[pipe->write_pos] = buf[written + i];
                pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
            }
            pipe->count += to_write;
            written += to_write;

            // Wake blocked reader if any
            task_t *reader = pipe->blocked_reader;
            pipe->blocked_reader = NULL;
            spinlock_release(&pipe->lock);

            if (reader) task_unblock(reader);

            if (written == count) return written;
            continue;
        }

        // Buffer full — block until space
        pipe->blocked_writer = task_get_by_id(task_current_id());
        spinlock_release(&pipe->lock);
        task_block();
    }

    return written;
}

void pipe_close(pipe_t *pipe, int is_write_end) {
    if (!pipe) return;

    spinlock_acquire(&pipe->lock);

    if (is_write_end) {
        pipe->writers--;
        if (pipe->writers == 0 && pipe->blocked_reader) {
            // Wake reader so it sees EOF
            task_t *reader = pipe->blocked_reader;
            pipe->blocked_reader = NULL;
            spinlock_release(&pipe->lock);
            task_unblock(reader);
        } else {
            spinlock_release(&pipe->lock);
        }
    } else {
        pipe->readers--;
        if (pipe->readers == 0 && pipe->blocked_writer) {
            // Wake writer so it sees broken pipe
            task_t *writer = pipe->blocked_writer;
            pipe->blocked_writer = NULL;
            spinlock_release(&pipe->lock);
            task_unblock(writer);
        } else {
            spinlock_release(&pipe->lock);
        }
    }

    // If both ends closed, free the pipe
    if (pipe->readers <= 0 && pipe->writers <= 0) {
        kfree(pipe);
    }
}

int pipe_create(struct fd_table *table, int *read_fd, int *write_fd) {
    if (!table || !read_fd || !write_fd) return -1;

    pipe_t *pipe = kmalloc(sizeof(pipe_t));
    if (!pipe) return -1;

    memset(pipe, 0, sizeof(pipe_t));
    spinlock_init(&pipe->lock);
    pipe->readers = 1;
    pipe->writers = 1;

    // Allocate read fd
    int rfd = fd_alloc(table);
    if (rfd < 0) { kfree(pipe); return -1; }

    table->entries[rfd].type = FD_PIPE;
    table->entries[rfd].flags = O_RDONLY;
    table->entries[rfd].pipe = pipe;

    // Allocate write fd
    int wfd = fd_alloc(table);
    if (wfd < 0) {
        fd_free(table, rfd);
        kfree(pipe);
        return -1;
    }

    table->entries[wfd].type = FD_PIPE;
    table->entries[wfd].flags = O_WRONLY;
    table->entries[wfd].pipe = pipe;

    *read_fd = rfd;
    *write_fd = wfd;
    return 0;
}

size_t pipe_bytes_available(pipe_t *pipe) {
    if (!pipe) return 0;
    spinlock_acquire(&pipe->lock);
    size_t n = pipe->count;
    spinlock_release(&pipe->lock);
    return n;
}
