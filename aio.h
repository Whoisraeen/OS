#ifndef AIO_H
#define AIO_H

#include <stdint.h>

// AIO Operation Types
#define AIO_OP_READ   1
#define AIO_OP_WRITE  2

// AIO Request Structure
// We use a named struct tag to avoid forward declaration issues
typedef struct aio_request {
    uint64_t aio_id;      // Unique ID for this request
    int fd;               // File descriptor
    void *buf;            // Buffer pointer
    uint64_t count;       // Bytes to read/write
    uint64_t offset;      // File offset
    int opcode;           // Operation (READ/WRITE)
} aio_request_t;

// AIO Result Structure
typedef struct {
    uint64_t aio_id;
    int64_t result;       // Return value (bytes transferred or error)
} aio_result_t;

// Kernel API
void aio_init(void);
uint64_t sys_aio_submit(aio_request_t *req);
uint64_t sys_aio_wait(uint64_t aio_id, aio_result_t *res);

#endif
