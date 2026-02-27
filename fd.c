#include "fd.h"
#include "heap.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "keyboard.h"

// --- Console device operations ---

static size_t console_dev_read(fd_device_ops_t *dev, uint8_t *buf, size_t count) {
    (void)dev;
    return keyboard_read_ascii(buf, count);
}

static size_t console_dev_write(fd_device_ops_t *dev, const uint8_t *buf, size_t count) {
    (void)dev;
    for (size_t i = 0; i < count; i++) {
        console_putc((char)buf[i]);
        serial_putc((char)buf[i]);
    }
    return count;
}

static fd_device_ops_t console_stdin_ops = {
    .read = console_dev_read,
    .write = NULL
};

static fd_device_ops_t console_stdout_ops = {
    .read = NULL,
    .write = console_dev_write
};

// --- fd table management ---

fd_table_t *fd_table_create(void) {
    fd_table_t *table = kmalloc(sizeof(fd_table_t));
    if (!table) return NULL;
    memset(table, 0, sizeof(fd_table_t));
    return table;
}

void fd_table_destroy(fd_table_t *table) {
    if (!table) return;
    kfree(table);
}

int fd_alloc(fd_table_t *table) {
    if (!table) return -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (table->entries[i].type == FD_NONE) {
            return i;
        }
    }
    return -1;
}

void fd_free(fd_table_t *table, int fd) {
    if (!table || fd < 0 || fd >= MAX_FDS) return;
    memset(&table->entries[fd], 0, sizeof(fd_entry_t));
}

fd_entry_t *fd_get(fd_table_t *table, int fd) {
    if (!table || fd < 0 || fd >= MAX_FDS) return NULL;
    if (table->entries[fd].type == FD_NONE) return NULL;
    return &table->entries[fd];
}

int fd_dup(fd_table_t *table, int old_fd) {
    fd_entry_t *src = fd_get(table, old_fd);
    if (!src) return -1;

    int new_fd = fd_alloc(table);
    if (new_fd < 0) return -1;

    memcpy(&table->entries[new_fd], src, sizeof(fd_entry_t));
    return new_fd;
}

int fd_dup2(fd_table_t *table, int old_fd, int new_fd) {
    if (!table || new_fd < 0 || new_fd >= MAX_FDS) return -1;

    fd_entry_t *src = fd_get(table, old_fd);
    if (!src) return -1;

    if (old_fd == new_fd) return new_fd;

    // Close new_fd if open
    if (table->entries[new_fd].type != FD_NONE) {
        fd_free(table, new_fd);
    }

    memcpy(&table->entries[new_fd], src, sizeof(fd_entry_t));
    return new_fd;
}

void fd_init_stdio(fd_table_t *table) {
    if (!table) return;

    // fd 0 = stdin (console read)
    table->entries[0].type = FD_DEVICE;
    table->entries[0].flags = O_RDONLY;
    table->entries[0].dev = &console_stdin_ops;

    // fd 1 = stdout (console write)
    table->entries[1].type = FD_DEVICE;
    table->entries[1].flags = O_WRONLY;
    table->entries[1].dev = &console_stdout_ops;

    // fd 2 = stderr (console write)
    table->entries[2].type = FD_DEVICE;
    table->entries[2].flags = O_WRONLY;
    table->entries[2].dev = &console_stdout_ops;
}
