#include "futex.h"
#include "sched.h"
#include "spinlock.h"

#define FUTEX_HASH_SIZE 64

typedef struct futex_waiter {
    uint64_t addr;          // Virtual address being waited on
    uint64_t cr3;           // Address space (to distinguish processes)
    task_t *task;
    struct futex_waiter *next;
} futex_waiter_t;

// Simple pool of waiter nodes (no heap needed in ISR-safe context)
#define MAX_FUTEX_WAITERS 128
static futex_waiter_t waiter_pool[MAX_FUTEX_WAITERS];
static spinlock_t futex_lock = {0};

// Hash buckets
static futex_waiter_t *futex_buckets[FUTEX_HASH_SIZE];

static uint32_t futex_hash(uint64_t addr, uint64_t cr3) {
    uint64_t key = addr ^ (cr3 >> 12);
    return (uint32_t)(key % FUTEX_HASH_SIZE);
}

static futex_waiter_t *alloc_waiter(void) {
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (waiter_pool[i].task == NULL) {
            return &waiter_pool[i];
        }
    }
    return NULL;
}

static void free_waiter(futex_waiter_t *w) {
    w->task = NULL;
    w->next = NULL;
}

int futex_op(uint64_t *addr, int op, uint32_t val) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    switch (op) {
        case FUTEX_WAIT: {
            spinlock_acquire(&futex_lock);

            // Check value atomically under lock
            if (*addr != val) {
                spinlock_release(&futex_lock);
                return -1; // Value changed, don't sleep
            }

            // Add to wait queue
            futex_waiter_t *w = alloc_waiter();
            if (!w) {
                spinlock_release(&futex_lock);
                return -1;
            }

            w->addr = (uint64_t)addr;
            w->cr3 = cr3;
            w->task = task_get_by_id(task_current_id());

            uint32_t bucket = futex_hash((uint64_t)addr, cr3);
            w->next = futex_buckets[bucket];
            futex_buckets[bucket] = w;

            spinlock_release(&futex_lock);

            // Block
            task_block();
            return 0;
        }

        case FUTEX_WAKE: {
            spinlock_acquire(&futex_lock);

            uint32_t bucket = futex_hash((uint64_t)addr, cr3);
            futex_waiter_t **prev = &futex_buckets[bucket];
            futex_waiter_t *cur = futex_buckets[bucket];
            int woken = 0;

            while (cur && (uint32_t)woken < val) {
                if (cur->addr == (uint64_t)addr && cur->cr3 == cr3) {
                    // Remove from list
                    *prev = cur->next;
                    task_t *t = cur->task;
                    free_waiter(cur);
                    cur = *prev;

                    // Wake the task
                    if (t) {
                        task_unblock(t);
                        woken++;
                    }
                } else {
                    prev = &cur->next;
                    cur = cur->next;
                }
            }

            spinlock_release(&futex_lock);
            return woken;
        }

        default:
            return -1;
    }
}
