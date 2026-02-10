#include <stdint.h>
#include <stddef.h>
#include "drivers/e1000.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "heap.h"

// ============================================================================
// lwIP Adapter Layer (sys_arch)
// ============================================================================

// Since we are running in kernel for now, we map lwIP primitives to kernel primitives

#include "semaphore.h"
#include "sched.h"
#include "ipc.h"

// Defines expected by lwIP
typedef int8_t err_t;
typedef semaphore_t sys_sem_t;
typedef semaphore_t sys_mutex_t;
typedef struct { int id; } sys_mbox_t; // Placeholder
typedef int sys_thread_t;

void sys_init(void) {
    // Initialized by kernel
}

// Semaphores
err_t sys_sem_new(sys_sem_t *sem, uint8_t count) {
    sem_init(sem, count);
    return 0; // ERR_OK
}

void sys_sem_free(sys_sem_t *sem) {
    // No-op for now (no destroy fn in semaphore.h)
}

void sys_sem_signal(sys_sem_t *sem) {
    sem_post(sem);
}

uint32_t sys_arch_sem_wait(sys_sem_t *sem, uint32_t timeout) {
    // TODO: Implement timeout
    sem_wait(sem);
    return 0; // time waited
}

// Mutexes (using semaphores for now)
err_t sys_mutex_new(sys_mutex_t *mutex) {
    sem_init(mutex, 1);
    return 0;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    sem_wait(mutex);
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    sem_post(mutex);
}

// Threads
sys_thread_t sys_thread_new(const char *name, void (*thread)(void *arg), void *arg, int stacksize, int prio) {
    // Kernel task_create only takes void(*)(void)
    // We need a wrapper to pass arg. For now, ignore arg or fix sched.c
    return task_create(name, (void(*)(void))thread);
}

// ============================================================================
// Network Interface (ethernetif)
// ============================================================================

// This would connect lwIP netif to E1000

// Forward declaration of lwIP struct (we don't have lwIP headers yet)
struct netif;
struct pbuf;

// Called by E1000 ISR callback
void ethernetif_input(const void *data, uint16_t len) {
    kprintf("[LWIP] Received packet (%d bytes)\n", len);
    // 1. Allocate pbuf
    // 2. Copy data
    // 3. Pass to tcpip_thread via mailbox
}

// Called by lwIP to send packet
int ethernetif_output(struct netif *netif, struct pbuf *p) {
    // Copy pbuf to flat buffer
    // e1000_send_packet(buf, len);
    return 0;
}

// Initialization
void ethernetif_init(struct netif *netif) {
    // Set MAC address from E1000
    uint8_t *mac = e1000_get_mac();
    // netif->hwaddr[0] = mac[0]; ...
    
    // Register callback
    e1000_set_rx_callback(ethernetif_input);
}
