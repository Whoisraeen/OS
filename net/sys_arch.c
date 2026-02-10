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

#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "semaphore.h"
#include "sched.h"
#include "ipc.h"
#include "heap.h"
#include "spinlock.h"

// Defines expected by lwIP
typedef semaphore_t sys_sem_t;
typedef semaphore_t sys_mutex_t;
typedef int sys_thread_t;

// Mailbox implementation
#define MBOX_SIZE 128
typedef struct {
    void *msgs[MBOX_SIZE];
    int head;
    int tail;
    int count;
    semaphore_t not_empty;
    semaphore_t not_full;
    spinlock_t lock;
    int valid;
} sys_mbox_t;

void sys_init(void) {
    // Initialized by kernel
}

// Semaphores
err_t sys_sem_new(sys_sem_t *sem, uint8_t count) {
    sem_init(sem, count);
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem) {
    (void)sem;
}

void sys_sem_signal(sys_sem_t *sem) {
    sem_post(sem);
}

uint32_t sys_arch_sem_wait(sys_sem_t *sem, uint32_t timeout) {
    // TODO: Implement timeout support in kernel semaphores
    (void)timeout;
    sem_wait(sem);
    return 0; // time waited
}

// Mutexes
err_t sys_mutex_new(sys_mutex_t *mutex) {
    sem_init(mutex, 1);
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    sem_wait(mutex);
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    sem_post(mutex);
}

// Mailboxes
err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    if (size > MBOX_SIZE) size = MBOX_SIZE;
    
    mbox->head = 0;
    mbox->tail = 0;
    mbox->count = 0;
    mbox->valid = 1;
    
    sem_init(&mbox->not_empty, 0);
    sem_init(&mbox->not_full, size);
    spinlock_init(&mbox->lock);
    
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    mbox->valid = 0;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    if (!mbox || !mbox->valid) return;
    
    sem_wait(&mbox->not_full);
    
    spinlock_acquire(&mbox->lock);
    mbox->msgs[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % MBOX_SIZE;
    mbox->count++;
    spinlock_release(&mbox->lock);
    
    sem_post(&mbox->not_empty);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    if (!mbox || !mbox->valid) return ERR_BUF;
    
    spinlock_acquire(&mbox->lock);
    if (mbox->count >= MBOX_SIZE) {
        spinlock_release(&mbox->lock);
        return ERR_MEM;
    }
    
    mbox->msgs[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) % MBOX_SIZE;
    mbox->count++;
    spinlock_release(&mbox->lock);
    
    sem_post(&mbox->not_empty);
    return ERR_OK;
}

uint32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, uint32_t timeout) {
    if (!mbox || !mbox->valid) return 0;
    
    // TODO: Timeout
    (void)timeout;
    sem_wait(&mbox->not_empty);
    
    spinlock_acquire(&mbox->lock);
    if (msg) {
        *msg = mbox->msgs[mbox->head];
    }
    mbox->head = (mbox->head + 1) % MBOX_SIZE;
    mbox->count--;
    spinlock_release(&mbox->lock);
    
    sem_post(&mbox->not_full);
    return 0;
}

uint32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
     if (!mbox || !mbox->valid) return 0; // Should be sys_arch_mbox_tryfetch return type... usually u32
     
     spinlock_acquire(&mbox->lock);
     if (mbox->count == 0) {
         spinlock_release(&mbox->lock);
         return 0xFFFFFFFF; // SYS_MBOX_EMPTY
     }
     
     if (msg) {
        *msg = mbox->msgs[mbox->head];
     }
     mbox->head = (mbox->head + 1) % MBOX_SIZE;
     mbox->count--;
     spinlock_release(&mbox->lock);
     
     sem_post(&mbox->not_full);
     return 0;
}

// Threads
// Wrapper to handle argument passing if we can't change task_create yet
// For now, we'll store the arg in a global if it's single threaded, 
// or we just assume the thread function doesn't need it for the main tcpip thread if it's global.
// BUT, better to fix task_create. For now, a stub.
sys_thread_t sys_thread_new(const char *name, void (*thread)(void *arg), void *arg, int stacksize, int prio) {
    (void)arg; (void)stacksize; (void)prio;
    // Warning: arg is dropped!
    return task_create(name, (void(*)(void))thread);
}

// ============================================================================
// Network Interface (ethernetif)
// ============================================================================

#include "lwip/arp.h"
#include "lwip/eth.h"
#include "lwip/endian.h"

// Called by E1000 ISR callback
void ethernetif_input(const void *data, uint16_t len) {
    if (len < sizeof(eth_hdr_t)) return;
    
    eth_hdr_t *eth = (eth_hdr_t *)data;
    uint16_t type = ntohs(eth->type);
    
    if (type == ETHTYPE_ARP) {
        arp_input((uint8_t*)data + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
    } else if (type == ETHTYPE_IP) {
        // Pass to IP layer (TODO: implement ip_input)
        kprintf("[LWIP] Received IP packet (%d bytes)\n", len);
    } else {
        // kprintf("[LWIP] Unknown eth type: 0x%x\n", type);
    }
}

// Called by lwIP to send packet
int ethernetif_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    
    // Copy pbuf chain to flat buffer
    // E1000 supports max frame size, usually < 2KB for standard frames
    uint8_t buf[2048];
    uint16_t len = 0;
    
    struct pbuf *q = p;
    while (q != NULL) {
        if (len + q->len > 2048) break; // Overflow check
        memcpy(buf + len, q->payload, q->len);
        len += q->len;
        q = q->next;
    }
    
    e1000_send_packet(buf, len);
    return 0;
}

// Initialization
void ethernetif_init(struct netif *netif) {
    // Set MAC address from E1000
    uint8_t *mac = e1000_get_mac();
    
    if (netif) {
        for (int i=0; i<6; i++) netif->hwaddr[i] = mac[i];
        netif->hwaddr_len = 6;
        netif->mtu = 1500;
        netif->flags = 0; // BROADCAST | ETHARP | LINK_UP
        
        netif->linkoutput = (err_t (*)(struct netif*, struct pbuf*))ethernetif_output;
        // netif->output is usually etharp_output
    }
    
    // Register callback
    e1000_set_rx_callback(ethernetif_input);
    
    kprintf("[LWIP] Ethernet interface initialized\n");
}
