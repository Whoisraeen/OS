#include "ipc.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "serial.h"
#include "sched.h"
#include <stddef.h>

// Internal Port Structure (kernel-side)
typedef struct {
    uint32_t id;                          // Port ID (index in port table)
    uint32_t owner_pid;                   // Process that owns this port
    uint32_t flags;                       // Port flags
    bool active;                          // Is this port allocated?

    // Message queue
    ipc_message_t queue[IPC_PORT_QUEUE_SIZE];
    uint32_t queue_head;                  // Index of next message to dequeue
    uint32_t queue_tail;                  // Index where next message will be enqueued
    uint32_t queue_count;                 // Number of messages in queue

    // Waiting processes (for blocking recv)
    uint32_t waiting_pid;                 // PID of process waiting for message (0 = none)

    // Name (for named ports)
    char name[32];                        // Port name (e.g., "system.storage", "system.network")
} ipc_port_internal_t;

// Internal Shared Memory Structure
typedef struct {
    uint32_t id;
    uint64_t phys_addr;
    size_t size;
    uint32_t owner_pid;
    uint32_t flags;
    uint32_t ref_count;
    bool active;

    // List of processes that have this mapped (simple array for now)
    uint32_t mapped_pids[16];
    uint32_t num_mapped;
} ipc_shmem_internal_t;

// Global IPC State
static ipc_port_internal_t ipc_ports[IPC_MAX_PORTS];
static ipc_shmem_internal_t ipc_shmem_regions[64];  // Max 64 shared memory regions
static bool ipc_initialized = false;
static uint32_t next_msg_id = 1;

// ========================================================================
// IPC Initialization
// ========================================================================

void ipc_init(void) {
    // Initialize all ports as inactive
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        ipc_ports[i].active = false;
        ipc_ports[i].id = i;
        ipc_ports[i].owner_pid = 0;
        ipc_ports[i].flags = 0;
        ipc_ports[i].queue_head = 0;
        ipc_ports[i].queue_tail = 0;
        ipc_ports[i].queue_count = 0;
        ipc_ports[i].waiting_pid = 0;
        ipc_ports[i].name[0] = '\0';
    }

    // Initialize shared memory regions
    for (int i = 0; i < 64; i++) {
        ipc_shmem_regions[i].active = false;
        ipc_shmem_regions[i].id = i;
        ipc_shmem_regions[i].ref_count = 0;
        ipc_shmem_regions[i].num_mapped = 0;
    }

    ipc_initialized = true;
    kprintf("[IPC] Initialized (max %d ports, %d shmem regions)\n",
            IPC_MAX_PORTS, 64);
}

// ========================================================================
// Port Management
// ========================================================================

ipc_port_t ipc_port_create(uint32_t owner_pid, uint32_t flags) {
    if (!ipc_initialized) {
        kprintf("[IPC] Error: IPC not initialized\n");
        return IPC_PORT_INVALID;
    }

    // Find a free port
    for (uint32_t i = 1; i < IPC_MAX_PORTS; i++) {  // Start at 1 (0 is invalid)
        if (!ipc_ports[i].active) {
            ipc_ports[i].active = true;
            ipc_ports[i].owner_pid = owner_pid;
            ipc_ports[i].flags = flags;
            ipc_ports[i].queue_head = 0;
            ipc_ports[i].queue_tail = 0;
            ipc_ports[i].queue_count = 0;
            ipc_ports[i].waiting_pid = 0;
            ipc_ports[i].name[0] = '\0';

            kprintf("[IPC] Port %d created for PID %d (flags=0x%x)\n", i, owner_pid, flags);
            return i;
        }
    }

    kprintf("[IPC] Error: No free ports available\n");
    return IPC_PORT_INVALID;
}

int ipc_port_destroy(ipc_port_t port, uint32_t pid) {
    if (port == 0 || port >= IPC_MAX_PORTS || !ipc_ports[port].active) {
        return IPC_ERR_INVALID_PORT;
    }

    // Check ownership (only owner or kernel can destroy)
    if (ipc_ports[port].owner_pid != pid && pid != 0) {
        return IPC_ERR_PERMISSION;
    }

    // Clear the port
    ipc_ports[port].active = false;
    ipc_ports[port].queue_count = 0;
    ipc_ports[port].name[0] = '\0';

    kprintf("[IPC] Port %d destroyed\n", port);
    return IPC_SUCCESS;
}

