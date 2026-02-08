#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "spinlock.h"
#include "sched.h"

typedef struct {
    spinlock_t lock;
    volatile int count;
    task_t *wait_head;
    task_t *wait_tail;
} semaphore_t;

void sem_init(semaphore_t *s, int initial_count);
void sem_wait(semaphore_t *s);      // Decrement (P operation), blocks if count == 0
void sem_post(semaphore_t *s);      // Increment (V operation), wakes one waiter
int sem_try_wait(semaphore_t *s);   // Returns 1 if decremented, 0 if would block

#endif
