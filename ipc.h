#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Maximum number of ports system-wide
#define IPC_MAX_PORTS 256

// Maximum number of messages in a port's queue
#define IPC_PORT_QUEUE_SIZE 16

// Maximum message size (for inline messages)
#define IPC_MAX_MSG_SIZE 128

// IPC Port Handle (userspace reference to a port)
typedef uint32_t ipc_port_t;

// Invalid port constant
#define IPC_PORT_INVALID 0

// IPC Message Structure
typedef struct {
    uint32_t msg_id;              // Message identifier (for request/response matching)
    uint32_t sender_pid;          // Process ID of sender (filled by kernel)
    uint32_t reply_port;          // Port for reply messages (0 if no reply needed)
    uint32_t size;                // Size of payload in bytes
    uint64_t timestamp;           // Timestamp when sent (filled by kernel)
    uint8_t data[IPC_MAX_MSG_SIZE]; // Inline data payload
} ipc_message_t;

// IPC Port Flags
typedef enum {
    IPC_PORT_FLAG_NONE        = 0,
    IPC_PORT_FLAG_KERNEL      = (1 << 0),  // Kernel-owned port
    IPC_PORT_FLAG_RECEIVE     = (1 << 1),  // Port can receive messages
    IPC_PORT_FLAG_SEND        = (1 << 2),  // Port can send messages
    IPC_PORT_FLAG_ONCE        = (1 << 3),  // Port receives one message then closes
} ipc_port_flags_t;

// IPC Send Flags
typedef enum {
    IPC_SEND_SYNC       = 0,        // Synchronous send (wait for receipt)
    IPC_SEND_ASYNC      = (1 << 0), // Asynchronous send (don't wait)
    IPC_SEND_TIMEOUT    = (1 << 1), // Use timeout (timeout_ms field)
} ipc_send_flags_t;

// IPC Receive Flags
typedef enum {
    IPC_RECV_BLOCKING   = 0,        // Block until message arrives
    IPC_RECV_NONBLOCK   = (1 << 0), // Return immediately if no message
    IPC_RECV_TIMEOUT    = (1 << 1), // Use timeout (timeout_ms field)
} ipc_recv_flags_t;

// IPC Error Codes
typedef enum {
    IPC_SUCCESS         = 0,
    IPC_ERR_INVALID_PORT = -1,
    IPC_ERR_NO_MEMORY   = -2,
    IPC_ERR_QUEUE_FULL  = -3,
    IPC_ERR_TIMEOUT     = -4,
    IPC_ERR_NO_MESSAGE  = -5,
    IPC_ERR_PERMISSION  = -6,
    IPC_ERR_INVALID_SIZE = -7,
} ipc_error_t;

// Shared Memory Region Structure
typedef struct {
    uint32_t shmem_id;            // Shared memory ID
    void *virt_addr;              // Virtual address in current process
    uint64_t phys_addr;           // Physical address (for kernel)
    size_t size;                  // Size in bytes
    uint32_t owner_pid;           // Process that created it
    uint32_t flags;               // Access flags
    uint32_t ref_count;           // Number of processes mapped
} ipc_shmem_t;

// Shared Memory Flags
typedef enum {
    IPC_SHMEM_READ   = (1 << 0),
    IPC_SHMEM_WRITE  = (1 << 1),
    IPC_SHMEM_EXEC   = (1 << 2),
} ipc_shmem_flags_t;

// === Kernel API (used within kernel) ===

// Initialize IPC subsystem
void ipc_init(void);

// Create a new port (returns port handle or IPC_PORT_INVALID)
ipc_port_t ipc_port_create(uint32_t owner_pid, uint32_t flags);

// Destroy a port
int ipc_port_destroy(ipc_port_t port, uint32_t pid);

// Send a message to a port
int ipc_send_message(ipc_port_t dest_port, ipc_message_t *msg,
                     uint32_t sender_pid, uint32_t flags, uint32_t timeout_ms);

// Receive a message from a port
int ipc_recv_message(ipc_port_t port, ipc_message_t *msg,
                     uint32_t receiver_pid, uint32_t flags, uint32_t timeout_ms);

// Check if a port exists and is accessible by process
bool ipc_port_valid(ipc_port_t port, uint32_t pid);

// Get port by name (for well-known system ports)
ipc_port_t ipc_port_lookup(const char *name);

// Register a named port (for services to advertise)
int ipc_port_register(ipc_port_t port, const char *name);

// Create shared memory region
uint32_t ipc_shmem_create(size_t size, uint32_t owner_pid, uint32_t flags);

// Map shared memory into process address space
void *ipc_shmem_map(uint32_t shmem_id, uint32_t pid);

// Unmap shared memory from process
int ipc_shmem_unmap(uint32_t shmem_id, uint32_t pid);

// Destroy shared memory region (when ref_count reaches 0)
int ipc_shmem_destroy(uint32_t shmem_id, uint32_t pid);

// === Debugging ===

// Print port statistics
void ipc_debug_print_ports(void);

// Print message queue for a port
void ipc_debug_print_queue(ipc_port_t port);

#endif // IPC_H