bool ipc_port_valid(ipc_port_t port, uint32_t pid) {
    if (port == 0 || port >= IPC_MAX_PORTS) {
        return false;
    }

    if (!ipc_ports[port].active) {
        return false;
    }

    // Kernel (PID 0) can access all ports
    if (pid == 0) {
        return true;
    }

    // Owner can always access their own port
    if (ipc_ports[port].owner_pid == pid) {
        return true;
    }

    // For now, allow any process to send to any port
    // (In full implementation, use capability system)
    return true;
}

int ipc_port_register(ipc_port_t port, const char *name) {
    if (port == 0 || port >= IPC_MAX_PORTS || !ipc_ports[port].active) {
        return IPC_ERR_INVALID_PORT;
    }

    // Copy name (max 31 chars + null terminator)
    int i;
    for (i = 0; i < 31 && name[i] != '\0'; i++) {
        ipc_ports[port].name[i] = name[i];
    }
    ipc_ports[port].name[i] = '\0';

    kprintf("[IPC] Port %d registered as '%s'\n", port, name);
    return IPC_SUCCESS;
}

ipc_port_t ipc_port_lookup(const char *name) {
    for (uint32_t i = 1; i < IPC_MAX_PORTS; i++) {
        if (!ipc_ports[i].active) continue;

        // Compare names
        bool match = true;
        for (int j = 0; j < 32; j++) {
            if (ipc_ports[i].name[j] != name[j]) {
                match = false;
                break;
            }
            if (name[j] == '\0') break;
        }

        if (match) {
            kprintf("[IPC] Port lookup '%s' -> port %d\n", name, i);
            return i;
        }
    }

    kprintf("[IPC] Port lookup '%s' -> not found\n", name);
    return IPC_PORT_INVALID;
}

// ========================================================================
// Message Passing
// ========================================================================

int ipc_send_message(ipc_port_t dest_port, ipc_message_t *msg,
                     uint32_t sender_pid, uint32_t flags, uint32_t timeout_ms) {
    (void)flags; (void)timeout_ms;
    // Validate port
    if (!ipc_port_valid(dest_port, sender_pid)) {
        return IPC_ERR_INVALID_PORT;
    }

    ipc_port_internal_t *port = &ipc_ports[dest_port];

    // Check if queue is full
    if (port->queue_count >= IPC_PORT_QUEUE_SIZE) {
        return IPC_ERR_QUEUE_FULL;
    }

    // Validate message size
    if (msg->size > IPC_MAX_MSG_SIZE) {
        return IPC_ERR_INVALID_SIZE;
    }

    // Fill kernel fields
    msg->msg_id = next_msg_id++;
    msg->sender_pid = sender_pid;
    // msg->timestamp = get_system_time();  // TODO: implement when timer is upgraded

    // Enqueue message
    port->queue[port->queue_tail] = *msg;  // Copy message into queue
    port->queue_tail = (port->queue_tail + 1) % IPC_PORT_QUEUE_SIZE;
    port->queue_count++;

    // If a process is waiting on this port, wake it up
    if (port->waiting_pid != 0) {
        task_t *waiter = task_get_by_id(port->waiting_pid);
        if (waiter && waiter->state == TASK_BLOCKED) {
            task_unblock(waiter);
            port->waiting_pid = 0; // Clear only if we actually unblocked someone
        }
    }

    kprintf("[IPC] Message sent: PID %d -> port %d (msg_id=%d, size=%d)\n",
            sender_pid, dest_port, msg->msg_id, msg->size);

    return IPC_SUCCESS;
}

int ipc_recv_message(ipc_port_t port, ipc_message_t *msg,
                     uint32_t receiver_pid, uint32_t flags, uint32_t timeout_ms) {
    (void)timeout_ms;
    // Validate port
    if (!ipc_port_valid(port, receiver_pid)) {
        return IPC_ERR_INVALID_PORT;
    }

    ipc_port_internal_t *iport = &ipc_ports[port];

    // Check ownership (only owner can receive)
    if (iport->owner_pid != receiver_pid && receiver_pid != 0) {
        return IPC_ERR_PERMISSION;
    }

    // Check if messages are available
    if (iport->queue_count == 0) {
        if (flags & IPC_RECV_NONBLOCK) {
            return IPC_ERR_NO_MESSAGE;
        }

        // Blocking receive: mark as waiting and sleep until a message arrives
        iport->waiting_pid = receiver_pid;

        // Block current task — ipc_send_message will call task_unblock
        task_block();

        // Woke up — check again (message should be available now)
        if (iport->queue_count == 0) {
            return IPC_ERR_NO_MESSAGE;
        }
    }

    // Dequeue message
    *msg = iport->queue[iport->queue_head];  // Copy message to caller
    iport->queue_head = (iport->queue_head + 1) % IPC_PORT_QUEUE_SIZE;
    iport->queue_count--;

    kprintf("[IPC] Message received: port %d -> PID %d (msg_id=%d, size=%d)\n",
            port, receiver_pid, msg->msg_id, msg->size);

    return IPC_SUCCESS;
}

// ========================================================================
// Shared Memory Management
// ========================================================================

uint32_t ipc_shmem_create(size_t size, uint32_t owner_pid, uint32_t flags) {
    if (!ipc_initialized) {
        kprintf("[IPC] Error: IPC not initialized\n");
        return 0;
    }

    // Round size up to page boundary
    size_t pages_needed = (size + 4095) / 4096;
    size = pages_needed * 4096;

    // Find free shmem region
    for (uint32_t i = 1; i < 64; i++) {
        if (!ipc_shmem_regions[i].active) {
            // Allocate physical pages
            void *phys_addr = pmm_alloc_pages(pages_needed);
            if (phys_addr == NULL) {
                return 0;  // Out of memory
            }

            ipc_shmem_regions[i].active = true;
            ipc_shmem_regions[i].phys_addr = (uint64_t)phys_addr;
            ipc_shmem_regions[i].size = size;
            ipc_shmem_regions[i].owner_pid = owner_pid;
            ipc_shmem_regions[i].flags = flags;
            ipc_shmem_regions[i].ref_count = 0;
            ipc_shmem_regions[i].num_mapped = 0;

            kprintf("[IPC] Shared memory %d created: size=%lu (%lu pages), owner=%d, phys=0x%lx\n",
                    i, size, pages_needed, owner_pid, (uint64_t)phys_addr);
            return i;
        }
    }

    kprintf("[IPC] Error: No free shared memory slots\n");
    return 0;
}

void *ipc_shmem_map(uint32_t shmem_id, uint32_t pid) {
    if (shmem_id == 0 || shmem_id >= 64 || !ipc_shmem_regions[shmem_id].active) {
        return NULL;
    }

    ipc_shmem_internal_t *shmem = &ipc_shmem_regions[shmem_id];

    // Choose a virtual address in user space
    uint64_t virt_addr = 0x10000000 + (shmem_id * 0x1000000); // 16MB apart to allow large regions

    // Map all pages of the region
    size_t num_pages = shmem->size / 4096;
    for (size_t p = 0; p < num_pages; p++) {
        vmm_map_user_page(virt_addr + (p * 4096), shmem->phys_addr + (p * 4096));
    }

    // Track mapping
    if (shmem->num_mapped < 16) {
        shmem->mapped_pids[shmem->num_mapped++] = pid;
    }
    shmem->ref_count++;

    kprintf("[IPC] Shared memory %d mapped to PID %d at virt 0x%lx (%lu pages)\n",
            shmem_id, pid, virt_addr, num_pages);

    return (void *)virt_addr;
}

int ipc_shmem_unmap(uint32_t shmem_id, uint32_t pid) {
    if (shmem_id == 0 || shmem_id >= 64 || !ipc_shmem_regions[shmem_id].active) {
        return IPC_ERR_INVALID_PORT;
    }

    ipc_shmem_internal_t *shmem = &ipc_shmem_regions[shmem_id];

    // Find and remove PID from mapped list
    for (uint32_t i = 0; i < shmem->num_mapped; i++) {
        if (shmem->mapped_pids[i] == pid) {
            // Shift remaining entries
            for (uint32_t j = i; j < shmem->num_mapped - 1; j++) {
                shmem->mapped_pids[j] = shmem->mapped_pids[j + 1];
            }
            shmem->num_mapped--;
            shmem->ref_count--;
            break;
        }
    }

    // TODO: Unmap from virtual address space (need VMM unmap function)

    kprintf("[IPC] Shared memory %d unmapped from PID %d\n", shmem_id, pid);
    return IPC_SUCCESS;
}

int ipc_shmem_destroy(uint32_t shmem_id, uint32_t pid) {
    if (shmem_id == 0 || shmem_id >= 64 || !ipc_shmem_regions[shmem_id].active) {
        return IPC_ERR_INVALID_PORT;
    }

    ipc_shmem_internal_t *shmem = &ipc_shmem_regions[shmem_id];

    // Check ownership
    if (shmem->owner_pid != pid && pid != 0) {
        return IPC_ERR_PERMISSION;
    }

    // Check if still mapped
    if (shmem->ref_count > 0) {
        kprintf("[IPC] Warning: Destroying shmem %d with ref_count=%d\n",
                shmem_id, shmem->ref_count);
    }

    // Free physical memory
    pmm_free_pages((void *)shmem->phys_addr, shmem->size / 4096);

    shmem->active = false;
    shmem->ref_count = 0;

    kprintf("[IPC] Shared memory %d destroyed\n", shmem_id);
    return IPC_SUCCESS;
}

void ipc_cleanup_process(uint32_t pid) {
    if (pid == 0) return;

    // Cleanup ports
    for (uint32_t i = 1; i < IPC_MAX_PORTS; i++) {
        if (ipc_ports[i].active && ipc_ports[i].owner_pid == pid) {
            ipc_port_destroy(i, pid);
        }
    }

    // Cleanup shmem
    for (uint32_t i = 1; i < 64; i++) {
        if (ipc_shmem_regions[i].active && ipc_shmem_regions[i].owner_pid == pid) {
            ipc_shmem_destroy(i, pid);
        }
    }
}

// ========================================================================
// Debugging
// ========================================================================

void ipc_debug_print_ports(void) {
    kprintf("\n[IPC] Port Status:\n");
    kprintf("  ID  Owner   Flags  Queue  Name\n");
    kprintf("  --  -----  ------  -----  ----\n");

    for (uint32_t i = 1; i < IPC_MAX_PORTS; i++) {
        if (ipc_ports[i].active) {
            kprintf("  %2d   %4d   0x%02x    %d/%d   %s\n",
                    i,
                    ipc_ports[i].owner_pid,
                    ipc_ports[i].flags,
                    ipc_ports[i].queue_count,
                    IPC_PORT_QUEUE_SIZE,
                    ipc_ports[i].name[0] ? ipc_ports[i].name : "(unnamed)");
        }
    }
}

void ipc_debug_print_queue(ipc_port_t port) {
    if (port == 0 || port >= IPC_MAX_PORTS || !ipc_ports[port].active) {
        kprintf("[IPC] Invalid port %d\n", port);
        return;
    }

    ipc_port_internal_t *p = &ipc_ports[port];

    kprintf("\n[IPC] Port %d Queue:\n", port);
    kprintf("  Head=%d, Tail=%d, Count=%d\n", p->queue_head, p->queue_tail, p->queue_count);

    if (p->queue_count == 0) {
        kprintf("  (empty)\n");
        return;
    }

    uint32_t idx = p->queue_head;
    for (uint32_t i = 0; i < p->queue_count; i++) {
        ipc_message_t *msg = &p->queue[idx];
        kprintf("  [%d] msg_id=%d, sender=%d, size=%d\n",
                i, msg->msg_id, msg->sender_pid, msg->size);
        idx = (idx + 1) % IPC_PORT_QUEUE_SIZE;
    }
}
